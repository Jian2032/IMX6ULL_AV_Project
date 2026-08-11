/* SPDX-License-Identifier: MIT */
/*
 * av_lcd_preview.c - AV-R2 latest-frame YUYV to LCD RGB565 consumer
 *
 * The V4L2/JPEG pipeline remains the sole camera producer.  This module asks
 * it for a private copy of the newest already-oriented cacheable YUYV frame.
 * Therefore LCD conversion and FBIOPAN_DISPLAY never hold a CSI MMAP buffer
 * and never delay VIDIOC_QBUF.
 */

#define _POSIX_C_SOURCE 200809L

#include "av_lcd_preview.h"
#include "av_mjpeg_pipeline.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#if defined(AV_ENABLE_NEON) && AV_ENABLE_NEON
#include <arm_neon.h>
#define AV_LCD_HAVE_NEON 1
#else
#define AV_LCD_HAVE_NEON 0
#endif

struct av_framebuffer {
	int fd;
	void *mapping;
	size_t mapping_length;
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;
	unsigned int page_count;
	unsigned int front_page;
};

struct av_yuv_tables {
	int y_base[256];
	int red_from_v[256];
	int green_from_u[256];
	int green_from_v[256];
	int blue_from_u[256];
};

struct av_lcd_preview {
	struct av_mjpeg_pipeline *pipeline;
	struct av_mjpeg_info video;
	struct av_framebuffer fb;
	struct av_yuv_tables tables;
	unsigned char *raw_copy;
	uint16_t *rgb_staging;
	unsigned int destination_x;
	unsigned int destination_y;
	unsigned int target_fps;
	uint64_t frame_interval_us;

	pthread_t thread;
	pthread_mutex_t lock;
	int lock_initialized;
	int thread_started;
	struct av_lcd_preview_stats stats;
};

static uint64_t av_lcd_now_us(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return (uint64_t)now.tv_sec * UINT64_C(1000000) +
	       (uint64_t)now.tv_nsec / UINT64_C(1000);
}

/*
 * LCD is a latest-frame consumer.  Bounding it below the 30 fps producer
 * leaves deterministic CPU time for CSI buffer return, JPEG and ALSA.
 */
static void av_lcd_wait_budget(struct av_lcd_preview *preview,
			       uint64_t cycle_begin_us)
{
	uint64_t deadline;

	if (preview->frame_interval_us == 0U ||
	    cycle_begin_us > UINT64_MAX - preview->frame_interval_us)
		return;
	deadline = cycle_begin_us + preview->frame_interval_us;
	for (;;) {
		struct timespec delay;
		uint64_t now = av_lcd_now_us();
		uint64_t remaining;

		if (now == 0U || now >= deadline)
			break;
		remaining = deadline - now;
		delay.tv_sec = (time_t)(remaining / UINT64_C(1000000));
		delay.tv_nsec =
			(long)(remaining % UINT64_C(1000000)) * 1000L;
		while (nanosleep(&delay, &delay) < 0 && errno == EINTR)
			;
	}
}

static void av_framebuffer_close(struct av_framebuffer *fb)
{
	if (fb->mapping)
		(void)munmap(fb->mapping, fb->mapping_length);
	if (fb->fd >= 0)
		(void)close(fb->fd);
	fb->mapping = NULL;
	fb->fd = -1;
}

static int av_framebuffer_present(struct av_framebuffer *fb,
				  unsigned int page)
{
	struct fb_var_screeninfo pan;
	int result;

	if (page >= fb->page_count)
		return -EINVAL;
	pan = fb->var;
	pan.xoffset = 0;
	pan.yoffset = page * fb->var.yres;
	pan.activate = FB_ACTIVATE_VBL;
	do {
		result = ioctl(fb->fd, FBIOPAN_DISPLAY, &pan);
	} while (result < 0 && errno == EINTR);
	if (result < 0)
		return -errno;
	fb->var.xoffset = 0;
	fb->var.yoffset = pan.yoffset;
	fb->front_page = page;
	return 0;
}

