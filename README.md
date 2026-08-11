# `src` 源码目录说明

本目录保存 i.MX6ULL 音视频终端的核心源码，覆盖 LCDIF framebuffer、V4L2 摄像头采集、ALSA 音频采集、JPEG/HTTP MJPEG 和最终音视频集成程序。

当前冻结基线为 `AV-R5`：OV5640 保持约 30 fps 采集，LCD 按最新帧策略约 15 fps 预览，HTTP MJPEG 按 CPU 预算约 5 fps 输出，WM8960 保持 48 kHz、双声道、S16_LE 采集。

## 数据链路

```text
OV5640
  └─ CSI DMA → V4L2 MMAP → 唯一采集线程 → cacheable YUYV raw槽
                                         ├─ LCD线程 → RGB565 → /dev/fb0
                                         └─ JPEG线程 → JPEG槽 → HTTP线程 → 浏览器

WM8960
  └─ SAI/SDMA → ALSA PCM → 音频采集线程 → 16槽period ring → monitor/统计
```

摄像头只由一个生产者执行 `DQBUF/QBUF`。LCD、JPEG 和网络均不持有 CSI DMA buffer；慢消费者通过“最新帧优先、丢弃旧帧”控制延迟。音频实时采集线程与统计处理分离，避免业务逻辑造成 XRUN。

## 目录结构

```text
src/
├─ apps/
│  ├─ video_capture/    V4L2查询、MMAP采集、预览、录像及OV5640诊断
│  ├─ audio_capture/    ALSA UAPI查询、录音、WAV和线程化period ring
│  ├─ http_mjpeg/       YUYV→JPEG、HTTP服务和MJPEG推流
│  ├─ av_terminal/      AV-R5最终集成程序
│  ├─ local_preview/    本地预览设计说明
│  └─ common/           公共组件预留说明
├─ kernel/
│  ├─ lcdif_fb/         自研LCDIF fbdev外部内核模块及fb_test
│  ├─ advanced/         CSI/PCLK与DVP驱动强度诊断记录
│  └─ ov5640_subdev/    取舍说明；当前不自写OV5640 subdev
└─ gui/                 后续GUI规划，当前未实现
```

## 模块状态与入口

| 模块 | 当前状态 | 主要入口 |
|---|---|---|
| LCDIF framebuffer | `LCD-R5`完成 | [`kernel/lcdif_fb/README.md`](kernel/lcdif_fb/README.md) |
| V4L2摄像头 | `VIDEO-R5`完成 | [`apps/video_capture/README.md`](apps/video_capture/README.md) |
| ALSA音频 | `AUDIO-R3`完成 | [`apps/audio_capture/README.md`](apps/audio_capture/README.md) |
| HTTP MJPEG | `STREAM-R4.1`完成 | [`apps/http_mjpeg/README.md`](apps/http_mjpeg/README.md) |
| 音视频集成 | `AV-R5`完成 | [`apps/av_terminal/README.md`](apps/av_terminal/README.md) |
| GUI | 仅规划 | [`gui/README.md`](gui/README.md) |
| 自写OV5640 subdev | 已取消 | [`kernel/ov5640_subdev/README.md`](kernel/ov5640_subdev/README.md) |

## 已实现边界

项目自行实现或完成：

- LCDIF fbdev外部驱动、DMA显存、RGB565、双页翻转与诊断统计；
- V4L2 UAPI能力查询、四缓冲MMAP、buffer所有权与线程化采集；
- YUYV到RGB565的ARM NEON转换、180度方向校正和LCD居中显示；
- ALSA UAPI硬件/软件参数、PCM period搬运、用户ring、XRUN与WAV；
- YUYV直接JPEG 4:2:2编码、HTTP multipart、单MJPEG客户端和慢客户端隔离；
- 单摄像头多消费者、统一 `CLOCK_MONOTONIC` 时基、健康位图和故障注入；
- WM8960 Main MIC的DTS/DAPM路由修复，以及OV5640 DVP驱动强度1×修复。

明确复用：

- NXP `mx6s-csi` Host驱动与NXP OV5640 subdev；
- WM8960 Codec、ASoC Machine Driver、SAI和SDMA驱动；
- 静态 `libjpeg-turbo`；
- Linux/POSIX socket、pthread、V4L2/ALSA/fbdev UAPI。

本阶段未实现PXP加速、H.264/H.265、网络音频、正式A/V播放同步或复杂GUI。

## 构建环境

