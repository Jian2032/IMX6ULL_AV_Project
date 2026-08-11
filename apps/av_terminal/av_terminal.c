/* SPDX-License-Identifier: MIT */
/*
 * av_terminal.c - AV-INTEGRATION-R5 final stability and fault audit terminal
 *
 * R5 keeps R4's single-camera and bounded-rate architecture, makes every
 * console and HTTP observation use one serialized system snapshot.  Video
 * arrival and audio packet timestamps share this process's CLOCK_MONOTONIC
 * domain, and adds process resource telemetry plus explicitly synthetic health
 * faults for deterministic final acceptance.  No fault mode touches hardware.
 *
 * Thread ownership in this revision:
 *
 *   av_mjpeg capture pthread  : the only DQBUF/QBUF owner
 *   av_mjpeg encoder pthread  : cacheable YUYV -> JPEG
 *   LCD consumer pthread      : newest raw copy -> RGB565 -> hidden fb page
 *   av_audio producer pthread : ALSA READI_FRAMES -> audio ring
 *   audio monitor pthread     : drains every audio period and measures levels
 *   HTTP accept pthread       : short control requests
 *   HTTP network pthread      : one client-owned MJPEG socket/copy
 *   main thread               : lifecycle, once-per-second status and signals
 *
 * These timestamps are diagnostics, not media presentation timestamps: ALSA
 * and V4L2 still run independently and no sample-level A/V resampling occurs.
 */

#define _POSIX_C_SOURCE 200809L

#include "av_audio_capture.h"
#include "av_http_server.h"
#include "av_lcd_preview.h"
#include "av_mjpeg_pipeline.h"

#include <errno.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#define AV_INTEGRATION_VERSION       "AV-R5"
#define AV_DEFAULT_SECONDS           30U
#define AV_MAX_SECONDS               86400U
#define AV_STATUS_PERIOD_MS          1000U
#define AV_AUDIO_READ_TIMEOUT_MS     250U

#define AV_VIDEO_WIDTH               640U
#define AV_VIDEO_HEIGHT              480U
#define AV_VIDEO_FPS                 30U
#define AV_VIDEO_CAPTURE_MODE        0U
#define AV_VIDEO_BUFFER_COUNT        4U
#define AV_JPEG_QUALITY              80
#define AV_JPEG_FPS                  5U
#define AV_LCD_FPS                   15U
#define AV_HTTP_DEFAULT_PORT         8080U
#define AV_DEFAULT_FAULT_AFTER       5U

static volatile sig_atomic_t g_stop_requested;

/* Statistics owned by the audio monitor consumer, not by the ALSA producer. */
struct av_audio_monitor_stats {
	uint64_t packets;
	uint64_t frames;
	uint64_t sequence_gaps;
	uint64_t timeouts;
	uint64_t first_timestamp_us;
	uint64_t last_timestamp_us;
	uint64_t first_frame;
	uint64_t last_first_frame;
	uint64_t last_sequence;
	unsigned int last_packet_frames;
	int have_sequence;
	int32_t peak[AV_AUDIO_CHANNELS];
	uint64_t square_sum[AV_AUDIO_CHANNELS];
	int failed;
	int error_number;
};

struct av_audio_monitor {
	struct av_audio_capture *capture;
	pthread_t thread;
	pthread_mutex_t lock;
	int lock_initialized;
	int thread_started;
	int16_t *period_samples;
	unsigned int period_frames;
	struct av_audio_monitor_stats stats;
};

/* A nonzero bit identifies the exact subsystem that degraded the snapshot. */
#define AV_HEALTH_VIDEO_FAILED       UINT32_C(0x00000001)
#define AV_HEALTH_VIDEO_GAP          UINT32_C(0x00000002)
#define AV_HEALTH_VIDEO_TIMEOUT      UINT32_C(0x00000004)
#define AV_HEALTH_LCD_FAILED         UINT32_C(0x00000008)
#define AV_HEALTH_AUDIO_FAILED       UINT32_C(0x00000010)
#define AV_HEALTH_AUDIO_XRUN         UINT32_C(0x00000020)
#define AV_HEALTH_AUDIO_DROP         UINT32_C(0x00000040)
#define AV_HEALTH_MONITOR_FAILED     UINT32_C(0x00000080)
#define AV_HEALTH_AUDIO_GAP          UINT32_C(0x00000100)
#define AV_HEALTH_HTTP_FAILED        UINT32_C(0x00000200)
#define AV_HEALTH_HTTP_SEND          UINT32_C(0x00000400)

struct av_system_snapshot {
	uint64_t serial;
	uint64_t collected_at_us;
	uint64_t master_time_us;
	uint64_t collection_us;
	uint32_t health_flags;
	struct av_mjpeg_pipeline_stats video;
	struct av_lcd_preview_stats lcd;
	struct av_audio_stats audio_capture;
	struct av_audio_monitor_stats audio;
	struct av_http_server_stats http;
	double capture_fps;
	double encode_fps;
	double lcd_fps;
	double audio_rate;
	double average_send_ms;
	uint64_t video_age_us;
	uint64_t audio_age_us;
	int64_t video_audio_head_delta_us;
	int64_t video_audio_span_delta_us;
	const char *fault_name;
	uint32_t synthetic_health_flags;
	long peak_rss_kb;
	uint64_t user_cpu_us;
	uint64_t system_cpu_us;
	long voluntary_context_switches;
	long involuntary_context_switches;
};

/* Objects sampled by the HTTP status/health callbacks while all are alive. */
struct av_integration_context {
	struct av_mjpeg_pipeline *video;
	struct av_lcd_preview *lcd;
	struct av_audio_capture *audio;
	struct av_audio_monitor *monitor;
	struct av_http_server *http;
	pthread_mutex_t snapshot_lock;
	int snapshot_lock_initialized;
	uint64_t started_us;
	uint64_t next_snapshot_serial;
	const char *fault_name;
	uint32_t fault_mask;
	uint64_t fault_after_us;
};

static void av_signal_handler(int signal_number)
{
	(void)signal_number;
	g_stop_requested = 1;
}

