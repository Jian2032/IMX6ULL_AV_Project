# AV-INTEGRATION-R1：音视频并行采集验收

## 本轮目标

AV-R1把已经验收的两个生产链路放入同一进程：

```text
OV5640 -> CSI/V4L2 -> raw槽 -> JPEG编码
WM8960 -> SAI/SDMA -> ALSA ring -> 用户音频ring -> monitor
```

本轮不接LCD，也不启动HTTP。这样可以先回答一个最基本的问题：摄像头、JPEG编码、WM8960、SAI/SDMA和两个用户空间生产者同时运行时，是否仍能维持稳定的数据率并干净退出。

## 线程和所有权

- 视频capture线程独占V4L2的`DQBUF/QBUF`。
- 视频encoder线程读取cacheable raw槽并生成JPEG。
- ALSA producer线程独占`READI_FRAMES`，把完整period发布到16槽用户ring。
- audio monitor线程持续消费音频period并统计RMS、peak、sequence和时间戳。
- 主线程只负责编排生命周期、每秒读取统计、处理`SIGINT/SIGTERM`。

不能由主线程按JPEG帧率顺便读取音频。i.MX6ULL当前JPEG编码约21fps，而每个音频period只有1024帧；若每个JPEG只消费一个period，消费速度约21,504 frame/s，远低于48,000 frame/s，最终一定发生音频ring覆盖或XRUN。

## 时间戳边界

视频时间戳来自V4L2完成帧，音频时间戳在ALSA producer完成一个period读取后使用`CLOCK_MONOTONIC`记录。R1打印的`span_delta`比较两条流各自从首个样本开始的相对时间跨度，用于发现明显速率漂移。

它还不是最终口型同步用的PTS，原因包括：

- 视频和音频时间戳分别位于不同数据交付边界；
- 首个视频帧和首个音频period不在同一物理采样时刻；
- 旧版V4L2驱动的时间戳语义还需要在AV-R4统一封装。

## Ubuntu/NFS文件布局

把本目录文件放到：

```text
/home/book/Workspace/linux/nfs/project/av_terminal
```

Makefile默认复用以下兄弟目录：

```text
../video_capture
../audio_capture
../http_mjpeg
../third_party/libjpeg-turbo-arm
```

## 交叉编译

```bash
cd /home/book/Workspace/linux/nfs/project/av_terminal
make clean
make info
make

file av_terminal
arm-linux-gnueabihf-readelf -h av_terminal | grep -E 'Class|Data|Machine'
```

预期为ARM ELF32、little-endian、EABI5，并且编译过程没有warning/error。

## 开发板短测

先确认没有其他程序占用摄像头或PCM：

```sh
cd /home/sun/nfs/project/av_terminal
./av_terminal --help
./av_terminal 30 /dev/video0 /dev/snd/pcmC0D0c /dev/snd/controlC0
echo "exit_code=$?"
```

R1不会创建`/dev/fb0`、不会监听TCP端口，也不会保存WAV/JPEG文件。

## 运行中观察

另开终端执行：

```sh
cat /proc/interrupts | grep -Ei 'csi|sai|sdma|21c4000|202c000|20ec000'

dmesg |
grep -Ei 'csi|wm8960|sai|sdma|xrun|overrun|overflow|warning|oops' |
tail -n 100
```

## Ctrl+C和重启验收

```sh
./av_terminal 0 /dev/video0 /dev/snd/pcmC0D0c /dev/snd/controlC0
# 运行数秒后按Ctrl+C
echo "interrupt_exit=$?"

./av_terminal 5 /dev/video0 /dev/snd/pcmC0D0c /dev/snd/controlC0
echo "restart_exit=$?"
```

## 通过标准

- 视频`captured_frames > 0`、`encoded_frames > 0`。
- `driver_sequence_gaps=0`、`capture_timeouts=0`。
- JPEG编码跟不上30fps时允许`raw_dropped > 0`，这是低延迟策略丢帧，不是驱动丢帧。
- 音频接近48,000 frame/s。
- `xruns=0`、`dropped=0`、`sequence_gaps=0`、最终`queued=0`。
- 输出`[PASS]`或干净的`[STOP]`，退出码为0。
- Ctrl+C后可以立即重新打开摄像头和PCM。
- 内核日志没有新增CSI overflow、ALSA overrun、WARNING或Oops。

## R1不解决的问题

- 不显示LCD预览。
- 不提供HTTP MJPEG服务。
- 不保存或编码音频。
- 不宣称实现最终音视频唇音同步。
- 不允许第二个进程同时打开`/dev/video0`；后续LCD和HTTP必须从同一个视频producer分发。

## 2026-08-10实机验收记录

ARM GCC 4.9.4交叉编译无warning/error，产物为ARM ELF32、little-endian、EABI5。

30秒正常测试：

- 视频采集901帧，30.02fps，JPEG编码632帧，约21.04fps。
- `driver_gaps=0`、`capture_timeouts=0`。
- 应用主动丢弃269个过期raw帧，严格满足`901 = 632 + 269`。
- 音频采集和消费均为1,440,768帧，实测约48,000 frame/s。
- `dropped=0`、`xruns=0`、`queued=0`、`sequence_gaps=0`。
- `span_delta`在约-17～-69ms内有界波动，没有持续累积趋势。
- 程序输出`[PASS]`并以0退出。

Ctrl+C和重启测试：

- 12.6秒处中断，视频采集378帧，音频605,184帧全部消费完成。
- 输出`[STOP] Signal requested a clean A/V shutdown.`，退出码0。
- 立即重启5秒成功：视频150帧、音频240,640帧，仍无driver gap、XRUN或ring drop。
- CSI与SDMA中断持续增长；内核日志无XRUN、overflow、WARNING或Oops。

AV-R1已冻结，下一轮进入单V4L2 producer的LCD/JPEG多路分发。
