// SPDX-License-Identifier: GPL-2.0
/*
 * av_lcdif_fb.c - i.MX6ULL LCDIF learning driver, round LCD-R3
 *
 * LCD-R3复用已经通过实机验证的R2时序、DMA和点屏代码，新增：
 *   1. 分配并初始化struct fb_info；
 *   2. 描述1024x600 RGB565固定显示模式和显存布局；
 *   3. 实现固定模式检查、伪调色板和DMA write-combine mmap；
 *   4. 调用register_framebuffer()创建/dev/fb0；
 *   5. 提供用户空间fb_test程序，通过mmap绘制测试图。
 *
 * LCD-R3仍然只有一帧显存，xoffset/yoffset固定为0。双缓冲、
 * FBIOPAN_DISPLAY和VSYNC中断属于LCD-R4。
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define AV_LCDIF_DRIVER_NAME		"av-lcdif-fb"

/*
 * i.MX LCDIF寄存器支持SET/CLR/TOG别名：
 *
 *     base + offset + 0x0：普通寄存器
 *     base + offset + 0x4：写1置位对应bit
 *     base + offset + 0x8：写1清除对应bit
 *     base + offset + 0xc：写1翻转对应bit
 *
 * 使用SET/CLR修改单个控制位，可以避免“read-modify-write”期间误改
 * 硬件自动更新的状态位。
 */
#define REG_SET				0x04
#define REG_CLR				0x08

/* i.MX6ULL使用LCDIF v4寄存器布局。 */
#define LCDC_CTRL			0x000
#define LCDC_CTRL1			0x010
#define LCDC_V4_CTRL2			0x020
#define LCDC_V4_TRANSFER_COUNT		0x030
#define LCDC_V4_CUR_BUF			0x040
#define LCDC_V4_NEXT_BUF		0x050
#define LCDC_VDCTRL0			0x070
#define LCDC_VDCTRL1			0x080
#define LCDC_VDCTRL2			0x090
#define LCDC_VDCTRL3			0x0a0
#define LCDC_VDCTRL4			0x0b0

/* LCDC_CTRL */
#define CTRL_BYPASS_COUNT		(1U << 19)
#define CTRL_DOTCLK_MODE		(1U << 17)
#define CTRL_SET_BUS_WIDTH(x)		(((x) & 0x3U) << 10)
#define CTRL_SET_WORD_LENGTH(x)		(((x) & 0x3U) << 8)
#define CTRL_MASTER			(1U << 5)
#define CTRL_RUN			(1U << 0)

/*
 * CTRL的总线宽度编码不是直接写8/16/18/24：
 *   0 -> 16-bit, 1 -> 8-bit, 2 -> 18-bit, 3 -> 24-bit
 */
#define STMLCDIF_24BIT			3U

/*
 * CTRL的word length编码0表示16-bit像素。当前Framebuffer格式固定
 * RGB565，所以一个像素占两个字节。
 */
#define STMLCDIF_WORD_LENGTH_16		0U

/* LCDC_CTRL1 */
#define CTRL1_RECOVERY_ON_UNDERFLOW	(1U << 24)
#define CTRL1_FIFO_CLEAR		(1U << 21)
#define CTRL1_SET_BYTE_PACKAGING(x)	(((x) & 0xfU) << 16)

/* LCDC_CTRL2：最多允许16个未完成的AXI读请求，提高扫描吞吐量。 */
#define CTRL2_OUTSTANDING_REQS_16	(3U << 21)

/* TRANSFER_COUNT：高16位是有效行数，低16位是每行有效像素数。 */
#define TRANSFER_COUNT_VCOUNT(x)	(((x) & 0xffffU) << 16)
#define TRANSFER_COUNT_HCOUNT(x)	((x) & 0xffffU)

/* LCDC_VDCTRL0 */
#define VDCTRL0_ENABLE_PRESENT		(1U << 28)
#define VDCTRL0_VSYNC_ACT_HIGH		(1U << 27)
#define VDCTRL0_HSYNC_ACT_HIGH		(1U << 26)
#define VDCTRL0_DOTCLK_ACT_FALLING	(1U << 25)
#define VDCTRL0_ENABLE_ACT_HIGH		(1U << 24)
#define VDCTRL0_VSYNC_PERIOD_UNIT	(1U << 21)
#define VDCTRL0_VSYNC_PULSE_WIDTH_UNIT	(1U << 20)
#define VDCTRL0_VSYNC_PULSE_WIDTH(x)	((x) & 0x3ffffU)

/* LCDC_VDCTRL2，LCDIF v4的HSYNC宽度位于[31:18]。 */
#define VDCTRL2_HSYNC_PULSE_WIDTH(x)	(((x) & 0x3fffU) << 18)
#define VDCTRL2_HSYNC_PERIOD(x)		((x) & 0x3ffffU)

/* LCDC_VDCTRL3 */
#define VDCTRL3_HORIZONTAL_WAIT(x)	(((x) & 0xfffU) << 16)
#define VDCTRL3_VERTICAL_WAIT(x)	((x) & 0xffffU)