static int av_framebuffer_open(struct av_framebuffer *fb, const char *device)
{
	size_t virtual_size;
	int result;

	memset(fb, 0, sizeof(*fb));
	fb->fd = -1;
	fb->fd = open(device, O_RDWR | O_CLOEXEC);
	if (fb->fd < 0)
		return -errno;
	if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->fix) < 0 ||
	    ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->var) < 0) {
		result = -errno;
		goto fail;
	}
	if (fb->var.bits_per_pixel != 16U ||
	    fb->var.red.offset != 11U || fb->var.red.length != 5U ||
	    fb->var.green.offset != 5U || fb->var.green.length != 6U ||
	    fb->var.blue.offset != 0U || fb->var.blue.length != 5U) {
		result = -EINVAL;
		fprintf(stderr, "Framebuffer is not RGB565\n");
		goto fail;
	}
	if (fb->var.yres_virtual < fb->var.yres * 2U ||
	    fb->fix.ypanstep == 0U) {
		result = -EINVAL;
		fprintf(stderr, "Framebuffer does not provide two pannable pages\n");
		goto fail;
	}
	fb->page_count = 2U;
	if (ioctl(fb->fd, FBIOBLANK, FB_BLANK_UNBLANK) < 0) {
		result = -errno;
		goto fail;
	}
	virtual_size = (size_t)fb->fix.line_length * fb->var.yres_virtual;
	if (virtual_size > fb->fix.smem_len) {
		result = -EINVAL;
		goto fail;
	}
	result = av_framebuffer_present(fb, 0U);
	if (result < 0)
		goto fail;

	fb->mapping_length = fb->fix.smem_len;
	fb->mapping = mmap(NULL, fb->mapping_length,
			   PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
	if (fb->mapping == MAP_FAILED) {
		fb->mapping = NULL;
		result = -errno;
		goto fail;
	}
	printf("framebuffer      : %.*s %ux%u, virtual=%ux%u, line=%u, "
	       "RGB565, pages=%u\n",
	       (int)sizeof(fb->fix.id), fb->fix.id, fb->var.xres, fb->var.yres,
	       fb->var.xres_virtual, fb->var.yres_virtual,
	       fb->fix.line_length, fb->page_count);
	return 0;

fail:
	av_framebuffer_close(fb);
	return result;
}

static void av_clear_pages(struct av_framebuffer *fb)
{
	unsigned int row;
	unsigned int rows = fb->var.yres * fb->page_count;

	for (row = 0; row < rows; ++row) {
		unsigned char *destination = (unsigned char *)fb->mapping +
			row * fb->fix.line_length;

		memset(destination, 0,
		       (size_t)fb->var.xres * sizeof(uint16_t));
	}
	__sync_synchronize();
}

static int av_clip_u8(int value)
{
	if (value < 0)
		return 0;
	if (value > 255)
		return 255;
	return value;
}

static void av_init_yuv_tables(struct av_yuv_tables *tables)
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

static uint16_t av_rgb565_from_terms(int y_base, int red_chroma,
				     int green_chroma, int blue_chroma)
{
	int red = av_clip_u8((y_base + red_chroma + 128) >> 8);
	int green = av_clip_u8((y_base + green_chroma + 128) >> 8);
	int blue = av_clip_u8((y_base + blue_chroma + 128) >> 8);

	return (uint16_t)(((unsigned int)(red & 0xf8) << 8) |
			  ((unsigned int)(green & 0xfc) << 3) |
			  ((unsigned int)blue >> 3));
}

#if AV_LCD_HAVE_NEON
static inline uint8x8_t av_neon_channel_to_u8(int32x4_t low,
					      int32x4_t high)
{
	const int32x4_t rounding = vdupq_n_s32(128);
	uint16x4_t low_u16;
	uint16x4_t high_u16;

	low_u16 = vqshrun_n_s32(vaddq_s32(low, rounding), 8);
	high_u16 = vqshrun_n_s32(vaddq_s32(high, rounding), 8);
	return vqmovn_u16(vcombine_u16(low_u16, high_u16));
}

