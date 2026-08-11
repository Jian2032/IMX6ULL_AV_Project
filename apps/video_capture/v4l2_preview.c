/*
 * VIDEO-R5: threaded low-latency preview.
 *
 * One producer thread owns V4L2 DQBUF/QBUF.  The main thread owns color
 * conversion and LCD output.  A small private raw-frame pool separates the
 * lifetime of camera MMAP buffers from the slower display pipeline.
 */

#include "av_video_capture.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fb.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#if defined(AV_ENABLE_NEON) && AV_ENABLE_NEON
#include <arm_neon.h>
#define AV_HAVE_NEON 1
#else
#define AV_HAVE_NEON 0
#endif

#define DEFAULT_VIDEO_DEVICE "/dev/video0"
#define DEFAULT_FB_DEVICE    "/dev/fb0"
#define DEFAULT_FRAME_COUNT  300U
#define CAPTURE_TIMEOUT_MS   100U
#define MAX_CAPTURE_TIMEOUTS 20U
#define RAW_SLOT_COUNT       3U

struct framebuffer {
	int fd;
	void *mapping;
	size_t mapping_length;
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;
	unsigned int page_count;
	unsigned int front_page;
};

/*
 * WRITING and READING are exclusive ownership states.  The producer may
 * replace an old READY frame, but it must never touch a READING slot.
 */
enum raw_slot_state {
	RAW_SLOT_FREE = 0,
	RAW_SLOT_WRITING,
	RAW_SLOT_READY,
	RAW_SLOT_READING,
};

struct raw_frame_slot {
	unsigned char *data;
	enum raw_slot_state state;
	size_t bytesused;
	uint32_t sequence;
	uint32_t flags;
	uint64_t timestamp_us;
	unsigned long serial;
};

struct capture_pipeline {
	struct av_video *video;
	struct raw_frame_slot slots[RAW_SLOT_COUNT];
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t frame_ready;
	int lock_initialized;
	int cond_initialized;
	int thread_started;
	int stop_requested;
	int capture_finished;
	int capture_failed;

	unsigned long next_serial;
	unsigned int frames_captured;
	unsigned int capture_timeouts;
	unsigned int driver_sequence_gaps;
	unsigned int stale_frames_dropped;
	uint32_t first_sequence;
	uint32_t last_sequence;
	uint64_t first_timestamp;
	uint64_t last_timestamp;
};

static volatile sig_atomic_t exit_requested;

static void handle_stop_signal(int signal_number)
{
	(void)signal_number;
	exit_requested = 1;
}

/*
 * These lookup tables move the constant BT.601 multiplications out of the
 * per-pixel hot path.  They occupy only 5 KiB and are initialized once.
 */
struct yuv_lookup_tables {
	int y_base[256];
	int red_from_v[256];
	int green_from_u[256];
	int green_from_v[256];
	int blue_from_u[256];
};

static struct yuv_lookup_tables yuv_tables;

static void framebuffer_close(struct framebuffer *fb);
static int framebuffer_present_page(struct framebuffer *fb,
				    unsigned int page);

static int parse_count(const char *text, unsigned int *count)
{
	char *end = NULL;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno || end == text || *end || value == 0UL || value > UINT_MAX) {
		fprintf(stderr, "Invalid frame count '%s'\n", text);
		return -1;
	}
	*count = (unsigned int)value;
	return 0;
}

/*
 * The legacy OV5640 subdevice exposes only 15 and 30 fps for VGA.  Keeping
 * this diagnostic argument restricted to those two values prevents an
 * apparently successful run from silently using a driver-clamped rate.
 */
static int parse_fps(const char *text, unsigned int *fps)
{
	char *end = NULL;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno || end == text || *end || (value != 15UL && value != 30UL)) {
		fprintf(stderr, "Invalid fps '%s': only 15 or 30 is supported\n",
			text);
		return -1;
	}
	*fps = (unsigned int)value;
	return 0;
}

static int framebuffer_open(struct framebuffer *fb, const char *device)
{
	size_t virtual_size;

	memset(fb, 0, sizeof(*fb));
	fb->fd = -1;
	fb->fd = open(device, O_RDWR);
	if (fb->fd == -1) {
		fprintf(stderr, "Cannot open %s: %s\n", device, strerror(errno));
		return -1;
	}
	if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->fix) == -1 ||
	    ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->var) == -1) {
		fprintf(stderr, "Cannot query framebuffer: %s\n", strerror(errno));
		goto fail;
	}
	if (fb->var.bits_per_pixel != 16U ||
	    fb->var.red.offset != 11U || fb->var.red.length != 5U ||
	    fb->var.green.offset != 5U || fb->var.green.length != 6U ||
	    fb->var.blue.offset != 0U || fb->var.blue.length != 5U) {
		fprintf(stderr, "Framebuffer is not RGB565\n");
		goto fail;
	}
	if (fb->var.yres_virtual < fb->var.yres * 2U ||
	    fb->fix.ypanstep == 0U) {
		fprintf(stderr,
			"Framebuffer does not provide two vertically pannable pages\n");
		goto fail;
	}
	fb->page_count = 2U;
	if (ioctl(fb->fd, FBIOBLANK, FB_BLANK_UNBLANK) == -1) {
		fprintf(stderr, "Cannot unblank framebuffer: %s\n", strerror(errno));
		goto fail;
	}
	virtual_size = (size_t)fb->fix.line_length * fb->var.yres_virtual;
	if (virtual_size > fb->fix.smem_len) {
		fprintf(stderr, "Framebuffer memory geometry is invalid\n");
		goto fail;
	}

	/* Establish a known front page before the first hidden-page draw. */
	if (framebuffer_present_page(fb, 0U) == -1)
		goto fail;

	fb->mapping_length = fb->fix.smem_len;
	fb->mapping = mmap(NULL, fb->mapping_length,
			   PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
	if (fb->mapping == MAP_FAILED) {
		fb->mapping = NULL;
		fprintf(stderr, "Framebuffer mmap failed: %s\n", strerror(errno));
		goto fail;
	}
	printf("framebuffer    : %.*s %ux%u, virtual=%ux%u, line=%u, "
	       "RGB565, pages=%u\n",
	       (int)sizeof(fb->fix.id), fb->fix.id, fb->var.xres, fb->var.yres,
	       fb->var.xres_virtual, fb->var.yres_virtual,
	       fb->fix.line_length, fb->page_count);
	return 0;

fail:
	framebuffer_close(fb);
	return -1;
}

