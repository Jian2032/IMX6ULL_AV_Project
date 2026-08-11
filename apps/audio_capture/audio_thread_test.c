/*
 * audio_thread_test.c - AUDIO-R3 producer/ring/consumer acceptance program
 *
 * The consumer copies PCM into RAM only.  WAV output happens after stop(), so
 * filesystem latency cannot make the ALSA producer miss a period deadline.
 */

#include "av_audio_capture.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_SECONDS 10U
#define MAX_SECONDS     60U
#define READ_TIMEOUT_MS 1000U

static volatile sig_atomic_t stop_requested;

struct level_stats {
	int32_t peak[AV_AUDIO_CHANNELS];
	uint64_t square_sum[AV_AUDIO_CHANNELS];
	uint64_t clipped[AV_AUDIO_CHANNELS];
	uint64_t frames;
};

static void signal_handler(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static int install_signal_handlers(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_handler = signal_handler;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) < 0 ||
	    sigaction(SIGTERM, &action, NULL) < 0)
		return -1;
	return 0;
}

static uint64_t now_us(void)
{
	struct timeval now;

	if (gettimeofday(&now, NULL) < 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000000ULL + (uint64_t)now.tv_usec;
}

static void update_levels(struct level_stats *stats, const int16_t *samples,
			  unsigned int frames)
{
	unsigned int frame;
	unsigned int channel;

	for (frame = 0; frame < frames; ++frame) {
		for (channel = 0; channel < AV_AUDIO_CHANNELS; ++channel) {
			int32_t sample = samples[frame * AV_AUDIO_CHANNELS + channel];
			int32_t magnitude = sample < 0 ? -sample : sample;

			if (magnitude > stats->peak[channel])
				stats->peak[channel] = magnitude;
			stats->square_sum[channel] +=
				(uint64_t)((int64_t)sample * sample);
			if (sample == INT16_MIN || sample == INT16_MAX)
				stats->clipped[channel]++;
		}
	}
	stats->frames += frames;
}

static double positive_square_root(double value)
{
	double estimate = 32768.0;
	unsigned int i;

	if (value <= 0.0)
		return 0.0;
	for (i = 0; i < 24; ++i)
		estimate = 0.5 * (estimate + value / estimate);
	return estimate;
}

static void print_levels(const struct level_stats *stats)
{
	unsigned int channel;

	if (stats->frames == 0)
		return;
	for (channel = 0; channel < AV_AUDIO_CHANNELS; ++channel) {
		double rms = positive_square_root(
			(double)stats->square_sum[channel] / stats->frames);

		printf("  channel %c    : peak=%d (%.2f%%), rms=%.2f (%.2f%%), "
		       "clipped=%llu\n", channel ? 'R' : 'L',
		       stats->peak[channel],
		       stats->peak[channel] * 100.0 / 32768.0,
		       rms, rms * 100.0 / 32768.0,
		       (unsigned long long)stats->clipped[channel]);
	}
}

static void put_le16(uint8_t *destination, uint16_t value)
{
	destination[0] = (uint8_t)value;
	destination[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *destination, uint32_t value)
{
	destination[0] = (uint8_t)value;
	destination[1] = (uint8_t)(value >> 8);
	destination[2] = (uint8_t)(value >> 16);
	destination[3] = (uint8_t)(value >> 24);
}

static int write_all(int fd, const void *buffer, size_t bytes)
{
	const uint8_t *position = buffer;

	while (bytes > 0) {
		ssize_t written = write(fd, position, bytes);

		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return -1;
		position += written;
		bytes -= (size_t)written;
	}
	return 0;
}

static int save_wav(const char *path, const int16_t *samples, uint64_t frames)
{
	uint64_t bytes64 = frames * AV_AUDIO_BYTES_PER_FRAME;
	uint8_t header[44];
	uint32_t bytes;
	int fd = -1;
	int result = -1;

	if (bytes64 > UINT32_MAX - 36U) {
		errno = EFBIG;
		return -1;
	}
	bytes = (uint32_t)bytes64;
	memset(header, 0, sizeof(header));
	memcpy(header, "RIFF", 4);
	put_le32(header + 4, 36U + bytes);
	memcpy(header + 8, "WAVEfmt ", 8);
	put_le32(header + 16, 16U);
	put_le16(header + 20, 1U);
	put_le16(header + 22, AV_AUDIO_CHANNELS);
	put_le32(header + 24, AV_AUDIO_SAMPLE_RATE);
	put_le32(header + 28,
		 AV_AUDIO_SAMPLE_RATE * AV_AUDIO_BYTES_PER_FRAME);
	put_le16(header + 32, AV_AUDIO_BYTES_PER_FRAME);
	put_le16(header + 34, AV_AUDIO_SAMPLE_BITS);
	memcpy(header + 36, "data", 4);
	put_le32(header + 40, bytes);

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	if (write_all(fd, header, sizeof(header)) < 0 ||
	    write_all(fd, samples, bytes) < 0)
		goto out;
	if (close(fd) < 0) {
		fd = -1;
		return -1;
	}
	fd = -1;
	result = 0;
out:
	if (fd >= 0)
		close(fd);
	return result;
}

static int parse_seconds(const char *text, unsigned int *seconds)
{
	char *end = NULL;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno || !end || *end != '\0' || value == 0 || value > MAX_SECONDS)
		return -1;
	*seconds = (unsigned int)value;
	return 0;
}

static void usage(const char *program)
{
	printf("Usage: %s [seconds] [output-wav] [pcm-device] [control-device]\n",
	       program);
	printf("Defaults: %u audio_thread_48k.wav /dev/snd/pcmC0D0c "
	       "/dev/snd/controlC0\n", DEFAULT_SECONDS);
}

int main(int argc, char **argv)
{
	const char *wav_path = "audio_thread_48k.wav";
	const char *pcm_path = "/dev/snd/pcmC0D0c";
	const char *control_path = "/dev/snd/controlC0";
	unsigned int seconds = DEFAULT_SECONDS;
	struct av_audio_config config;
	struct av_audio_capture *capture = NULL;
	struct av_audio_stats stream_stats;
	struct level_stats levels;
	int16_t *recording = NULL;
	int16_t *packet_samples = NULL;
	uint64_t target_frames;
	uint64_t recorded_frames = 0;
	uint64_t start_us;
	uint64_t end_us;
	uint64_t next_progress = AV_AUDIO_SAMPLE_RATE;
	unsigned int timeouts = 0;
	int started = 0;
	int exit_code = EXIT_FAILURE;

	if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
		usage(argv[0]);
		return EXIT_SUCCESS;
	}
	if (argc > 5 || (argc >= 2 && parse_seconds(argv[1], &seconds) < 0)) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc >= 3)
		wav_path = argv[2];
	if (argc >= 4)
		pcm_path = argv[3];
	if (argc >= 5)
		control_path = argv[4];
	if (install_signal_handlers() < 0)
		return EXIT_FAILURE;

	target_frames = (uint64_t)seconds * AV_AUDIO_SAMPLE_RATE;
	if (target_frames > SIZE_MAX / AV_AUDIO_BYTES_PER_FRAME) {
		fprintf(stderr, "Recording allocation would overflow size_t\n");
		return EXIT_FAILURE;
	}
	recording = malloc((size_t)target_frames * AV_AUDIO_BYTES_PER_FRAME);
	packet_samples = malloc((size_t)AV_AUDIO_PERIOD_FRAMES *
				AV_AUDIO_BYTES_PER_FRAME);
	if (!recording || !packet_samples) {
		fprintf(stderr, "Cannot allocate AUDIO-R3 buffers\n");
		goto cleanup;
	}

	memset(&config, 0, sizeof(config));
	config.pcm_device = pcm_path;
	config.control_device = control_path;
	config.ring_slots = AV_AUDIO_DEFAULT_RING_SLOTS;
	memset(&levels, 0, sizeof(levels));
	memset(&stream_stats, 0, sizeof(stream_stats));

	printf("AUDIO-R3 threaded ALSA capture acceptance test\n");
	printf("target          : %u seconds, %llu frames, %llu bytes\n",
	       seconds, (unsigned long long)target_frames,
	       (unsigned long long)(target_frames * AV_AUDIO_BYTES_PER_FRAME));
	printf("output WAV      : %s\n", wav_path);
	printf("input route     : ALPHA main MIC, LINPUT1/LINPUT2 differential\n");

	if (av_audio_open(&capture, &config) < 0 ||
	    av_audio_start(capture) < 0)
		goto cleanup;
	started = 1;
	start_us = now_us();

	while (recorded_frames < target_frames && !stop_requested) {
		struct av_audio_packet_info packet;
		unsigned int keep_frames;
		int read_result = av_audio_read(capture, packet_samples,
			AV_AUDIO_PERIOD_FRAMES, &packet, READ_TIMEOUT_MS);

		if (read_result == AV_AUDIO_TIMEOUT) {
			timeouts++;
			fprintf(stderr, "Audio consumer timeout #%u\n", timeouts);
			continue;
		}
		if (read_result == AV_AUDIO_STOPPED) {
			fprintf(stderr, "Audio producer stopped before target\n");
			break;
		}
		if (read_result < 0) {
			fprintf(stderr, "av_audio_read failed: %s\n", strerror(errno));
			goto cleanup;
		}
		keep_frames = packet.frames;
		if (keep_frames > target_frames - recorded_frames)
			keep_frames = (unsigned int)(target_frames - recorded_frames);
		memcpy(recording + recorded_frames * AV_AUDIO_CHANNELS,
		       packet_samples,
		       (size_t)keep_frames * AV_AUDIO_BYTES_PER_FRAME);
		update_levels(&levels, packet_samples, keep_frames);
		recorded_frames += keep_frames;
		if (recorded_frames >= next_progress || recorded_frames == target_frames) {
			printf("  consumed      : %llu/%llu frames (packet=%llu)\n",
			       (unsigned long long)recorded_frames,
			       (unsigned long long)target_frames,
			       (unsigned long long)packet.sequence);
			next_progress += AV_AUDIO_SAMPLE_RATE;
		}
	}

	/* Exclude thread-join/poll wake-up latency from the delivered-rate value. */
	end_us = now_us();
	if (av_audio_stop(capture) < 0)
		goto cleanup;
	started = 0;
	av_audio_get_stats(capture, &stream_stats);

	printf("thread stats    : captured=%llu packets/%llu frames\n",
	       (unsigned long long)stream_stats.captured_packets,
	       (unsigned long long)stream_stats.captured_frames);
	printf("  consumed      : %llu packets/%llu frames\n",
	       (unsigned long long)stream_stats.consumed_packets,
	       (unsigned long long)stream_stats.consumed_frames);
	printf("  ring          : high=%u/%u queued=%u\n",
	       stream_stats.ring_high_watermark, stream_stats.ring_slots,
	       stream_stats.ring_queued);
	printf("  loss/error    : dropped=%llu packets/%llu frames, xruns=%u, "
	       "timeouts=%u, error=%d\n",
	       (unsigned long long)stream_stats.dropped_packets,
	       (unsigned long long)stream_stats.dropped_frames,
	       stream_stats.xruns, timeouts, stream_stats.capture_error);
	printf("recording       : %llu frames, %.2f frames/s\n",
	       (unsigned long long)recorded_frames,
	       end_us > start_us ?
	       (double)recorded_frames * 1000000.0 / (end_us - start_us) : 0.0);
	print_levels(&levels);

	if (recorded_frames == 0) {
		fprintf(stderr, "No PCM frames were recorded; WAV was not created\n");
		goto cleanup;
	}
	if (save_wav(wav_path, recording, recorded_frames) < 0) {
		fprintf(stderr, "Cannot save %s: %s\n", wav_path,
			strerror(errno));
		goto cleanup;
	}
	printf("WAV saved       : %s (%llu bytes)\n", wav_path,
	       (unsigned long long)(recorded_frames * AV_AUDIO_BYTES_PER_FRAME + 44));

	if (!stop_requested && recorded_frames == target_frames &&
	    stream_stats.dropped_packets == 0 && stream_stats.xruns == 0 &&
	    stream_stats.capture_error == 0 && timeouts == 0) {
		printf("[PASS] Threaded ALSA capture and ring delivery completed "
		       "without loss.\n");
		exit_code = EXIT_SUCCESS;
	} else if (stop_requested && recorded_frames > 0) {
		printf("[STOP] Signal requested a clean shutdown.\n");
		exit_code = EXIT_SUCCESS;
	}

cleanup:
	if (started)
		(void)av_audio_stop(capture);
	av_audio_close(capture);
	free(packet_samples);
	free(recording);
	return exit_code;
}
