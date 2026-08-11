/*
 * v4l2_capture.c - VIDEO-R2: V4L2 MMAP capture baseline
 *
 * This program establishes the first real video-data path in the project:
 *
 *   OV5640 -> mx6s-csi -> videobuf2 DMA buffer -> userspace mmap
 *
 * R2 intentionally stops at raw YUYV capture.  LCD output and YUYV-to-RGB565
 * conversion belong to VIDEO-R3, while signal handling and automatic recovery
 * belong to VIDEO-R5.  Keeping this round small makes DMA/buffer failures easy
 * to distinguish from display or color-conversion failures.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_VIDEO_DEVICE "/dev/video0"
#define DEFAULT_OUTPUT_FILE  "frame_640x480_yuyv.raw"
#define DEFAULT_FRAME_COUNT  120U

#define REQUESTED_WIDTH      640U
#define REQUESTED_HEIGHT     480U
#define REQUESTED_FPS        30U
#define REQUESTED_MODE       0U  /* NXP OV5640 mode 0 is VGA 640x480. */
#define REQUESTED_BUFFERS    4U
#define SELECT_TIMEOUT_SEC   2

struct mapped_buffer {
	void *start;
	size_t length;
};

struct capture_context {
	int fd;
	struct mapped_buffer *buffers;
	unsigned int buffer_count;
	int buffers_requested;
	int streaming;

	unsigned int width;
	unsigned int height;
	unsigned int bytesperline;
	unsigned int sizeimage;
	uint32_t pixel_format;

	unsigned char *saved_frame;
	size_t saved_bytes;
	unsigned int saved_capture_number;
};

struct capture_stats {
	unsigned int captured;
	unsigned int sequence_gaps;
	uint32_t first_sequence;
	uint32_t last_sequence;
	uint64_t first_timestamp_us;
	uint64_t last_timestamp_us;
	size_t min_bytesused;
	size_t max_bytesused;
};

static int xioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, request, arg);
	} while (ret == -1 && errno == EINTR);

	return ret;
}

static const char *fourcc_to_string(uint32_t fourcc, char text[5])
{
	unsigned int i;

	for (i = 0; i < 4; ++i) {
		unsigned char ch = (unsigned char)((fourcc >> (i * 8U)) & 0xffU);

		text[i] = isprint(ch) ? (char)ch : '.';
	}
	text[4] = '\0';
	return text;
}

static uint64_t timeval_to_us(const struct timeval *time)
{
	return (uint64_t)time->tv_sec * 1000000ULL +
	       (uint64_t)time->tv_usec;
}

static int parse_frame_count(const char *text, unsigned int *value)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' ||
	    parsed == 0UL || parsed > (unsigned long)UINT_MAX) {
		fprintf(stderr, "Invalid frame count '%s'\n", text);
		return -1;
	}

	*value = (unsigned int)parsed;
	return 0;
}

static int query_capture_capability(int fd)
{
	struct v4l2_capability cap;
	uint32_t active_caps;

	memset(&cap, 0, sizeof(cap));
	if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == -1) {
		fprintf(stderr, "VIDIOC_QUERYCAP failed: %s\n", strerror(errno));
		return -1;
	}

	if ((cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U)
		active_caps = cap.device_caps;
	else
		active_caps = cap.capabilities;

	printf("device capability:\n");
	printf("  driver       : %.*s\n", (int)sizeof(cap.driver),
	       (const char *)cap.driver);
	printf("  card         : %.*s\n", (int)sizeof(cap.card),
	       (const char *)cap.card);
	printf("  bus_info     : %.*s\n", (int)sizeof(cap.bus_info),
	       (const char *)cap.bus_info);
	printf("  device_caps  : 0x%08x\n", active_caps);

	if ((active_caps & V4L2_CAP_VIDEO_CAPTURE) == 0U) {
		fprintf(stderr, "Device does not support single-planar capture\n");
		return -1;
	}
	if ((active_caps & V4L2_CAP_STREAMING) == 0U) {
		fprintf(stderr, "Device does not support streaming/MMAP I/O\n");
		return -1;
	}

	return 0;
}

static int select_camera_input(int fd)
{
	unsigned int input = 0;

	/* mx6s-csi exposes exactly one input named Camera at index 0. */
	if (xioctl(fd, VIDIOC_S_INPUT, &input) == -1) {
		fprintf(stderr, "VIDIOC_S_INPUT(0) failed: %s\n", strerror(errno));
		return -1;
	}

	printf("selected input : %u (Camera)\n", input);
	return 0;
}

