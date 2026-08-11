/* SPDX-License-Identifier: MIT */
/*
 * av_mjpeg_pipeline.c - STREAM-R4 bounded latest-frame producer pipeline
 *
 * Ownership is explicit at both boundaries:
 *
 *   CSI MMAP -> capture thread -> three cacheable YUYV slots
 *            -> encoder thread -> three complete JPEG slots
 *            -> network-owned JPEG copy
 *
 * A producer never waits for an old READY frame.  It replaces the oldest
 * READY slot, while READING/WRITING slots remain exclusive.  This keeps live
 * latency bounded and, most importantly, prevents socket behavior from ever
 * stopping VIDIOC_DQBUF/QBUF circulation.
 */

#define _POSIX_C_SOURCE 200809L

#include "av_mjpeg_pipeline.h"
#include "av_jpeg_encoder.h"
#include "av_video_capture.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AV_RAW_SLOT_COUNT          3U
#define AV_JPEG_SLOT_COUNT         3U
#define AV_CAPTURE_TIMEOUT_MS      100U
#define AV_MAX_CAPTURE_TIMEOUTS    20U

enum av_raw_slot_state {
	AV_RAW_FREE = 0,
	AV_RAW_WRITING,
	AV_RAW_READY,
	AV_RAW_READING,
};

enum av_jpeg_slot_state {
	AV_JPEG_FREE = 0,
	AV_JPEG_WRITING,
	AV_JPEG_READY,
};

struct av_raw_slot {
	unsigned char *data;
	enum av_raw_slot_state state;
	/* LCD snapshot copies pin immutable storage without holding pipeline->lock. */
	unsigned int snapshot_readers;
	size_t bytesused;
	uint32_t sequence;
	uint64_t timestamp_us;
	uint64_t capture_time_us;
	uint64_t serial;
};

struct av_jpeg_slot {
	unsigned char *data;
	enum av_jpeg_slot_state state;
	size_t size;
	uint32_t sequence;
	uint64_t timestamp_us;
	uint64_t capture_time_us;
	uint64_t serial;
};

struct av_mjpeg_pipeline {
	struct av_video video;
	struct av_jpeg_encoder encoder;
	struct av_mjpeg_info info;
	struct av_raw_slot raw_slots[AV_RAW_SLOT_COUNT];
	struct av_jpeg_slot jpeg_slots[AV_JPEG_SLOT_COUNT];

	pthread_mutex_t lock;
	pthread_cond_t raw_ready;
	pthread_cond_t jpeg_ready;
	pthread_t capture_thread;
	pthread_t encode_thread;
	int lock_initialized;
	int raw_cond_initialized;
	int jpeg_cond_initialized;
	int capture_thread_started;
	int encode_thread_started;
	int started;
	int stop_requested;
	int capture_finished;
	int encode_finished;
	int failed;
	int error_number;

	uint64_t next_raw_serial;
	uint64_t next_jpeg_serial;
	uint64_t encode_interval_us;
	struct av_mjpeg_pipeline_stats stats;
};

static uint64_t av_now_us(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return (uint64_t)now.tv_sec * UINT64_C(1000000) +
	       (uint64_t)now.tv_nsec / UINT64_C(1000);
}

static int av_pipeline_should_stop(struct av_mjpeg_pipeline *pipeline)
{
	int stop;

	pthread_mutex_lock(&pipeline->lock);
	stop = pipeline->stop_requested;
	pthread_mutex_unlock(&pipeline->lock);
	return stop;
}

/* Rate limiting applies only to the cacheable JPEG consumer, never capture. */
static void av_wait_encode_budget(struct av_mjpeg_pipeline *pipeline,
				  uint64_t cycle_begin_us)
{
	uint64_t deadline;

	if (pipeline->encode_interval_us == 0U ||
	    cycle_begin_us > UINT64_MAX - pipeline->encode_interval_us)
		return;
	deadline = cycle_begin_us + pipeline->encode_interval_us;
	while (!av_pipeline_should_stop(pipeline)) {
		struct timespec delay;
		uint64_t now = av_now_us();
		uint64_t remaining;

		if (now == 0U || now >= deadline)
			break;
		remaining = deadline - now;
		/* A short slice keeps Ctrl+C shutdown responsive. */
		if (remaining > 5000U)
			remaining = 5000U;
		delay.tv_sec = 0;
		delay.tv_nsec = (long)remaining * 1000L;
		while (nanosleep(&delay, &delay) < 0 && errno == EINTR)
			;
	}
}

