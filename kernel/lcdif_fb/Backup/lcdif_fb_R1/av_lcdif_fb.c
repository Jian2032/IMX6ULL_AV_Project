// SPDX-License-Identifier: GPL-2.0
/*
 * av_lcdif_fb.c - i.MX6ULL LCDIF learning driver, round LCD-R1
 *
 * LCD-R1只完成platform驱动的“资源探测”阶段：
 *   1. 通过设备树compatible匹配LCDIF控制器；
 *   2. 获取并映射LCDIF寄存器区；
 *   3. 获取中断号、三个时钟和默认pinctrl状态；
 *   4. 通过display phandle读取LCD总线宽度和像素位数；
 *   5. 短暂打开时钟，验证时钟资源可用，然后立即关闭。
 *
 * 本轮有意不写任何LCDIF寄存器、不申请显存、不注册fb_info，
 * 因此加载成功后不会出现/dev/fb0，LCD画面也不会发生变化。
 * 等本轮在开发板验证通过后，LCD-R2再增加时序和DMA显存。
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define AV_LCDIF_DRIVER_NAME "av-lcdif-fb"

/*
 * 每一个成功匹配的LCDIF控制器对应一个该结构体实例。
 *
 * dev:
 *   指回platform device中的struct device，供日志和devm接口使用。
 * regs:
 *   LCDIF寄存器物理区经ioremap后的内核虚拟地址。CPU只能通过它
 *   配置控制器，不能直接把物理地址0x021c8000当指针使用。
 * irq:
 *   LCDIF中断号。R1只读取，不注册中断处理函数。
 * clk_*:
 *   来自设备树clock-names的时钟句柄。句柄本身不是频率；真正使用
 *   LCDIF前还需要prepare/enable，并在不用时反向disable/unprepare。
 * bus_width/bits_per_pixel:
 *   来自display phandle所指向的面板描述。这里分别应为24和16。
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
};

/*
 * 读取板级DTS中的：
 *
 *     &lcdif {
 *         display = <&display0>;
 *     };
 *
 * display是phandle，不应该依赖子节点名字“display”或“display#1”查找。
 * 因此即使启动日志出现重复节点被重命名，只要phandle仍然正确，驱动
 * 依然能取得真正被引用的面板节点。
 */
static int av_lcdif_parse_display(struct platform_device *pdev,
				   struct av_lcdif *lcdif)
{
	struct device_node *display_np;
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
		goto out_put_node;
	}

	ret = of_property_read_u32(display_np, "bits-per-pixel",
				   &lcdif->bits_per_pixel);
	if (ret) {
		dev_err(&pdev->dev, "display node has no bits-per-pixel\n");
		goto out_put_node;
	}

	/*
	 * i.MX6ULL LCDIF支持多种总线宽度，但本项目硬件已经确定为
	 * 24-bit RGB总线。Framebuffer第一阶段固定为RGB565，即16bpp。
	 * R1直接拒绝意外配置，避免R2把错误DTS当成寄存器问题调试。
	 */
	if (lcdif->bus_width != 24 || lcdif->bits_per_pixel != 16) {
		dev_err(&pdev->dev,
			"unsupported panel: bus-width=%u, bpp=%u\n",
			lcdif->bus_width, lcdif->bits_per_pixel);
		ret = -EINVAL;
		goto out_put_node;
	}

	dev_info(&pdev->dev,
		 "display node %s: bus-width=%u, bpp=%u\n",
		 display_np->full_name, lcdif->bus_width,
		 lcdif->bits_per_pixel);

out_put_node:
	/* of_parse_phandle增加了节点引用计数，所有出口都必须释放。 */
	of_node_put(display_np);
	return ret;
}

/*
 * 短暂打开三个时钟只用于验证资源和clock provider是否工作。
 *
 * 错误回滚顺序与成功开启顺序相反：
 *   enable:  axi -> disp_axi -> pix
 *   disable: pix -> disp_axi -> axi
 *
 * 这种逐级回滚是后续probe错误路径的基本写法。R1完成后所有时钟
 * 都处于关闭状态，不会意外启动LCD DMA。
 */
