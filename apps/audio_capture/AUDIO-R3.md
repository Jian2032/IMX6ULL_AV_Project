# AUDIO-R3：音频采集线程与环形缓冲

## 本轮目标

AUDIO-R2.2 已经证明板载 MIC、WM8960 ADC、SAI2、SDMA、ALSA PCM 和 WAV 全部正常。
AUDIO-R3 将采集链路封装成后续音视频整机可复用的独立模块。

新增文件：

- `av_audio_capture.h`：对外 API、PCM 布局、packet 元数据和统计。
- `av_audio_capture.c`：ALSA UAPI、板载 MIC mixer、生产者线程和环形缓冲。
- `audio_thread_test.c`：消费环形缓冲到 RAM，停止 DMA 后保存 WAV。

## 数据流

```text
WM8960 ADC
   ↓
SAI2 + SDMA
   ↓
ALSA kernel ring（4 × 1024 frames）
   ↓ READI_FRAMES，producer thread
userspace ring（16 × 1024 frames）
   ↓ av_audio_read()
consumer / encoder / network sender
```

48 kHz 下一个 1024-frame period 约为 21.33 ms。16 个用户空间 slot 可吸收约 341 ms
的短期消费者抖动，占用约 64 KiB。

## 为什么采集线程不写文件

ALSA 内核 ring 只有约 85.33 ms。若采集线程在 `write()`、`fsync()` 或网络阻塞中停留
太久，SDMA 将覆盖未读 PCM，形成 XRUN。producer 只做：

1. `poll()` 等待 PCM 可读。
2. `SNDRV_PCM_IOCTL_READI_FRAMES` 取出一个 period。
3. 复制到用户环形缓冲并立即继续采集。

测试程序先收集到 RAM，`av_audio_stop()` 完成线程 join、PCM DROP 后才写 WAV。

## 环形缓冲满时的策略

采集线程不能长时间等待消费者。当 16 个 slot 全满时，模块丢弃最旧 period，并累加：

```text
dropped_packets
dropped_frames
```

这会形成一次可观测的音频不连续，但避免 producer 被用户空间阻塞后进一步引发
硬件 XRUN。正常验收中两个丢弃值都必须为 0。

## packet 元数据

`av_audio_read()` 每次返回：

- `sequence`：用户空间 period 序号。
- `first_frame`：该 packet 第一帧 PCM 在整条音频流中的累计位置。
- `timestamp_us`：producer 取得该 period 后的 `CLOCK_MONOTONIC` 时间。
- `frames`：本次有效 PCM 帧数。

后续音视频同步使用单调时钟，不使用可能被 NTP 或手工修改的墙上时钟。

## 资源生命周期

```text
av_audio_open()
  → 初始化 mutex/condition
  → 保存并设置 mainmic-route
  → open PCM
  → HW_PARAMS / SW_PARAMS / PREPARE
  → 分配 period buffer 和 userspace ring

av_audio_start()
  → pthread_create()
  → producer显式执行SNDRV_PCM_IOCTL_START
  → SAI/SDMA启动后进入poll/READI_FRAMES循环

av_audio_stop()
  → stop_requested
  → pthread_join()
  → SNDRV_PCM_IOCTL_DROP

av_audio_close()
  → SNDRV_PCM_IOCTL_HW_FREE
  → close PCM
  → 反向恢复 mixer
  → 销毁 ring/condition/mutex
```

## Ubuntu 编译

```sh
cd /home/book/Workspace/linux/nfs/project/audio_capture

make clean
make info
make

file audio_thread_test
arm-linux-gnueabihf-readelf -h audio_thread_test |
grep -E 'Class|Data|Machine'
```

`audio_thread_test` 使用 pthread，Makefile 已增加 `-pthread`。

## 开发板验收

```sh
cd /home/sun/nfs/project/audio_capture

./audio_thread_test --help

./audio_thread_test 10 /tmp/audio_r3_10s.wav \
    /dev/snd/pcmC0D0c /dev/snd/controlC0

echo "exit_code=$?"
ls -l /tmp/audio_r3_10s.wav
wc -c /tmp/audio_r3_10s.wav

cp /tmp/audio_r3_10s.wav /home/sun/nfs/project/audio_capture/
sync

dmesg |
grep -Ei 'wm8960|asoc|sai|sdma|audio|xrun|overrun|oops' |
tail -n 100
```

10 秒期望 WAV 大小：

```text
48000 × 10 × 2 × 2 + 44 = 1,920,044 bytes
```

验收标准：

- `recording=480000 frames`；
- `dropped=0 packets/0 frames`；
- `xruns=0`；
- `timeouts=0`；
- `error=0`；
- WAV 为 1,920,044 字节且能听见板载 MIC 人声；
- 程序退出后 mixer 恢复为原值。

## R3.1启动修正

首轮实机测试中，producer与consumer均正常创建，但持续超时且捕获帧数为0。根因是
PCM以`O_NONBLOCK`打开并停留在`PREPARED`状态，producer在首次读取前先调用`poll()`；
此时SAI/SDMA尚未启动，不可能产生数据令PCM变为可读，于是形成互相等待。

R3.1在producer线程进入`poll()`前显式执行`SNDRV_PCM_IOCTL_START`。XRUN执行
`PREPARE`后同样重新`START`，保证恢复路径不会再次停在PREPARED状态。零帧退出现在
明确报告未取得PCM数据，不再把未设置的`errno`显示为`Success`。

## 最终实机验收

R3.1在开发板上完成三组生命周期测试：

- 10秒正常录音：480000帧，WAV为1920044字节，丢包、XRUN、timeout和错误均为0；
- 运行约5.7秒后按Ctrl+C：保存273408帧部分WAV，退出码0；
- Ctrl+C后立即重新录制3秒：144000帧，WAV为576044字节，全部错误计数仍为0；
- 退出后`Capture Switch`、`Left Input Mixer Boost Switch`和
  `Left Boost Mixer LINPUT2 Switch`均恢复为0。

正常测试结束时producer可能比目标多取得一个period，因此统计中允许`queued=1`；
这不是丢帧。测试程序只把精确目标帧数写入WAV，未消费或最后一个period中多出的帧
不会进入文件。至此AUDIO-R3正式冻结。