static void av_pipeline_fail(struct av_mjpeg_pipeline *pipeline,
			     int error_number)
{
	pthread_mutex_lock(&pipeline->lock);
	pipeline->failed = 1;
	pipeline->error_number = error_number ? error_number : EIO;
	pipeline->stop_requested = 1;
	pipeline->stats.failed = 1;
	pipeline->stats.error_number = pipeline->error_number;
	pthread_cond_broadcast(&pipeline->raw_ready);
	pthread_cond_broadcast(&pipeline->jpeg_ready);
	pthread_mutex_unlock(&pipeline->lock);
}

/* Select FREE storage, or replace the oldest complete frame nobody is using. */
static struct av_raw_slot *av_reserve_raw_slot(
					struct av_mjpeg_pipeline *pipeline)
{
	struct av_raw_slot *oldest_ready = NULL;
	unsigned int index;

	pthread_mutex_lock(&pipeline->lock);
	for (index = 0; index < AV_RAW_SLOT_COUNT; ++index) {
		struct av_raw_slot *slot = &pipeline->raw_slots[index];

		if (slot->state == AV_RAW_FREE) {
			slot->state = AV_RAW_WRITING;
			pthread_mutex_unlock(&pipeline->lock);
			return slot;
		}
		if (slot->state == AV_RAW_READY &&
		    slot->snapshot_readers == 0U &&
		    (!oldest_ready || slot->serial < oldest_ready->serial))
			oldest_ready = slot;
	}

	if (oldest_ready) {
		oldest_ready->state = AV_RAW_WRITING;
		pipeline->stats.raw_frames_dropped++;
	}
	pthread_mutex_unlock(&pipeline->lock);
	return oldest_ready;
}

/*
 * Leave the CSI MMAP hot path as one contiguous copy.
 *
 * Pixel-by-pixel rotation directly from this DMA mapping measured 36.21 ms on
 * the i.MX6ULL, which already exceeds the complete 33.33 ms budget at 30 fps.
 * The raw slots are normal cacheable RAM, so orientation is deliberately
 * deferred to the consumers that already traverse the pixels.
 */
static int av_copy_yuyv_frame(struct av_mjpeg_pipeline *pipeline,
			      struct av_raw_slot *slot,
			      const struct av_video_frame *frame)
{
	size_t stride = pipeline->video.bytesperline;
	size_t height = pipeline->video.height;
	size_t required;

	if (stride == 0U || height == 0U || stride > SIZE_MAX / height)
		return -EMSGSIZE;
	required = stride * height;
	if (pipeline->video.sizeimage < required ||
	    frame->bytesused < pipeline->video.sizeimage)
		return -EMSGSIZE;
	memcpy(slot->data, frame->data, pipeline->video.sizeimage);
	return 0;
}

/*
 * Produce the API's oriented LCD snapshot from cacheable raw storage.
 * Vertical row copies use libc memcpy(); the horizontal pass only touches the
 * private cacheable destination, never the CSI DMA mapping.
 */
static void av_copy_rotate_180_cacheable(unsigned char *destination,
					 const unsigned char *source,
					 size_t width, size_t height,
					 size_t stride)
{
	size_t active_bytes = width * 2U;
	size_t row;

	for (row = 0; row < height; ++row) {
		size_t source_row = height - 1U - row;

		memcpy(destination + row * stride,
		       source + source_row * stride, stride);
	}
	for (row = 0; row < height; ++row) {
		unsigned char *left = destination + row * stride;
		unsigned char *right = left + active_bytes - 4U;

		while (left < right) {
			unsigned char left_y0 = left[0];
			unsigned char left_u = left[1];
			unsigned char left_y1 = left[2];
			unsigned char left_v = left[3];
			unsigned char right_y0 = right[0];
			unsigned char right_u = right[1];
			unsigned char right_y1 = right[2];
			unsigned char right_v = right[3];

			left[0] = right_y1;
			left[1] = right_u;
			left[2] = right_y0;
			left[3] = right_v;
			right[0] = left_y1;
			right[1] = left_u;
			right[2] = left_y0;
			right[3] = left_v;
			left += 4U;
			right -= 4U;
		}
		if (left == right) {
			unsigned char y0 = left[0];

			left[0] = left[2];
			left[2] = y0;
		}
	}
}

