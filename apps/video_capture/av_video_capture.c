/* Shared V4L2 MMAP capture layer introduced by VIDEO-R3. */

#include "av_video_capture.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int xioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, request, arg);
	} while (ret == -1 && errno == EINTR);
	return ret;
}

static const char *fourcc_string(uint32_t fourcc, char text[5])
{
	unsigned int i;

	for (i = 0; i < 4; ++i) {
		unsigned char ch = (unsigned char)((fourcc >> (i * 8U)) & 0xffU);
		text[i] = isprint(ch) ? (char)ch : '.';
	}
	text[4] = '\0';
	return text;
}

static int check_capability(int fd)
{
	struct v4l2_capability cap;
	uint32_t caps;

	memset(&cap, 0, sizeof(cap));
	if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == -1) {
		fprintf(stderr, "VIDIOC_QUERYCAP failed: %s\n", strerror(errno));
		return -1;
	}
	caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ?
	       cap.device_caps : cap.capabilities;

	printf("video device   : driver=%.*s card=%.*s caps=0x%08x\n",
	       (int)sizeof(cap.driver), (const char *)cap.driver,
	       (int)sizeof(cap.card), (const char *)cap.card, caps);
	if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
		fprintf(stderr, "Video node lacks CAPTURE or STREAMING capability\n");
		return -1;
	}
	return 0;
}

static int configure_video(struct av_video *video,
			   const struct av_video_config *config)
{
	struct v4l2_streamparm parm;
	struct v4l2_format format;
	unsigned int input = 0;
	char fourcc[5];

	if (xioctl(video->fd, VIDIOC_S_INPUT, &input) == -1) {
		fprintf(stderr, "VIDIOC_S_INPUT failed: %s\n", strerror(errno));
		return -1;
	}

	/* NXP OV5640 uses capturemode in S_PARM to select its register table. */
	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	parm.parm.capture.capturemode = config->capture_mode;
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = config->fps;
	if (xioctl(video->fd, VIDIOC_S_PARM, &parm) == -1) {
		fprintf(stderr, "VIDIOC_S_PARM failed: %s\n", strerror(errno));
		return -1;
	}
	if (!parm.parm.capture.timeperframe.numerator ||
	    parm.parm.capture.timeperframe.denominator !=
	    config->fps * parm.parm.capture.timeperframe.numerator) {
		fprintf(stderr, "Driver did not accept %u fps\n", config->fps);
		return -1;
	}

	memset(&format, 0, sizeof(format));
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	format.fmt.pix.width = config->width;
	format.fmt.pix.height = config->height;
	format.fmt.pix.pixelformat = config->pixel_format;
	format.fmt.pix.field = V4L2_FIELD_NONE;
	if (xioctl(video->fd, VIDIOC_S_FMT, &format) == -1) {
		fprintf(stderr, "VIDIOC_S_FMT failed: %s\n", strerror(errno));
		return -1;
	}
	if (format.fmt.pix.width != config->width ||
	    format.fmt.pix.height != config->height ||
	    format.fmt.pix.pixelformat != config->pixel_format) {
		fprintf(stderr, "Driver changed the requested video format\n");
		return -1;
	}
	if (format.fmt.pix.bytesperline < config->width * 2U ||
	    format.fmt.pix.sizeimage <
	    format.fmt.pix.bytesperline * format.fmt.pix.height) {
		fprintf(stderr, "Driver returned invalid YUYV buffer geometry\n");
		return -1;
	}

	video->width = format.fmt.pix.width;
	video->height = format.fmt.pix.height;
	video->bytesperline = format.fmt.pix.bytesperline;
	video->sizeimage = format.fmt.pix.sizeimage;
	video->pixel_format = format.fmt.pix.pixelformat;
	printf("video format   : %ux%u '%s' %u fps, line=%u size=%u\n",
	       video->width, video->height,
	       fourcc_string(video->pixel_format, fourcc), config->fps,
	       video->bytesperline, video->sizeimage);
	return 0;
}

static int map_buffers(struct av_video *video, unsigned int requested)
{
	struct v4l2_requestbuffers request;
	unsigned int i;

	memset(&request, 0, sizeof(request));
	request.count = requested;
	request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	request.memory = V4L2_MEMORY_MMAP;
	if (xioctl(video->fd, VIDIOC_REQBUFS, &request) == -1) {
		fprintf(stderr, "VIDIOC_REQBUFS failed: %s\n", strerror(errno));
		return -1;
	}
	video->buffers_requested = 1;
	if (request.count < 2U) {
		fprintf(stderr, "Driver allocated fewer than two video buffers\n");
		return -1;
	}

	video->buffers = calloc(request.count, sizeof(*video->buffers));
	if (!video->buffers) {
		fprintf(stderr, "Cannot allocate video buffer table: %s\n",
			strerror(errno));
		return -1;
	}
	video->buffer_count = request.count;

	for (i = 0; i < video->buffer_count; ++i) {
		struct v4l2_buffer buffer;

		memset(&buffer, 0, sizeof(buffer));
		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.index = i;
		if (xioctl(video->fd, VIDIOC_QUERYBUF, &buffer) == -1) {
			fprintf(stderr, "VIDIOC_QUERYBUF[%u] failed: %s\n",
				i, strerror(errno));
			return -1;
		}
		if (buffer.length < video->sizeimage) {
			fprintf(stderr, "Video buffer %u is too small\n", i);
			return -1;
		}

		video->buffers[i].length = buffer.length;
		video->buffers[i].start = mmap(NULL, buffer.length,
						PROT_READ | PROT_WRITE, MAP_SHARED,
						video->fd, buffer.m.offset);
		if (video->buffers[i].start == MAP_FAILED) {
			video->buffers[i].start = NULL;
			fprintf(stderr, "mmap video buffer %u failed: %s\n",
				i, strerror(errno));
			return -1;
		}
	}
	printf("video buffers  : %u MMAP buffers\n", video->buffer_count);
	return 0;
}