/* Eight packed YUYV pixels become eight native RGB565 pixels. */
static inline void av_convert_eight_neon(const unsigned char *source,
					 uint16_t *destination,
					 uint8x8_t u_indices,
					 uint8x8_t v_indices)
{
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
	uint16x8_t rgb565;

	y = vmaxq_s16(vsubq_s16(y, vdupq_n_s16(16)), vdupq_n_s16(0));
	u = vsubq_s16(u, vdupq_n_s16(128));
	v = vsubq_s16(v, vdupq_n_s16(128));
	y_low = vget_low_s16(y);
	y_high = vget_high_s16(y);
	u_low = vget_low_s16(u);
	u_high = vget_high_s16(u);
	v_low = vget_low_s16(v);
	v_high = vget_high_s16(v);
	y_term_low = vmull_n_s16(y_low, 298);
	y_term_high = vmull_n_s16(y_high, 298);
	red_low = vmlal_n_s16(y_term_low, v_low, 409);
	red_high = vmlal_n_s16(y_term_high, v_high, 409);
	green_low = vmlal_n_s16(y_term_low, u_low, -100);
	green_high = vmlal_n_s16(y_term_high, u_high, -100);
	green_low = vmlal_n_s16(green_low, v_low, -208);
	green_high = vmlal_n_s16(green_high, v_high, -208);
	blue_low = vmlal_n_s16(y_term_low, u_low, 516);
	blue_high = vmlal_n_s16(y_term_high, u_high, 516);
	red = av_neon_channel_to_u8(red_low, red_high);
	green = av_neon_channel_to_u8(green_low, green_high);
	blue = av_neon_channel_to_u8(blue_low, blue_high);
	rgb565 = vshlq_n_u16(vshrq_n_u16(vmovl_u8(red), 3), 11);
	rgb565 = vorrq_u16(rgb565,
		vshlq_n_u16(vshrq_n_u16(vmovl_u8(green), 2), 5));
	rgb565 = vorrq_u16(rgb565, vshrq_n_u16(vmovl_u8(blue), 3));
	vst1q_u16(destination, rgb565);
}
#endif

static void av_convert_yuyv_to_rgb565(struct av_lcd_preview *preview)
{
	unsigned int row;
#if AV_LCD_HAVE_NEON
	static const uint8_t u_index_data[8] = { 0, 0, 2, 2, 4, 4, 6, 6 };
	static const uint8_t v_index_data[8] = { 1, 1, 3, 3, 5, 5, 7, 7 };
	const uint8x8_t u_indices = vld1_u8(u_index_data);
	const uint8x8_t v_indices = vld1_u8(v_index_data);
#endif

	for (row = 0; row < preview->video.height; ++row) {
		const unsigned char *source = preview->raw_copy +
			row * preview->video.bytesperline;
		uint16_t *destination = preview->rgb_staging +
			row * preview->video.width;
		unsigned int x = 0;

#if AV_LCD_HAVE_NEON
		for (; x + 8U <= preview->video.width; x += 8U) {
			av_convert_eight_neon(source, destination + x,
					      u_indices, v_indices);
			source += 16;
		}
#endif
		for (; x + 1U < preview->video.width; x += 2U) {
			unsigned int u = source[1];
			unsigned int v = source[3];
			int red_chroma = preview->tables.red_from_v[v];
			int green_chroma = preview->tables.green_from_u[u] +
				preview->tables.green_from_v[v];
			int blue_chroma = preview->tables.blue_from_u[u];

			destination[x] = av_rgb565_from_terms(
				preview->tables.y_base[source[0]], red_chroma,
				green_chroma, blue_chroma);
			destination[x + 1U] = av_rgb565_from_terms(
				preview->tables.y_base[source[2]], red_chroma,
				green_chroma, blue_chroma);
			source += 4;
		}
	}
}

static void av_blit_hidden_page(struct av_lcd_preview *preview,
				unsigned int page)
{
	unsigned int row;
	size_t row_bytes =
		(size_t)preview->video.width * sizeof(uint16_t);
	unsigned int destination_y = preview->destination_y +
		page * preview->fb.var.yres;

	for (row = 0; row < preview->video.height; ++row) {
		unsigned char *destination =
			(unsigned char *)preview->fb.mapping +
			(destination_y + row) * preview->fb.fix.line_length +
			preview->destination_x * sizeof(uint16_t);
		const uint16_t *source = preview->rgb_staging +
			row * preview->video.width;

		memcpy(destination, source, row_bytes);
	}
	/* Complete write-combine stores before LCDIF receives the new DMA page. */
	__sync_synchronize();
}