static void av_publish_raw_slot(struct av_mjpeg_pipeline *pipeline,
				struct av_raw_slot *slot,
				const struct av_video_frame *frame,
				uint64_t capture_time_us,
				uint64_t copy_us)
{
	pthread_mutex_lock(&pipeline->lock);
	slot->bytesused = pipeline->video.sizeimage;
	slot->sequence = frame->sequence;
	slot->timestamp_us = frame->timestamp_us;
	slot->capture_time_us = capture_time_us;
	slot->serial = ++pipeline->next_raw_serial;
	slot->state = AV_RAW_READY;

	if (pipeline->stats.captured_frames == 0) {
		pipeline->stats.first_sequence = frame->sequence;
		pipeline->stats.first_capture_timestamp_us = frame->timestamp_us;
		pipeline->stats.first_capture_time_us = capture_time_us;
	} else {
		uint32_t delta = frame->sequence - pipeline->stats.last_sequence;

		if (delta > 1U && delta < UINT32_C(0x80000000))
			pipeline->stats.driver_sequence_gaps += delta - 1U;
		else if (delta == 0U || delta >= UINT32_C(0x80000000))
			pipeline->stats.driver_sequence_gaps++;
	}
	pipeline->stats.last_sequence = frame->sequence;
	pipeline->stats.last_capture_timestamp_us = frame->timestamp_us;
	pipeline->stats.last_capture_time_us = capture_time_us;
	pipeline->stats.captured_frames++;
	pipeline->stats.copy_us += copy_us;
	/* Encoder and optional LCD consumers must both observe the new serial. */
	pthread_cond_broadcast(&pipeline->raw_ready);
	pthread_mutex_unlock(&pipeline->lock);
}

static void av_finish_capture(struct av_mjpeg_pipeline *pipeline)
{
	pthread_mutex_lock(&pipeline->lock);
	pipeline->capture_finished = 1;
	pthread_cond_broadcast(&pipeline->raw_ready);
	pthread_mutex_unlock(&pipeline->lock);
}

/* Only this thread owns VIDIOC_DQBUF/QBUF calls after streaming starts. */
static void *av_capture_thread_main(void *argument)
{
	struct av_mjpeg_pipeline *pipeline = argument;
	unsigned int consecutive_timeouts = 0;

	while (!av_pipeline_should_stop(pipeline)) {
		struct av_video_frame frame;
		struct av_raw_slot *slot;
		uint64_t begin;
		uint64_t end;
		uint64_t capture_time_us;
		int result;

		result = av_video_dequeue(&pipeline->video, &frame,
					  AV_CAPTURE_TIMEOUT_MS);
		if (result == AV_VIDEO_TIMEOUT) {
			pthread_mutex_lock(&pipeline->lock);
			pipeline->stats.capture_timeouts++;
			pthread_mutex_unlock(&pipeline->lock);
			if (++consecutive_timeouts >= AV_MAX_CAPTURE_TIMEOUTS) {
				fprintf(stderr, "No camera frame for %u ms\n",
					AV_CAPTURE_TIMEOUT_MS *
					AV_MAX_CAPTURE_TIMEOUTS);
				av_pipeline_fail(pipeline, ETIMEDOUT);
				break;
			}
			continue;
		}
		if (result < 0) {
			av_pipeline_fail(pipeline, EIO);
			break;
		}
		consecutive_timeouts = 0;
		capture_time_us = av_now_us();

		if (av_pipeline_should_stop(pipeline)) {
			(void)av_video_queue(&pipeline->video, &frame);
			break;
		}

		slot = av_reserve_raw_slot(pipeline);
		if (!slot) {
			fprintf(stderr, "No writable raw-frame slot\n");
			(void)av_video_queue(&pipeline->video, &frame);
			av_pipeline_fail(pipeline, EBUSY);
			break;
		}

		begin = av_now_us();
		result = av_copy_yuyv_frame(pipeline, slot, &frame);
		if (result < 0) {
			pthread_mutex_lock(&pipeline->lock);
			slot->state = AV_RAW_FREE;
			pthread_mutex_unlock(&pipeline->lock);
			(void)av_video_queue(&pipeline->video, &frame);
			av_pipeline_fail(pipeline, EMSGSIZE);
			break;
		}

		/* The contiguous copy is the only MMAP read; return DMA ownership now. */
		if (av_video_queue(&pipeline->video, &frame) < 0) {
			pthread_mutex_lock(&pipeline->lock);
			slot->state = AV_RAW_FREE;
			pthread_mutex_unlock(&pipeline->lock);
			av_pipeline_fail(pipeline, EIO);
			break;
		}

		end = av_now_us();
		av_publish_raw_slot(pipeline, slot, &frame, capture_time_us,
				    end >= begin ? end - begin : 0);
	}

	av_finish_capture(pipeline);
	return NULL;
}