static int av_install_signal_handlers(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_handler = av_signal_handler;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) < 0 ||
	    sigaction(SIGTERM, &action, NULL) < 0)
		return -1;
	return 0;
}

static uint64_t av_now_us(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return (uint64_t)now.tv_sec * UINT64_C(1000000) +
	       (uint64_t)now.tv_nsec / UINT64_C(1000);
}

static int av_parse_seconds(const char *text, unsigned int *seconds)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' ||
	    parsed > AV_MAX_SECONDS)
		return -1;
	*seconds = (unsigned int)parsed;
	return 0;
}

static int av_parse_port(const char *text, unsigned int *port)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' ||
	    parsed == 0 || parsed > 65535UL)
		return -1;
	*port = (unsigned int)parsed;
	return 0;
}

static int av_parse_fault(const char *text, const char **name, uint32_t *mask)
{
	if (strcmp(text, "none") == 0) {
		*name = "none";
		*mask = 0U;
	} else if (strcmp(text, "video-gap") == 0) {
		*name = "video-gap";
		*mask = AV_HEALTH_VIDEO_GAP;
	} else if (strcmp(text, "video-timeout") == 0) {
		*name = "video-timeout";
		*mask = AV_HEALTH_VIDEO_TIMEOUT;
	} else if (strcmp(text, "audio-xrun") == 0) {
		*name = "audio-xrun";
		*mask = AV_HEALTH_AUDIO_XRUN;
	} else if (strcmp(text, "audio-drop") == 0) {
		*name = "audio-drop";
		*mask = AV_HEALTH_AUDIO_DROP;
	} else if (strcmp(text, "http-send") == 0) {
		*name = "http-send";
		*mask = AV_HEALTH_HTTP_SEND;
	} else {
		return -1;
	}
	return 0;
}

static void av_usage(const char *program)
{
	printf("Usage: %s [seconds] [video-device] [pcm-device] "
	       "[control-device] [fb-device] [http-port] "
	       "[fault-mode] [fault-after-seconds]\n", program);
	printf("Defaults: %u /dev/video0 /dev/snd/pcmC0D0c "
	       "/dev/snd/controlC0 /dev/fb0 %u\n",
	       AV_DEFAULT_SECONDS, AV_HTTP_DEFAULT_PORT);
	printf("seconds=0 runs until SIGINT or SIGTERM.\n");
	printf("fault-mode: none, video-gap, video-timeout, audio-xrun, "
	       "audio-drop or http-send (synthetic health flag only).\n");
	printf("%s runs one V4L2 producer feeding LCD and HTTP MJPEG while "
	       "ALSA captures concurrently.\n",
	       AV_INTEGRATION_VERSION);
}

static int av_monitor_init(struct av_audio_monitor *monitor,
			   struct av_audio_capture *capture)
{
	int error;

	memset(monitor, 0, sizeof(*monitor));
	monitor->capture = capture;
	monitor->period_frames = av_audio_period_frames(capture);
	if (monitor->period_frames == 0) {
		errno = EINVAL;
		return -1;
	}
	monitor->period_samples = calloc(
		(size_t)monitor->period_frames * AV_AUDIO_CHANNELS,
		sizeof(*monitor->period_samples));
	if (!monitor->period_samples)
		return -1;
	error = pthread_mutex_init(&monitor->lock, NULL);
	if (error) {
		free(monitor->period_samples);
		monitor->period_samples = NULL;
		errno = error;
		return -1;
	}
	monitor->lock_initialized = 1;
	return 0;
}

static void av_monitor_update_levels(struct av_audio_monitor_stats *stats,
				     const int16_t *samples,
				     unsigned int frames)
{
	unsigned int frame;
	unsigned int channel;

	for (frame = 0; frame < frames; ++frame) {
		for (channel = 0; channel < AV_AUDIO_CHANNELS; ++channel) {
			int32_t sample =
				samples[frame * AV_AUDIO_CHANNELS + channel];
			int32_t magnitude = sample < 0 ? -sample : sample;

			if (magnitude > stats->peak[channel])
				stats->peak[channel] = magnitude;
			stats->square_sum[channel] +=
				(uint64_t)((int64_t)sample * sample);
		}
	}
}

static void av_monitor_publish_packet(struct av_audio_monitor *monitor,
				      const struct av_audio_packet_info *packet)
{
	struct av_audio_monitor_stats *stats = &monitor->stats;

	pthread_mutex_lock(&monitor->lock);
	if (stats->packets == 0) {
		stats->first_timestamp_us = packet->timestamp_us;
		stats->first_frame = packet->first_frame;
	}

	if (stats->have_sequence) {
		if (packet->sequence > stats->last_sequence + 1U)
			stats->sequence_gaps +=
				packet->sequence - stats->last_sequence - 1U;
		else if (packet->sequence <= stats->last_sequence)
			stats->sequence_gaps++;
	}

	av_monitor_update_levels(stats, monitor->period_samples, packet->frames);
	stats->packets++;
	stats->frames += packet->frames;
	stats->last_timestamp_us = packet->timestamp_us;
	stats->last_first_frame = packet->first_frame;
	stats->last_sequence = packet->sequence;
	stats->last_packet_frames = packet->frames;
	stats->have_sequence = 1;
	pthread_mutex_unlock(&monitor->lock);
}

static void *av_monitor_thread_main(void *argument)
{
	struct av_audio_monitor *monitor = argument;

	for (;;) {
		struct av_audio_packet_info packet;
		int result;

		memset(&packet, 0, sizeof(packet));
		result = av_audio_read(monitor->capture,
				       monitor->period_samples,
				       monitor->period_frames, &packet,
				       AV_AUDIO_READ_TIMEOUT_MS);
		if (result == 0) {
			av_monitor_publish_packet(monitor, &packet);
			continue;
		}
		if (result == AV_AUDIO_TIMEOUT) {
			pthread_mutex_lock(&monitor->lock);
			monitor->stats.timeouts++;
			pthread_mutex_unlock(&monitor->lock);
			continue;
		}
		if (result == AV_AUDIO_STOPPED)
			break;

		pthread_mutex_lock(&monitor->lock);
		monitor->stats.failed = 1;
		monitor->stats.error_number = errno ? errno : EIO;
		pthread_mutex_unlock(&monitor->lock);
		break;
	}
	return NULL;
}