static void av_lcd_fail(struct av_lcd_preview *preview, int error_number)
{
	pthread_mutex_lock(&preview->lock);
	preview->stats.failed = 1;
	preview->stats.error_number = error_number ? error_number : EIO;
	pthread_mutex_unlock(&preview->lock);
}

static void av_lcd_publish_stats(struct av_lcd_preview *preview,
				 const struct av_mjpeg_raw_frame *frame,
				 uint64_t copy_us, uint64_t convert_us,
				 uint64_t blit_us, uint64_t flip_us,
				 uint64_t present_time_us)
{
	pthread_mutex_lock(&preview->lock);
	if (preview->stats.displayed_frames == 0)
		preview->stats.first_present_time_us = present_time_us;
	else if (frame->serial > preview->stats.last_source_serial + 1U)
		preview->stats.source_frames_skipped +=
			frame->serial - preview->stats.last_source_serial - 1U;
	preview->stats.displayed_frames++;
	preview->stats.raw_copy_us += copy_us;
	preview->stats.convert_us += convert_us;
	preview->stats.blit_us += blit_us;
	preview->stats.flip_us += flip_us;
	preview->stats.last_present_time_us = present_time_us;
	preview->stats.last_source_timestamp_us = frame->timestamp_us;
	preview->stats.last_source_serial = frame->serial;
	preview->stats.last_source_sequence = frame->sequence;
	preview->stats.front_page = preview->fb.front_page;
	pthread_mutex_unlock(&preview->lock);
}

static void *av_lcd_thread_main(void *argument)
{
	struct av_lcd_preview *preview = argument;
	uint64_t after_serial = 0;

	for (;;) {
		struct av_mjpeg_raw_frame frame;
		unsigned int back_page;
		uint64_t cycle_begin;
		uint64_t copied;
		uint64_t converted;
		uint64_t blitted;
		uint64_t presented;
		int result;

		cycle_begin = av_lcd_now_us();
		memset(&frame, 0, sizeof(frame));
		result = av_mjpeg_pipeline_copy_latest_raw(
			preview->pipeline, after_serial, preview->raw_copy,
			preview->video.sizeimage, &frame);
		copied = av_lcd_now_us();
		if (result == AV_MJPEG_STOPPED)
			break;
		if (result < 0) {
			av_lcd_fail(preview, -result);
			break;
		}
		if (frame.size < preview->video.sizeimage) {
			av_lcd_fail(preview, EMSGSIZE);
			break;
		}
		after_serial = frame.serial;
		av_convert_yuyv_to_rgb565(preview);
		converted = av_lcd_now_us();
		back_page = preview->fb.front_page ^ 1U;
		av_blit_hidden_page(preview, back_page);
		blitted = av_lcd_now_us();
		result = av_framebuffer_present(&preview->fb, back_page);
		presented = av_lcd_now_us();
		if (result < 0) {
			av_lcd_fail(preview, -result);
			break;
		}
		av_lcd_publish_stats(preview, &frame,
			frame.copy_us,
			converted >= copied ? converted - copied : 0,
			blitted >= converted ? blitted - converted : 0,
			presented >= blitted ? presented - blitted : 0,
			presented);
		av_lcd_wait_budget(preview, cycle_begin);
	}
	return NULL;
}