static int set_stream_parameters(int fd)
{
	struct v4l2_streamparm parm;
	const struct v4l2_fract *timeperframe;
	double actual_fps;

	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	parm.parm.capture.capturemode = REQUESTED_MODE;
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = REQUESTED_FPS;

	/*
	 * In NXP's OV5640 driver S_PARM is also the mode-selection operation:
	 * capturemode 0 programs the VGA register table, and 1/30 selects its
	 * 30-fps table.  Do this before S_FMT configures the CSI DMA geometry.
	 */
	if (xioctl(fd, VIDIOC_S_PARM, &parm) == -1) {
		fprintf(stderr, "VIDIOC_S_PARM failed: %s\n", strerror(errno));
		return -1;
	}

	timeperframe = &parm.parm.capture.timeperframe;
	if (timeperframe->numerator == 0U || timeperframe->denominator == 0U) {
		fprintf(stderr, "Driver returned an invalid timeperframe %u/%u\n",
			timeperframe->numerator, timeperframe->denominator);
		return -1;
	}

	actual_fps = (double)timeperframe->denominator /
		     (double)timeperframe->numerator;
	printf("stream params  : mode=%u, timeperframe=%u/%u s (%.2f fps)\n",
	       parm.parm.capture.capturemode,
	       timeperframe->numerator, timeperframe->denominator, actual_fps);

	if (timeperframe->denominator !=
	    REQUESTED_FPS * timeperframe->numerator) {
		fprintf(stderr, "Driver did not accept the requested %u fps\n",
			REQUESTED_FPS);
		return -1;
	}

	return 0;
}

static int set_capture_format(struct capture_context *ctx)
{
	struct v4l2_format format;
	const struct v4l2_pix_format *pix;
	uint64_t minimum_size;
	char fourcc[5];

	memset(&format, 0, sizeof(format));
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	format.fmt.pix.width = REQUESTED_WIDTH;
	format.fmt.pix.height = REQUESTED_HEIGHT;
	format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	format.fmt.pix.field = V4L2_FIELD_NONE;

	/* S_FMT is input/output: every field below is read from the returned struct. */
	if (xioctl(ctx->fd, VIDIOC_S_FMT, &format) == -1) {
		fprintf(stderr, "VIDIOC_S_FMT failed: %s\n", strerror(errno));
		return -1;
	}

	pix = &format.fmt.pix;
	printf("negotiated fmt : %ux%u '%s', field=%u\n",
	       pix->width, pix->height,
	       fourcc_to_string(pix->pixelformat, fourcc), pix->field);
	printf("  bytesperline : %u\n", pix->bytesperline);
	printf("  sizeimage    : %u\n", pix->sizeimage);
	printf("  colorspace   : %u\n", pix->colorspace);

	if (pix->width != REQUESTED_WIDTH ||
	    pix->height != REQUESTED_HEIGHT ||
	    pix->pixelformat != V4L2_PIX_FMT_YUYV) {
		fprintf(stderr,
			"Driver did not accept exact 640x480 YUYV baseline\n");
		return -1;
	}
	if (pix->bytesperline < pix->width * 2U) {
		fprintf(stderr, "Invalid bytesperline %u for YUYV width %u\n",
			pix->bytesperline, pix->width);
		return -1;
	}

	minimum_size = (uint64_t)pix->bytesperline * (uint64_t)pix->height;
	if ((uint64_t)pix->sizeimage < minimum_size) {
		fprintf(stderr,
			"Invalid sizeimage %u, expected at least %llu bytes\n",
			pix->sizeimage, (unsigned long long)minimum_size);
		return -1;
	}

	ctx->width = pix->width;
	ctx->height = pix->height;
	ctx->bytesperline = pix->bytesperline;
	ctx->sizeimage = pix->sizeimage;
	ctx->pixel_format = pix->pixelformat;
	return 0;
}