/*
 * FBIOPAN_DISPLAY only changes LCDIF's scanout address.  The custom LCD-R5
 * driver writes NEXT_BUF and sleeps until CUR_FRAME_DONE, so a successful
 * return means the page switch crossed a complete display-frame boundary.
 */
static int framebuffer_present_page(struct framebuffer *fb,
				    unsigned int page)
{
	struct fb_var_screeninfo pan;
	int ioctl_ret;

	if (page >= fb->page_count) {
		errno = EINVAL;
		return -1;
	}
	pan = fb->var;
	pan.xoffset = 0;
	pan.yoffset = page * fb->var.yres;
	pan.activate = FB_ACTIVATE_VBL;
	do {
		ioctl_ret = ioctl(fb->fd, FBIOPAN_DISPLAY, &pan);
	} while (ioctl_ret == -1 && errno == EINTR);
	if (ioctl_ret == -1) {
		fprintf(stderr, "Cannot present framebuffer page %u: %s\n",
			page, strerror(errno));
		return -1;
	}
	fb->var.xoffset = 0;
	fb->var.yoffset = pan.yoffset;
	fb->front_page = page;
	return 0;
}

static void framebuffer_close(struct framebuffer *fb)
{
	if (fb->mapping)
		(void)munmap(fb->mapping, fb->mapping_length);
	if (fb->fd >= 0)
		(void)close(fb->fd);
	fb->mapping = NULL;
	fb->fd = -1;
}

static int clip_u8(int value)
{
	if (value < 0)
		return 0;
	if (value > 255)
		return 255;
	return value;
}

/* Build the ITU-R BT.601 limited-range integer-conversion lookup tables. */
static void init_yuv_tables(struct yuv_lookup_tables *tables)
{
	unsigned int value;

	for (value = 0; value < 256U; ++value) {
		int y = (int)value - 16;
		int chroma = (int)value - 128;

		if (y < 0)
			y = 0;
		tables->y_base[value] = 298 * y;
		tables->red_from_v[value] = 409 * chroma;
		tables->green_from_u[value] = -100 * chroma;
		tables->green_from_v[value] = -208 * chroma;
		tables->blue_from_u[value] = 516 * chroma;
	}
}

static uint16_t rgb565_from_terms(int y_base, int red_chroma,
				  int green_chroma, int blue_chroma)
{
	int red;
	int green;
	int blue;

	red = clip_u8((y_base + red_chroma + 128) >> 8);
	green = clip_u8((y_base + green_chroma + 128) >> 8);
	blue = clip_u8((y_base + blue_chroma + 128) >> 8);
	return (uint16_t)(((unsigned int)(red & 0xf8) << 8) |
			  ((unsigned int)(green & 0xfc) << 3) |
			  ((unsigned int)blue >> 3));
}

#if AV_HAVE_NEON
/*
 * Add rounding, shift the signed BT.601 result by eight fractional bits, then
 * saturate it into an unsigned 8-bit color channel.  NEON performs this for
 * eight pixels in parallel.
 */
static inline uint8x8_t neon_channel_to_u8(int32x4_t low, int32x4_t high)
{
	const int32x4_t rounding = vdupq_n_s32(128);
	uint16x4_t low_u16;
	uint16x4_t high_u16;

	low = vaddq_s32(low, rounding);
	high = vaddq_s32(high, rounding);
	low_u16 = vqshrun_n_s32(low, 8);
	high_u16 = vqshrun_n_s32(high, 8);
	return vqmovn_u16(vcombine_u16(low_u16, high_u16));
}