int av_lcd_preview_create(struct av_lcd_preview **preview_out,
			  struct av_mjpeg_pipeline *pipeline,
			  const char *framebuffer_device,
			  unsigned int target_fps)
{
	struct av_lcd_preview *preview;
	size_t pixel_count;
	int error;

	if (!preview_out || !pipeline || !framebuffer_device ||
	    target_fps > 1000U)
		return -EINVAL;
	*preview_out = NULL;
	preview = calloc(1, sizeof(*preview));
	if (!preview)
		return -ENOMEM;
	preview->pipeline = pipeline;
	preview->target_fps = target_fps;
	if (target_fps != 0U)
		preview->frame_interval_us = UINT64_C(1000000) / target_fps;
	preview->fb.fd = -1;
	av_mjpeg_pipeline_get_info(pipeline, &preview->video);
	if (preview->video.width == 0 || (preview->video.width & 1U) != 0 ||
	    preview->video.height == 0 ||
	    preview->video.bytesperline < preview->video.width * 2U ||
	    preview->video.sizeimage <
		preview->video.bytesperline * preview->video.height) {
		error = -EINVAL;
		goto fail;
	}
	error = pthread_mutex_init(&preview->lock, NULL);
	if (error) {
		error = -error;
		goto fail;
	}
	preview->lock_initialized = 1;
	error = av_framebuffer_open(&preview->fb, framebuffer_device);
	if (error < 0)
		goto fail;
	if (preview->video.width > preview->fb.var.xres ||
	    preview->video.height > preview->fb.var.yres) {
		error = -EINVAL;
		fprintf(stderr, "Video does not fit the visible framebuffer\n");
		goto fail;
	}
	preview->destination_x =
		(preview->fb.var.xres - preview->video.width) / 2U;
	preview->destination_y =
		(preview->fb.var.yres - preview->video.height) / 2U;
	preview->raw_copy = malloc(preview->video.sizeimage);
	pixel_count = (size_t)preview->video.width * preview->video.height;
	if (pixel_count > SIZE_MAX / sizeof(*preview->rgb_staging)) {
		error = -EOVERFLOW;
		goto fail;
	}
	preview->rgb_staging = malloc(
		pixel_count * sizeof(*preview->rgb_staging));
	if (!preview->raw_copy || !preview->rgb_staging) {
		error = -ENOMEM;
		goto fail;
	}
	av_init_yuv_tables(&preview->tables);
	av_clear_pages(&preview->fb);
	printf("LCD placement    : %ux%u at (%u,%u), shared raw producer\n",
	       preview->video.width, preview->video.height,
	       preview->destination_x, preview->destination_y);
	printf("LCD rate budget  : %u fps%s\n", target_fps,
	       target_fps ? " (latest frame)" : " (unlimited)");
#if AV_LCD_HAVE_NEON
	printf("LCD converter    : ARM NEON, 8 pixels/iteration\n");
#else
	printf("LCD converter    : scalar lookup fallback\n");
#endif
	*preview_out = preview;
	return 0;

fail:
	av_lcd_preview_destroy(preview);
	return error;
}

int av_lcd_preview_start(struct av_lcd_preview *preview)
{
	int error;

	if (!preview || preview->thread_started)
		return -EINVAL;
	error = pthread_create(&preview->thread, NULL,
			       av_lcd_thread_main, preview);
	if (error)
		return -error;
	preview->thread_started = 1;
	return 0;
}

void av_lcd_preview_get_stats(struct av_lcd_preview *preview,
			      struct av_lcd_preview_stats *stats)
{
	if (!preview || !stats)
		return;
	pthread_mutex_lock(&preview->lock);
	*stats = preview->stats;
	pthread_mutex_unlock(&preview->lock);
}

int av_lcd_preview_failed(struct av_lcd_preview *preview)
{
	int failed;

	if (!preview)
		return 1;
	pthread_mutex_lock(&preview->lock);
	failed = preview->stats.failed;
	pthread_mutex_unlock(&preview->lock);
	return failed;
}

void av_lcd_preview_join(struct av_lcd_preview *preview)
{
	if (!preview || !preview->thread_started)
		return;
	(void)pthread_join(preview->thread, NULL);
	preview->thread_started = 0;
}

void av_lcd_preview_destroy(struct av_lcd_preview *preview)
{
	if (!preview)
		return;
	av_lcd_preview_join(preview);
	av_framebuffer_close(&preview->fb);
	free(preview->rgb_staging);
	free(preview->raw_copy);
	if (preview->lock_initialized)
		(void)pthread_mutex_destroy(&preview->lock);
	free(preview);
}