static int av_monitor_start(struct av_audio_monitor *monitor)
{
	int error;

	error = pthread_create(&monitor->thread, NULL,
			       av_monitor_thread_main, monitor);
	if (error) {
		errno = error;
		return -1;
	}
	monitor->thread_started = 1;
	return 0;
}

static void av_monitor_snapshot(struct av_audio_monitor *monitor,
				struct av_audio_monitor_stats *stats)
{
	pthread_mutex_lock(&monitor->lock);
	*stats = monitor->stats;
	pthread_mutex_unlock(&monitor->lock);
}

static void av_monitor_join(struct av_audio_monitor *monitor)
{
	if (!monitor->thread_started)
		return;
	(void)pthread_join(monitor->thread, NULL);
	monitor->thread_started = 0;
}

static void av_monitor_destroy(struct av_audio_monitor *monitor)
{
	av_monitor_join(monitor);
	if (monitor->lock_initialized)
		(void)pthread_mutex_destroy(&monitor->lock);
	free(monitor->period_samples);
	memset(monitor, 0, sizeof(*monitor));
}

static double av_rate(uint64_t units, uint64_t elapsed_us)
{
	if (elapsed_us == 0)
		return 0.0;
	return (double)units * 1000000.0 / (double)elapsed_us;
}

static double av_audio_rms(const struct av_audio_monitor_stats *stats,
			   unsigned int channel)
{
	double value;
	double estimate;
	unsigned int iteration;

	if (stats->frames == 0 || channel >= AV_AUDIO_CHANNELS)
		return 0.0;
	value = (double)stats->square_sum[channel] / (double)stats->frames;
	if (value <= 0.0)
		return 0.0;
	/* Newton iteration avoids making status reporting depend on libm sqrt(). */
	estimate = 32768.0;
	for (iteration = 0; iteration < 24; ++iteration)
		estimate = 0.5 * (estimate + value / estimate);
	return estimate;
}

static int64_t av_signed_delta_us(uint64_t left, uint64_t right)
{
	uint64_t difference;

	if (left >= right) {
		difference = left - right;
		return difference > (uint64_t)INT64_MAX ? INT64_MAX :
			(int64_t)difference;
	}
	difference = right - left;
	return difference > (uint64_t)INT64_MAX ? INT64_MIN :
		-(int64_t)difference;
}

static uint64_t av_timeval_us(const struct timeval *value)
{
	if (value->tv_sec < 0 || value->tv_usec < 0)
		return 0U;
	return (uint64_t)value->tv_sec * UINT64_C(1000000) +
		(uint64_t)value->tv_usec;
}

static uint32_t av_snapshot_health_flags(
	const struct av_system_snapshot *snapshot)
{
	uint32_t flags = 0;

	if (snapshot->video.failed)
		flags |= AV_HEALTH_VIDEO_FAILED;
	if (snapshot->video.driver_sequence_gaps != 0)
		flags |= AV_HEALTH_VIDEO_GAP;
	if (snapshot->video.capture_timeouts != 0)
		flags |= AV_HEALTH_VIDEO_TIMEOUT;
	if (snapshot->lcd.failed)
		flags |= AV_HEALTH_LCD_FAILED;
	if (snapshot->audio_capture.capture_error)
		flags |= AV_HEALTH_AUDIO_FAILED;
	if (snapshot->audio_capture.xruns != 0)
		flags |= AV_HEALTH_AUDIO_XRUN;
	if (snapshot->audio_capture.dropped_packets != 0 ||
	    snapshot->audio_capture.dropped_frames != 0)
		flags |= AV_HEALTH_AUDIO_DROP;
	if (snapshot->audio.failed)
		flags |= AV_HEALTH_MONITOR_FAILED;
	if (snapshot->audio.sequence_gaps != 0)
		flags |= AV_HEALTH_AUDIO_GAP;
	if (snapshot->http.failed)
		flags |= AV_HEALTH_HTTP_FAILED;
	if (snapshot->http.send_errors != 0)
		flags |= AV_HEALTH_HTTP_SEND;
	return flags;
}

/*
 * Serialize snapshot collection so the console and /status never interleave
 * two independent multi-module reads.  Individual drivers still advance
 * during collection; collection_us publishes that bounded observation window.
 */
static int av_collect_snapshot(struct av_integration_context *context,
	const struct av_http_server_stats *http_override,
	struct av_system_snapshot *snapshot)
{
	uint64_t begin;
	uint64_t end;
	uint64_t video_span;
	uint64_t audio_span;
	struct rusage usage;

	if (!context || !snapshot || !context->snapshot_lock_initialized)
		return -EINVAL;
	pthread_mutex_lock(&context->snapshot_lock);
	begin = av_now_us();
	memset(snapshot, 0, sizeof(*snapshot));
	av_mjpeg_pipeline_get_stats(context->video, &snapshot->video);
	av_lcd_preview_get_stats(context->lcd, &snapshot->lcd);
	av_audio_get_stats(context->audio, &snapshot->audio_capture);
	av_monitor_snapshot(context->monitor, &snapshot->audio);
	if (http_override)
		snapshot->http = *http_override;
	else if (context->http)
		av_http_server_get_stats(context->http, &snapshot->http);

	if (snapshot->video.captured_frames > 1U &&
	    snapshot->video.last_capture_time_us >
		snapshot->video.first_capture_time_us)
		snapshot->capture_fps = av_rate(
			snapshot->video.captured_frames - 1U,
			snapshot->video.last_capture_time_us -
			snapshot->video.first_capture_time_us);
	if (snapshot->video.encoded_frames > 1U &&
	    snapshot->video.last_encode_time_us >
		snapshot->video.first_encode_time_us)
		snapshot->encode_fps = av_rate(
			snapshot->video.encoded_frames - 1U,
			snapshot->video.last_encode_time_us -
			snapshot->video.first_encode_time_us);
	if (snapshot->lcd.displayed_frames > 1U &&
	    snapshot->lcd.last_present_time_us >
		snapshot->lcd.first_present_time_us)
		snapshot->lcd_fps = av_rate(snapshot->lcd.displayed_frames - 1U,
			snapshot->lcd.last_present_time_us -
			snapshot->lcd.first_present_time_us);
	if (snapshot->audio.packets > 1U &&
	    snapshot->audio.last_timestamp_us >
		snapshot->audio.first_timestamp_us &&
	    snapshot->audio.last_first_frame >= snapshot->audio.first_frame)
		snapshot->audio_rate = av_rate(
			snapshot->audio.last_first_frame -
			snapshot->audio.first_frame,
			snapshot->audio.last_timestamp_us -
			snapshot->audio.first_timestamp_us);
	if (snapshot->http.stream_frames != 0U)
		snapshot->average_send_ms =
			(double)snapshot->http.stream_send_us /
			(double)snapshot->http.stream_frames / 1000.0;

