# OV5640 V4L2 subdev模块

计划版本：

- `CAM-R1`：I²C、GPIO、MCLK和芯片ID。
- `CAM-R2`：VGA YUYV寄存器初始化。
- `CAM-R3`：V4L2 subdev格式接口。
- `CAM-R4`：`s_stream`和基础控制。
- `CAM-R5`：CSI联调、稳定性和文档。

第一阶段复用现有`mx6s-csi`。加载自写Sensor驱动前需要关闭原厂OV5640驱动。

