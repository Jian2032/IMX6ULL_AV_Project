/*
 * fb_test.c - LCD-R3 user-space framebuffer test
 *
 * This program intentionally uses only Linux framebuffer ioctls, mmap and
 * libc. It does not know any LCDIF register, DMA address or device-tree
 * detail. That separation demonstrates the value of the fbdev abstraction:
 *
 *     application -> /dev/fb0 -> fbdev core -> av_lcdif_fb -> LCDIF
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define DEFAULT_FB_DEVICE	"/dev/fb0"
#define COLOR_BAR_COUNT		8U
#define CHECKER_SIZE		64U

/*
 * 把8-bit RGB分量压缩成RGB565。应用使用这个函数时不需要知道LCDIF
 * 物理总线是24位，因为驱动已经通过FBIOGET_VSCREENINFO告诉应用，
 * Framebuffer内存布局是R5/G6/B5。
 */
static uint16_t make_rgb565(unsigned int red, unsigned int green,
			    unsigned int blue)
{
	return (uint16_t)(((red & 0xf8U) << 8) |
			  ((green & 0xfcU) << 3) |
			  ((blue & 0xf8U) >> 3));
}

/*
 * 不能假设每行恰好等于xres * 2。fix.line_length由驱动给出，未来可能
 * 因硬件对齐大于可见宽度。通过它定位行首能避免跨行写错位置。
 */
static uint16_t *get_pixel(void *mapping,
			    const struct fb_fix_screeninfo *fix,
			    const struct fb_var_screeninfo *var,
			    unsigned int x, unsigned int y)
{
	uint8_t *line;

	line = (uint8_t *)mapping +
	       (y + var->yoffset) * fix->line_length;
	return (uint16_t *)(line +
		(x + var->xoffset) * sizeof(uint16_t));
}

static void draw_color_bars(void *mapping,
			    const struct fb_fix_screeninfo *fix,
			    const struct fb_var_screeninfo *var)
{
	static const uint16_t colors[COLOR_BAR_COUNT] = {
		0xffff,	/* white */
		0xffe0,	/* yellow */
		0x07ff,	/* cyan */
		0x07e0,	/* green */
		0xf81f,	/* magenta */
		0xf800,	/* red */
		0x001f,	/* blue */
		0x0000,	/* black */
	};
	unsigned int x;
	unsigned int y;

	for (y = 0; y < var->yres; y++) {
		for (x = 0; x < var->xres; x++) {
			unsigned int bar = x * COLOR_BAR_COUNT / var->xres;

			*get_pixel(mapping, fix, var, x, y) = colors[bar];
		}
	}
}

/*
 * 渐变图同时验证红、绿、蓝位域以及显存的行跨度：
 *   水平方向红色增强；
 *   垂直方向绿色增强；
 *   左上到右下蓝色增强。
 */
static void draw_gradient(void *mapping,
			  const struct fb_fix_screeninfo *fix,
			  const struct fb_var_screeninfo *var)
{
	unsigned int x;
	unsigned int y;
	unsigned int denominator = var->xres + var->yres - 2;

	for (y = 0; y < var->yres; y++) {
		for (x = 0; x < var->xres; x++) {
			unsigned int red = x * 255U / (var->xres - 1);
			unsigned int green = y * 255U / (var->yres - 1);
			unsigned int blue = (x + y) * 255U / denominator;

			*get_pixel(mapping, fix, var, x, y) =
				make_rgb565(red, green, blue);
		}
	}
}

/*
 * 棋盘图特别容易暴露line_length错误、显存越界或分辨率错误。如果每行
 * 地址计算不正确，方块边缘会错位或呈斜线。
 */
static void draw_checker(void *mapping,
			 const struct fb_fix_screeninfo *fix,
			 const struct fb_var_screeninfo *var)
{
	const uint16_t light = make_rgb565(240, 240, 240);
	const uint16_t dark = make_rgb565(20, 60, 140);
	unsigned int x;
	unsigned int y;

	for (y = 0; y < var->yres; y++) {
		for (x = 0; x < var->xres; x++) {
			unsigned int cell_x = x / CHECKER_SIZE;
			unsigned int cell_y = y / CHECKER_SIZE;
			uint16_t color = ((cell_x + cell_y) & 1U) ?
					 dark : light;

			*get_pixel(mapping, fix, var, x, y) = color;
		}
	}
}