/* Convert eight packed YUYV pixels to eight packed RGB565 pixels. */
static inline void convert_eight_yuyv_neon(const unsigned char *source,
					   uint16_t *destination,
					   uint8x8_t u_indices,
					   uint8x8_t v_indices)
{
	/* vld2 separates Y0,U0,Y1,V0... into Y bytes and interleaved UV. */
	uint8x8x2_t separated = vld2_u8(source);
	uint8x8_t u_bytes = vtbl1_u8(separated.val[1], u_indices);
	uint8x8_t v_bytes = vtbl1_u8(separated.val[1], v_indices);
	int16x8_t y = vreinterpretq_s16_u16(vmovl_u8(separated.val[0]));
	int16x8_t u = vreinterpretq_s16_u16(vmovl_u8(u_bytes));
	int16x8_t v = vreinterpretq_s16_u16(vmovl_u8(v_bytes));
	int16x4_t y_low;
	int16x4_t y_high;
	int16x4_t u_low;
	int16x4_t u_high;
	int16x4_t v_low;
	int16x4_t v_high;
	int32x4_t y_term_low;
	int32x4_t y_term_high;
	int32x4_t red_low;
	int32x4_t red_high;
	int32x4_t green_low;
	int32x4_t green_high;
	int32x4_t blue_low;
	int32x4_t blue_high;
	uint8x8_t red;
	uint8x8_t green;
	uint8x8_t blue;
	uint16x8_t red_u16;
	uint16x8_t green_u16;
	uint16x8_t blue_u16;
	uint16x8_t rgb565;

	/* Limited-range BT.601 uses C=max(Y-16, 0), D=U-128, E=V-128. */
	y = vmaxq_s16(vsubq_s16(y, vdupq_n_s16(16)), vdupq_n_s16(0));
	u = vsubq_s16(u, vdupq_n_s16(128));
	v = vsubq_s16(v, vdupq_n_s16(128));
	y_low = vget_low_s16(y);
	y_high = vget_high_s16(y);
	u_low = vget_low_s16(u);
	u_high = vget_high_s16(u);
	v_low = vget_low_s16(v);
	v_high = vget_high_s16(v);

	/* R=(298C+409E+128)>>8. */
	y_term_low = vmull_n_s16(y_low, 298);
	y_term_high = vmull_n_s16(y_high, 298);
	red_low = vmlal_n_s16(y_term_low, v_low, 409);
	red_high = vmlal_n_s16(y_term_high, v_high, 409);

	/* G=(298C-100D-208E+128)>>8. */
	green_low = vmlal_n_s16(y_term_low, u_low, -100);
	green_high = vmlal_n_s16(y_term_high, u_high, -100);
	green_low = vmlal_n_s16(green_low, v_low, -208);
	green_high = vmlal_n_s16(green_high, v_high, -208);

	/* B=(298C+516D+128)>>8. */
	blue_low = vmlal_n_s16(y_term_low, u_low, 516);
	blue_high = vmlal_n_s16(y_term_high, u_high, 516);
	red = neon_channel_to_u8(red_low, red_high);
	green = neon_channel_to_u8(green_low, green_high);
	blue = neon_channel_to_u8(blue_low, blue_high);

	/* Pack R5:G6:B5 into the LCD driver's native 16-bit pixel layout. */
	red_u16 = vmovl_u8(red);
	green_u16 = vmovl_u8(green);
	blue_u16 = vmovl_u8(blue);
	rgb565 = vshlq_n_u16(vshrq_n_u16(red_u16, 3), 11);
	rgb565 = vorrq_u16(rgb565,
			     vshlq_n_u16(vshrq_n_u16(green_u16, 2), 5));
	rgb565 = vorrq_u16(rgb565, vshrq_n_u16(blue_u16, 3));
	vst1q_u16(destination, rgb565);
}
#endif

static void clear_framebuffer_pages(struct framebuffer *fb)
{
	unsigned int y;
	unsigned int rows = fb->var.yres * fb->page_count;

	for (y = 0; y < rows; ++y) {
		unsigned char *row = (unsigned char *)fb->mapping +
				     y * fb->fix.line_length;
		memset(row, 0, fb->var.xres * sizeof(uint16_t));
	}
	__sync_synchronize();
}

/*
 * Convert into ordinary cacheable memory first.  The ARM target uses NEON to
 * process eight pixels per iteration; the scalar path remains for a short tail
 * or for non-NEON build-time diagnostics.
 */
static void convert_to_staging(const struct av_video *video,
			       const struct av_video_frame *frame,
			       uint16_t *staging,
			       const struct yuv_lookup_tables *tables)
{
	const unsigned char *source = frame->data;
	unsigned int y;
#if AV_HAVE_NEON
	static const uint8_t u_index_data[8] = { 0, 0, 2, 2, 4, 4, 6, 6 };
	static const uint8_t v_index_data[8] = { 1, 1, 3, 3, 5, 5, 7, 7 };
	const uint8x8_t u_indices = vld1_u8(u_index_data);
	const uint8x8_t v_indices = vld1_u8(v_index_data);
#endif

	for (y = 0; y < video->height; ++y) {
		const unsigned char *src = source + y * video->bytesperline;
		uint16_t *dst = staging + y * video->width;
		unsigned int x = 0;

#if AV_HAVE_NEON
		for (; x + 8U <= video->width; x += 8U) {
			convert_eight_yuyv_neon(src, dst + x,
						u_indices, v_indices);
			src += 16;
		}
#endif
		/* An even-width scalar tail also makes this routine generally usable. */
		for (; x + 1U < video->width; x += 2U) {
			unsigned int u = src[1];
			unsigned int v = src[3];
			int red_chroma = tables->red_from_v[v];
			int green_chroma = tables->green_from_u[u] +
					   tables->green_from_v[v];
			int blue_chroma = tables->blue_from_u[u];
			uint16_t pixel0;
			uint16_t pixel1;

			pixel0 = rgb565_from_terms(tables->y_base[src[0]],
						 red_chroma, green_chroma,
						 blue_chroma);
			pixel1 = rgb565_from_terms(tables->y_base[src[2]],
						 red_chroma, green_chroma,
						 blue_chroma);
			dst[x] = pixel0;
			dst[x + 1U] = pixel1;
			src += 4;
		}
	}
}