/* Acquire newest READY raw frame and discard any older READY history. */
static int av_acquire_latest_raw(struct av_mjpeg_pipeline *pipeline,
				 struct av_raw_slot **slot_out)
{
	struct av_raw_slot *newest;
	unsigned int index;

	pthread_mutex_lock(&pipeline->lock);
	for (;;) {
		newest = NULL;
		for (index = 0; index < AV_RAW_SLOT_COUNT; ++index) {
			struct av_raw_slot *slot = &pipeline->raw_slots[index];

			if (slot->state == AV_RAW_READY &&
			    slot->snapshot_readers == 0U &&
			    (!newest || slot->serial > newest->serial))
				newest = slot;
		}
		if (newest)
			break;
		if (pipeline->stop_requested || pipeline->capture_finished) {
			int failed = pipeline->failed;

			pthread_mutex_unlock(&pipeline->lock);
			return failed ? -EIO : AV_MJPEG_STOPPED;
		}
		pthread_cond_wait(&pipeline->raw_ready, &pipeline->lock);
	}

	for (index = 0; index < AV_RAW_SLOT_COUNT; ++index) {
		struct av_raw_slot *slot = &pipeline->raw_slots[index];

		if (slot != newest && slot->state == AV_RAW_READY &&
		    slot->snapshot_readers == 0U) {
			slot->state = AV_RAW_FREE;
			pipeline->stats.raw_frames_dropped++;
		}
	}
	newest->state = AV_RAW_READING;
	*slot_out = newest;
	pthread_mutex_unlock(&pipeline->lock);
	return 0;
}

static void av_release_raw(struct av_mjpeg_pipeline *pipeline,
			   struct av_raw_slot *slot)
{
	pthread_mutex_lock(&pipeline->lock);
	while (slot->snapshot_readers != 0U)
		pthread_cond_wait(&pipeline->raw_ready, &pipeline->lock);
	slot->state = AV_RAW_FREE;
	pthread_mutex_unlock(&pipeline->lock);
}

static struct av_jpeg_slot *av_reserve_jpeg_slot(
					struct av_mjpeg_pipeline *pipeline)
{
	struct av_jpeg_slot *oldest_ready = NULL;
	unsigned int index;

	pthread_mutex_lock(&pipeline->lock);
	for (index = 0; index < AV_JPEG_SLOT_COUNT; ++index) {
		struct av_jpeg_slot *slot = &pipeline->jpeg_slots[index];

		if (slot->state == AV_JPEG_FREE) {
			slot->state = AV_JPEG_WRITING;
			pthread_mutex_unlock(&pipeline->lock);
			return slot;
		}
		if (slot->state == AV_JPEG_READY &&
		    (!oldest_ready || slot->serial < oldest_ready->serial))
			oldest_ready = slot;
	}
	if (oldest_ready) {
		oldest_ready->state = AV_JPEG_WRITING;
		pipeline->stats.jpeg_frames_replaced++;
	}
	pthread_mutex_unlock(&pipeline->lock);
	return oldest_ready;
}

