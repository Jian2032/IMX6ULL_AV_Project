# AV-R4：统一主时基、健康位图和系统快照

## 本轮目标

AV-R4不改变已经通过的实时预算和图像处理路径：CSI保持30fps、LCD保持15fps、
JPEG/HTTP保持5fps、ALSA保持48kHz。它只统一系统的观测语义：

```text
V4L2 DQBUF ----> process CLOCK_MONOTONIC capture_time_us --+
                                                         +--> serialized snapshot
ALSA producer --> process CLOCK_MONOTONIC timestamp_us ----+
                                                         +--> console + /health + /status
HTTP/LCD/module counters ---------------------------------+
```

V4L2自带的`timestamp_us`继续作为源元数据保留；跨模块诊断只使用DQBUF完成后由
本进程采样的`capture_time_us`。这避免假定旧内核V4L2时间戳与ALSA时间戳必然同源。

## 快照语义

`av_collect_snapshot()`用集成层互斥锁串行化控制台、`/health`和`/status`的多模块
读取。每份快照包含：

- 单调递增的`snapshot_serial`；
- 从系统RUNNING开始计算的`master_time_us`；
- 多模块读取所占的`snapshot_collection_us`；
- 视频、LCD、音频、HTTP原始计数和统一计算出的速率；
- 最新视频和音频数据的`age_ms`；
- 同一单调时钟域中的`video_audio_head_delta_ms`和跨度差；
- 可定位子系统的`health_flags`/`health_hex`。

快照不是“同一个硬件时钟沿上的原子采样”。模块在线程中仍持续更新，因此AV-R4
明确公开采集窗口耗时；它保证的是不同观察者不会把两轮模块读取交叉拼接。

健康位定义如下：

```text
0x00000001 video fatal       0x00000002 V4L2 sequence gap
0x00000004 capture timeout   0x00000008 LCD fatal
0x00000010 ALSA fatal        0x00000020 ALSA XRUN
0x00000040 audio drop        0x00000080 audio monitor fatal
0x00000100 audio seq gap     0x00000200 HTTP fatal
0x00000400 HTTP send error
```

`health_flags=0`时`/health`返回200和`ok`；任意错误位出现时返回503和`degraded`。

## Ubuntu交叉编译

```bash
cd /home/book/Workspace/linux/nfs/project/av_terminal

make clean
make info
make

file av_terminal
arm-linux-gnueabihf-readelf -h av_terminal | grep -E 'Class|Data|Machine'
grep -n 'AV-R4' av_terminal.c
```

预期没有warning，输出为32位ARM EABI5。

## 开发板运行

确认`/dev/fb0`已经存在且fbcon已解绑，然后执行：

```sh
cd /home/sun/nfs/project/av_terminal

./av_terminal 0 \
    /dev/video0 \
    /dev/snd/pcmC0D0c \
    /dev/snd/controlC0 \
    /dev/fb0 \
    8080
```

控制台每秒应打印同一份快照的序号、`health=0x00000000`、采样窗口、A/V head/span
诊断以及四条链路统计。

## Windows验收

浏览器打开：

```text
http://192.168.1.50:8080/
```

PowerShell连续执行两轮：

```powershell
curl.exe http://192.168.1.50:8080/health
curl.exe http://192.168.1.50:8080/status
Start-Sleep -Seconds 3
curl.exe http://192.168.1.50:8080/status
```

验收重点：

- `version=AV-R4`、`system=running`、`health_flags=0`；
- 第二份`snapshot_serial`和`master_time_us`大于第一份；
- `time_base=clock-monotonic-process`，`snapshot_collection_us`有界；
- `driver_sequence_gaps=0`、`capture_timeouts=0`；
- `audio_xruns=0`、`audio_dropped_frames=0`、`send_errors=0`；
- 视频约30fps、LCD约15fps、JPEG/HTTP约5fps、音频约48,000 frame/s；
- `video_age_ms`、`audio_age_ms`持续刷新；A/V head/span为诊断值，不要求为0；
- LCD和浏览器方向、颜色、几何、动态画面与AV-R3完全一致。

最后Ctrl+C并立即进行3秒重启：

```sh
echo "av_r4_exit=$?"

./av_terminal 3 \
    /dev/video0 \
    /dev/snd/pcmC0D0c \
    /dev/snd/controlC0 \
    /dev/fb0 \
    8080

echo "restart_exit=$?"
```

两次退出码均应为0，最终快照`health=0x00000000`。

## 最终实机验收记录（2026-08-10）

AV-R4已经完成并冻结：

- 两次HTTP快照的序号由12增长到16，主时基由10.546秒增长到13.636秒；
  `snapshot_collection_us`仅14～15微秒，证明统一快照开销有界。
- 活动流测试约25.4秒：视频采集760帧、JPEG 129帧，LCD显示379帧；音频采集
  与消费均为1,215,488帧；HTTP发送92帧，平均0.478ms，零skip、零send error；
  第二路流正确返回503。
- 全程`health_flags=0`、CSI driver gap/timeout为0、ALSA drop/XRUN为0；A/V head
  与span在同一单调时基下保持有界，没有单调发散。
- Ctrl+C退出码为0；紧接着3秒重启采集视频90帧、JPEG 17帧、LCD 45帧、音频
  144,384帧，最终ring为空并输出`[PASS]`，退出码仍为0。
- 用户确认LCD和浏览器方向、镜像、颜色、几何及动态画面继续正常。

因此AV-R4的主时基、统一快照、健康位图和生命周期目标全部通过。