- 目标：i.MX6ULL Cortex-A7，ARM hard-float；
- 内核：NXP Linux 4.1.15；
- 工具链：`arm-linux-gnueabihf-gcc` 4.9.4；
- RootFS：BusyBox；
- JPEG：ARM静态 `libturbojpeg.a`。

`src/` 是 Windows 项目中按模块整理的源码视图。实际参与最终构建的 Ubuntu 工作快照保存在 [`../sources/current/`](../sources/current/README.md)。

## 在Ubuntu中交叉编译

先设置工具链：

```sh
export CROSS_COMPILE=arm-linux-gnueabihf-
```

各用户态模块可分别构建：

```sh
cd src/apps/video_capture
make clean && make

cd ../audio_capture
make clean && make

cd ../http_mjpeg
make clean && make \
    JPEG_PREFIX=../../../third_party/libjpeg-turbo-arm

cd ../av_terminal
make clean && make \
    JPEG_PREFIX=../../../third_party/libjpeg-turbo-arm
```

`http_mjpeg`和`av_terminal`必须使用上面的 `JPEG_PREFIX` 覆盖值，因为整理后的 `src/apps/` 与 NFS 实际工作树的目录层级不同。

编译LCDIF模块和测试程序：

```sh
cd src/kernel/lcdif_fb

make clean
make \
    KDIR=/home/book/Workspace/nxp_linux/linux-imx-rel_imx_4.1.15_2.1.0_ga \
    ARCH=arm \
    CROSS_COMPILE=arm-linux-gnueabihf-
```

不要在 x86 Ubuntu 上直接运行生成的 ARM ELF；应复制到开发板执行。

## 开发板运行

当前板内独立运行布局：

```text
/opt/imx6ull-av/
├─ bin/av_terminal
├─ bin/fb_test
├─ modules/av_lcdif_fb.ko
├─ start_av_terminal.sh
└─ SHA256SUMS.txt
```

板内一键启动脚本源文件见 [`../scripts/start_av_terminal_local.sh`](../scripts/start_av_terminal_local.sh)。部署后执行：

```sh
/opt/imx6ull-av/start_av_terminal.sh start
/opt/imx6ull-av/start_av_terminal.sh status
/opt/imx6ull-av/start_av_terminal.sh stop
```

运行时主要设备节点：

```text
/dev/video0
/dev/snd/controlC0
/dev/snd/pcmC0D0c
/dev/fb0
```

## 最终验收基线

- 正式长测持续 3600.665 秒并返回 `[PASS]`；
- 视频约 30.02 fps，`driver_sequence_gaps=0`、`capture_timeouts=0`；
- LCD约 14.97 fps，`failed=0`；
- JPEG约 4.99 fps，HTTP `send_errors=0`；
- 音频约 48,000 frame/s，`dropped=0`、`xruns=0`、`sequence_gaps=0`；
- 全部健康快照及最终健康位均为 `0x00000000`。

`raw_frames_dropped`和LCD/JPEG跳帧属于应用层最新帧策略，不代表CSI驱动丢帧。

## 推荐阅读顺序

1. [`apps/video_capture/av_video_capture.h`](apps/video_capture/av_video_capture.h) 与 [`av_video_capture.c`](apps/video_capture/av_video_capture.c)
2. [`apps/audio_capture/av_audio_capture.h`](apps/audio_capture/av_audio_capture.h) 与 [`av_audio_capture.c`](apps/audio_capture/av_audio_capture.c)
3. [`apps/http_mjpeg/av_mjpeg_pipeline.h`](apps/http_mjpeg/av_mjpeg_pipeline.h) 与 [`av_mjpeg_pipeline.c`](apps/http_mjpeg/av_mjpeg_pipeline.c)
4. [`apps/av_terminal/av_lcd_preview.c`](apps/av_terminal/av_lcd_preview.c)
5. [`apps/av_terminal/av_http_server.c`](apps/av_terminal/av_http_server.c)
6. [`apps/av_terminal/av_terminal.c`](apps/av_terminal/av_terminal.c)
7. [`kernel/lcdif_fb/av_lcdif_fb.c`](kernel/lcdif_fb/av_lcdif_fb.c)

## 相关文档

- [项目总览](../README.md)
- [当前项目状态](../PROJECT_STATUS.md)
- [系统架构](../docs/01_system_architecture.md)
- [测试与验收](../docs/05_test_and_acceptance.md)
- [第6天音视频集成学习](../docs/learning/day6_av_integration_complete.md)
- [第7天最终交付](../docs/10_day7_delivery.md)
- [系统发布文件](../release/current/README.md)