/* LCDC_VDCTRL4 */
#define VDCTRL4_SYNC_SIGNALS_ON		(1U << 18)
#define VDCTRL4_VALID_DATA_COUNT(x)	((x) & 0x3ffffU)

#define AV_LCDIF_BYTES_PER_PIXEL	2U
#define AV_LCDIF_COLOR_BAR_COUNT	8U
#define AV_LCDIF_STOP_RETRIES		1000U

/*
 * 从设备树读取的面板时序。
 *
 * active：真正显示图像的像素或行。
 * front porch：有效区结束到同步脉冲开始之间的空白区。
 * sync len：HSYNC/VSYNC脉冲宽度。
 * back porch：同步脉冲结束到下一有效区开始之间的空白区。
 *
 * 一行总时钟数：
 *   hactive + hfront_porch + hsync_len + hback_porch
 *
 * 一帧总行数：
 *   vactive + vfront_porch + vsync_len + vback_porch
 */
struct av_lcdif_timing {
	u32 pixelclock;

	u32 hactive;
	u32 hfront_porch;
	u32 hback_porch;
	u32 hsync_len;

	u32 vactive;
	u32 vfront_porch;
	u32 vback_porch;
	u32 vsync_len;

	u32 hsync_active;
	u32 vsync_active;
	u32 de_active;
	u32 pixelclk_active;
};

/*
 * 每个LCDIF硬件实例对应一个结构体。
 *
 * fb_virt：
 *   CPU在内核中填充颜色条所使用的虚拟地址。
 * fb_dma：
 *   LCDIF DMA读取显存使用的总线地址，写入NEXT_BUF寄存器。
 * fb_size：
 *   当前只申请一帧：1024 * 600 * 2 = 1,228,800 bytes。
 *
 * clocks_enabled/controller_enabled：
 *   记录资源当前状态，保证probe错误路径和remove只释放已经成功取得
 *   的资源，并严格遵循“先停DMA，再关时钟，最后释放显存”的顺序。
 */
struct av_lcdif {
	struct device *dev;
	void __iomem *regs;
	int irq;

	struct clk *clk_pix;
	struct clk *clk_axi;
	struct clk *clk_disp_axi;

	u32 bus_width;
	u32 bits_per_pixel;
	struct av_lcdif_timing timing;

	void *fb_virt;
	dma_addr_t fb_dma;
	size_t fb_size;
	struct fb_info *fb_info;
	u32 pseudo_palette[16];

	bool clk_pix_enabled;
	bool clk_axi_enabled;
	bool clk_disp_axi_enabled;
	bool controller_enabled;
	bool framebuffer_registered;
};

/*
 * 当前板级DTS的时序属性都是单个u32。把错误日志集中在这个函数中，
 * 可以在设备树缺少属性时明确指出具体名字。
 */
static int av_lcdif_read_u32(struct device *dev, struct device_node *np,
			     const char *property, u32 *value)
{
	int ret;

	ret = of_property_read_u32(np, property, value);
	if (ret)
		dev_err(dev, "timing node %s has no valid %s\n",
			np->full_name, property);

	return ret;
}

/*
 * 解析关系：
 *
 * lcdif node --display phandle--> display0
 * display0/display-timings --native-mode phandle--> timing0
 *
 * 不按“display”或“timing0”的字符串直接搜索最终节点，从而尊重DTS
 * 中明确指定的引用关系。
 */
static int av_lcdif_parse_display(struct platform_device *pdev,
				   struct av_lcdif *lcdif)
{
	struct av_lcdif_timing *t = &lcdif->timing;
	struct device_node *display_np = NULL;
	struct device_node *timings_np = NULL;
	struct device_node *timing_np = NULL;
	u32 htotal;
	u32 vtotal;
	u32 refresh_hz;
	int ret;

