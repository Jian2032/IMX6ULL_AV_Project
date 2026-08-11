/* SPDX-License-Identifier: MIT */
/* av_lcd_preview.h - AV-R2 LCD consumer for one shared video producer. */

#ifndef AV_LCD_PREVIEW_H
#define AV_LCD_PREVIEW_H

#include <stdint.h>

struct av_mjpeg_pipeline;
struct av_lcd_preview;

struct av_lcd_preview_stats {
	uint64_t displayed_frames;
	uint64_t source_frames_skipped;
	uint64_t raw_copy_us;
	uint64_t convert_us;
	uint64_t blit_us;
	uint64_t flip_us;
	uint64_t first_present_time_us;
	uint64_t last_present_time_us;
	uint64_t last_source_timestamp_us;
	uint64_t last_source_serial;
	uint32_t last_source_sequence;
	unsigned int front_page;
	int failed;
	int error_number;
};

/*
 * create opens/maps fbdev and allocates private YUYV/RGB565 working buffers.
 * start creates the LCD consumer thread.
 * join must run after av_mjpeg_pipeline_stop(), which wakes a blocked raw-copy
 * wait.  destroy is then non-blocking and releases fbdev plus private buffers.
 */
int av_lcd_preview_create(struct av_lcd_preview **preview_out,
			  struct av_mjpeg_pipeline *pipeline,
			  const char *framebuffer_device,
			  unsigned int target_fps);
int av_lcd_preview_start(struct av_lcd_preview *preview);
void av_lcd_preview_get_stats(struct av_lcd_preview *preview,
			      struct av_lcd_preview_stats *stats);
int av_lcd_preview_failed(struct av_lcd_preview *preview);
void av_lcd_preview_join(struct av_lcd_preview *preview);
void av_lcd_preview_destroy(struct av_lcd_preview *preview);

#endif /* AV_LCD_PREVIEW_H */