static int request_mmap_buffers(struct capture_context *ctx)
{
	struct v4l2_requestbuffers request;
	unsigned int i;

	memset(&request, 0, sizeof(request));
	request.count = REQUESTED_BUFFERS;
	request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	request.memory = V4L2_MEMORY_MMAP;

	if (xioctl(ctx->fd, VIDIOC_REQBUFS, &request) == -1) {
		fprintf(stderr, "VIDIOC_REQBUFS failed: %s\n", strerror(errno));
		return -1;
	}
	ctx->buffers_requested = 1;

	/* mx6s_start_streaming() directly requires at least two queued buffers. */
	if (request.count < 2U) {
		fprintf(stderr, "Driver allocated only %u buffer(s); at least 2 required\n",
			request.count);
		return -1;
	}

	ctx->buffers = calloc(request.count, sizeof(*ctx->buffers));
	if (ctx->buffers == NULL) {
		fprintf(stderr, "Cannot allocate userspace buffer table: %s\n",
			strerror(errno));
		return -1;
	}
	ctx->buffer_count = request.count;

	printf("MMAP buffers   : requested=%u, allocated=%u\n",
	       REQUESTED_BUFFERS, request.count);

	for (i = 0; i < ctx->buffer_count; ++i) {
		struct v4l2_buffer buffer;

		memset(&buffer, 0, sizeof(buffer));
		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.index = i;

		if (xioctl(ctx->fd, VIDIOC_QUERYBUF, &buffer) == -1) {
			fprintf(stderr, "VIDIOC_QUERYBUF[%u] failed: %s\n",
				i, strerror(errno));
			return -1;
		}
		if (buffer.length < ctx->sizeimage) {
			fprintf(stderr,
				"Buffer %u length %u is smaller than sizeimage %u\n",
				i, buffer.length, ctx->sizeimage);
			return -1;
		}

		ctx->buffers[i].length = buffer.length;
		ctx->buffers[i].start = mmap(NULL, buffer.length,
					     PROT_READ | PROT_WRITE, MAP_SHARED,
					     ctx->fd, buffer.m.offset);
		if (ctx->buffers[i].start == MAP_FAILED) {
			ctx->buffers[i].start = NULL;
			fprintf(stderr, "mmap buffer %u failed: %s\n",
				i, strerror(errno));
			return -1;
		}

		printf("  buffer[%u]   : offset=0x%08lx length=%u map=%p\n",
		       i, (unsigned long)buffer.m.offset,
		       buffer.length, ctx->buffers[i].start);
	}

	return 0;
}

static int queue_all_buffers(struct capture_context *ctx)
{
	unsigned int i;

	for (i = 0; i < ctx->buffer_count; ++i) {
		struct v4l2_buffer buffer;

		memset(&buffer, 0, sizeof(buffer));
		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.index = i;

		if (xioctl(ctx->fd, VIDIOC_QBUF, &buffer) == -1) {
			fprintf(stderr, "Initial VIDIOC_QBUF[%u] failed: %s\n",
				i, strerror(errno));
			return -1;
		}
	}

	printf("queued buffers : %u\n", ctx->buffer_count);
	return 0;
}

static int start_streaming(struct capture_context *ctx)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (xioctl(ctx->fd, VIDIOC_STREAMON, &type) == -1) {
		fprintf(stderr, "VIDIOC_STREAMON failed: %s\n", strerror(errno));
		return -1;
	}

	ctx->streaming = 1;
	printf("stream state   : ON\n");
	return 0;
}

static int stop_streaming(struct capture_context *ctx)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (!ctx->streaming)
		return 0;

	if (xioctl(ctx->fd, VIDIOC_STREAMOFF, &type) == -1) {
		fprintf(stderr, "VIDIOC_STREAMOFF failed: %s\n", strerror(errno));
		/* Keep the flag set so the common cleanup path can retry once. */
		return -1;
	}

	ctx->streaming = 0;
	printf("stream state   : OFF\n");
	return 0;
}

static int requeue_buffer(struct capture_context *ctx,
			  struct v4l2_buffer *buffer)
{
	if (xioctl(ctx->fd, VIDIOC_QBUF, buffer) == -1) {
		fprintf(stderr, "VIDIOC_QBUF[%u] failed: %s\n",
			buffer->index, strerror(errno));
		return -1;
	}
	return 0;
}

static int capture_frames(struct capture_context *ctx,
			  unsigned int target_frames,
			  struct capture_stats *stats)
{
	unsigned int save_at;

	memset(stats, 0, sizeof(*stats));
	/* Save a middle frame so startup transients cannot become the artifact. */
	save_at = target_frames / 2U;