	display_np = of_parse_phandle(pdev->dev.of_node, "display", 0);
	if (!display_np) {
		dev_err(&pdev->dev, "missing display phandle\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(display_np, "bus-width",
				   &lcdif->bus_width);
	if (ret) {
		dev_err(&pdev->dev, "display node has no bus-width\n");
		goto out_put_nodes;
	}

	ret = of_property_read_u32(display_np, "bits-per-pixel",
				   &lcdif->bits_per_pixel);
	if (ret) {
		dev_err(&pdev->dev, "display node has no bits-per-pixel\n");
		goto out_put_nodes;
	}

	if (lcdif->bus_width != 24 || lcdif->bits_per_pixel != 16) {
		dev_err(&pdev->dev,
			"unsupported panel: bus-width=%u, bpp=%u\n",
			lcdif->bus_width, lcdif->bits_per_pixel);
		ret = -EINVAL;
		goto out_put_nodes;
	}

	timings_np = of_get_child_by_name(display_np, "display-timings");
	if (!timings_np) {
		dev_err(&pdev->dev, "display node has no display-timings\n");
		ret = -EINVAL;
		goto out_put_nodes;
	}

	timing_np = of_parse_phandle(timings_np, "native-mode", 0);
	if (!timing_np) {
		/*
		 * 当前DTS存在native-mode。保留“第一个子节点”回退路径，
		 * 便于以后使用只有一个timing但没有native-mode的面板。
		 */
		timing_np = of_get_next_child(timings_np, NULL);
	}
	if (!timing_np) {
		dev_err(&pdev->dev, "display-timings has no timing entry\n");
		ret = -EINVAL;
		goto out_put_nodes;
	}

#define READ_TIMING(_name, _member)					\
	do {								\
		ret = av_lcdif_read_u32(&pdev->dev, timing_np,		\
					 _name, &t->_member);		\
		if (ret)						\
			goto out_put_nodes;				\
	} while (0)

	READ_TIMING("clock-frequency", pixelclock);
	READ_TIMING("hactive", hactive);
	READ_TIMING("hfront-porch", hfront_porch);
	READ_TIMING("hback-porch", hback_porch);
	READ_TIMING("hsync-len", hsync_len);
	READ_TIMING("vactive", vactive);
	READ_TIMING("vfront-porch", vfront_porch);
	READ_TIMING("vback-porch", vback_porch);
	READ_TIMING("vsync-len", vsync_len);
	READ_TIMING("hsync-active", hsync_active);
	READ_TIMING("vsync-active", vsync_active);
	READ_TIMING("de-active", de_active);
	READ_TIMING("pixelclk-active", pixelclk_active);

#undef READ_TIMING

	/*
	 * R2只接受已经确认的1024x600面板。以后支持其他面板时，应改为
	 * 控制器能力范围检查，而不是保留这个固定分辨率判断。
	 */
	if (t->hactive != 1024 || t->vactive != 600) {
		dev_err(&pdev->dev, "unexpected resolution %ux%u\n",
			t->hactive, t->vactive);
		ret = -EINVAL;
		goto out_put_nodes;
	}

	if (!t->pixelclock || !t->hsync_len || !t->vsync_len) {
		dev_err(&pdev->dev, "invalid zero value in display timing\n");
		ret = -EINVAL;
		goto out_put_nodes;
	}

	if (t->hsync_active > 1 || t->vsync_active > 1 ||
	    t->de_active > 1 || t->pixelclk_active > 1) {
		dev_err(&pdev->dev, "display polarity must be 0 or 1\n");
		ret = -EINVAL;
		goto out_put_nodes;
	}

	htotal = t->hactive + t->hfront_porch +
		 t->hsync_len + t->hback_porch;
	vtotal = t->vactive + t->vfront_porch +
		 t->vsync_len + t->vback_porch;
	refresh_hz = t->pixelclock / (htotal * vtotal);

	dev_info(&pdev->dev,
		 "timing %s: %ux%u, pixelclock=%u Hz, refresh~%u Hz\n",
		 timing_np->full_name, t->hactive, t->vactive,
		 t->pixelclock, refresh_hz);
	dev_info(&pdev->dev,
		 "horizontal: active=%u front=%u sync=%u back=%u total=%u\n",
		 t->hactive, t->hfront_porch, t->hsync_len,
		 t->hback_porch, htotal);
	dev_info(&pdev->dev,
		 "vertical: active=%u front=%u sync=%u back=%u total=%u\n",
		 t->vactive, t->vfront_porch, t->vsync_len,
		 t->vback_porch, vtotal);
	dev_info(&pdev->dev,
		 "polarity: hsync=%u vsync=%u de=%u pixelclk=%u\n",
		 t->hsync_active, t->vsync_active,
		 t->de_active, t->pixelclk_active);

	ret = 0;

out_put_nodes:
	of_node_put(timing_np);
	of_node_put(timings_np);
	of_node_put(display_np);
	return ret;
}

/*
 * 使能时钟前先设置像素时钟频率。旧版NXP驱动也要求在pix clock关闭
 * 状态下调用clk_set_rate()，否则时钟树可能拒绝改频。
 */
static int av_lcdif_enable_clocks(struct av_lcdif *lcdif)
{
	unsigned long requested = lcdif->timing.pixelclock;
	unsigned long actual;
	unsigned long difference;
	int ret;

	ret = clk_set_rate(lcdif->clk_pix, requested);
	if (ret) {
		dev_err(lcdif->dev, "failed to set pix clock to %lu Hz: %d\n",
			requested, ret);
		return ret;
	}

	ret = clk_prepare_enable(lcdif->clk_axi);
	if (ret) {
		dev_err(lcdif->dev, "failed to enable axi clock: %d\n", ret);
		return ret;
	}
	lcdif->clk_axi_enabled = true;

	ret = clk_prepare_enable(lcdif->clk_disp_axi);
	if (ret) {
		dev_err(lcdif->dev,
			"failed to enable disp_axi clock: %d\n", ret);
		goto disable_axi;
	}
	lcdif->clk_disp_axi_enabled = true;

	ret = clk_prepare_enable(lcdif->clk_pix);
	if (ret) {
		dev_err(lcdif->dev, "failed to enable pix clock: %d\n", ret);
		goto disable_disp_axi;
	}
	lcdif->clk_pix_enabled = true;

	actual = clk_get_rate(lcdif->clk_pix);
	difference = actual > requested ? actual - requested :
		     requested - actual;

	dev_info(lcdif->dev,
		 "clock rates: requested pix=%lu Hz, actual pix=%lu Hz, "
		 "axi=%lu Hz, disp_axi=%lu Hz\n",
		 requested, actual, clk_get_rate(lcdif->clk_axi),
		 clk_get_rate(lcdif->clk_disp_axi));

	/* 允许clock framework产生小量舍入，超过1%则明确警告。 */
	if (difference > requested / 100)
		dev_warn(lcdif->dev,
			 "actual pixel clock differs from request by >1%%\n");

	return 0;

disable_disp_axi:
	clk_disable_unprepare(lcdif->clk_disp_axi);
	lcdif->clk_disp_axi_enabled = false;
disable_axi:
	clk_disable_unprepare(lcdif->clk_axi);
	lcdif->clk_axi_enabled = false;
	return ret;
}

static void av_lcdif_disable_clocks(struct av_lcdif *lcdif)
{
	if (lcdif->clk_pix_enabled) {
		clk_disable_unprepare(lcdif->clk_pix);
		lcdif->clk_pix_enabled = false;
	}

	if (lcdif->clk_disp_axi_enabled) {
		clk_disable_unprepare(lcdif->clk_disp_axi);
		lcdif->clk_disp_axi_enabled = false;
	}

	if (lcdif->clk_axi_enabled) {
		clk_disable_unprepare(lcdif->clk_axi);
		lcdif->clk_axi_enabled = false;
	}
}

/*
 * RGB565布局：
 *
 *   bit[15:11] Red
 *   bit[10:5]  Green
 *   bit[4:0]   Blue
 *
 * 8条颜色从左到右是白、黄、青、绿、品红、红、蓝、黑。若颜色顺序
 * 或分量明显错误，后续应检查像素位域、字节序和LCD数据线连接。
 */
static void av_lcdif_fill_color_bars(struct av_lcdif *lcdif)
{
	static const u16 colors[AV_LCDIF_COLOR_BAR_COUNT] = {
		0xffff,	/* white */
		0xffe0,	/* yellow */
		0x07ff,	/* cyan */
		0x07e0,	/* green */
		0xf81f,	/* magenta */
		0xf800,	/* red */
		0x001f,	/* blue */
		0x0000,	/* black */
	};
	const u32 width = lcdif->timing.hactive;
	const u32 height = lcdif->timing.vactive;
	u16 *pixels = lcdif->fb_virt;
	u32 x;
	u32 y;

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			u32 bar = (x * AV_LCDIF_COLOR_BAR_COUNT) / width;

			pixels[y * width + x] = colors[bar];
		}
	}

	/*
	 * dma_alloc_writecombine()返回的内存对设备是DMA一致的。wmb()确保
	 * CPU填色写入在启动LCDIF DMA之前已经对外可见。
	 */
	wmb();
}

