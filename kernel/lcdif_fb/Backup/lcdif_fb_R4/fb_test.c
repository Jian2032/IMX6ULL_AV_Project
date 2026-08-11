/*
 * fb_test.c - LCD-R4 user-space framebuffer test
 *
 * This program intentionally uses only Linux framebuffer ioctls, mmap and
 * libc. It does not know any LCDIF register, DMA address or device-tree
 * detail. That separation demonstrates the value of the fbdev abstraction:
 *
 *     application -> /dev/fb0 -> fbdev core -> av_lcdif_fb -> LCDIF
 *
 * LCD-R4新增两条测试路径：
 *   flip  - 在隐藏页绘制移动色块，再用FBIOPAN_DISPLAY切到该页；
 *   vsync - 连续调用FBIO_WAITFORVSYNC并计算实际同步频率。
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
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_FB_DEVICE	"/dev/fb0"
#define COLOR_BAR_COUNT		8U
#define CHECKER_SIZE		64U
#define DEFAULT_SYNC_COUNT	180U
#define MAX_SYNC_COUNT		10000U
#define MOVING_BAR_WIDTH	96U

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

/*
 * 在指定后备页绘制一帧简单动画。每帧先填充深蓝背景，再画一个移动
 * 的亮色色块。绘图始终发生在当前不可见页，完成后才执行PAN，因此
 * LCDIF不会扫描到“只画了一半”的画面。
 */
static void draw_flip_frame(void *mapping,
			    const struct fb_fix_screeninfo *fix,
			    const struct fb_var_screeninfo *var,
			    unsigned int page_yoffset,
			    unsigned int frame_number)
{
	struct fb_var_screeninfo page = *var;
	const uint16_t background = make_rgb565(8, 18, 48);
	const uint16_t bar = make_rgb565(250,
					 80U + (frame_number % 120U),
					 30);
	unsigned int travel;
	unsigned int bar_x;
	unsigned int x;
	unsigned int y;

	page.xoffset = 0;
	page.yoffset = page_yoffset;

	travel = page.xres > MOVING_BAR_WIDTH ?
		 page.xres - MOVING_BAR_WIDTH : 1U;
	bar_x = (frame_number * 13U) % travel;

	for (y = 0; y < page.yres; y++) {
		uint16_t *line = get_pixel(mapping, fix, &page, 0, y);

		for (x = 0; x < page.xres; x++)
			line[x] = background;

		/*
		 * 留出上下边框，使移动区域和整帧是否撕裂更容易观察。
		 */
		if (y >= 40U && y + 40U < page.yres) {
			for (x = bar_x;
			     x < bar_x + MOVING_BAR_WIDTH && x < page.xres;
			     x++)
				line[x] = bar;
		}
	}
}

static unsigned long elapsed_us(const struct timeval *start,
				 const struct timeval *end)
{
	long seconds = end->tv_sec - start->tv_sec;
	long microseconds = end->tv_usec - start->tv_usec;

	if (microseconds < 0) {
		seconds--;
		microseconds += 1000000L;
	}

	return (unsigned long)seconds * 1000000UL +
	       (unsigned long)microseconds;
}

/*
 * 解析flip/vsync后面的次数。设置上限是为了避免输错一个巨大数字后
 * 测试程序长时间占用终端。
 */
static int parse_count(const char *text, unsigned int *count)
{
	char *end;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno || *text == '\0' || *end != '\0' ||
	    value == 0 || value > MAX_SYNC_COUNT)
		return -1;

	*count = (unsigned int)value;
	return 0;
}

/*
 * 双缓冲动画流程：
 *
 *   当前页(front)正在被LCDIF扫描
 *       -> CPU只绘制另一页(back)
 *       -> FBIOPAN_DISPLAY(back)
 *       -> 驱动等待帧完成中断
 *       -> back成为新的front
 *
 * ioctl返回前驱动已经跨过帧边界，所以循环不会覆盖仍在扫描的页面。
 */
static int run_flip_test(int fd, void *mapping,
			 const struct fb_fix_screeninfo *fix,
			 const struct fb_var_screeninfo *var,
			 unsigned int frame_count)
{
	struct fb_var_screeninfo pan;
	struct timeval start;
	struct timeval end;
	unsigned long duration;
	unsigned int front;
	unsigned int back;
	unsigned int frame;

	if (var->yres_virtual < var->yres * 2U || fix->ypanstep == 0) {
		fprintf(stderr,
			"double buffering unavailable: virtual_y=%u "
			"visible_y=%u ypanstep=%u\n",
			var->yres_virtual, var->yres,
			(unsigned int)fix->ypanstep);
		return -1;
	}

	front = var->yoffset >= var->yres ? 1U : 0U;
	if (gettimeofday(&start, NULL) < 0) {
		fprintf(stderr, "gettimeofday failed: %s\n",
			strerror(errno));
		return -1;
	}

	for (frame = 0; frame < frame_count; frame++) {
		back = front ^ 1U;
		draw_flip_frame(mapping, fix, var,
				back * var->yres, frame);

		/*
		 * 确保编译器和CPU不把显存写入重排到ioctl之后。内核驱动
		 * 中还会执行wmb()，再把后备页地址交给LCDIF。
		 */
		__sync_synchronize();

		pan = *var;
		pan.xoffset = 0;
		pan.yoffset = back * var->yres;
		pan.activate = FB_ACTIVATE_VBL;

		if (ioctl(fd, FBIOPAN_DISPLAY, &pan) < 0) {
			fprintf(stderr,
				"FBIOPAN_DISPLAY frame %u failed: %s\n",
				frame, strerror(errno));
			return -1;
		}

		front = back;
	}

	if (gettimeofday(&end, NULL) < 0) {
		fprintf(stderr, "gettimeofday failed: %s\n",
			strerror(errno));
		return -1;
	}

	duration = elapsed_us(&start, &end);
	printf("flip test passed: frames=%u, elapsed=%.3f s, "
	       "average=%.2f flips/s, final_yoffset=%u\n",
	       frame_count, duration / 1000000.0,
	       duration ? frame_count * 1000000.0 / duration : 0.0,
	       front * var->yres);
	return 0;
}