	if (snapshot->video.last_capture_time_us != 0U &&
	    snapshot->audio.last_timestamp_us != 0U)
		snapshot->video_audio_head_delta_us = av_signed_delta_us(
			snapshot->video.last_capture_time_us,
			snapshot->audio.last_timestamp_us);
	if (snapshot->video.first_capture_time_us != 0U &&
	    snapshot->video.last_capture_time_us >=
		snapshot->video.first_capture_time_us &&
	    snapshot->audio.first_timestamp_us != 0U &&
	    snapshot->audio.last_timestamp_us >=
		snapshot->audio.first_timestamp_us) {
		video_span = snapshot->video.last_capture_time_us -
			snapshot->video.first_capture_time_us;
		audio_span = snapshot->audio.last_timestamp_us -
			snapshot->audio.first_timestamp_us;
		snapshot->video_audio_span_delta_us =
			av_signed_delta_us(video_span, audio_span);
	}
	if (getrusage(RUSAGE_SELF, &usage) == 0) {
		snapshot->peak_rss_kb = usage.ru_maxrss;
		snapshot->user_cpu_us = av_timeval_us(&usage.ru_utime);
		snapshot->system_cpu_us = av_timeval_us(&usage.ru_stime);
		snapshot->voluntary_context_switches = usage.ru_nvcsw;
		snapshot->involuntary_context_switches = usage.ru_nivcsw;
	}
	end = av_now_us();
	snapshot->serial = ++context->next_snapshot_serial;
	snapshot->collected_at_us = end;
	snapshot->master_time_us = context->started_us != 0U &&
		end >= context->started_us ? end - context->started_us : 0U;
	snapshot->collection_us = end >= begin ? end - begin : 0U;
	snapshot->fault_name = context->fault_name ? context->fault_name : "none";
	if (context->fault_mask != 0U && context->started_us != 0U &&
	    snapshot->master_time_us >= context->fault_after_us)
		snapshot->synthetic_health_flags = context->fault_mask;
	if (snapshot->video.last_capture_time_us != 0U &&
	    end >= snapshot->video.last_capture_time_us)
		snapshot->video_age_us = end -
			snapshot->video.last_capture_time_us;
	if (snapshot->audio.last_timestamp_us != 0U &&
	    end >= snapshot->audio.last_timestamp_us)
		snapshot->audio_age_us = end - snapshot->audio.last_timestamp_us;
	snapshot->health_flags = av_snapshot_health_flags(snapshot) |
		snapshot->synthetic_health_flags;
	pthread_mutex_unlock(&context->snapshot_lock);
	return 0;
}

static void av_print_status(const struct av_system_snapshot *snapshot)
{
	const struct av_mjpeg_pipeline_stats *video = &snapshot->video;
	const struct av_lcd_preview_stats *lcd = &snapshot->lcd;
	const struct av_audio_stats *audio_capture = &snapshot->audio_capture;
	const struct av_audio_monitor_stats *audio = &snapshot->audio;
	const struct av_http_server_stats *http = &snapshot->http;

	printf("  snapshot %5llu health=0x%08x collect=%llu us at %6.1f s\n",
	       (unsigned long long)snapshot->serial, snapshot->health_flags,
	       (unsigned long long)snapshot->collection_us,
	       snapshot->master_time_us / 1000000.0);
	printf("                    resource peak_rss=%ld KiB cpu=%.3f/%.3f s "
	       "csw=%ld/%ld fault=%s%s\n",
	       snapshot->peak_rss_kb, snapshot->user_cpu_us / 1000000.0,
	       snapshot->system_cpu_us / 1000000.0,
	       snapshot->voluntary_context_switches,
	       snapshot->involuntary_context_switches, snapshot->fault_name,
	       snapshot->synthetic_health_flags ? "(triggered)" : "");
	printf("                    video=%llu/%.2f fps jpeg=%llu/%.2f fps "
	       "raw_drop=%llu gaps=%llu age=%.2f ms copy=%.2f ms\n",
	       (unsigned long long)video->captured_frames, snapshot->capture_fps,
	       (unsigned long long)video->encoded_frames, snapshot->encode_fps,
	       (unsigned long long)video->raw_frames_dropped,
	       (unsigned long long)video->driver_sequence_gaps,
	       snapshot->video_age_us / 1000.0,
	       video->captured_frames ?
		(double)video->copy_us / video->captured_frames / 1000.0 : 0.0);
	printf("                    lcd=%llu/%.2f fps skipped=%llu page=%u "
	       "copy/convert/blit/flip=%.2f/%.2f/%.2f/%.2f ms\n",
	       (unsigned long long)lcd->displayed_frames, snapshot->lcd_fps,
	       (unsigned long long)lcd->source_frames_skipped, lcd->front_page,
	       lcd->displayed_frames ?
		(double)lcd->raw_copy_us / lcd->displayed_frames / 1000.0 : 0.0,
	       lcd->displayed_frames ?
		(double)lcd->convert_us / lcd->displayed_frames / 1000.0 : 0.0,
	       lcd->displayed_frames ?
		(double)lcd->blit_us / lcd->displayed_frames / 1000.0 : 0.0,
	       lcd->displayed_frames ?
		(double)lcd->flip_us / lcd->displayed_frames / 1000.0 : 0.0);
	printf("                    audio=%llu/%.2f frame/s ring=%u/%u "
	       "drop=%llu xrun=%u age=%.2f ms rms=%.1f/%.1f\n",
	       (unsigned long long)audio->frames, snapshot->audio_rate,
	       audio_capture->ring_queued, audio_capture->ring_slots,
	       (unsigned long long)audio_capture->dropped_packets,
	       audio_capture->xruns, snapshot->audio_age_us / 1000.0,
	       av_audio_rms(audio, 0), av_audio_rms(audio, 1));
	printf("                    A/V head=%+.3f ms span=%+.3f ms; "
	       "HTTP=%s frames=%llu skipped=%llu send=%.2f ms errors=%llu\n",
	       snapshot->video_audio_head_delta_us / 1000.0,
	       snapshot->video_audio_span_delta_us / 1000.0,
	       http->stream_client_active ? "active" : "idle",
	       (unsigned long long)http->stream_frames,
	       (unsigned long long)http->client_frames_skipped,
	       snapshot->average_send_ms,
	       (unsigned long long)http->send_errors);
}

