#ifndef AV_VIDEO_CAPTURE_H
#define AV_VIDEO_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

/* Return value used by av_video_dequeue() when select() reaches its timeout. */
#define AV_VIDEO_TIMEOUT 1

struct av_video_config {
	unsigned int width;
	unsigned int height;
	unsigned int fps;
	unsigned int capture_mode;
	unsigned int buffer_count;
	uint32_t pixel_format;
};

struct av_video_buffer {
	void *start;
	size_t length;
};

struct av_video {
	int fd;
	struct av_video_buffer *buffers;
	unsigned int buffer_count;
	int buffers_requested;
	int streaming;

	unsigned int width;
	unsigned int height;
	unsigned int bytesperline;
	unsigned int sizeimage;
	uint32_t pixel_format;
};

struct av_video_frame {
	void *data;
	size_t bytesused;
	unsigned int index;
	uint32_t sequence;
	uint32_t flags;
	uint64_t timestamp_us;
};

int av_video_open(struct av_video *video, const char *device,
		  const struct av_video_config *config);
int av_video_start(struct av_video *video);
int av_video_dequeue(struct av_video *video, struct av_video_frame *frame,
		     unsigned int timeout_ms);
int av_video_queue(struct av_video *video, const struct av_video_frame *frame);
int av_video_stop(struct av_video *video);
void av_video_close(struct av_video *video);

#endif
