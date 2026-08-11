# VIDEO-R1：V4L2设备能力与格式探测

## 1. 本轮目标

在不依赖`v4l2-ctl`、不采集视频、也不修改摄像头配置的前提下，确认：

1. `/dev/video0`确实是`mx6s-csi`单平面采集节点；
2. 节点同时支持`V4L2_CAP_VIDEO_CAPTURE`和`V4L2_CAP_STREAMING`；
3. OV5640向CSI暴露的像素格式、分辨率和帧率能够通过标准ioctl枚举；
4. 当前输入、当前格式和当前帧率参数可以读取；
5. 为VIDEO-R2选择正确的格式与MMAP采集方式。

R1只执行查询类ioctl。代码中没有`S_FMT`、`REQBUFS`、`QBUF`、`STREAMON`，所以
即使查询结果异常，也不会启动DMA或改变传感器寄存器。

## 2. 与Linux 4.1.15源码的对应关系

当前内核源码中的`mx6s_capture.c`实现了：

- `VIDIOC_QUERYCAP`：报告单平面视频采集与流式I/O；
- `VIDIOC_ENUMINPUT/G_INPUT`：输入0为`Camera`；
- `VIDIOC_ENUM_FMT`：把sensor的media-bus格式映射成V4L2 fourcc；
- `VIDIOC_ENUM_FRAMESIZES`和`VIDIOC_ENUM_FRAMEINTERVALS`：转发给OV5640 subdev；
- `VIDIOC_G_FMT`和`VIDIOC_G_PARM`：读取当前图像格式与采集参数；
- R2需要的`REQBUFS/QUERYBUF/QBUF/DQBUF/STREAMON/STREAMOFF`。

当前`subdev/ov5640.c`只声明`MEDIA_BUS_FMT_YUYV8_2X8`，因此R1正常情况下应只枚举出
`YUYV`。驱动代码包含以下离散分辨率：176×144、320×240、640×480、720×480、
720×576、1024×768、1280×720、1920×1080和2592×1944；通常枚举15fps和30fps，
其中高分辨率受模式表限制。

一个容易误判的点：该版`mx6s-csi`在`probe()`时通过`kzalloc`把当前格式清零，却没有
主动设置默认格式。因此系统启动后尚无应用调用`S_FMT`时，R1的`G_FMT`可能打印0×0、
fourcc `....`。这不代表枚举失败；R2会设置640×480 YUYV，再读取驱动协商后的结果。

## 3. 文件准备

将正式工程中的以下三个文件放入Ubuntu NFS目录：

```text
src/apps/video_capture/v4l2_probe.c
src/apps/video_capture/Makefile
src/apps/video_capture/VIDEO-R1.md
```

Ubuntu目标目录：

```text
/home/book/Workspace/linux/nfs/project/video_capture
```

## 4. Ubuntu交叉编译

```sh
cd /home/book/Workspace/linux/nfs/project/video_capture

make clean
make info
make

file v4l2_probe
arm-linux-gnueabihf-readelf -h v4l2_probe | grep -E 'Class|Data|Machine'
```

预期：

- 编译过程无warning和error；
- `v4l2_probe`为`ELF 32-bit LSB executable, ARM, EABI5`；
- 动态加载器应与现有BusyBox RootFS的hard-float运行库匹配。

## 5. 开发板实机测试

```sh
cd /home/sun/nfs/project/video_capture

ls -l /dev/video0
cat /sys/class/video4linux/video0/name

./v4l2_probe --help
./v4l2_probe /dev/video0
echo "exit_code=$?"

dmesg | tail -n 40
```

必须确认：

- `driver`为`mx6s-csi`；
- active flags同时包含`V4L2_CAP_VIDEO_CAPTURE`和`V4L2_CAP_STREAMING`；
- input 0为`Camera`；
- 至少枚举到`YUYV`；
- 至少枚举到640×480；
- 640×480至少存在一种有效帧率；
- 最后打印`[PASS]`，退出码为0；
- `dmesg`没有新增Oops、WARNING、I²C错误或CSI异常。

当前格式为0×0时保留完整输出即可，不把它算作失败。

## 6. 可选的节点边界验证

`/dev/video1`是PxP节点，不是OV5640直连CSI采集节点。可执行：

```sh
./v4l2_probe /dev/video1
echo "pxp_exit_code=$?"
```

它可能显示不同能力并因“不支持单平面采集”退出。这个结果只用于理解节点职责，
不能代替`/dev/video0`验收。

## 7. 失败时保留的信息

如果R1失败，不要继续R2。请完整粘贴：

```sh
./v4l2_probe /dev/video0
echo $?
dmesg | tail -n 80
cat /sys/class/video4linux/video0/name
```

同时说明失败发生在capability、格式、分辨率还是帧率枚举阶段。

## 8. 实机验收结果（2026-08-08）

VIDEO-R1已通过，代码冻结：

- GCC 4.9.4交叉编译零warning/error；产物为ARM ELF32、little-endian、EABI5；
- video node和driver均确认是`mx6s-csi`，总线地址为`platform:21c4000.csi`；
- `capabilities=0x84200001`，`device_caps=0x04200001`；
- input 0为`Camera`，status为ok；
- 唯一格式为`YUYV`，与当前OV5640 subdev源码一致；
- 成功枚举640×480、320×240、720×480、720×576、1280×720、1920×1080、
  2592×1944、176×144和1024×768；
- 640×480支持15fps和30fps，高分辨率模式按sensor模式表限制帧率；
- 首次`G_FMT`返回0×0和未初始化fourcc，符合4.1.15 `mx6s-csi`源码行为；
- `G_PARM`返回1/30秒；最终打印`[PASS]`，退出码为0；
- 查询后内核日志无新增Oops、WARNING、I²C错误或CSI异常。

因此VIDEO-R2可采用640×480 YUYV、30fps目标和videobuf2 MMAP方式开始采集。