static int av_lcdif_alloc_framebuffer(struct av_lcdif *lcdif)
{
	lcdif->fb_size = lcdif->timing.hactive *
			 lcdif->timing.vactive *
			 AV_LCDIF_BYTES_PER_PIXEL;

	lcdif->fb_virt = dma_alloc_writecombine(lcdif->dev,
						lcdif->fb_size,
						&lcdif->fb_dma,
						GFP_KERNEL | GFP_DMA);
	if (!lcdif->fb_virt) {
		dev_err(lcdif->dev,
			"failed to allocate %zu-byte DMA framebuffer\n",
			lcdif->fb_size);
		return -ENOMEM;
	}

	dev_info(lcdif->dev,
		 "DMA framebuffer: cpu=%p, dma=0x%08llx, size=%zu bytes\n",
		 lcdif->fb_virt, (unsigned long long)lcdif->fb_dma,
		 lcdif->fb_size);

	av_lcdif_fill_color_bars(lcdif);
	return 0;
}

static void av_lcdif_free_framebuffer(struct av_lcdif *lcdif)
{
	if (!lcdif->fb_virt)
		return;

	dma_free_writecombine(lcdif->dev, lcdif->fb_size,
			      lcdif->fb_virt, lcdif->fb_dma);
	lcdif->fb_virt = NULL;
	lcdif->fb_dma = (dma_addr_t)0;
	lcdif->fb_size = 0;
}

/*
 * fbdev的颜色参数使用16-bit无符号范围0x0000～0xffff，而RGB565显存
 * 只保留5/6/5位。该函数按fb_bitfield描述完成截断和移位。
 */
static u32 av_lcdif_chan_to_field(u32 channel,
				   const struct fb_bitfield *field)
{
	channel &= 0xffffU;
	channel >>= 16 - field->length;
	return channel << field->offset;
}

/*
 * R3只公开一个固定模式，不允许应用改变分辨率、虚拟分辨率、像素格式
 * 或显示偏移。这样用户空间写越界和意外重配LCDIF都能在进入硬件前被
 * 拒绝。R4增加第二帧后，yres_virtual和yoffset才会放开。
 */
static int av_lcdif_fb_check_var(struct fb_var_screeninfo *var,
				  struct fb_info *info)
{
	struct av_lcdif *lcdif = info->par;