static void av_publish_jpeg(struct av_mjpeg_pipeline *pipeline,
			    struct av_jpeg_slot *slot,
			    uint32_t sequence, uint64_t timestamp_us,
			    uint64_t capture_time_us,
			    uint64_t unpack_us, uint64_t encode_us,
			    uint64_t publish_us)
{
	uint64_t now = av_now_us();

	pthread_mutex_lock(&pipeline->lock);
	slot->size = av_jpeg_encoder_size(&pipeline->encoder);
	slot->sequence = sequence;
	slot->timestamp_us = timestamp_us;
	slot->capture_time_us = capture_time_us;
	slot->serial = ++pipeline->next_jpeg_serial;
	slot->state = AV_JPEG_READY;

	if (pipeline->stats.encoded_frames == 0)
		pipeline->stats.first_encode_time_us = now;
	pipeline->stats.last_encode_time_us = now;
	pipeline->stats.encoded_frames++;
	pipeline->stats.jpeg_bytes += slot->size;
	pipeline->stats.unpack_us += unpack_us;
	pipeline->stats.encode_us += encode_us;
	pipeline->stats.jpeg_publish_us += publish_us;
	pthread_cond_broadcast(&pipeline->jpeg_ready);
	pthread_mutex_unlock(&pipeline->lock);
}

static void av_finish_encode(struct av_mjpeg_pipeline *pipeline)
{
	pthread_mutex_lock(&pipeline->lock);
	pipeline->encode_finished = 1;
	pthread_cond_broadcast(&pipeline->jpeg_ready);
	pthread_mutex_unlock(&pipeline->lock);
}

/* The TurboJPEG object is owned exclusively by this thread. */
static void *av_encode_thread_main(void *argument)
{
	struct av_mjpeg_pipeline *pipeline = argument;

	for (;;) {
		struct av_raw_slot *raw;
		struct av_jpeg_slot *jpeg;
		uint64_t begin;
		uint64_t unpack_done;
		uint64_t encode_done;
		uint64_t publish_done;
		uint32_t sequence;
		uint64_t timestamp_us;
		uint64_t capture_time_us;
		int result;

		result = av_acquire_latest_raw(pipeline, &raw);
		if (result == AV_MJPEG_STOPPED)
			break;
		if (result < 0)
			break;

		sequence = raw->sequence;
		timestamp_us = raw->timestamp_us;
		capture_time_us = raw->capture_time_us;
		begin = av_now_us();
		result = av_jpeg_encoder_unpack_yuyv_rotate_180(&pipeline->encoder,
				raw->data, raw->bytesused,
				pipeline->video.bytesperline);
		unpack_done = av_now_us();
		/* Unpack owns private planes now; release YUYV before compression. */
		av_release_raw(pipeline, raw);
		if (result < 0) {
			fprintf(stderr, "YUYV unpack failed: %s\n",
				av_jpeg_encoder_error(&pipeline->encoder));
			av_pipeline_fail(pipeline, EIO);
			break;
		}

		result = av_jpeg_encoder_compress(&pipeline->encoder);
		encode_done = av_now_us();
		if (result < 0) {
			fprintf(stderr, "JPEG encode failed: %s\n",
				av_jpeg_encoder_error(&pipeline->encoder));
			av_pipeline_fail(pipeline, EIO);
			break;
		}

		jpeg = av_reserve_jpeg_slot(pipeline);
		if (!jpeg) {
			fprintf(stderr, "No writable JPEG slot\n");
			av_pipeline_fail(pipeline, EBUSY);
			break;
		}
		memcpy(jpeg->data, av_jpeg_encoder_data(&pipeline->encoder),
		       av_jpeg_encoder_size(&pipeline->encoder));
		publish_done = av_now_us();

		/* Publish from local metadata; the raw slot may already be reused. */
		av_publish_jpeg(pipeline, jpeg, sequence, timestamp_us,
				capture_time_us,
				unpack_done >= begin ? unpack_done - begin : 0,
				encode_done >= unpack_done ?
				encode_done - unpack_done : 0,
				publish_done >= encode_done ?
				publish_done - encode_done : 0);
		av_wait_encode_budget(pipeline, begin);
	}

	av_finish_encode(pipeline);
	return NULL;
}

static int av_allocate_slots(struct av_mjpeg_pipeline *pipeline)
{
	unsigned int index;

	for (index = 0; index < AV_RAW_SLOT_COUNT; ++index) {
		pipeline->raw_slots[index].data =
			malloc(pipeline->video.sizeimage);
		if (!pipeline->raw_slots[index].data)
			return -ENOMEM;
	}
	for (index = 0; index < AV_JPEG_SLOT_COUNT; ++index) {
		pipeline->jpeg_slots[index].data =
			malloc(pipeline->encoder.jpeg_capacity);
		if (!pipeline->jpeg_slots[index].data)
			return -ENOMEM;
	}
	return 0;
}