static int av_health_passed(const struct av_system_snapshot *snapshot,
			    int cleanup_failed)
{
	if (cleanup_failed || snapshot->health_flags != 0U)
		return 0;
	if (snapshot->video.captured_frames == 0U ||
	    snapshot->video.encoded_frames == 0U ||
	    snapshot->lcd.displayed_frames == 0U || snapshot->audio.frames == 0U)
		return 0;
	if (snapshot->audio_capture.ring_queued != 0U ||
	    snapshot->audio_capture.captured_frames !=
		snapshot->audio_capture.consumed_frames +
		snapshot->audio_capture.dropped_frames ||
	    snapshot->audio.frames != snapshot->audio_capture.consumed_frames)
		return 0;
	return 1;
}

static int av_http_health_ok(void *opaque)
{
	struct av_system_snapshot snapshot;

	if (av_collect_snapshot(opaque, NULL, &snapshot) < 0)
		return 0;
	return snapshot.health_flags == 0U;
}

static int av_http_format_status(void *opaque,
				 const struct av_http_server_stats *http,
				 char *destination, size_t capacity)
{
	struct av_system_snapshot snapshot;
	double average_jpeg_bytes;
	double estimated_mjpeg_mbps;
	int length;

	if (av_collect_snapshot(opaque, http, &snapshot) < 0)
		return -EIO;
	average_jpeg_bytes = snapshot.video.encoded_frames ?
		(double)snapshot.video.jpeg_bytes /
		snapshot.video.encoded_frames : 0.0;
	estimated_mjpeg_mbps = average_jpeg_bytes * snapshot.encode_fps *
		8.0 / 1000000.0;
	length = snprintf(destination, capacity,
		"{\n"
		"  \"version\": \"%s\",\n"
		"  \"system\": \"%s\",\n"
		"  \"health_flags\": %u,\n"
		"  \"health_hex\": \"0x%08x\",\n"
		"  \"fault_injection\": \"%s\",\n"
		"  \"fault_triggered\": %s,\n"
		"  \"snapshot_serial\": %llu,\n"
		"  \"master_time_us\": %llu,\n"
		"  \"snapshot_collection_us\": %llu,\n"
		"  \"peak_rss_kb\": %ld,\n"
		"  \"user_cpu_ms\": %.3f,\n"
		"  \"system_cpu_ms\": %.3f,\n"
		"  \"voluntary_context_switches\": %ld,\n"
		"  \"involuntary_context_switches\": %ld,\n"
		"  \"http\": \"ready\",\n"
		"  \"camera\": \"%s\",\n"
		"  \"jpeg\": \"quality-%d-422-%u-fps\",\n"
		"  \"lcd\": \"rgb565-%u-fps\",\n"
		"  \"audio\": \"48000-stereo-s16le\",\n"
		"  \"time_base\": \"clock-monotonic-process\",\n"
		"  \"orientation\": \"rotate-180\",\n"
		"  \"queue_policy\": \"latest-frame-drop-oldest\",\n"
		"  \"stream_client\": \"%s\",\n"
		"  \"uptime_ms\": %llu,\n"
		"  \"http_uptime_ms\": %llu,\n"
		"  \"accepted\": %llu,\n"
		"  \"completed\": %llu,\n"
		"  \"bad_requests\": %llu,\n"
		"  \"rejected_streams\": %llu,\n"
		"  \"stream_sessions\": %llu,\n"
		"  \"stream_frames\": %llu,\n"
		"  \"client_frames_skipped\": %llu,\n"
		"  \"send_errors\": %llu,\n"
		"  \"captured_frames\": %llu,\n"
		"  \"encoded_frames\": %llu,\n"
		"  \"driver_sequence_gaps\": %llu,\n"
		"  \"capture_timeouts\": %llu,\n"
		"  \"raw_frames_dropped\": %llu,\n"
		"  \"capture_fps\": %.3f,\n"
		"  \"encode_fps\": %.3f,\n"
		"  \"average_jpeg_bytes\": %.2f,\n"
		"  \"estimated_mjpeg_mbps\": %.3f,\n"
		"  \"http_bytes_sent\": %llu,\n"
		"  \"video_age_ms\": %.3f,\n"
		"  \"lcd_frames\": %llu,\n"
		"  \"lcd_source_frames_skipped\": %llu,\n"
		"  \"lcd_fps\": %.3f,\n"
		"  \"audio_captured_frames\": %llu,\n"
		"  \"audio_consumed_frames\": %llu,\n"
		"  \"audio_dropped_frames\": %llu,\n"
		"  \"audio_xruns\": %u,\n"
		"  \"audio_ring_queued\": %u,\n"
		"  \"audio_rate\": %.2f,\n"
		"  \"audio_age_ms\": %.3f,\n"
		"  \"video_audio_head_delta_ms\": %.3f,\n"
		"  \"video_audio_span_delta_ms\": %.3f,\n"
		"  \"average_send_ms\": %.3f\n"
		"}\n",
		AV_INTEGRATION_VERSION,
		snapshot.health_flags ? "degraded" : "running",
		snapshot.health_flags, snapshot.health_flags,
		snapshot.fault_name,
		snapshot.synthetic_health_flags ? "true" : "false",
		(unsigned long long)snapshot.serial,
		(unsigned long long)snapshot.master_time_us,
		(unsigned long long)snapshot.collection_us,
		snapshot.peak_rss_kb, snapshot.user_cpu_us / 1000.0,
		snapshot.system_cpu_us / 1000.0,
		snapshot.voluntary_context_switches,
		snapshot.involuntary_context_switches,
		snapshot.video.failed ? "failed" : "streaming",
		AV_JPEG_QUALITY, AV_JPEG_FPS, AV_LCD_FPS,
		snapshot.http.stream_client_active ? "active" : "idle",
		(unsigned long long)(snapshot.master_time_us / 1000U),
		(unsigned long long)snapshot.http.uptime_ms,
		(unsigned long long)snapshot.http.accepted,
		(unsigned long long)snapshot.http.completed,
		(unsigned long long)snapshot.http.bad_requests,
		(unsigned long long)snapshot.http.rejected_streams,
		(unsigned long long)snapshot.http.stream_sessions,
		(unsigned long long)snapshot.http.stream_frames,
		(unsigned long long)snapshot.http.client_frames_skipped,
		(unsigned long long)snapshot.http.send_errors,
		(unsigned long long)snapshot.video.captured_frames,
		(unsigned long long)snapshot.video.encoded_frames,
		(unsigned long long)snapshot.video.driver_sequence_gaps,
		(unsigned long long)snapshot.video.capture_timeouts,
		(unsigned long long)snapshot.video.raw_frames_dropped,
		snapshot.capture_fps, snapshot.encode_fps,
		average_jpeg_bytes, estimated_mjpeg_mbps,
		(unsigned long long)snapshot.http.bytes_sent,
		snapshot.video_age_us / 1000.0,
		(unsigned long long)snapshot.lcd.displayed_frames,
		(unsigned long long)snapshot.lcd.source_frames_skipped,
		snapshot.lcd_fps,
		(unsigned long long)snapshot.audio_capture.captured_frames,
		(unsigned long long)snapshot.audio_capture.consumed_frames,
		(unsigned long long)snapshot.audio_capture.dropped_frames,
		snapshot.audio_capture.xruns,
		snapshot.audio_capture.ring_queued, snapshot.audio_rate,
		snapshot.audio_age_us / 1000.0,
		snapshot.video_audio_head_delta_us / 1000.0,
		snapshot.video_audio_span_delta_us / 1000.0,
		snapshot.average_send_ms);
	if (length < 0 || (size_t)length >= capacity)
		return -ENOSPC;
	return length;
}

