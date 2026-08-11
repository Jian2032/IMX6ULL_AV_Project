/* VIDEO-R4 diagnostic: record consecutive YUYV frames without LCD output. */

#include "av_video_capture.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_VIDEO_DEVICE "/dev/video0"
#define DEFAULT_FRAME_COUNT  60U
#define DEFAULT_OUTPUT_FILE  "record_640x480_yuyv_60f.raw"
#define VIDEO_TIMEOUT_MS     2000U
#define MAX_FRAME_COUNT      120U
#define MAX_RECORD_BYTES     (80U * 1024U * 1024U)

static int parse_count(const char *text, unsigned int *count)
{
	char *end = NULL;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno || end == text || *end || value == 0UL ||
	    value > MAX_FRAME_COUNT || value > UINT_MAX) {
		fprintf(stderr, "Invalid frame count '%s' (expected 1..%u)\n",
			text, MAX_FRAME_COUNT);
		return -1;
	}
	*count = (unsigned int)value;
	return 0;
}

static int write_all(int fd, const unsigned char *data, size_t bytes)
{
	while (bytes != 0U) {
		ssize_t written = write(fd, data, bytes);

		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0) {
			fprintf(stderr, "Output write failed: %s\n",
				written < 0 ? strerror(errno) : "short zero-byte write");
			return -1;
		}
		data += written;
		bytes -= (size_t)written;
	}
	return 0;
}

static void usage(const char *program)
{
	printf("Usage: %s [video-device] [frame-count] [output-file]\n",
	       program);
	printf("Defaults: %s %u %s\n", DEFAULT_VIDEO_DEVICE,
	       DEFAULT_FRAME_COUNT, DEFAULT_OUTPUT_FILE);
}

int main(int argc, char *argv[])
{
	const char *video_device = DEFAULT_VIDEO_DEVICE;
	const char *output_file = DEFAULT_OUTPUT_FILE;
	unsigned int target_frames = DEFAULT_FRAME_COUNT;
	const struct av_video_config config = {
		.width = 640,
		.height = 480,
		.fps = 30,
		.capture_mode = 0,
		.buffer_count = 4,
		.pixel_format = V4L2_PIX_FMT_YUYV,
	};
	struct av_video video;
	unsigned char *recording = NULL;
	size_t frame_bytes;
	size_t recording_bytes;
	unsigned int captured = 0;
	unsigned int sequence_gaps = 0;
	uint32_t first_sequence = 0;
	uint32_t last_sequence = 0;
	uint64_t first_timestamp = 0;
	uint64_t last_timestamp = 0;
	int output_fd = -1;
	int video_opened = 0;
	int ret = EXIT_FAILURE;

	memset(&video, 0, sizeof(video));
	video.fd = -1;
	if (argc > 4) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc >= 2) {
		if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
			usage(argv[0]);
			return EXIT_SUCCESS;
		}
		video_device = argv[1];
	}
	if (argc >= 3 && parse_count(argv[2], &target_frames) == -1)
		return EXIT_FAILURE;
	if (argc >= 4)
		output_file = argv[3];

	printf("VIDEO-R4 continuous raw YUYV recorder\n");
	printf("operation      : capture to RAM, then write after STREAMOFF\n");
	if (av_video_open(&video, video_device, &config) == -1)
		goto out;
	video_opened = 1;
	frame_bytes = video.sizeimage;
	if (frame_bytes == 0U ||
	    frame_bytes > MAX_RECORD_BYTES / target_frames) {
		fprintf(stderr, "Requested recording is too large\n");
		goto out;
	}
	recording_bytes = frame_bytes * target_frames;
	recording = malloc(recording_bytes);
	if (!recording) {
		fprintf(stderr, "Cannot allocate %lu-byte recording buffer\n",
			(unsigned long)recording_bytes);
		goto out;
	}
	printf("record target  : %u frames, %lu bytes -> %s\n",
	       target_frames, (unsigned long)recording_bytes, output_file);

	if (av_video_start(&video) == -1)
		goto out;
	while (captured < target_frames) {
		struct av_video_frame frame;
		int dequeue_ret;

		dequeue_ret = av_video_dequeue(&video, &frame, VIDEO_TIMEOUT_MS);
		if (dequeue_ret == AV_VIDEO_TIMEOUT) {
			fprintf(stderr, "Video frame timeout\n");
			goto out;
		}
		if (dequeue_ret == -1)
			goto out;
		if (frame.bytesused < frame_bytes) {
			fprintf(stderr,
				"Frame %u is short: bytesused=%lu expected=%lu\n",
				captured, (unsigned long)frame.bytesused,
				(unsigned long)frame_bytes);
			(void)av_video_queue(&video, &frame);
			goto out;
		}

		memcpy(recording + (size_t)captured * frame_bytes,
		       frame.data, frame_bytes);
		if (captured == 0U) {
			first_sequence = frame.sequence;
			first_timestamp = frame.timestamp_us;
		} else {
			uint32_t delta = frame.sequence - last_sequence;

			if (delta > 1U && delta < 0x80000000U)
				sequence_gaps += delta - 1U;
		}
		last_sequence = frame.sequence;
		last_timestamp = frame.timestamp_us;
		if (av_video_queue(&video, &frame) == -1)
			goto out;
		captured++;
		if (captured == 1U || captured % 30U == 0U ||
		    captured == target_frames)
			printf("  captured %u/%u: sequence=%u\n",
			       captured, target_frames, last_sequence);
	}
	if (av_video_stop(&video) == -1)
		goto out;
	av_video_close(&video);
	video_opened = 0;

	printf("capture stats  : first=%u last=%u gaps=%u\n",
	       first_sequence, last_sequence, sequence_gaps);
	if (captured > 1U && last_timestamp > first_timestamp) {
		double elapsed =
			(double)(last_timestamp - first_timestamp) / 1000000.0;
		printf("  measured fps : %.2f\n", (captured - 1U) / elapsed);
	}
	if (sequence_gaps != 0U) {
		fprintf(stderr, "Recording lost %u input frame(s)\n",
			sequence_gaps);
		goto out;
	}

	output_fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (output_fd == -1) {
		fprintf(stderr, "Cannot open %s: %s\n",
			output_file, strerror(errno));
		goto out;
	}
	if (write_all(output_fd, recording, recording_bytes) == -1)
		goto out;
	if (close(output_fd) == -1) {
		output_fd = -1;
		fprintf(stderr, "Cannot close %s: %s\n",
			output_file, strerror(errno));
		goto out;
	}
	output_fd = -1;
	printf("[PASS] Saved %u consecutive YUYV frames (%lu bytes).\n",
	       captured, (unsigned long)recording_bytes);
	ret = EXIT_SUCCESS;

out:
	if (output_fd >= 0)
		(void)close(output_fd);
	if (video_opened)
		av_video_close(&video);
	free(recording);
	return ret;
}