/*
 * Copy complete scanlines to WC framebuffer memory.  Large sequential memcpy
 * bursts are substantially faster than scattered 16-bit pixel stores.
 */
static void blit_to_framebuffer(const struct av_video *video,
				const uint16_t *staging,
				struct framebuffer *fb,
				unsigned int destination_x,
				unsigned int destination_y)
{
	size_t row_bytes = (size_t)video->width * sizeof(uint16_t);
	unsigned int y;

	for (y = 0; y < video->height; ++y) {
		unsigned char *dst = (unsigned char *)fb->mapping +
			(destination_y + y) * fb->fix.line_length +
			destination_x * sizeof(uint16_t);
		const uint16_t *src = staging + y * video->width;

		memcpy(dst, src, row_bytes);
	}
	/* Finish WC pixel stores before reporting the frame as displayed. */
	__sync_synchronize();
}

static uint64_t now_us(void)
{
	struct timeval now;

	if (gettimeofday(&now, NULL) == -1)
		return 0;
	return (uint64_t)now.tv_sec * 1000000ULL + (uint64_t)now.tv_usec;
}

static int capture_pipeline_init(struct capture_pipeline *pipeline,
				 struct av_video *video)
{
	unsigned int i;
	int error;

	memset(pipeline, 0, sizeof(*pipeline));
	pipeline->video = video;
	error = pthread_mutex_init(&pipeline->lock, NULL);
	if (error) {
		fprintf(stderr, "pthread_mutex_init failed: %s\n",
			strerror(error));
		return -1;
	}
	pipeline->lock_initialized = 1;
	error = pthread_cond_init(&pipeline->frame_ready, NULL);
	if (error) {
		fprintf(stderr, "pthread_cond_init failed: %s\n",
			strerror(error));
		return -1;
	}
	pipeline->cond_initialized = 1;

	/*
	 * Each slot stores an independent YUYV copy.  This is mandatory because
	 * the CSI driver may DMA new pixels into an MMAP buffer immediately after
	 * QBUF, while the display thread can still be converting the old frame.
	 */
	for (i = 0; i < RAW_SLOT_COUNT; ++i) {
		pipeline->slots[i].data = malloc(video->sizeimage);
		if (!pipeline->slots[i].data) {
			fprintf(stderr, "Cannot allocate raw frame slot %u\n", i);
			return -1;
		}
		pipeline->slots[i].state = RAW_SLOT_FREE;
	}
	return 0;
}

static int capture_pipeline_should_stop(struct capture_pipeline *pipeline)
{
	int stop;

	pthread_mutex_lock(&pipeline->lock);
	stop = pipeline->stop_requested;
	pthread_mutex_unlock(&pipeline->lock);
	return stop;
}

static void capture_pipeline_finish(struct capture_pipeline *pipeline,
				    int failed)
{
	pthread_mutex_lock(&pipeline->lock);
	pipeline->capture_failed = failed;
	pipeline->capture_finished = 1;
	pthread_cond_broadcast(&pipeline->frame_ready);
	pthread_mutex_unlock(&pipeline->lock);
}

/* Select a FREE slot, or replace the oldest frame that nobody is reading. */
static struct raw_frame_slot *capture_pipeline_reserve_slot(
					struct capture_pipeline *pipeline)
{
	struct raw_frame_slot *oldest_ready = NULL;
	unsigned int i;

	pthread_mutex_lock(&pipeline->lock);
	for (i = 0; i < RAW_SLOT_COUNT; ++i) {
		struct raw_frame_slot *slot = &pipeline->slots[i];

		if (slot->state == RAW_SLOT_FREE) {
			slot->state = RAW_SLOT_WRITING;
			pthread_mutex_unlock(&pipeline->lock);
			return slot;
		}
		if (slot->state == RAW_SLOT_READY &&
		    (!oldest_ready || slot->serial < oldest_ready->serial))
			oldest_ready = slot;
	}

	/* With one consumer, at most one slot can be READING, so this is expected. */
	if (oldest_ready) {
		oldest_ready->state = RAW_SLOT_WRITING;
		pipeline->stale_frames_dropped++;
	}
	pthread_mutex_unlock(&pipeline->lock);
	return oldest_ready;
}

static void capture_pipeline_publish(struct capture_pipeline *pipeline,
				     struct raw_frame_slot *slot,
				     const struct av_video_frame *frame)
{
	pthread_mutex_lock(&pipeline->lock);
	slot->bytesused = pipeline->video->sizeimage;
	slot->sequence = frame->sequence;
	slot->flags = frame->flags;
	slot->timestamp_us = frame->timestamp_us;
	slot->serial = pipeline->next_serial++;
	slot->state = RAW_SLOT_READY;

	if (pipeline->frames_captured == 0U) {
		pipeline->first_sequence = frame->sequence;
		pipeline->first_timestamp = frame->timestamp_us;
	} else {
		uint32_t delta = frame->sequence - pipeline->last_sequence;

		if (delta > 1U && delta < 0x80000000U)
			pipeline->driver_sequence_gaps += delta - 1U;
	}
	pipeline->last_sequence = frame->sequence;
	pipeline->last_timestamp = frame->timestamp_us;
	pipeline->frames_captured++;
	pthread_cond_signal(&pipeline->frame_ready);
	pthread_mutex_unlock(&pipeline->lock);
}