int av_mjpeg_pipeline_create(struct av_mjpeg_pipeline **pipeline_out,
			     const struct av_mjpeg_config *config)
{
	struct av_mjpeg_pipeline *pipeline;
	struct av_video_config video_config;
	int error;

	if (!pipeline_out || !config || !config->video_device ||
	    config->encode_fps > 1000U)
		return -EINVAL;
	*pipeline_out = NULL;
	pipeline = calloc(1, sizeof(*pipeline));
	if (!pipeline)
		return -ENOMEM;
	pipeline->video.fd = -1;

	error = pthread_mutex_init(&pipeline->lock, NULL);
	if (error) {
		free(pipeline);
		return -error;
	}
	pipeline->lock_initialized = 1;
	error = pthread_cond_init(&pipeline->raw_ready, NULL);
	if (error) {
		av_mjpeg_pipeline_destroy(pipeline);
		return -error;
	}
	pipeline->raw_cond_initialized = 1;
	error = pthread_cond_init(&pipeline->jpeg_ready, NULL);
	if (error) {
		av_mjpeg_pipeline_destroy(pipeline);
		return -error;
	}
	pipeline->jpeg_cond_initialized = 1;

	memset(&video_config, 0, sizeof(video_config));
	video_config.width = config->width;
	video_config.height = config->height;
	video_config.fps = config->fps;
	video_config.capture_mode = config->capture_mode;
	video_config.buffer_count = config->video_buffer_count;
	video_config.pixel_format = config->pixel_format;
	if (av_video_open(&pipeline->video, config->video_device,
			  &video_config) < 0) {
		av_mjpeg_pipeline_destroy(pipeline);
		return -EIO;
	}
	if (av_jpeg_encoder_init(&pipeline->encoder, pipeline->video.width,
				 pipeline->video.height,
				 config->jpeg_quality) < 0) {
		fprintf(stderr, "JPEG init failed: %s\n",
			av_jpeg_encoder_error(&pipeline->encoder));
		av_mjpeg_pipeline_destroy(pipeline);
		return -EIO;
	}
	if (av_allocate_slots(pipeline) < 0) {
		fprintf(stderr, "Cannot allocate STREAM-R4 frame slots\n");
		av_mjpeg_pipeline_destroy(pipeline);
		return -ENOMEM;
	}

	pipeline->info.width = pipeline->video.width;
	pipeline->info.height = pipeline->video.height;
	pipeline->info.bytesperline = pipeline->video.bytesperline;
	pipeline->info.sizeimage = pipeline->video.sizeimage;
	pipeline->info.fps = config->fps;
	pipeline->info.video_buffer_count = pipeline->video.buffer_count;
	pipeline->info.jpeg_quality = config->jpeg_quality;
	pipeline->info.encode_fps = config->encode_fps;
	pipeline->info.jpeg_capacity = pipeline->encoder.jpeg_capacity;
	if (config->encode_fps != 0U)
		pipeline->encode_interval_us =
			UINT64_C(1000000) / config->encode_fps;
	*pipeline_out = pipeline;
	return 0;
}

int av_mjpeg_pipeline_start(struct av_mjpeg_pipeline *pipeline)
{
	int error;

	if (!pipeline || pipeline->started)
		return -EINVAL;
	if (av_video_start(&pipeline->video) < 0)
		return -EIO;
	pipeline->started = 1;

	error = pthread_create(&pipeline->capture_thread, NULL,
			       av_capture_thread_main, pipeline);
	if (error) {
		av_mjpeg_pipeline_stop(pipeline);
		return -error;
	}
	pipeline->capture_thread_started = 1;
	error = pthread_create(&pipeline->encode_thread, NULL,
			       av_encode_thread_main, pipeline);
	if (error) {
		av_mjpeg_pipeline_stop(pipeline);
		return -error;
	}
	pipeline->encode_thread_started = 1;
	return 0;
}

