# 内核驱动目录

第一阶段：

- `lcdif_fb/`：自写LCDIF framebuffer驱动。
- `ov5640_subdev/`：自写简化OV5640 V4L2 subdev驱动。

第二阶段放入`advanced/`：

- 自写CSI capture/videobuf2驱动。
- LCDIF DRM/KMS驱动。
- ASoC Machine Driver。

内核驱动统一以当前Linux 4.1.15源码为API基准。