	if (var->xres != lcdif->timing.hactive ||
	    var->yres != lcdif->timing.vactive ||
	    var->xres_virtual != lcdif->timing.hactive ||
	    var->yres_virtual != lcdif->timing.vactive ||
	    var->bits_per_pixel != 16 ||
	    var->xoffset != 0 || var->yoffset != 0)
		return -EINVAL;

	/* 固定为RGB565：R5、G6、B5，没有透明通道。 */
	var->red.offset = 11;
	var->red.length = 5;
	var->red.msb_right = 0;
	var->green.offset = 5;
	var->green.length = 6;
	var->green.msb_right = 0;
	var->blue.offset = 0;
	var->blue.length = 5;
	var->blue.msb_right = 0;
	var->transp.offset = 0;
	var->transp.length = 0;
	var->transp.msb_right = 0;

	return 0;
}

/*
 * 当前显示模式在probe时已经写入LCDIF，R3不允许动态改模式。因此
 * set_par只再次验证info->var并维护line_length，不重启控制器。
 */
static int av_lcdif_fb_set_par(struct fb_info *info)
{
	int ret;

	ret = av_lcdif_fb_check_var(&info->var, info);
	if (ret)
		return ret;

	info->fix.line_length = info->var.xres *
				(info->var.bits_per_pixel / 8);
	return 0;
}

/*
 * TrueColor模式没有硬件调色板，但fbcon等fbdev用户仍会设置前16个
 * “颜色寄存器”。驱动把计算结果保存到pseudo_palette，cfb_fillrect
 * 和cfb_imageblit会使用这16个值。
 */
static int av_lcdif_fb_setcolreg(unsigned int regno, unsigned int red,
				  unsigned int green, unsigned int blue,
				  unsigned int transp,
				  struct fb_info *info)
{
	u32 value;
	u32 *palette = info->pseudo_palette;

	if (regno >= 16)
		return -EINVAL;

	if (info->var.grayscale)
		red = green = blue =
			(19595U * red + 38470U * green + 7471U * blue) >> 16;

	value = av_lcdif_chan_to_field(red, &info->var.red);
	value |= av_lcdif_chan_to_field(green, &info->var.green);
	value |= av_lcdif_chan_to_field(blue, &info->var.blue);
	value |= av_lcdif_chan_to_field(transp, &info->var.transp);
	palette[regno] = value;

	return 0;
}

/*
 * 用户程序调用mmap(/dev/fb0)时，不能使用普通匿名内存映射。这里必须
 * 以与dma_alloc_writecombine()一致的write-combine属性，把同一组
 * 物理页映射到用户进程。否则CPU缓存属性不一致可能导致画面不更新。
 */
static int av_lcdif_fb_mmap(struct fb_info *info,
			    struct vm_area_struct *vma)
{
	struct av_lcdif *lcdif = info->par;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long length = vma->vm_end - vma->vm_start;

	if (offset >= lcdif->fb_size ||
	    length > lcdif->fb_size - offset)
		return -EINVAL;

	return dma_mmap_writecombine(lcdif->dev, vma,
				     lcdif->fb_virt, lcdif->fb_dma,
				     lcdif->fb_size);
}

static struct fb_ops av_lcdif_fb_ops = {
	.owner = THIS_MODULE,
	.fb_check_var = av_lcdif_fb_check_var,
	.fb_set_par = av_lcdif_fb_set_par,
	.fb_setcolreg = av_lcdif_fb_setcolreg,
	.fb_mmap = av_lcdif_fb_mmap,
	.fb_fillrect = cfb_fillrect,
	.fb_copyarea = cfb_copyarea,
	.fb_imageblit = cfb_imageblit,
};

/*
 * 将R2已经工作的一帧DMA显存注册为Linux framebuffer。
 *
 * fix描述不随模式改变的硬件事实，例如显存地址和每行字节数。
 * var描述当前可见模式，例如分辨率、porch和RGB位域。
 */