int av_mjpeg_pipeline_copy_latest_timeout(
				  struct av_mjpeg_pipeline *pipeline,
				  uint64_t after_serial,
				  unsigned char *destination,
				  size_t destination_capacity,
				  struct av_mjpeg_frame *frame,
				  unsigned int timeout_ms)
{
	struct av_jpeg_slot *newest;
	struct timespec deadline;
	unsigned int index;
	int wait_error;

	if (!pipeline || !destination || !frame)
		return -EINVAL;
	if (timeout_ms != 0U) {
		if (clock_gettime(CLOCK_REALTIME, &deadline) < 0)
			return -errno;
		deadline.tv_sec += timeout_ms / 1000U;
		deadline.tv_nsec +=
			(long)(timeout_ms % 1000U) * 1000000L;
		if (deadline.tv_nsec >= 1000000000L) {
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000000000L;
		}
	}
	pthread_mutex_lock(&pipeline->lock);
	for (;;) {
		newest = NULL;
		for (index = 0; index < AV_JPEG_SLOT_COUNT; ++index) {
			struct av_jpeg_slot *slot = &pipeline->jpeg_slots[index];

			if (slot->state == AV_JPEG_READY &&
			    slot->serial > after_serial &&
			    (!newest || slot->serial > newest->serial))
				newest = slot;
		}
		if (newest)
			break;
		if (pipeline->stop_requested || pipeline->encode_finished) {
			int failed = pipeline->failed;

			pthread_mutex_unlock(&pipeline->lock);
			return failed ? -EIO : AV_MJPEG_STOPPED;
		}
		if (timeout_ms == 0U) {
			wait_error = pthread_cond_wait(&pipeline->jpeg_ready,
						       &pipeline->lock);
		} else {
			wait_error = pthread_cond_timedwait(&pipeline->jpeg_ready,
							&pipeline->lock,
							&deadline);
		}
		if (wait_error == ETIMEDOUT) {
			pthread_mutex_unlock(&pipeline->lock);
			return AV_MJPEG_TIMEOUT;
		}
		if (wait_error != 0) {
			pthread_mutex_unlock(&pipeline->lock);
			return -wait_error;
		}
	}

	if (newest->size > destination_capacity) {
		pthread_mutex_unlock(&pipeline->lock);
		return -ENOSPC;
	}
	memcpy(destination, newest->data, newest->size);
	frame->size = newest->size;
	frame->sequence = newest->sequence;
	frame->timestamp_us = newest->timestamp_us;
	frame->capture_time_us = newest->capture_time_us;
	frame->serial = newest->serial;

	/* The network now owns a private copy; all older JPEG history is stale. */
	for (index = 0; index < AV_JPEG_SLOT_COUNT; ++index) {
		struct av_jpeg_slot *slot = &pipeline->jpeg_slots[index];

		if (slot->state == AV_JPEG_READY &&
		    slot->serial <= newest->serial)
			slot->state = AV_JPEG_FREE;
	}
	pthread_mutex_unlock(&pipeline->lock);
	return 0;
}

int av_mjpeg_pipeline_copy_latest(struct av_mjpeg_pipeline *pipeline,
				  uint64_t after_serial,
				  unsigned char *destination,
				  size_t destination_capacity,
				  struct av_mjpeg_frame *frame)
{
	return av_mjpeg_pipeline_copy_latest_timeout(pipeline, after_serial,
			destination, destination_capacity, frame, 0U);
}

int av_mjpeg_pipeline_copy_latest_raw(
				  struct av_mjpeg_pipeline *pipeline,
				  uint64_t after_serial,
				  unsigned char *destination,
				  size_t destination_capacity,
				  struct av_mjpeg_raw_frame *frame)
{
	struct av_raw_slot *newest;
	unsigned int index;
	size_t bytesused;
	uint64_t copy_begin;
	uint64_t copy_end;

	if (!pipeline || !destination || !frame)
		return -EINVAL;
	pthread_mutex_lock(&pipeline->lock);
	for (;;) {
		newest = NULL;
		for (index = 0; index < AV_RAW_SLOT_COUNT; ++index) {
			struct av_raw_slot *slot = &pipeline->raw_slots[index];

			/* READY and encoder-owned READING storage are both immutable. */
			if ((slot->state == AV_RAW_READY ||
			     slot->state == AV_RAW_READING) &&
			    slot->serial > after_serial &&
			    (!newest || slot->serial > newest->serial))
				newest = slot;
		}
		if (newest)
			break;
		if (pipeline->stop_requested || pipeline->capture_finished) {
			int failed = pipeline->failed;

			pthread_mutex_unlock(&pipeline->lock);
			return failed ? -EIO : AV_MJPEG_STOPPED;
		}
		pthread_cond_wait(&pipeline->raw_ready, &pipeline->lock);
	}

