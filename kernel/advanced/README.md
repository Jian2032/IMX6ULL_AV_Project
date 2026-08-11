# 高级内核模块

MVP完成后再开始：

- `csi_capture/`：CSI Host、videobuf2、DMA和帧中断。
- `lcdif_drm/`：DRM/KMS、plane、CRTC、connector和page flip。
- `asoc_machine/`：SAI2、WM8960和Machine Driver。

这些模块不会与第一阶段同时抢占同一硬件。

## 当前诊断例外

`csi_capture/`目前只存放VIDEO-R5 PCLK采样边沿A/B补丁与测试说明，不是第二套CSI Host
驱动，也不会以模块方式和内建`mx6s-csi`同时绑定硬件。补丁需要应用到Linux 4.1.15源码并
重编zImage；验证结束后要么回退，要么再整理成设备树可配置的正式修复。