/*
 * The producer is the only thread allowed to call DQBUF/QBUF.  It holds a CSI
 * MMAP buffer only long enough to copy one frame into cacheable process memory.
 */
static void *capture_thread_main(void *argument)
{
	struct capture_pipeline *pipeline = argument;
	struct av_video *video = pipeline->video;
	unsigned int consecutive_timeouts = 0;
	int failed = 0;

	while (!capture_pipeline_should_stop(pipeline) && !exit_requested) {
		struct av_video_frame frame;
		struct raw_frame_slot *slot;
		int dequeue_ret;

		dequeue_ret = av_video_dequeue(video, &frame, CAPTURE_TIMEOUT_MS);
		if (dequeue_ret == AV_VIDEO_TIMEOUT) {
			if (exit_requested)
				break;
			pipeline->capture_timeouts++;
			consecutive_timeouts++;
			if (consecutive_timeouts >= MAX_CAPTURE_TIMEOUTS) {
				fprintf(stderr, "No camera frame for %u ms\n",
					MAX_CAPTURE_TIMEOUTS * CAPTURE_TIMEOUT_MS);
				failed = 1;
				break;
			}
			continue;
		}
		if (dequeue_ret == -1) {
			failed = 1;
			break;
		}
		consecutive_timeouts = 0;

		/* A stop can arrive while select() is waiting; return this buffer first. */
		if (capture_pipeline_should_stop(pipeline)) {
			if (av_video_queue(video, &frame) == -1)
				failed = 1;
			break;
		}

		slot = capture_pipeline_reserve_slot(pipeline);
		if (!slot) {
			fprintf(stderr, "Raw frame pool has no writable slot\n");
			(void)av_video_queue(video, &frame);
			failed = 1;
			break;
		}
		memcpy(slot->data, frame.data, video->sizeimage);

		/* QBUF before publication keeps all CSI MMAP buffers circulating. */
		if (av_video_queue(video, &frame) == -1) {
			pthread_mutex_lock(&pipeline->lock);
			slot->state = RAW_SLOT_FREE;
			pthread_mutex_unlock(&pipeline->lock);
			failed = 1;
			break;
		}
		capture_pipeline_publish(pipeline, slot, &frame);
	}

	capture_pipeline_finish(pipeline, failed);
	return NULL;
}

static int capture_pipeline_start(struct capture_pipeline *pipeline)
{
	int error = pthread_create(&pipeline->thread, NULL,
				   capture_thread_main, pipeline);

	if (error) {
		fprintf(stderr, "pthread_create failed: %s\n", strerror(error));
		return -1;
	}
	pipeline->thread_started = 1;
	return 0;
}

/*
 * Acquire the newest READY slot and release all older READY slots.  Dropping
 * stale frames here is deliberate: a preview terminal values bounded latency
 * over displaying every historical frame after the CPU falls behind.
 */
static int capture_pipeline_acquire_latest(struct capture_pipeline *pipeline,
					   struct av_video_frame *frame,
					   struct raw_frame_slot **held_slot)
{
	struct raw_frame_slot *newest;
	unsigned int i;

	pthread_mutex_lock(&pipeline->lock);
	for (;;) {
		newest = NULL;
		for (i = 0; i < RAW_SLOT_COUNT; ++i) {
			struct raw_frame_slot *slot = &pipeline->slots[i];

			if (slot->state == RAW_SLOT_READY &&
			    (!newest || slot->serial > newest->serial))
				newest = slot;
		}
		if (newest)
			break;
		if (pipeline->capture_failed || pipeline->capture_finished) {
			int failed = pipeline->capture_failed;

			pthread_mutex_unlock(&pipeline->lock);
			return failed ? -1 : AV_VIDEO_TIMEOUT;
		}
		pthread_cond_wait(&pipeline->frame_ready, &pipeline->lock);
	}

	for (i = 0; i < RAW_SLOT_COUNT; ++i) {
		struct raw_frame_slot *slot = &pipeline->slots[i];

		if (slot != newest && slot->state == RAW_SLOT_READY) {
			slot->state = RAW_SLOT_FREE;
			pipeline->stale_frames_dropped++;
		}
	}
	newest->state = RAW_SLOT_READING;
	frame->data = newest->data;
	frame->bytesused = newest->bytesused;
	frame->index = 0;
	frame->sequence = newest->sequence;
	frame->flags = newest->flags;
	frame->timestamp_us = newest->timestamp_us;
	*held_slot = newest;
	pthread_mutex_unlock(&pipeline->lock);
	return 0;
}

static void capture_pipeline_release(struct capture_pipeline *pipeline,
				     struct raw_frame_slot *slot)
{
	pthread_mutex_lock(&pipeline->lock);
	slot->state = RAW_SLOT_FREE;
	pthread_mutex_unlock(&pipeline->lock);
}