	while (stats->captured < target_frames) {
		struct v4l2_buffer buffer;
		struct timeval timeout;
		fd_set read_fds;
		uint64_t timestamp_us;
		int ready;

		FD_ZERO(&read_fds);
		FD_SET(ctx->fd, &read_fds);
		timeout.tv_sec = SELECT_TIMEOUT_SEC;
		timeout.tv_usec = 0;

		ready = select(ctx->fd + 1, &read_fds, NULL, NULL, &timeout);
		if (ready == -1) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "select() failed: %s\n", strerror(errno));
			return -1;
		}
		if (ready == 0) {
			fprintf(stderr, "Capture timeout after %d seconds\n",
				SELECT_TIMEOUT_SEC);
			return -1;
		}

		memset(&buffer, 0, sizeof(buffer));
		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;

		if (xioctl(ctx->fd, VIDIOC_DQBUF, &buffer) == -1) {
			if (errno == EAGAIN)
				continue;
			fprintf(stderr, "VIDIOC_DQBUF failed: %s\n", strerror(errno));
			return -1;
		}

		if (buffer.index >= ctx->buffer_count) {
			fprintf(stderr, "Driver returned invalid buffer index %u\n",
				buffer.index);
			return -1;
		}
		if (buffer.bytesused > ctx->buffers[buffer.index].length) {
			fprintf(stderr,
				"Buffer %u bytesused %u exceeds mapped length %lu\n",
				buffer.index, buffer.bytesused,
				(unsigned long)ctx->buffers[buffer.index].length);
			return -1;
		}
		if ((buffer.flags & V4L2_BUF_FLAG_ERROR) != 0U) {
			fprintf(stderr, "Buffer %u sequence %u has ERROR flag\n",
				buffer.index, buffer.sequence);
			(void)requeue_buffer(ctx, &buffer);
			return -1;
		}
		if (buffer.bytesused < ctx->sizeimage) {
			fprintf(stderr,
				"Short frame: bytesused=%u, expected at least %u\n",
				buffer.bytesused, ctx->sizeimage);
			(void)requeue_buffer(ctx, &buffer);
			return -1;
		}

		timestamp_us = timeval_to_us(&buffer.timestamp);
		if (stats->captured == 0U) {
			stats->first_sequence = buffer.sequence;
			stats->first_timestamp_us = timestamp_us;
			stats->min_bytesused = buffer.bytesused;
			stats->max_bytesused = buffer.bytesused;
		} else {
			uint32_t sequence_delta = buffer.sequence - stats->last_sequence;

			/* Unsigned subtraction also handles the normal 32-bit wrap. */
			if (sequence_delta > 1U && sequence_delta < 0x80000000U)
				stats->sequence_gaps += sequence_delta - 1U;
			if (buffer.bytesused < stats->min_bytesused)
				stats->min_bytesused = buffer.bytesused;
			if (buffer.bytesused > stats->max_bytesused)
				stats->max_bytesused = buffer.bytesused;
		}

		stats->last_sequence = buffer.sequence;
		stats->last_timestamp_us = timestamp_us;

		if (stats->captured == save_at) {
			/* Copy once; file I/O is postponed until after STREAMOFF. */
			memcpy(ctx->saved_frame,
			       ctx->buffers[buffer.index].start,
			       ctx->sizeimage);
			ctx->saved_bytes = ctx->sizeimage;
			ctx->saved_capture_number = stats->captured + 1U;
		}

		if (stats->captured == 0U ||
		    (stats->captured + 1U) % 30U == 0U ||
		    stats->captured + 1U == target_frames) {
			printf("  frame %u/%u: index=%u seq=%u bytes=%u ts=%ld.%06ld\n",
			       stats->captured + 1U, target_frames,
			       buffer.index, buffer.sequence, buffer.bytesused,
			       (long)buffer.timestamp.tv_sec,
			       (long)buffer.timestamp.tv_usec);
		}

		stats->captured++;
		if (requeue_buffer(ctx, &buffer) == -1)
			return -1;
	}

	return 0;
}

