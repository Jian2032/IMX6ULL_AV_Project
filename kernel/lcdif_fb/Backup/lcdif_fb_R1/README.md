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

当前为`LCD-R1`。它只探测资源，不会写LCDIF寄存器，也不会创建
`/dev/fb0`。编译和实机步骤见[《LCD-R1》](LCD-R1.md)。

## 文件

```text
av_lcdif_fb.c   LCD-R1驱动源码
Makefile        Linux 4.1.15外部模块构建入口
LCD-R1.md       本轮目标、编译、测试和预期结果
```