static void print_fb_info(const char *device,
			  const struct fb_fix_screeninfo *fix,
			  const struct fb_var_screeninfo *var)
{
	printf("device: %s\n", device);
	printf("id: %s\n", fix->id);
	printf("visible: %ux%u\n", var->xres, var->yres);
	printf("virtual: %ux%u\n", var->xres_virtual, var->yres_virtual);
	printf("offset: x=%u y=%u\n", var->xoffset, var->yoffset);
	printf("bits_per_pixel: %u\n", var->bits_per_pixel);
	printf("line_length: %u bytes\n", fix->line_length);
	printf("smem_start: 0x%08lx\n", fix->smem_start);
	printf("smem_len: %u bytes\n", fix->smem_len);
	printf("red: offset=%u length=%u\n",
	       var->red.offset, var->red.length);
	printf("green: offset=%u length=%u\n",
	       var->green.offset, var->green.length);
	printf("blue: offset=%u length=%u\n",
	       var->blue.offset, var->blue.length);
	printf("pixclock: %u ps\n", var->pixclock);
	printf("timing: left=%u right=%u upper=%u lower=%u "
	       "hsync=%u vsync=%u\n",
	       var->left_margin, var->right_margin,
	       var->upper_margin, var->lower_margin,
	       var->hsync_len, var->vsync_len);
}

static void print_usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s [info|bars|gradient|checker] [fb-device]\n"
		"Examples:\n"
		"  %s info\n"
		"  %s checker /dev/fb0\n",
		program, program, program);
}

int main(int argc, char **argv)
{
	const char *pattern = argc > 1 ? argv[1] : "bars";
	const char *device = argc > 2 ? argv[2] : DEFAULT_FB_DEVICE;
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;
	void *mapping;
	size_t visible_size;
	int fd;
	int ret = EXIT_FAILURE;

	if (argc > 3) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	fd = open(device, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n",
			device, strerror(errno));
		return EXIT_FAILURE;
	}

	if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		fprintf(stderr, "FBIOGET_FSCREENINFO failed: %s\n",
			strerror(errno));
		goto close_fd;
	}

	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
		fprintf(stderr, "FBIOGET_VSCREENINFO failed: %s\n",
			strerror(errno));
		goto close_fd;
	}

	print_fb_info(device, &fix, &var);

	if (strcmp(pattern, "info") == 0) {
		ret = EXIT_SUCCESS;
		goto close_fd;
	}

	if (var.bits_per_pixel != 16) {
		fprintf(stderr, "unsupported bpp %u; expected RGB565\n",
			var.bits_per_pixel);
		goto close_fd;
	}

	visible_size = (size_t)fix.line_length * var.yres_virtual;
	if (visible_size > fix.smem_len) {
		fprintf(stderr,
			"invalid framebuffer size: visible=%lu, smem=%u\n",
			(unsigned long)visible_size, fix.smem_len);
		goto close_fd;
	}

	mapping = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %s\n", strerror(errno));
		goto close_fd;
	}

	if (strcmp(pattern, "bars") == 0) {
		draw_color_bars(mapping, &fix, &var);
	} else if (strcmp(pattern, "gradient") == 0) {
		draw_gradient(mapping, &fix, &var);
	} else if (strcmp(pattern, "checker") == 0) {
		draw_checker(mapping, &fix, &var);
	} else {
		fprintf(stderr, "unknown pattern: %s\n", pattern);
		print_usage(argv[0]);
		goto unmap_fb;
	}

	printf("pattern '%s' written successfully\n", pattern);
	ret = EXIT_SUCCESS;

unmap_fb:
	if (munmap(mapping, fix.smem_len) < 0)
		fprintf(stderr, "munmap failed: %s\n", strerror(errno));
close_fd:
	if (close(fd) < 0)
		fprintf(stderr, "close failed: %s\n", strerror(errno));
	return ret;
}