static void print_capture_stats(const struct capture_stats *stats)
{
	double elapsed;
	double measured_fps;

	printf("capture stats  :\n");
	printf("  frames       : %u\n", stats->captured);
	if (stats->captured == 0U)
		return;

	printf("  sequence     : first=%u last=%u gaps=%u\n",
	       stats->first_sequence, stats->last_sequence,
	       stats->sequence_gaps);
	printf("  bytesused    : min=%lu max=%lu\n",
	       (unsigned long)stats->min_bytesused,
	       (unsigned long)stats->max_bytesused);

	if (stats->captured < 2U ||
	    stats->last_timestamp_us <= stats->first_timestamp_us) {
		printf("  measured fps : unavailable\n");
		return;
	}

	elapsed = (double)(stats->last_timestamp_us -
			   stats->first_timestamp_us) / 1000000.0;
	measured_fps = (double)(stats->captured - 1U) / elapsed;
	printf("  elapsed      : %.3f s (first-to-last timestamp)\n", elapsed);
	printf("  measured fps : %.2f\n", measured_fps);
}

static void analyze_yuyv_frame(const struct capture_context *ctx)
{
	const unsigned char *data = ctx->saved_frame;
	uint64_t y_sum = 0;
	uint64_t u_sum = 0;
	uint64_t v_sum = 0;
	uint64_t pixel_count = 0;
	uint64_t pair_count = 0;
	uint32_t fnv1a = 2166136261U;
	unsigned int y_min = 255U;
	unsigned int y_max = 0U;
	size_t i;

	for (i = 0; i < ctx->saved_bytes; ++i) {
		fnv1a ^= data[i];
		fnv1a *= 16777619U;
	}

	for (i = 0; i + 3U < ctx->saved_bytes; i += 4U) {
		unsigned int y0 = data[i];
		unsigned int u = data[i + 1U];
		unsigned int y1 = data[i + 2U];
		unsigned int v = data[i + 3U];

		if (y0 < y_min)
			y_min = y0;
		if (y1 < y_min)
			y_min = y1;
		if (y0 > y_max)
			y_max = y0;
		if (y1 > y_max)
			y_max = y1;

		y_sum += y0 + y1;
		u_sum += u;
		v_sum += v;
		pixel_count += 2U;
		pair_count++;
	}

	printf("saved frame    : capture #%u, %lu bytes\n",
	       ctx->saved_capture_number, (unsigned long)ctx->saved_bytes);
	printf("  FNV-1a       : 0x%08x\n", fnv1a);
	if (pixel_count != 0U && pair_count != 0U) {
		printf("  Y range/mean : %u..%u / %.2f\n",
		       y_min, y_max, (double)y_sum / (double)pixel_count);
		printf("  U/V mean     : %.2f / %.2f\n",
		       (double)u_sum / (double)pair_count,
		       (double)v_sum / (double)pair_count);
	}
}

static int save_frame_file(const char *path, const unsigned char *data,
			   size_t length)
{
	size_t written = 0;
	int output_fd;

	output_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (output_fd == -1) {
		fprintf(stderr, "Cannot create %s: %s\n", path, strerror(errno));
		return -1;
	}

	while (written < length) {
		ssize_t ret = write(output_fd, data + written, length - written);

		if (ret == -1) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "write(%s) failed: %s\n",
				path, strerror(errno));
			(void)close(output_fd);
			return -1;
		}
		if (ret == 0) {
			fprintf(stderr, "write(%s) returned zero\n", path);
			(void)close(output_fd);
			return -1;
		}
		written += (size_t)ret;
	}

	if (close(output_fd) == -1) {
		fprintf(stderr, "close(%s) failed: %s\n", path, strerror(errno));
		return -1;
	}

	printf("output file    : %s (%lu bytes)\n",
	       path, (unsigned long)length);
	return 0;
}

static void release_mmap_buffers(struct capture_context *ctx)
{
	unsigned int i;

	if (ctx->buffers != NULL) {
		for (i = 0; i < ctx->buffer_count; ++i) {
			if (ctx->buffers[i].start != NULL &&
			    ctx->buffers[i].start != MAP_FAILED) {
				if (munmap(ctx->buffers[i].start,
					   ctx->buffers[i].length) == -1) {
					fprintf(stderr,
						"munmap buffer %u failed: %s\n",
						i, strerror(errno));
				}
			}
		}
		free(ctx->buffers);
		ctx->buffers = NULL;
		ctx->buffer_count = 0;
	}

	if (ctx->buffers_requested && ctx->fd >= 0) {
		struct v4l2_requestbuffers request;

		memset(&request, 0, sizeof(request));
		request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		request.memory = V4L2_MEMORY_MMAP;
		request.count = 0;
		if (xioctl(ctx->fd, VIDIOC_REQBUFS, &request) == -1)
			fprintf(stderr, "Release VIDIOC_REQBUFS failed: %s\n",
				strerror(errno));
		ctx->buffers_requested = 0;
	}
}