/*
 * 标准FBIO_WAITFORVSYNC的参数是CRTC编号。本平台只有LCDIF一个输出，
 * 传0即可。连续等待N次并统计频率，可以同时验证VSYNC中断和面板实际
 * 刷新率；预期应接近当前像素时钟对应的约58.5Hz。
 */
static int run_vsync_test(int fd, unsigned int wait_count)
{
	struct timeval start;
	struct timeval end;
	unsigned long duration;
	unsigned int crtc = 0;
	unsigned int index;

	if (gettimeofday(&start, NULL) < 0) {
		fprintf(stderr, "gettimeofday failed: %s\n",
			strerror(errno));
		return -1;
	}

	for (index = 0; index < wait_count; index++) {
		if (ioctl(fd, FBIO_WAITFORVSYNC, &crtc) < 0) {
			fprintf(stderr,
				"FBIO_WAITFORVSYNC %u failed: %s\n",
				index, strerror(errno));
			return -1;
		}
	}

	if (gettimeofday(&end, NULL) < 0) {
		fprintf(stderr, "gettimeofday failed: %s\n",
			strerror(errno));
		return -1;
	}

	duration = elapsed_us(&start, &end);
	printf("vsync test passed: waits=%u, elapsed=%.3f s, "
	       "measured=%.2f Hz\n",
	       wait_count, duration / 1000000.0,
	       duration ? wait_count * 1000000.0 / duration : 0.0);
	return 0;
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
	printf("pan steps: x=%u y=%u ywrap=%u\n",
	       (unsigned int)fix->xpanstep,
	       (unsigned int)fix->ypanstep,
	       (unsigned int)fix->ywrapstep);
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
		"       %s flip [frames] [fb-device]\n"
		"       %s vsync [waits] [fb-device]\n"
		"Examples:\n"
		"  %s info\n"
		"  %s checker /dev/fb0\n"
		"  %s flip 180 /dev/fb0\n"
		"  %s vsync 180 /dev/fb0\n",
		program, program, program, program,
		program, program, program);
}

int main(int argc, char **argv)
{
	const char *command = argc > 1 ? argv[1] : "bars";
	const char *device = DEFAULT_FB_DEVICE;
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;
	void *mapping;
	size_t virtual_size;
	unsigned int sync_count = DEFAULT_SYNC_COUNT;
	int is_sync_command;
	int fd;
	int ret = EXIT_FAILURE;

	is_sync_command = strcmp(command, "flip") == 0 ||
			  strcmp(command, "vsync") == 0;

	if (is_sync_command) {
		if (argc > 4) {
			print_usage(argv[0]);
			return EXIT_FAILURE;
		}
		if (argc > 2 && parse_count(argv[2], &sync_count) < 0) {
			fprintf(stderr,
				"invalid count '%s'; expected 1..%u\n",
				argv[2], MAX_SYNC_COUNT);
			return EXIT_FAILURE;
		}
		if (argc > 3)
			device = argv[3];
	} else {
		if (argc > 3) {
			print_usage(argv[0]);
			return EXIT_FAILURE;
		}
		if (argc > 2)
			device = argv[2];
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

	if (strcmp(command, "info") == 0) {
		ret = EXIT_SUCCESS;
		goto close_fd;
	}

	if (strcmp(command, "vsync") == 0) {
		ret = run_vsync_test(fd, sync_count) == 0 ?
		      EXIT_SUCCESS : EXIT_FAILURE;
		goto close_fd;
	}

	if (var.bits_per_pixel != 16) {
		fprintf(stderr, "unsupported bpp %u; expected RGB565\n",
			var.bits_per_pixel);
		goto close_fd;
	}

	virtual_size = (size_t)fix.line_length * var.yres_virtual;
	if (virtual_size > fix.smem_len) {
		fprintf(stderr,
			"invalid framebuffer size: virtual=%lu, smem=%u\n",
			(unsigned long)virtual_size, fix.smem_len);
		goto close_fd;
	}

	mapping = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %s\n", strerror(errno));
		goto close_fd;
	}

	if (strcmp(command, "flip") == 0) {
		ret = run_flip_test(fd, mapping, &fix, &var,
				    sync_count) == 0 ?
		      EXIT_SUCCESS : EXIT_FAILURE;
		goto unmap_fb;
	} else if (strcmp(command, "bars") == 0) {
		draw_color_bars(mapping, &fix, &var);
	} else if (strcmp(command, "gradient") == 0) {
		draw_gradient(mapping, &fix, &var);
	} else if (strcmp(command, "checker") == 0) {
		draw_checker(mapping, &fix, &var);
	} else {
		fprintf(stderr, "unknown command: %s\n", command);
		print_usage(argv[0]);
		goto unmap_fb;
	}

	printf("pattern '%s' written successfully\n", command);
	ret = EXIT_SUCCESS;

unmap_fb:
	if (munmap(mapping, fix.smem_len) < 0)
		fprintf(stderr, "munmap failed: %s\n", strerror(errno));
close_fd:
	if (close(fd) < 0)
		fprintf(stderr, "close failed: %s\n", strerror(errno));
	return ret;
}