static int av_lcdif_validate_clocks(struct av_lcdif *lcdif)
{
	int ret;

	ret = clk_prepare_enable(lcdif->clk_axi);
	if (ret) {
		dev_err(lcdif->dev, "failed to enable axi clock: %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(lcdif->clk_disp_axi);
	if (ret) {
		dev_err(lcdif->dev,
			"failed to enable disp_axi clock: %d\n", ret);
		goto disable_axi;
	}

	ret = clk_prepare_enable(lcdif->clk_pix);
	if (ret) {
		dev_err(lcdif->dev, "failed to enable pix clock: %d\n", ret);
		goto disable_disp_axi;
	}

	dev_info(lcdif->dev,
		 "clock rates: pix=%lu Hz, axi=%lu Hz, disp_axi=%lu Hz\n",
		 clk_get_rate(lcdif->clk_pix), clk_get_rate(lcdif->clk_axi),
		 clk_get_rate(lcdif->clk_disp_axi));

	clk_disable_unprepare(lcdif->clk_pix);
disable_disp_axi:
	clk_disable_unprepare(lcdif->clk_disp_axi);
disable_axi:
	clk_disable_unprepare(lcdif->clk_axi);

	return ret;
}

static int av_lcdif_probe(struct platform_device *pdev)
{
	struct av_lcdif *lcdif;
	struct resource *res;
	struct pinctrl *pinctrl;
	int ret;

	dev_info(&pdev->dev, "LCD-R1 probe begin\n");

	/*
	 * devm_kzalloc分配的内存绑定到设备生命周期。probe失败或remove后，
	 * driver core会自动释放，减少手工错误回滚代码。
	 */
	lcdif = devm_kzalloc(&pdev->dev, sizeof(*lcdif), GFP_KERNEL);
	if (!lcdif)
		return -ENOMEM;

	lcdif->dev = &pdev->dev;
	platform_set_drvdata(pdev, lcdif);

	/*
	 * reg = <0x021c8000 0x4000>被转换为IORESOURCE_MEM。
	 * devm_ioremap_resource同时检查区域、申请所有权并完成ioremap。
	 */
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

	/* R1只确认中断资源存在，R4才会request_irq并处理中断状态。 */
	lcdif->irq = platform_get_irq(pdev, 0);
	if (lcdif->irq < 0) {
		dev_err(&pdev->dev, "missing LCDIF irq: %d\n", lcdif->irq);
		return lcdif->irq;
	}

	/*
	 * 选择DTS中的pinctrl-0，把LCD_DATA、CLK、ENABLE、HSYNC、VSYNC
	 * 等引脚切换到LCDIF功能。devm接口会在解绑时自动释放状态句柄。
	 */
	pinctrl = devm_pinctrl_get_select_default(&pdev->dev);
	if (IS_ERR(pinctrl)) {
		ret = PTR_ERR(pinctrl);
		dev_err(&pdev->dev,
			"failed to select default pinctrl state: %d\n", ret);
		return ret;
	}

	/* clock-names必须与DTS中的pix、axi、disp_axi完全一致。 */
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

	ret = av_lcdif_validate_clocks(lcdif);
	if (ret)
		return ret;

	dev_info(&pdev->dev,
		 "LCD-R1 ready: regs=0x%08llx-0x%08llx, irq=%d\n",
		 (unsigned long long)res->start,
		 (unsigned long long)res->end, lcdif->irq);
	dev_info(&pdev->dev,
		 "resource validation passed; LCD registers were not modified\n");

	return 0;
}

static int av_lcdif_remove(struct platform_device *pdev)
{
	/*
	 * R1所有资源都由devm管理，remove不需要手工iounmap或clk_put。
	 * 后续轮次出现显存、运行时钟和IRQ状态后，这里会增加对称清理。
	 */
	dev_info(&pdev->dev, "LCD-R1 remove\n");
	platform_set_drvdata(pdev, NULL);
	return 0;
}

static const struct of_device_id av_lcdif_of_match[] = {
	/* DTS同时包含imx6ul和imx28兼容串，优先匹配更具体的SoC。 */
	{ .compatible = "fsl,imx6ul-lcdif" },
	{ .compatible = "fsl,imx28-lcdif" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, av_lcdif_of_match);

static struct platform_driver av_lcdif_driver = {
	.probe = av_lcdif_probe,
	.remove = av_lcdif_remove,
	.driver = {
		.name = AV_LCDIF_DRIVER_NAME,
		.of_match_table = av_lcdif_of_match,
	},
};

module_platform_driver(av_lcdif_driver);

MODULE_AUTHOR("IMX6ULL_AV_Project");
MODULE_DESCRIPTION("i.MX6ULL LCDIF learning framebuffer driver - LCD-R1");
MODULE_LICENSE("GPL");
MODULE_VERSION("R1");