static void capture_pipeline_stop(struct capture_pipeline *pipeline)
{
	if (!pipeline->lock_initialized)
		return;
	pthread_mutex_lock(&pipeline->lock);
	pipeline->stop_requested = 1;
	if (pipeline->cond_initialized)
		pthread_cond_broadcast(&pipeline->frame_ready);
	pthread_mutex_unlock(&pipeline->lock);
	if (pipeline->thread_started) {
		(void)pthread_join(pipeline->thread, NULL);
		pipeline->thread_started = 0;
	}
}

static void capture_pipeline_destroy(struct capture_pipeline *pipeline)
{
	unsigned int i;

	capture_pipeline_stop(pipeline);
	for (i = 0; i < RAW_SLOT_COUNT; ++i) {
		free(pipeline->slots[i].data);
		pipeline->slots[i].data = NULL;
	}
	if (pipeline->cond_initialized)
		(void)pthread_cond_destroy(&pipeline->frame_ready);
	if (pipeline->lock_initialized)
		(void)pthread_mutex_destroy(&pipeline->lock);
	pipeline->cond_initialized = 0;
	pipeline->lock_initialized = 0;
}

static int write_binary_file(const char *path, const void *data, size_t length)
{
	const unsigned char *bytes = data;
	size_t written = 0;
	int fd;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd == -1) {
		fprintf(stderr, "Cannot create %s: %s\n", path, strerror(errno));
		return -1;
	}
	while (written < length) {
		ssize_t result = write(fd, bytes + written, length - written);

		if (result == -1 && errno == EINTR)
			continue;
		if (result <= 0) {
			fprintf(stderr, "Cannot write %s: %s\n", path,
				result == 0 ? "short write" : strerror(errno));
			(void)close(fd);
			return -1;
		}
		written += (size_t)result;
	}
	if (close(fd) == -1) {
		fprintf(stderr, "Cannot close %s: %s\n", path, strerror(errno));
		return -1;
	}
	printf("snapshot file  : %s (%lu bytes)\n",
	       path, (unsigned long)length);
	return 0;
}

static int make_snapshot_path(char path[PATH_MAX], const char *prefix,
			      const char *suffix)
{
	int length = snprintf(path, PATH_MAX, "%s%s", prefix, suffix);

	if (length < 0 || length >= PATH_MAX) {
		fprintf(stderr, "Snapshot path is too long\n");
		return -1;
	}
	return 0;
}

/*
 * Save one fault frame at three boundaries after STREAMOFF:
 *
 *   1. last_yuyv: bytes copied directly from the V4L2 frame;
 *   2. rgb_staging: the same frame after NEON YUYV-to-RGB565 conversion;
 *   3. visible framebuffer page: centered RGB565 image plus black borders.
 *
 * Comparing these files on the PC identifies the first stage where horizontal
 * alignment becomes wrong.  File I/O happens only after real-time capture has
 * stopped, so NFS performance cannot create the fault being diagnosed.
 */
static int save_diagnostic_snapshot(const char *prefix,
				    const struct av_video *video,
				    const unsigned char *last_yuyv,
				    const uint16_t *rgb_staging,
				    const struct framebuffer *fb,
				    uint32_t sequence)
{
	char yuyv_path[PATH_MAX];
	char rgb_path[PATH_MAX];
	char fb_path[PATH_MAX];
	unsigned char *packed_fb = NULL;
	size_t rgb_length;
	size_t fb_row_bytes;
	size_t fb_length;
	unsigned int y;
	int ret = -1;

	if (make_snapshot_path(yuyv_path, prefix, "_last_yuyv.raw") == -1 ||
	    make_snapshot_path(rgb_path, prefix, "_last_rgb565.raw") == -1 ||
	    make_snapshot_path(fb_path, prefix, "_last_fb_rgb565.raw") == -1)
		return -1;

	rgb_length = (size_t)video->width * video->height * sizeof(uint16_t);
	fb_row_bytes = (size_t)fb->var.xres * sizeof(uint16_t);
	fb_length = fb_row_bytes * fb->var.yres;
	packed_fb = malloc(fb_length);
	if (!packed_fb) {
		fprintf(stderr, "Cannot allocate packed framebuffer snapshot\n");
		return -1;
	}

	for (y = 0; y < fb->var.yres; ++y) {
		const unsigned char *source = (const unsigned char *)fb->mapping +
			(fb->front_page * fb->var.yres + y) * fb->fix.line_length;

		memcpy(packed_fb + y * fb_row_bytes, source, fb_row_bytes);
	}

	printf("snapshot       : displayed sequence=%u page=%u\n",
	       sequence, fb->front_page);
	if (write_binary_file(yuyv_path, last_yuyv, video->sizeimage) == -1 ||
	    write_binary_file(rgb_path, rgb_staging, rgb_length) == -1 ||
	    write_binary_file(fb_path, packed_fb, fb_length) == -1)
		goto out;
	ret = 0;

out:
	free(packed_fb);
	return ret;
}

static void usage(const char *program)
{
	printf("Usage: %s [video-device] [fb-device] [frame-count] "
	       "[snapshot-prefix] [fps]\n", program);
	printf("Defaults: %s %s %u, fps=30\n",
	       DEFAULT_VIDEO_DEVICE, DEFAULT_FB_DEVICE, DEFAULT_FRAME_COUNT);
	printf("Example : %s /dev/video0 /dev/fb0 300 natural_15 15\n",
	       program);
}

