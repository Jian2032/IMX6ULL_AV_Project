# LCDIF framebuffer模块

本模块用于学习并实现i.MX6ULL LCDIF framebuffer驱动。现有
`CONFIG_FB_MXS`保持关闭，自写驱动直接复用当前LCDIF设备树节点。

## 开发轮次

- `LCD-R1`：platform driver、寄存器资源、中断、pinctrl和时钟验证。
- `LCD-R2`：解析LCD时序、DMA显存和基础点亮。
- `LCD-R3`：fbdev接口及RGB565测试应用。
- `LCD-R4`：双缓冲、VSYNC和`pan_display`。
- `LCD-R5`：blank、卸载、异常处理和稳定性。

## 当前版本

`LCD-R1`已经通过实机验证。当前为`LCD-R2`：解析完整显示时序、申请
一帧DMA显存并让LCDIF扫描内核生成的RGB565颜色条。R2仍然不会创建
`/dev/fb0`。编译和实机步骤见[《LCD-R2》](LCD-R2.md)。

## 文件

```text
av_lcdif_fb.c   当前LCD-R2驱动源码
Makefile        Linux 4.1.15外部模块构建入口
LCD-R1.md       R1资源探测记录
LCD-R2.md       R2点屏目标、编译、测试和回滚
```