static int av_lcdif_register_framebuffer(struct av_lcdif *lcdif)
{
	const struct av_lcdif_timing *t = &lcdif->timing;
	struct fb_info *info;
	int ret;

	info = framebuffer_alloc(0, lcdif->dev);
	if (!info)
		return -ENOMEM;

	lcdif->fb_info = info;
	info->par = lcdif;
	info->fbops = &av_lcdif_fb_ops;
	info->flags = FBINFO_FLAG_DEFAULT | FBINFO_READS_FAST;
	info->pseudo_palette = lcdif->pseudo_palette;
	info->screen_base = (char __iomem *)lcdif->fb_virt;
	info->screen_size = lcdif->fb_size;
	INIT_LIST_HEAD(&info->modelist);

	snprintf(info->fix.id, sizeof(info->fix.id), "av-lcdif");
	info->fix.smem_start = (unsigned long)lcdif->fb_dma;
	info->fix.smem_len = lcdif->fb_size;
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.type_aux = 0;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.xpanstep = 0;
	info->fix.ypanstep = 0;
	info->fix.ywrapstep = 0;
	info->fix.line_length = t->hactive * AV_LCDIF_BYTES_PER_PIXEL;
	info->fix.accel = FB_ACCEL_NONE;

	info->var.xres = t->hactive;
	info->var.yres = t->vactive;
	info->var.xres_virtual = t->hactive;
	info->var.yres_virtual = t->vactive;
	info->var.xoffset = 0;
	info->var.yoffset = 0;
	info->var.bits_per_pixel = 16;
	info->var.grayscale = 0;

	info->var.red.offset = 11;
	info->var.red.length = 5;
	info->var.green.offset = 5;
	info->var.green.length = 6;
	info->var.blue.offset = 0;
	info->var.blue.length = 5;
	info->var.transp.offset = 0;
	info->var.transp.length = 0;

	/* fbdev中的pixclock单位是皮秒，而DTS使用Hz。 */
	info->var.pixclock = KHZ2PICOS(t->pixelclock / 1000U);
	info->var.left_margin = t->hback_porch;
	info->var.right_margin = t->hfront_porch;
	info->var.upper_margin = t->vback_porch;
	info->var.lower_margin = t->vfront_porch;
	info->var.hsync_len = t->hsync_len;
	info->var.vsync_len = t->vsync_len;
	info->var.sync = 0;
	if (t->hsync_active)
		info->var.sync |= FB_SYNC_HOR_HIGH_ACT;
	if (t->vsync_active)
		info->var.sync |= FB_SYNC_VERT_HIGH_ACT;
	info->var.vmode = FB_VMODE_NONINTERLACED;
	info->var.activate = FB_ACTIVATE_NOW;
	info->var.height = -1;
	info->var.width = -1;

	ret = av_lcdif_fb_check_var(&info->var, info);
	if (ret) {
		dev_err(lcdif->dev,
			"internal framebuffer mode validation failed: %d\n",
			ret);
		goto release_info;
	}

	ret = register_framebuffer(info);
	if (ret) {
		dev_err(lcdif->dev,
			"failed to register framebuffer: %d\n", ret);
		goto release_info;
	}

	lcdif->framebuffer_registered = true;
	dev_info(lcdif->dev,
		 "registered /dev/fb%d: %ux%u RGB565, line=%u, memory=%u\n",
		 info->node, info->var.xres, info->var.yres,
		 info->fix.line_length, info->fix.smem_len);
	return 0;

release_info:
	framebuffer_release(info);
	lcdif->fb_info = NULL;
	return ret;
}

static void av_lcdif_unregister_framebuffer(struct av_lcdif *lcdif)
{
	if (!lcdif->fb_info)
		return;

	if (lcdif->framebuffer_registered) {
		unregister_framebuffer(lcdif->fb_info);
		lcdif->framebuffer_registered = false;
	}

	framebuffer_release(lcdif->fb_info);
	lcdif->fb_info = NULL;
}

/*
 * 把设备树时序转换成LCDIF v4寄存器值。
 *
 * 所有寄存器访问都必须发生在axi/disp_axi/pix时钟打开之后。NXP同代
 * 驱动明确指出：在部分SoC上关闭pixel clock后访问LCDIF寄存器可能
 * 导致总线挂起。
 */