int main(int argc, char **argv)
{
	const char *video_device = "/dev/video0";
	const char *pcm_device = "/dev/snd/pcmC0D0c";
	const char *control_device = "/dev/snd/controlC0";
	const char *framebuffer_device = "/dev/fb0";
	unsigned int seconds = AV_DEFAULT_SECONDS;
	unsigned int http_port = AV_HTTP_DEFAULT_PORT;
	unsigned int fault_after_seconds = AV_DEFAULT_FAULT_AFTER;
	const char *fault_name = "none";
	uint32_t fault_mask = 0U;
	struct av_mjpeg_config video_config;
	struct av_audio_config audio_config;
	struct av_http_server_config http_config;
	struct av_mjpeg_pipeline *video = NULL;
	struct av_audio_capture *audio = NULL;
	struct av_lcd_preview *lcd = NULL;
	struct av_http_server *http = NULL;
	struct av_audio_monitor monitor;
	struct av_integration_context integration;
	struct av_mjpeg_info video_info;
	struct av_system_snapshot snapshot;
	uint64_t started_us = 0;
	int monitor_initialized = 0;
	int video_started = 0;
	int audio_started = 0;
	int lcd_started = 0;
	int http_started = 0;
	int interrupted = 0;
	int cleanup_failed = 0;
	int result = EXIT_FAILURE;
	int api_result;

	memset(&monitor, 0, sizeof(monitor));
	memset(&integration, 0, sizeof(integration));
	if (argc > 1 && strcmp(argv[1], "--help") == 0) {
		av_usage(argv[0]);
		return EXIT_SUCCESS;
	}
	if (argc > 9 ||
	    (argc > 1 && av_parse_seconds(argv[1], &seconds) < 0)) {
		av_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc > 2)
		video_device = argv[2];
	if (argc > 3)
		pcm_device = argv[3];
	if (argc > 4)
		control_device = argv[4];
	if (argc > 5)
		framebuffer_device = argv[5];
	if (argc > 6 && av_parse_port(argv[6], &http_port) < 0) {
		fprintf(stderr, "Invalid HTTP port: %s\n", argv[6]);
		av_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc > 7 && av_parse_fault(argv[7], &fault_name, &fault_mask) < 0) {
		fprintf(stderr, "Invalid synthetic fault mode: %s\n", argv[7]);
		av_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc > 8 &&
	    av_parse_seconds(argv[8], &fault_after_seconds) < 0) {
		fprintf(stderr, "Invalid fault delay: %s\n", argv[8]);
		av_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (av_install_signal_handlers() < 0) {
		fprintf(stderr, "Cannot install signal handlers: %s\n",
			strerror(errno));
		return EXIT_FAILURE;
	}
	api_result = pthread_mutex_init(&integration.snapshot_lock, NULL);
	if (api_result != 0) {
		fprintf(stderr, "Cannot initialize snapshot lock: %s\n",
			strerror(api_result));
		return EXIT_FAILURE;
	}
	integration.snapshot_lock_initialized = 1;
	integration.fault_name = fault_name;
	integration.fault_mask = fault_mask;
	integration.fault_after_us =
		(uint64_t)fault_after_seconds * UINT64_C(1000000);

	memset(&video_config, 0, sizeof(video_config));
	video_config.video_device = video_device;
	video_config.width = AV_VIDEO_WIDTH;
	video_config.height = AV_VIDEO_HEIGHT;
	video_config.fps = AV_VIDEO_FPS;
	video_config.capture_mode = AV_VIDEO_CAPTURE_MODE;
	video_config.video_buffer_count = AV_VIDEO_BUFFER_COUNT;
	video_config.pixel_format = V4L2_PIX_FMT_YUYV;
	video_config.jpeg_quality = AV_JPEG_QUALITY;
	video_config.encode_fps = AV_JPEG_FPS;

	memset(&audio_config, 0, sizeof(audio_config));
	audio_config.pcm_device = pcm_device;
	audio_config.control_device = control_device;
	audio_config.ring_slots = AV_AUDIO_DEFAULT_RING_SLOTS;

	printf("AV-INTEGRATION-R5 final stability/fault-audit terminal\n");
	printf("duration         : %u second(s)%s\n", seconds,
	       seconds ? "" : " (until signal)");
	printf("video pipeline   : %s, %ux%u YUYV at %u fps, "
	       "JPEG q%d at %u fps\n",
	       video_device, AV_VIDEO_WIDTH, AV_VIDEO_HEIGHT, AV_VIDEO_FPS,
	       AV_JPEG_QUALITY, AV_JPEG_FPS);
	printf("LCD budget       : %u fps, latest-frame policy\n", AV_LCD_FPS);
	printf("audio pipeline   : %s, %u Hz stereo S16_LE\n", pcm_device,
	       AV_AUDIO_SAMPLE_RATE);
	printf("control device   : %s\n", control_device);
	printf("framebuffer      : %s\n", framebuffer_device);
	printf("HTTP endpoint    : 0.0.0.0:%u, one MJPEG client\n", http_port);
	printf("fault injection  : %s", fault_name);
	if (fault_mask != 0U)
		printf(" after %u second(s), synthetic status only",
		       fault_after_seconds);
	printf("\n");

	api_result = av_mjpeg_pipeline_create(&video, &video_config);
	if (api_result < 0) {
		fprintf(stderr, "Video pipeline create failed: %s\n",
			strerror(-api_result));
		goto cleanup;
	}
	av_mjpeg_pipeline_get_info(video, &video_info);
	printf("video negotiated : %ux%u, line=%u size=%u, buffers=%u\n",
	       video_info.width, video_info.height, video_info.bytesperline,
	       video_info.sizeimage, video_info.video_buffer_count);
	api_result = av_lcd_preview_create(&lcd, video, framebuffer_device,
				   AV_LCD_FPS);
	if (api_result < 0) {
		fprintf(stderr, "LCD preview create failed: %s\n",
			strerror(-api_result));
		goto cleanup;
	}

	if (av_audio_open(&audio, &audio_config) < 0) {
		fprintf(stderr, "Audio pipeline open failed: %s\n",
			strerror(errno));
		goto cleanup;
	}
	if (av_monitor_init(&monitor, audio) < 0) {
		fprintf(stderr, "Audio monitor init failed: %s\n",
			strerror(errno));
		goto cleanup;
	}
	monitor_initialized = 1;
	integration.video = video;
	integration.lcd = lcd;
	integration.audio = audio;
	integration.monitor = &monitor;
	memset(&http_config, 0, sizeof(http_config));
	http_config.pipeline = video;
	http_config.port = http_port;
	http_config.version = AV_INTEGRATION_VERSION;
	http_config.format_status = av_http_format_status;
	http_config.health_ok = av_http_health_ok;
	http_config.callback_opaque = &integration;
	api_result = av_http_server_create(&http, &http_config);
	if (api_result < 0) {
		fprintf(stderr, "HTTP server create failed: %s\n",
			strerror(-api_result));
		goto cleanup;
	}
	integration.http = http;

	api_result = av_mjpeg_pipeline_start(video);
	if (api_result < 0) {
		fprintf(stderr, "Video pipeline start failed: %s\n",
			strerror(-api_result));
		goto cleanup;
	}
	video_started = 1;
	api_result = av_lcd_preview_start(lcd);
	if (api_result < 0) {
		fprintf(stderr, "LCD preview start failed: %s\n",
			strerror(-api_result));
		goto cleanup;
	}
	lcd_started = 1;
	if (av_audio_start(audio) < 0) {
		fprintf(stderr, "Audio pipeline start failed: %s\n",
			strerror(errno));
		goto cleanup;
	}
	audio_started = 1;
	if (av_monitor_start(&monitor) < 0) {
		fprintf(stderr, "Audio monitor start failed: %s\n",
			strerror(errno));
		goto cleanup;
	}
	api_result = av_http_server_start(http);
	if (api_result < 0) {
		fprintf(stderr, "HTTP server start failed: %s\n",
			strerror(-api_result));
		goto cleanup;
	}
	http_started = 1;
	started_us = av_now_us();
	pthread_mutex_lock(&integration.snapshot_lock);
	integration.started_us = started_us;
	pthread_mutex_unlock(&integration.snapshot_lock);
	printf("system state     : RUNNING, Ctrl+C requests a clean stop\n");
	printf("master time base : CLOCK_MONOTONIC, serialized system snapshots\n");
	printf("index URL        : http://192.168.1.50:%u/\n", http_port);
	printf("stream URL       : http://192.168.1.50:%u/stream.mjpg\n",
	       http_port);

	for (;;) {
		struct timespec delay;
		uint64_t runtime_us;

		if (g_stop_requested) {
			interrupted = 1;
			break;
		}
		runtime_us = av_now_us() - started_us;
		if (seconds != 0 &&
		    runtime_us >= (uint64_t)seconds * UINT64_C(1000000))
			break;

		delay.tv_sec = AV_STATUS_PERIOD_MS / 1000U;
		delay.tv_nsec =
			(long)(AV_STATUS_PERIOD_MS % 1000U) * 1000000L;
		while (nanosleep(&delay, &delay) < 0 && errno == EINTR &&
		       !g_stop_requested)
			;

		if (av_collect_snapshot(&integration, NULL, &snapshot) < 0) {
			fprintf(stderr, "Cannot collect system snapshot\n");
			break;
		}
		av_print_status(&snapshot);
		if (snapshot.health_flags &
		    (AV_HEALTH_VIDEO_FAILED | AV_HEALTH_LCD_FAILED |
		     AV_HEALTH_AUDIO_FAILED | AV_HEALTH_MONITOR_FAILED |
		     AV_HEALTH_HTTP_FAILED)) {
			fprintf(stderr, "A producer/consumer reported a fatal error\n");
			break;
		}
	}

cleanup:
	/*
	 * HTTP owns the only possibly blocking external socket.  Stop and join
	 * that consumer before stopping or freeing the JPEG producer it reads.
	 */
	if (http_started) {
		av_http_server_stop(http);
		http_started = 0;
	}
	/*
	 * Stop the ALSA producer first.  It marks producer_finished and wakes the
	 * monitor, which then drains any already-published periods before it sees
	 * AV_AUDIO_STOPPED.  Only after join may its buffer and mutex be freed.
	 */
	if (audio_started) {
		if (av_audio_stop(audio) < 0)
			cleanup_failed = 1;
		audio_started = 0;
	}
	/* Stopping the shared producer wakes a blocked LCD raw-copy wait. */
	if (video_started) {
		av_mjpeg_pipeline_stop(video);
		video_started = 0;
	}
	if (monitor_initialized)
		av_monitor_join(&monitor);
	if (lcd_started) {
		av_lcd_preview_join(lcd);
		lcd_started = 0;
	}

	if (video && lcd && audio && http && monitor_initialized) {
		if (av_collect_snapshot(&integration, NULL, &snapshot) < 0) {
			fprintf(stderr, "Final system snapshot failed\n");
			cleanup_failed = 1;
			memset(&snapshot, 0, sizeof(snapshot));
			snapshot.fault_name = "unavailable";
		}
		printf("system state     : STOPPED\n");
		printf("snapshot final   : serial=%llu master=%.3f s collect=%llu us "
		       "health=0x%08x\n",
		       (unsigned long long)snapshot.serial,
		       snapshot.master_time_us / 1000000.0,
		       (unsigned long long)snapshot.collection_us,
		       snapshot.health_flags);
		printf("resource final   : peak_rss=%ld KiB cpu=%.3f/%.3f s "
		       "csw=%ld/%ld fault=%s%s\n",
		       snapshot.peak_rss_kb, snapshot.user_cpu_us / 1000000.0,
		       snapshot.system_cpu_us / 1000000.0,
		       snapshot.voluntary_context_switches,
		       snapshot.involuntary_context_switches,
		       snapshot.fault_name,
		       snapshot.synthetic_health_flags ? "(triggered)" : "");
		printf("video final      : captured=%llu encoded=%llu "
		       "driver_gaps=%llu timeouts=%llu raw_dropped=%llu "
		       "capture_copy=%.2f ms\n",
		       (unsigned long long)snapshot.video.captured_frames,
		       (unsigned long long)snapshot.video.encoded_frames,
		       (unsigned long long)snapshot.video.driver_sequence_gaps,
		       (unsigned long long)snapshot.video.capture_timeouts,
		       (unsigned long long)snapshot.video.raw_frames_dropped,
		       snapshot.video.captured_frames ?
			(double)snapshot.video.copy_us /
			snapshot.video.captured_frames / 1000.0 : 0.0);
		printf("LCD final        : displayed=%llu skipped=%llu "
		       "last_seq=%u page=%u failed=%d\n",
		       (unsigned long long)snapshot.lcd.displayed_frames,
		       (unsigned long long)snapshot.lcd.source_frames_skipped,
		       snapshot.lcd.last_source_sequence, snapshot.lcd.front_page,
		       snapshot.lcd.failed);
		printf("audio final      : captured=%llu consumed=%llu "
		       "dropped=%llu xruns=%u queued=%u sequence_gaps=%llu\n",
		       (unsigned long long)snapshot.audio_capture.captured_frames,
		       (unsigned long long)snapshot.audio_capture.consumed_frames,
		       (unsigned long long)snapshot.audio_capture.dropped_frames,
		       snapshot.audio_capture.xruns,
		       snapshot.audio_capture.ring_queued,
		       (unsigned long long)snapshot.audio.sequence_gaps);
		printf("HTTP final       : accepted=%llu completed=%llu streams=%llu "
		       "frames=%llu skipped=%llu send_errors=%llu\n",
		       (unsigned long long)snapshot.http.accepted,
		       (unsigned long long)snapshot.http.completed,
		       (unsigned long long)snapshot.http.stream_sessions,
		       (unsigned long long)snapshot.http.stream_frames,
		       (unsigned long long)snapshot.http.client_frames_skipped,
		       (unsigned long long)snapshot.http.send_errors);
		printf("network final    : average_jpeg=%.2f bytes "
		       "estimated_payload=%.3f Mbit/s http_bytes=%llu\n",
		       snapshot.video.encoded_frames ?
			(double)snapshot.video.jpeg_bytes /
			snapshot.video.encoded_frames : 0.0,
		       snapshot.video.encoded_frames ?
			(double)snapshot.video.jpeg_bytes /
			snapshot.video.encoded_frames * snapshot.encode_fps *
			8.0 / 1000000.0 : 0.0,
		       (unsigned long long)snapshot.http.bytes_sent);
		printf("A/V final        : head=%+.3f ms span=%+.3f ms\n",
		       snapshot.video_audio_head_delta_us / 1000.0,
		       snapshot.video_audio_span_delta_us / 1000.0);
		if (av_health_passed(&snapshot,
				     cleanup_failed || started_us == 0U)) {
			if (interrupted)
				printf("[STOP] Signal requested a clean A/V shutdown.\n");
			else
				printf("[PASS] One camera fed LCD and HTTP while ALSA "
				       "captured without driver loss or XRUN.\n");
			result = EXIT_SUCCESS;
		} else {
			fprintf(stderr, "[FAIL] AV-R5 health criteria were not met.\n");
		}
	}

	if (http)
		av_http_server_destroy(http);
	if (monitor_initialized)
		av_monitor_destroy(&monitor);
	if (lcd)
		av_lcd_preview_destroy(lcd);
	if (audio)
		av_audio_close(audio);
	if (video)
		av_mjpeg_pipeline_destroy(video);
	if (integration.snapshot_lock_initialized)
		(void)pthread_mutex_destroy(&integration.snapshot_lock);
	return result;
}