int av_video_open(struct av_video *video, const char *device,
		  const struct av_video_config *config)
{
	struct stat st;

	memset(video, 0, sizeof(*video));
	video->fd = -1;
	if (stat(device, &st) == -1 || !S_ISCHR(st.st_mode)) {
		fprintf(stderr, "%s is not an accessible character device\n", device);
		return -1;
	}
	video->fd = open(device, O_RDWR | O_NONBLOCK);
	if (video->fd == -1) {
		fprintf(stderr, "Cannot open %s: %s\n", device, strerror(errno));
		return -1;
	}
	if (check_capability(video->fd) == -1 ||
	    configure_video(video, config) == -1 ||
	    map_buffers(video, config->buffer_count) == -1) {
		av_video_close(video);
		return -1;
	}
	return 0;
}

int av_video_start(struct av_video *video)
{
	unsigned int i;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	for (i = 0; i < video->buffer_count; ++i) {
		struct v4l2_buffer buffer;

		memset(&buffer, 0, sizeof(buffer));
		buffer.type = type;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.index = i;
		if (xioctl(video->fd, VIDIOC_QBUF, &buffer) == -1) {
			fprintf(stderr, "Initial QBUF[%u] failed: %s\n",
				i, strerror(errno));
			return -1;
		}
	}
	if (xioctl(video->fd, VIDIOC_STREAMON, &type) == -1) {
		fprintf(stderr, "VIDIOC_STREAMON failed: %s\n", strerror(errno));
		return -1;
	}
	video->streaming = 1;
	return 0;
}

int av_video_dequeue(struct av_video *video, struct av_video_frame *frame,
		     unsigned int timeout_ms)
{
	struct v4l2_buffer buffer;
	struct timeval timeout;
	fd_set read_fds;
	int ready;

	for (;;) {
		int ioctl_ret;

		FD_ZERO(&read_fds);
		FD_SET(video->fd, &read_fds);
		timeout.tv_sec = timeout_ms / 1000U;
		timeout.tv_usec = (timeout_ms % 1000U) * 1000U;
		ready = select(video->fd + 1, &read_fds, NULL, NULL, &timeout);
		if (ready == -1 && errno == EINTR)
			continue;
		if (ready == -1) {
			fprintf(stderr, "select video failed: %s\n", strerror(errno));
			return -1;
		}
		if (ready == 0)
			return AV_VIDEO_TIMEOUT;

		memset(&buffer, 0, sizeof(buffer));
		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;
		ioctl_ret = xioctl(video->fd, VIDIOC_DQBUF, &buffer);
		if (ioctl_ret == -1) {
			if (errno == EAGAIN)
				continue;
			fprintf(stderr, "VIDIOC_DQBUF failed: %s\n", strerror(errno));
			return -1;
		}
		break;
	}

	if (buffer.index >= video->buffer_count ||
	    buffer.bytesused < video->sizeimage ||
	    buffer.bytesused > video->buffers[buffer.index].length ||
	    (buffer.flags & V4L2_BUF_FLAG_ERROR)) {
		fprintf(stderr, "Invalid/error video frame: index=%u bytes=%u flags=0x%x\n",
			buffer.index, buffer.bytesused, buffer.flags);
		return -1;
	}

	frame->data = video->buffers[buffer.index].start;
	frame->bytesused = buffer.bytesused;
	frame->index = buffer.index;
	frame->sequence = buffer.sequence;
	frame->flags = buffer.flags;
	frame->timestamp_us = (uint64_t)buffer.timestamp.tv_sec * 1000000ULL +
			      (uint64_t)buffer.timestamp.tv_usec;
	return 0;
}

int av_video_queue(struct av_video *video, const struct av_video_frame *frame)
{
	struct v4l2_buffer buffer;

	memset(&buffer, 0, sizeof(buffer));
	buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buffer.memory = V4L2_MEMORY_MMAP;
	buffer.index = frame->index;
	if (xioctl(video->fd, VIDIOC_QBUF, &buffer) == -1) {
		fprintf(stderr, "VIDIOC_QBUF[%u] failed: %s\n",
			frame->index, strerror(errno));
		return -1;
	}
	return 0;
}

int av_video_stop(struct av_video *video)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (!video->streaming)
		return 0;
	if (xioctl(video->fd, VIDIOC_STREAMOFF, &type) == -1) {
		fprintf(stderr, "VIDIOC_STREAMOFF failed: %s\n", strerror(errno));
		return -1;
	}
	video->streaming = 0;
	return 0;
}

void av_video_close(struct av_video *video)
{
	unsigned int i;
	int force_close = 0;

	if (video->streaming && av_video_stop(video) == -1)
		force_close = 1;
	if (force_close && video->fd >= 0) {
		(void)close(video->fd);
		video->fd = -1;
		video->streaming = 0;
	}

	for (i = 0; i < video->buffer_count; ++i) {
		if (video->buffers[i].start)
			(void)munmap(video->buffers[i].start, video->buffers[i].length);
	}
	free(video->buffers);
	video->buffers = NULL;
	video->buffer_count = 0;

	if (video->buffers_requested && video->fd >= 0) {
		struct v4l2_requestbuffers request;

		memset(&request, 0, sizeof(request));
		request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		request.memory = V4L2_MEMORY_MMAP;
		request.count = 0;
		(void)xioctl(video->fd, VIDIOC_REQBUFS, &request);
	}
	video->buffers_requested = 0;
	if (video->fd >= 0)
		(void)close(video->fd);
	video->fd = -1;
}