int main(int argc, char *argv[])
{
	const char *video_device = DEFAULT_VIDEO_DEVICE;
	const char *fb_device = DEFAULT_FB_DEVICE;
	const char *snapshot_prefix = NULL;
	unsigned int target_frames = DEFAULT_FRAME_COUNT;
	struct av_video_config config = {
		.width = 640,
		.height = 480,
		.fps = 30,
		.capture_mode = 0,
		.buffer_count = 4,
		.pixel_format = V4L2_PIX_FMT_YUYV,
	};
	struct av_video video;
	struct framebuffer fb;
	struct capture_pipeline pipeline;
	unsigned int destination_x;
	unsigned int destination_y;
	unsigned int displayed = 0;
	uint64_t display_start = 0;
	uint64_t display_finish = 0;
	uint64_t conversion_total = 0;
	uint64_t conversion_max = 0;
	uint64_t blit_total = 0;
	uint64_t blit_max = 0;
	uint64_t flip_total = 0;
	uint64_t flip_max = 0;
	uint64_t pipeline_total = 0;
	uint64_t pipeline_max = 0;
	uint16_t *rgb_staging = NULL;
	unsigned char *last_yuyv = NULL;
	uint32_t last_displayed_sequence = 0;
	int snapshot_valid = 0;
	int video_opened = 0;
	int fb_opened = 0;
	int pipeline_initialized = 0;
	int ret = EXIT_FAILURE;

	memset(&video, 0, sizeof(video));
	memset(&fb, 0, sizeof(fb));
	memset(&pipeline, 0, sizeof(pipeline));
	video.fd = -1;
	fb.fd = -1;
	if (argc > 6) {
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
	if (argc >= 3)
		fb_device = argv[2];
	if (argc >= 4 && parse_count(argv[3], &target_frames) == -1)
		return EXIT_FAILURE;
	if (argc >= 5) {
		if (argv[4][0] == '\0') {
			fprintf(stderr, "Snapshot prefix cannot be empty\n");
			return EXIT_FAILURE;
		}
		snapshot_prefix = argv[4];
	}
	if (argc >= 6 && parse_fps(argv[5], &config.fps) == -1)
		return EXIT_FAILURE;
	if (signal(SIGINT, handle_stop_signal) == SIG_ERR ||
	    signal(SIGTERM, handle_stop_signal) == SIG_ERR) {
		fprintf(stderr, "Cannot install stop signal handlers\n");
		return EXIT_FAILURE;
	}

	printf("VIDEO-R5 threaded low-latency LCD preview\n");
	if (av_video_open(&video, video_device, &config) == -1)
		goto out;
	video_opened = 1;
	if (framebuffer_open(&fb, fb_device) == -1)
		goto out;
	fb_opened = 1;
	if (video.width > fb.var.xres || video.height > fb.var.yres ||
	    (video.width & 1U)) {
		fprintf(stderr, "Video frame cannot be centered on this framebuffer\n");
		goto out;
	}
	destination_x = (fb.var.xres - video.width) / 2U;
	destination_y = (fb.var.yres - video.height) / 2U;
	printf("placement      : %ux%u at (%u,%u), borders L/R=%u T/B=%u\n",
	       video.width, video.height, destination_x, destination_y,
	       destination_x, destination_y);
	rgb_staging = malloc((size_t)video.width * video.height *
			     sizeof(*rgb_staging));
	if (!rgb_staging) {
		fprintf(stderr, "Cannot allocate RGB565 staging buffer\n");
		goto out;
	}
	if (snapshot_prefix) {
		last_yuyv = malloc(video.sizeimage);
		if (!last_yuyv) {
			fprintf(stderr, "Cannot allocate diagnostic YUYV snapshot\n");
			goto out;
		}
		printf("snapshot mode  : prefix=%s (files written after STREAMOFF)\n",
		       snapshot_prefix);
	}
	init_yuv_tables(&yuv_tables);
#if AV_HAVE_NEON
	printf("converter      : ARM NEON, 8 pixels/iteration\n");
#else
	printf("converter      : scalar lookup fallback\n");
#endif
	clear_framebuffer_pages(&fb);
	pipeline_initialized = 1;
	if (capture_pipeline_init(&pipeline, &video) == -1)
		goto out;
	if (av_video_start(&video) == -1)
		goto out;
	if (capture_pipeline_start(&pipeline) == -1)
		goto out;
	display_start = now_us();

	while (displayed < target_frames && !exit_requested) {
		struct av_video_frame frame;
		struct raw_frame_slot *held_slot = NULL;
		uint64_t convert_start;
		uint64_t convert_finish;
		uint64_t blit_finish;
		uint64_t flip_finish;
		uint64_t convert_duration = 0;
		uint64_t blit_duration = 0;
		uint64_t flip_duration = 0;
		uint64_t pipeline_duration = 0;
		unsigned int back_page;
		int acquire_ret;

		acquire_ret = capture_pipeline_acquire_latest(&pipeline, &frame,
						       &held_slot);
		if (acquire_ret == AV_VIDEO_TIMEOUT) {
			if (exit_requested)
				break;
			fprintf(stderr, "Capture thread stopped before target count\n");
			goto out;
		}
		if (acquire_ret == -1)
			goto out;
		back_page = fb.front_page ^ 1U;
		if (last_yuyv) {
			memcpy(last_yuyv, frame.data, video.sizeimage);
			last_displayed_sequence = frame.sequence;
			snapshot_valid = 1;
		}

		convert_start = now_us();
		convert_to_staging(&video, &frame, rgb_staging, &yuv_tables);
		convert_finish = now_us();
		blit_to_framebuffer(&video, rgb_staging, &fb,
				    destination_x,
				    back_page * fb.var.yres + destination_y);
		blit_finish = now_us();
		if (framebuffer_present_page(&fb, back_page) == -1) {
			capture_pipeline_release(&pipeline, held_slot);
			goto out;
		}
		flip_finish = now_us();
		if (convert_finish >= convert_start) {
			convert_duration = convert_finish - convert_start;
			conversion_total += convert_duration;
			if (convert_duration > conversion_max)
				conversion_max = convert_duration;
		}
		if (blit_finish >= convert_finish) {
			blit_duration = blit_finish - convert_finish;
			blit_total += blit_duration;
			if (blit_duration > blit_max)
				blit_max = blit_duration;
		}
		if (flip_finish >= blit_finish) {
			flip_duration = flip_finish - blit_finish;
			flip_total += flip_duration;
			if (flip_duration > flip_max)
				flip_max = flip_duration;
		}
		if (flip_finish >= convert_start) {
			pipeline_duration = flip_finish - convert_start;
			pipeline_total += pipeline_duration;
			if (pipeline_duration > pipeline_max)
				pipeline_max = pipeline_duration;
		}
		capture_pipeline_release(&pipeline, held_slot);

		displayed++;
		if (displayed == 1U || displayed % 30U == 0U ||
		    displayed == target_frames) {
			printf("  preview %u/%u: seq=%u page=%u convert=%llu us "
			       "blit=%llu us flip=%llu us total=%llu us\n",
			       displayed, target_frames, frame.sequence,
			       fb.front_page,
			       (unsigned long long)convert_duration,
			       (unsigned long long)blit_duration,
			       (unsigned long long)flip_duration,
			       (unsigned long long)pipeline_duration);
		}
	}

	display_finish = now_us();
	capture_pipeline_stop(&pipeline);
	if (pipeline.capture_failed) {
		fprintf(stderr, "Capture thread reported an error\n");
		goto out;
	}
	if (av_video_stop(&video) == -1)
		goto out;
	printf("capture stats  : frames=%u driver_sequence_gaps=%u timeouts=%u\n",
	       pipeline.frames_captured, pipeline.driver_sequence_gaps,
	       pipeline.capture_timeouts);
	printf("  sequence     : first=%u last=%u\n",
	       pipeline.first_sequence, pipeline.last_sequence);
	if (pipeline.frames_captured > 1U &&
	    pipeline.last_timestamp > pipeline.first_timestamp) {
		double elapsed = (double)(pipeline.last_timestamp -
					  pipeline.first_timestamp) / 1000000.0;
		printf("  capture fps  : %.2f\n",
		       (pipeline.frames_captured - 1U) / elapsed);
	}
	printf("display stats  : frames=%u stale_frames_dropped=%u\n",
	       displayed, pipeline.stale_frames_dropped);
	if (display_finish > display_start) {
		double elapsed = (double)(display_finish - display_start) / 1000000.0;
		printf("  display fps  : %.2f\n", displayed / elapsed);
	}
	printf("  convert avg  : %.2f ms\n",
	       displayed ? (double)conversion_total / displayed / 1000.0 : 0.0);
	printf("  convert max  : %.2f ms\n", (double)conversion_max / 1000.0);
	printf("  blit avg     : %.2f ms\n",
	       displayed ? (double)blit_total / displayed / 1000.0 : 0.0);
	printf("  blit max     : %.2f ms\n", (double)blit_max / 1000.0);
	printf("  flip avg     : %.2f ms\n",
	       displayed ? (double)flip_total / displayed / 1000.0 : 0.0);
	printf("  flip max     : %.2f ms\n", (double)flip_max / 1000.0);
	printf("  pipeline avg : %.2f ms\n",
	       displayed ? (double)pipeline_total / displayed / 1000.0 : 0.0);
	printf("  pipeline max : %.2f ms\n", (double)pipeline_max / 1000.0);
	if (pipeline.driver_sequence_gaps != 0U) {
		fprintf(stderr, "CSI capture lost %u frame(s) before user space\n",
			pipeline.driver_sequence_gaps);
		goto out;
	}
	if (snapshot_prefix) {
		if (!snapshot_valid) {
			fprintf(stderr, "No displayed frame is available for snapshot\n");
			goto out;
		}
		if (save_diagnostic_snapshot(snapshot_prefix, &video,
					     last_yuyv, rgb_staging, &fb,
					     last_displayed_sequence) == -1)
			goto out;
	}
	if (exit_requested) {
		printf("[STOP] Signal requested a clean shutdown after %u frames.\n",
		       displayed);
		ret = EXIT_SUCCESS;
		goto out;
	}
	printf("[PASS] Captured without driver gaps and presented %u latest frames.\n",
	       displayed);
	ret = EXIT_SUCCESS;

out:
	if (pipeline_initialized)
		capture_pipeline_destroy(&pipeline);
	if (video_opened)
		av_video_close(&video);
	free(last_yuyv);
	free(rgb_staging);
	if (fb_opened)
		framebuffer_close(&fb);
	return ret;
}
