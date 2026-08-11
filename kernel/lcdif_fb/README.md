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

`LCD-R1`～`LCD-R5`已经全部通过实机验证。最终版本支持blank/背光
联动、异常计数、sysfs诊断、双缓冲和VSYNC；300次长采样刷新率为
58.66Hz，五轮重复加载/卸载无警告、无异常计数、无节点残留。
验收证据见[《LCD-R5》](LCD-R5.md)，完整讲解见
[《第1天：LCDIF最终代码完整学习》](../../../docs/learning/day1_lcdif_complete.md)。

## 文件

```text
av_lcdif_fb.c   当前LCD-R5最终驱动源码
fb_test.c       用户空间绘图、双缓冲、VSYNC和blank测试
Makefile        Linux 4.1.15外部模块构建入口
LCD-R1.md       R1资源探测记录
LCD-R2.md       R2点屏目标、编译、测试和回滚
LCD-R3.md       R3 fbdev注册与用户空间测试
LCD-R4.md       R4双缓冲、翻页、中断同步与测试
LCD-R5.md       R5稳定性、诊断与最终验收
```