static void usage(const char *program)
{
	printf("Usage: %s [video-device] [frame-count] [output-file]\n", program);
	printf("Defaults: %s %u %s\n",
	       DEFAULT_VIDEO_DEVICE, DEFAULT_FRAME_COUNT, DEFAULT_OUTPUT_FILE);
	printf("Example : %s /dev/video0 120 frame_640x480_yuyv.raw\n",
	       program);
}

int main(int argc, char *argv[])
{
	const char *device = DEFAULT_VIDEO_DEVICE;
	const char *output_file = DEFAULT_OUTPUT_FILE;
	unsigned int frame_count = DEFAULT_FRAME_COUNT;
	struct capture_context ctx;
	struct capture_stats stats;
	struct stat st;
	int capture_ok = 0;
	int ret = EXIT_FAILURE;

	memset(&ctx, 0, sizeof(ctx));
	memset(&stats, 0, sizeof(stats));
	ctx.fd = -1;

	if (argc > 4) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc >= 2) {
		if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
			usage(argv[0]);
			return EXIT_SUCCESS;
		}
		device = argv[1];
	}
	if (argc >= 3 && parse_frame_count(argv[2], &frame_count) == -1)
		return EXIT_FAILURE;
	if (argc >= 4)
		output_file = argv[3];

	if (stat(device, &st) == -1) {
		fprintf(stderr, "Cannot stat %s: %s\n", device, strerror(errno));
		return EXIT_FAILURE;
	}
	if (!S_ISCHR(st.st_mode)) {
		fprintf(stderr, "%s is not a character device\n", device);
		return EXIT_FAILURE;
	}

	ctx.fd = open(device, O_RDWR | O_NONBLOCK);
	if (ctx.fd == -1) {
		fprintf(stderr, "Cannot open %s: %s\n", device, strerror(errno));
		return EXIT_FAILURE;
	}

	printf("VIDEO-R2 V4L2 MMAP capture\n");
	printf("device         : %s\n", device);
	printf("target         : %ux%u YUYV, %u fps, %u frames\n",
	       REQUESTED_WIDTH, REQUESTED_HEIGHT,
	       REQUESTED_FPS, frame_count);
	printf("save path      : %s\n", output_file);

	if (query_capture_capability(ctx.fd) == -1)
		goto out;
	if (select_camera_input(ctx.fd) == -1)
		goto out;
	if (set_stream_parameters(ctx.fd) == -1)
		goto out;
	if (set_capture_format(&ctx) == -1)
		goto out;

	ctx.saved_frame = malloc(ctx.sizeimage);
	if (ctx.saved_frame == NULL) {
		fprintf(stderr, "Cannot allocate %u-byte frame copy: %s\n",
			ctx.sizeimage, strerror(errno));
		goto out;
	}

	if (request_mmap_buffers(&ctx) == -1)
		goto out;
	if (queue_all_buffers(&ctx) == -1)
		goto out;
	if (start_streaming(&ctx) == -1)
		goto out;
	if (capture_frames(&ctx, frame_count, &stats) == -1)
		goto out;
	capture_ok = 1;

	if (stop_streaming(&ctx) == -1)
		goto out;

	print_capture_stats(&stats);
	if (ctx.saved_bytes == 0U) {
		fprintf(stderr, "No frame was copied for output\n");
		goto out;
	}
	analyze_yuyv_frame(&ctx);
	if (save_frame_file(output_file, ctx.saved_frame, ctx.saved_bytes) == -1)
		goto out;

	printf("[PASS] captured %u frames through V4L2 MMAP.\n",
	       stats.captured);
	ret = EXIT_SUCCESS;

out:
	if (ctx.streaming && stop_streaming(&ctx) == -1)
		ret = EXIT_FAILURE;
	if (!capture_ok && stats.captured != 0U)
		print_capture_stats(&stats);
	release_mmap_buffers(&ctx);
	free(ctx.saved_frame);
	if (ctx.fd >= 0 && close(ctx.fd) == -1) {
		fprintf(stderr, "close(%s) failed: %s\n", device, strerror(errno));
		ret = EXIT_FAILURE;
	}
	return ret;
}