	if (newest->bytesused > destination_capacity) {
		pthread_mutex_unlock(&pipeline->lock);
		return -ENOSPC;
	}
	newest->snapshot_readers++;
	bytesused = newest->bytesused;
	frame->size = bytesused;
	frame->sequence = newest->sequence;
	frame->timestamp_us = newest->timestamp_us;
	frame->capture_time_us = newest->capture_time_us;
	frame->serial = newest->serial;
	/*
	 * snapshot_readers pins the slot, so the 600 KiB copy need not serialize
	 * capture publication, slot reservation or unrelated statistics reads.
	 */
	pthread_mutex_unlock(&pipeline->lock);
	copy_begin = av_now_us();
	av_copy_rotate_180_cacheable(destination, newest->data,
		pipeline->video.width, pipeline->video.height,
		pipeline->video.bytesperline);
	copy_end = av_now_us();
	frame->copy_us = copy_end >= copy_begin ? copy_end - copy_begin : 0;
	pthread_mutex_lock(&pipeline->lock);
	newest->snapshot_readers--;
	pthread_cond_broadcast(&pipeline->raw_ready);
	pthread_mutex_unlock(&pipeline->lock);
	return 0;
}

void av_mjpeg_pipeline_get_info(struct av_mjpeg_pipeline *pipeline,
				struct av_mjpeg_info *info)
{
	if (!pipeline || !info)
		return;
	pthread_mutex_lock(&pipeline->lock);
	*info = pipeline->info;
	pthread_mutex_unlock(&pipeline->lock);
}

void av_mjpeg_pipeline_get_stats(struct av_mjpeg_pipeline *pipeline,
				 struct av_mjpeg_pipeline_stats *stats)
{
	if (!pipeline || !stats)
		return;
	pthread_mutex_lock(&pipeline->lock);
	*stats = pipeline->stats;
	stats->failed = pipeline->failed;
	stats->error_number = pipeline->error_number;
	pthread_mutex_unlock(&pipeline->lock);
}

int av_mjpeg_pipeline_failed(struct av_mjpeg_pipeline *pipeline)
{
	int failed;

	if (!pipeline)
		return 1;
	pthread_mutex_lock(&pipeline->lock);
	failed = pipeline->failed;
	pthread_mutex_unlock(&pipeline->lock);
	return failed;
}

void av_mjpeg_pipeline_stop(struct av_mjpeg_pipeline *pipeline)
{
	if (!pipeline || !pipeline->lock_initialized)
		return;
	pthread_mutex_lock(&pipeline->lock);
	pipeline->stop_requested = 1;
	if (pipeline->raw_cond_initialized)
		pthread_cond_broadcast(&pipeline->raw_ready);
	if (pipeline->jpeg_cond_initialized)
		pthread_cond_broadcast(&pipeline->jpeg_ready);
	pthread_mutex_unlock(&pipeline->lock);

	if (pipeline->capture_thread_started) {
		(void)pthread_join(pipeline->capture_thread, NULL);
		pipeline->capture_thread_started = 0;
	}
	if (pipeline->encode_thread_started) {
		(void)pthread_join(pipeline->encode_thread, NULL);
		pipeline->encode_thread_started = 0;
	}
	if (pipeline->started) {
		(void)av_video_stop(&pipeline->video);
		pipeline->started = 0;
	}
}

void av_mjpeg_pipeline_destroy(struct av_mjpeg_pipeline *pipeline)
{
	unsigned int index;

	if (!pipeline)
		return;
	av_mjpeg_pipeline_stop(pipeline);
	for (index = 0; index < AV_RAW_SLOT_COUNT; ++index)
		free(pipeline->raw_slots[index].data);
	for (index = 0; index < AV_JPEG_SLOT_COUNT; ++index)
		free(pipeline->jpeg_slots[index].data);
	av_jpeg_encoder_destroy(&pipeline->encoder);
	av_video_close(&pipeline->video);
	if (pipeline->jpeg_cond_initialized)
		(void)pthread_cond_destroy(&pipeline->jpeg_ready);
	if (pipeline->raw_cond_initialized)
		(void)pthread_cond_destroy(&pipeline->raw_ready);
	if (pipeline->lock_initialized)
		(void)pthread_mutex_destroy(&pipeline->lock);
	free(pipeline);
}
