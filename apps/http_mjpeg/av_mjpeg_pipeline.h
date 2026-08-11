/* SPDX-License-Identifier: MIT */
/*
 * av_mjpeg_pipeline.h - STREAM-R4 threaded V4L2 to JPEG producer pipeline
 *
 * The HTTP layer deliberately sees only complete JPEG snapshots.  CSI MMAP
 * ownership, raw-frame slots and the TurboJPEG working object remain private
 * to the pipeline implementation.
 */

#ifndef AV_MJPEG_PIPELINE_H
#define AV_MJPEG_PIPELINE_H

#include <stddef.h>
#include <stdint.h>

#define AV_MJPEG_STOPPED 1
#define AV_MJPEG_TIMEOUT 2

struct av_mjpeg_pipeline;

struct av_mjpeg_config {
	const char *video_device;
	unsigned int width;
	unsigned int height;
	unsigned int fps;
	unsigned int capture_mode;
	unsigned int video_buffer_count;
	uint32_t pixel_format;
	int jpeg_quality;
	/* 0 encodes as fast as possible; nonzero bounds the JPEG consumer. */
	unsigned int encode_fps;
};

struct av_mjpeg_info {
	unsigned int width;
	unsigned int height;
	unsigned int bytesperline;
	unsigned int sizeimage;
	unsigned int fps;
	unsigned int video_buffer_count;
	int jpeg_quality;
	unsigned int encode_fps;
	size_t jpeg_capacity;
};

/* Metadata copied together with one complete JPEG image. */
struct av_mjpeg_frame {
	size_t size;
	uint32_t sequence;
	/* Original V4L2 timestamp, retained as source metadata. */
	uint64_t timestamp_us;
	/* CLOCK_MONOTONIC sampled by this process immediately after DQBUF. */
	uint64_t capture_time_us;
	uint64_t serial;
};

/*
 * A private copy of the newest cacheable YUYV frame.  This is not a V4L2
 * MMAP pointer: copy_latest_raw() finishes the copy before returning, so a
 * display consumer may keep using its destination after CSI continues.
 */
struct av_mjpeg_raw_frame {
	size_t size;
	uint32_t sequence;
	uint64_t timestamp_us;
	uint64_t capture_time_us;
	uint64_t serial;
	uint64_t copy_us;
};

struct av_mjpeg_pipeline_stats {
	uint64_t captured_frames;
	uint64_t encoded_frames;
	uint64_t driver_sequence_gaps;
	uint64_t capture_timeouts;
	uint64_t raw_frames_dropped;
	uint64_t jpeg_frames_replaced;
	uint64_t jpeg_bytes;
	uint64_t copy_us;
	uint64_t unpack_us;
	uint64_t encode_us;
	uint64_t jpeg_publish_us;
	uint32_t first_sequence;
	uint32_t last_sequence;
	uint64_t first_capture_timestamp_us;
	uint64_t last_capture_timestamp_us;
	/* Process-owned monotonic time base used for cross-module diagnostics. */
	uint64_t first_capture_time_us;
	uint64_t last_capture_time_us;
	uint64_t first_encode_time_us;
	uint64_t last_encode_time_us;
	int failed;
	int error_number;
};

int av_mjpeg_pipeline_create(struct av_mjpeg_pipeline **pipeline_out,
			     const struct av_mjpeg_config *config);
int av_mjpeg_pipeline_start(struct av_mjpeg_pipeline *pipeline);

/*
 * Wait for and copy the newest JPEG whose serial is greater than after_serial.
 * The caller owns destination, so it may perform a blocking socket send after
 * this function returns without blocking capture or encoding.
 */
int av_mjpeg_pipeline_copy_latest(struct av_mjpeg_pipeline *pipeline,
				  uint64_t after_serial,
				  unsigned char *destination,
				  size_t destination_capacity,
				  struct av_mjpeg_frame *frame);

/*
 * Timed variant used by independently stoppable consumers such as AV-R3's
 * HTTP worker.  timeout_ms=0 retains the original indefinite wait.
 */
int av_mjpeg_pipeline_copy_latest_timeout(
				  struct av_mjpeg_pipeline *pipeline,
				  uint64_t after_serial,
				  unsigned char *destination,
				  size_t destination_capacity,
				  struct av_mjpeg_frame *frame,
				  unsigned int timeout_ms);

/*
 * Wait for and copy the newest oriented YUYV frame newer than after_serial.
 * The copy observes READY or encoder-owned READING storage while holding the
 * pipeline lock; both states are immutable.  It never consumes or frees the
 * encoder's raw slot.
 */
int av_mjpeg_pipeline_copy_latest_raw(
				  struct av_mjpeg_pipeline *pipeline,
				  uint64_t after_serial,
				  unsigned char *destination,
				  size_t destination_capacity,
				  struct av_mjpeg_raw_frame *frame);

void av_mjpeg_pipeline_get_info(struct av_mjpeg_pipeline *pipeline,
				struct av_mjpeg_info *info);
void av_mjpeg_pipeline_get_stats(struct av_mjpeg_pipeline *pipeline,
				 struct av_mjpeg_pipeline_stats *stats);
int av_mjpeg_pipeline_failed(struct av_mjpeg_pipeline *pipeline);
void av_mjpeg_pipeline_stop(struct av_mjpeg_pipeline *pipeline);
void av_mjpeg_pipeline_destroy(struct av_mjpeg_pipeline *pipeline);

#endif /* AV_MJPEG_PIPELINE_H */
