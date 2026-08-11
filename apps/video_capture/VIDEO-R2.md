# VIDEO-R2：640×480 YUYV的V4L2 MMAP采集

## 1. 本轮目标

建立第一条真实视频数据通路：

```text
OV5640
  → 并行YUYV总线
  → i.MX6ULL CSI
  → videobuf2连续DMA buffer
  → 用户空间mmap地址
  → 保存一帧614400字节YUYV原始图像
```

本轮不连接LCD、不转换RGB565，也不加入复杂的信号与自动恢复逻辑。这样一旦失败，
问题只可能位于sensor模式、CSI、videobuf2或用户空间buffer流程。

## 2. 4.1.15旧驱动的关键约束

根据当前内核`mx6s_capture.c`：

- `open()`时初始化`vb2_queue`，支持`VB2_MMAP | VB2_USERPTR`；
- `mx6s_start_streaming()`要求至少2个buffer已经QBUF，否则返回`-ENOBUFS`；
- 队列使用`vb2_dma_contig_memops`，适合CSI连续DMA；
- 帧中断填写`sequence`和单调时钟`timestamp`；
- 用户空间未及时QBUF时，驱动会切换到内部discard buffer，之后sequence会出现缺口；
- 单次队列最多使用64MiB视频内存。本程序4×614400字节，远低于限制。

根据当前`ov5640.c`：

- `capturemode=0`对应VGA 640×480；
- `timeperframe=1/30`选择30fps模式表；
- `S_PARM`实际负责写入sensor模式寄存器，因此本程序先设置模式/帧率，再用`S_FMT`
  配置CSI的宽、高、格式和DMA帧长度。

## 3. R2 buffer生命周期

```text
VIDIOC_REQBUFS(count=4)
  → VIDIOC_QUERYBUF
  → mmap
  → 4次VIDIOC_QBUF
  → VIDIOC_STREAMON
  → select等待
  → VIDIOC_DQBUF
  → 读取index/bytesused/sequence/timestamp
  → VIDIOC_QBUF归还
  → 重复到目标帧数
  → VIDIOC_STREAMOFF
  → munmap
  → VIDIOC_REQBUFS(count=0)
  → close
```

保存文件时不在采集循环中执行磁盘/NFS写入。程序只在中间帧到来时`memcpy()`一次，
等`STREAMOFF`后再写文件，避免慢速I/O占住采集buffer造成丢帧。

## 4. Ubuntu交叉编译

```sh
cd /home/book/Workspace/linux/nfs/project/video_capture

make clean
make info
make

file v4l2_probe v4l2_capture
arm-linux-gnueabihf-readelf -h v4l2_capture | grep -E 'Class|Data|Machine'
```

预期：

- 两个程序均编译成功且无warning/error；
- `v4l2_capture`为ARM ELF32、little-endian、EABI5。

## 5. 开发板采集测试

```sh
cd /home/sun/nfs/project/video_capture

./v4l2_capture --help
cat /proc/interrupts | grep -Ei 'csi|21c4000'

./v4l2_capture /dev/video0 120 frame_640x480_yuyv.raw
echo "exit_code=$?"

ls -l frame_640x480_yuyv.raw
wc -c frame_640x480_yuyv.raw
od -An -tx1 -N 64 frame_640x480_yuyv.raw

cat /proc/interrupts | grep -Ei 'csi|21c4000'
dmesg | tail -n 80
```

必须确认：

- 协商结果为640×480、`YUYV`、30fps；
- `bytesperline=1280`，`sizeimage=614400`；
- 分配并mmap至少2个buffer，正常应为4个；
- 完成120帧，`bytesused`始终为614400；
- `sequence gaps=0`；
- 实测帧率接近30fps；
- 原始文件大小严格为614400字节；
- Y范围、均值和FNV-1a能够正常输出，数据不是全0；
- 最后打印`[PASS]`，退出码为0；
- CSI中断数增加，内核没有FIFO overflow、Hresponse error、Oops或WARNING。

## 6. 在PC或Ubuntu查看原始帧

如果安装了FFmpeg，可以转换成PNG：

```sh
ffmpeg -f rawvideo -pixel_format yuyv422 -video_size 640x480 \
    -i frame_640x480_yuyv.raw -frames:v 1 frame_640x480.png
```

也可以直接查看：

```sh
ffplay -f rawvideo -pixel_format yuyv422 -video_size 640x480 \
    frame_640x480_yuyv.raw
```

图像颜色和几何方向应基本正确。R2不负责缩放到1024×600，也不负责显示到LCD。

## 7. 失败时保留的信息

如果出现超时、短帧、sequence缺口或内核错误，不进入R3。请完整保留：

```sh
./v4l2_capture /dev/video0 120 frame_640x480_yuyv.raw
echo $?
ls -l frame_640x480_yuyv.raw 2>/dev/null
cat /proc/interrupts | grep -Ei 'csi|21c4000'
dmesg | tail -n 120
```

同时说明失败发生在`S_PARM`、`S_FMT`、`REQBUFS`、`STREAMON`、`select`还是`DQBUF`。

## 8. 实机验收结果（2026-08-08）

VIDEO-R2已通过，代码冻结：

- 两个程序均由GCC 4.9.4零告警编译；R2产物为ARM ELF32、little-endian、EABI5；
- 协商为640×480 YUYV、30fps，`field=V4L2_FIELD_NONE`；
- `bytesperline=1280`、`sizeimage=614400`；
- 4个MMAP buffer的长度均为614400字节；
- 连续采集120帧，sequence从0到119，gaps为0；
- 每帧`bytesused=614400`；首末时间戳跨度3.964秒，实测30.02fps；
- 保存第61帧，文件大小严格为614400字节；
- FNV-1a为`0x29658a27`，Y范围7～233，Y均值142.29，U/V均值
  125.14/127.35，原始字节有正常变化；
- CSI中断计数从0增加到240；
- 最终打印`[PASS]`、退出码为0，内核无FIFO overflow、Hresponse、Oops或WARNING。

本次输出中的`field=1`即`V4L2_FIELD_NONE`。`colorspace=0`来自旧OV5640
`s_mbus_fmt`没有为已支持格式回填colorspace，不改变已经确认的YUYV内存排列。
