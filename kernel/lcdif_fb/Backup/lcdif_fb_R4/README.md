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

`LCD-R1`～`LCD-R3`已经通过实机验证。当前为`LCD-R4`：在两帧连续
DMA显存之间通过`FBIOPAN_DISPLAY`翻页，并使用帧完成和VSYNC中断
同步。编译和实机步骤见[《LCD-R4》](LCD-R4.md)。

## 文件

```text
av_lcdif_fb.c   当前LCD-R4驱动源码
fb_test.c       用户空间绘图、双缓冲和VSYNC测试
Makefile        Linux 4.1.15外部模块构建入口
LCD-R1.md       R1资源探测记录
LCD-R2.md       R2点屏目标、编译、测试和回滚
LCD-R3.md       R3 fbdev注册与用户空间测试
LCD-R4.md       R4双缓冲、翻页、中断同步与测试
```