static int av_lcdif_start_controller(struct av_lcdif *lcdif)
{
	const struct av_lcdif_timing *t = &lcdif->timing;
	u32 ctrl;
	u32 vdctrl0;
	u32 vdctrl4;
	u32 htotal;
	u32 vtotal;
	u32 run_state;

	htotal = t->hactive + t->hfront_porch +
		 t->hsync_len + t->hback_porch;
	vtotal = t->vactive + t->vfront_porch +
		 t->vsync_len + t->vback_porch;

	/*
	 * 先停止并清空控制寄存器，避免继承U-Boot可能留下的像素格式、
	 * DMA地址或运行状态。本项目已经先准备好新显存，因此屏幕可能在
	 * 这里短暂闪烁，随后切换到颜色条。
	 */
	writel(0, lcdif->regs + LCDC_CTRL);
	writel(0, lcdif->regs + LCDC_CTRL1);
	writel(0, lcdif->regs + LCDC_V4_CTRL2);

	/* 清理FIFO，再选择RGB565的16-bit字节打包。 */
	writel(CTRL1_FIFO_CLEAR,
	       lcdif->regs + LCDC_CTRL1 + REG_SET);
	writel(CTRL1_SET_BYTE_PACKAGING(0xf),
	       lcdif->regs + LCDC_CTRL1);

	ctrl = CTRL_BYPASS_COUNT |
	       CTRL_MASTER |
	       CTRL_SET_BUS_WIDTH(STMLCDIF_24BIT) |
	       CTRL_SET_WORD_LENGTH(STMLCDIF_WORD_LENGTH_16);
	writel(ctrl, lcdif->regs + LCDC_CTRL);

	writel(TRANSFER_COUNT_VCOUNT(t->vactive) |
	       TRANSFER_COUNT_HCOUNT(t->hactive),
	       lcdif->regs + LCDC_V4_TRANSFER_COUNT);

	vdctrl0 = VDCTRL0_ENABLE_PRESENT |
		  VDCTRL0_VSYNC_PERIOD_UNIT |
		  VDCTRL0_VSYNC_PULSE_WIDTH_UNIT |
		  VDCTRL0_VSYNC_PULSE_WIDTH(t->vsync_len);

	if (t->hsync_active)
		vdctrl0 |= VDCTRL0_HSYNC_ACT_HIGH;
	if (t->vsync_active)
		vdctrl0 |= VDCTRL0_VSYNC_ACT_HIGH;
	if (t->de_active)
		vdctrl0 |= VDCTRL0_ENABLE_ACT_HIGH;
	if (!t->pixelclk_active)
		vdctrl0 |= VDCTRL0_DOTCLK_ACT_FALLING;

	writel(vdctrl0, lcdif->regs + LCDC_VDCTRL0);
	writel(vtotal, lcdif->regs + LCDC_VDCTRL1);

	writel(VDCTRL2_HSYNC_PULSE_WIDTH(t->hsync_len) |
	       VDCTRL2_HSYNC_PERIOD(htotal),
	       lcdif->regs + LCDC_VDCTRL2);

	writel(VDCTRL3_HORIZONTAL_WAIT(t->hback_porch + t->hsync_len) |
	       VDCTRL3_VERTICAL_WAIT(t->vback_porch + t->vsync_len),
	       lcdif->regs + LCDC_VDCTRL3);

	vdctrl4 = VDCTRL4_VALID_DATA_COUNT(t->hactive);
	writel(vdctrl4, lcdif->regs + LCDC_VDCTRL4);

	/*
	 * i.MX6ULL是32-bit SoC，当前DMA地址落在32-bit物理地址空间。
	 * LCDIF从NEXT_BUF装载第一帧地址，启动后硬件会更新CUR_BUF。
	 */
	writel((u32)lcdif->fb_dma,
	       lcdif->regs + LCDC_V4_NEXT_BUF);

	/* 提高AXI读取并发数，减少高分辨率扫描时的FIFO underflow。 */
	writel(CTRL2_OUTSTANDING_REQS_16,
	       lcdif->regs + LCDC_V4_CTRL2 + REG_SET);

	/* 先开启DOTCLK和同步输出，最后置RUN启动DMA扫描。 */
	writel(CTRL_DOTCLK_MODE,
	       lcdif->regs + LCDC_CTRL + REG_SET);

	vdctrl4 = readl(lcdif->regs + LCDC_VDCTRL4);
	vdctrl4 |= VDCTRL4_SYNC_SIGNALS_ON;
	writel(vdctrl4, lcdif->regs + LCDC_VDCTRL4);

	writel(CTRL_MASTER,
	       lcdif->regs + LCDC_CTRL + REG_SET);
	writel(CTRL_RUN,
	       lcdif->regs + LCDC_CTRL + REG_SET);
	writel(CTRL1_RECOVERY_ON_UNDERFLOW,
	       lcdif->regs + LCDC_CTRL1 + REG_SET);

	lcdif->controller_enabled = true;

	/* 等待约3帧，再检查RUN和硬件当前扫描地址。 */
	msleep(50);
	run_state = readl(lcdif->regs + LCDC_CTRL);

	dev_info(lcdif->dev,
		 "LCDIF state: CTRL=0x%08x CUR_BUF=0x%08x NEXT_BUF=0x%08x\n",
		 run_state,
		 readl(lcdif->regs + LCDC_V4_CUR_BUF),
		 readl(lcdif->regs + LCDC_V4_NEXT_BUF));

	if (!(run_state & CTRL_RUN)) {
		dev_err(lcdif->dev, "LCDIF RUN bit did not stay set\n");
		return -EIO;
	}

	return 0;
}

/*
 * 停止顺序参考硬件扫描特点：
 *   1. 清DOTCLK_MODE，请求控制器在FIFO排空后停止；
 *   2. 等待RUN自动清零；
 *   3. 超时也强制清RUN和MASTER，确保不再读取显存；
 *   4. 关闭同步信号。
 *
 * 必须完成这些步骤后才能释放fb_dma，否则LCDIF可能继续读取已经归还
 * 给内存管理器的物理页，造成花屏甚至破坏其他数据。
 */
static void av_lcdif_stop_controller(struct av_lcdif *lcdif)
{
	u32 vdctrl4;
	u32 retry;

	if (!lcdif->controller_enabled)
		return;

	writel(CTRL_DOTCLK_MODE,
	       lcdif->regs + LCDC_CTRL + REG_CLR);

	for (retry = 0; retry < AV_LCDIF_STOP_RETRIES; retry++) {
		if (!(readl(lcdif->regs + LCDC_CTRL) & CTRL_RUN))
			break;
		udelay(1);
	}

	if (retry == AV_LCDIF_STOP_RETRIES)
		dev_warn(lcdif->dev,
			 "timeout waiting for LCDIF RUN to clear\n");

	writel(CTRL_RUN | CTRL_MASTER,
	       lcdif->regs + LCDC_CTRL + REG_CLR);

	vdctrl4 = readl(lcdif->regs + LCDC_VDCTRL4);
	vdctrl4 &= ~VDCTRL4_SYNC_SIGNALS_ON;
	writel(vdctrl4, lcdif->regs + LCDC_VDCTRL4);

	lcdif->controller_enabled = false;
	dev_info(lcdif->dev, "LCDIF controller stopped\n");
}

static int av_lcdif_probe(struct platform_device *pdev)
{
	struct av_lcdif *lcdif;
	struct resource *res;
	struct pinctrl *pinctrl;
	int ret;

	dev_info(&pdev->dev, "LCD-R3 probe begin\n");

	lcdif = devm_kzalloc(&pdev->dev, sizeof(*lcdif), GFP_KERNEL);
	if (!lcdif)
		return -ENOMEM;

	lcdif->dev = &pdev->dev;
	platform_set_drvdata(pdev, lcdif);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "missing LCDIF register resource\n");
		return -ENODEV;
	}

	lcdif->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(lcdif->regs)) {
		ret = PTR_ERR(lcdif->regs);
		dev_err(&pdev->dev, "failed to map registers: %d\n", ret);
		return ret;
	}

	lcdif->irq = platform_get_irq(pdev, 0);
	if (lcdif->irq < 0) {
		dev_err(&pdev->dev, "missing LCDIF irq: %d\n", lcdif->irq);
		return lcdif->irq;
	}

	pinctrl = devm_pinctrl_get_select_default(&pdev->dev);
	if (IS_ERR(pinctrl)) {
		ret = PTR_ERR(pinctrl);
		dev_err(&pdev->dev,
			"failed to select default pinctrl state: %d\n", ret);
		return ret;
	}

	lcdif->clk_pix = devm_clk_get(&pdev->dev, "pix");
	if (IS_ERR(lcdif->clk_pix)) {
		ret = PTR_ERR(lcdif->clk_pix);
		dev_err(&pdev->dev, "failed to get pix clock: %d\n", ret);
		return ret;
	}

	lcdif->clk_axi = devm_clk_get(&pdev->dev, "axi");
	if (IS_ERR(lcdif->clk_axi)) {
		ret = PTR_ERR(lcdif->clk_axi);
		dev_err(&pdev->dev, "failed to get axi clock: %d\n", ret);
		return ret;
	}

	lcdif->clk_disp_axi = devm_clk_get(&pdev->dev, "disp_axi");
	if (IS_ERR(lcdif->clk_disp_axi)) {
		ret = PTR_ERR(lcdif->clk_disp_axi);
		dev_err(&pdev->dev,
			"failed to get disp_axi clock: %d\n", ret);
		return ret;
	}

	ret = av_lcdif_parse_display(pdev, lcdif);
	if (ret)
		return ret;

	ret = av_lcdif_alloc_framebuffer(lcdif);
	if (ret)
		return ret;

	ret = av_lcdif_enable_clocks(lcdif);
	if (ret)
		goto free_framebuffer;

	ret = av_lcdif_start_controller(lcdif);
	if (ret)
		goto stop_controller;

	ret = av_lcdif_register_framebuffer(lcdif);
	if (ret)
		goto stop_controller;

	dev_info(&pdev->dev,
		 "LCD-R3 ready: framebuffer is available to user space\n");
	return 0;

stop_controller:
	av_lcdif_stop_controller(lcdif);
	av_lcdif_disable_clocks(lcdif);
free_framebuffer:
	av_lcdif_free_framebuffer(lcdif);
	return ret;
}

static int av_lcdif_remove(struct platform_device *pdev)
{
	struct av_lcdif *lcdif = platform_get_drvdata(pdev);

	/*
	 * 先从fbdev core注销，阻止新用户打开设备；然后停止DMA、关闭
	 * 寄存器访问所需的时钟，最后释放Framebuffer物理页。
	 */
	av_lcdif_unregister_framebuffer(lcdif);
	av_lcdif_stop_controller(lcdif);
	av_lcdif_disable_clocks(lcdif);
	av_lcdif_free_framebuffer(lcdif);

	platform_set_drvdata(pdev, NULL);
	dev_info(&pdev->dev, "LCD-R3 remove complete\n");
	return 0;
}

static void av_lcdif_shutdown(struct platform_device *pdev)
{
	struct av_lcdif *lcdif = platform_get_drvdata(pdev);

	/*
	 * reboot/poweroff时至少停止LCDIF，避免控制器在BootROM重新采样
	 * 启动引脚期间继续访问旧显存。正常模块卸载仍走remove完整释放。
	 */
	av_lcdif_stop_controller(lcdif);
}

static const struct of_device_id av_lcdif_of_match[] = {
	{ .compatible = "fsl,imx6ul-lcdif" },
	{ .compatible = "fsl,imx28-lcdif" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, av_lcdif_of_match);

static struct platform_driver av_lcdif_driver = {
	.probe = av_lcdif_probe,
	.remove = av_lcdif_remove,
	.shutdown = av_lcdif_shutdown,
	.driver = {
		.name = AV_LCDIF_DRIVER_NAME,
		.of_match_table = av_lcdif_of_match,
	},
};

module_platform_driver(av_lcdif_driver);

MODULE_AUTHOR("IMX6ULL_AV_Project");
MODULE_DESCRIPTION("i.MX6ULL LCDIF learning framebuffer driver - LCD-R3");
MODULE_LICENSE("GPL");
MODULE_VERSION("R3");
