# AUDIO-R2：48kHz双声道PCM采集与WAV保存

## R1基线

开发板实测确认：

- 声卡0为`wm8960-audio`。
- `pcmC0D0c`是`HiFi wm8960-hifi-0`直连采集端。
- 支持`RW_INTERLEAVED`和`MMAP_INTERLEAVED`。
- 支持`S16_LE/S24_LE/S32_LE/S20_3LE`。
- 支持1～2声道、8000～48000Hz。
- `48000Hz + S16_LE + 2ch + RW_INTERLEAVED`组合通过`HW_REFINE`。
- period size为16～16384帧，period数量为2～255。
- AUDIO-R1退出码0，内核日志没有ASoC或XRUN错误。

因此R2不修改设备树和内核，直接验证PCM数据通路。

## 开发板实测结果

2026-08-09首次5秒录音的数字采集与WAV封装已经通过：

```text
frames        = 240000
PCM bytes     = 960000
WAV bytes     = 960044
elapsed       = 5.014 s
measured rate = 47869.23 frames/s
xruns         = 0
L peak/RMS    = 11822 / 233.35
R peak/RMS    = 383 / 23.66
clipped       = 0 / 0
exit code     = 0
```

`Capture Switch`由`0,0`临时改为`1,1`，退出时成功恢复为`0,0`；日志确认执行了
`DROP`和`HW_FREE`，内核没有ASoC、SDMA、XRUN或Oops错误。WAV头中的PCM格式、双声道、
48000Hz、192000字节/秒、4字节帧对齐、16位采样和960000字节data长度均正确。

PC整段、左声道和右声道分别试听均没有听到声音，因此“麦克风有效音频”尚未通过。
这不否定PCM/SDMA/WAV数字通路，但在模拟输入路由解决前不能把音频模块标为功能完成。

ALPHA V2.4底板原理图确认板载MIC实际接在`LINPUT2/LINPUT1`：`LINPUT2`接MIC正端，
`LINPUT1`接MIC负端；现有设备树却把`Main MIC`连接到`RINPUT1/RINPUT2`。同时R1快照中
`Left Boost Mixer LINPUT2 Switch=0`，实际信号脚没有进入当前Boost路径。下一步先排除PC
播放设备问题，再用可恢复的mixer A/B测试验证`LINPUT1/LINPUT2`路径，验证后才做最小DTS
路由修正。

## 固定参数

```text
sample rate  = 48000 frames/s
channels     = 2
format       = signed 16-bit little-endian
frame bytes  = 2 channels × 2 bytes = 4 bytes
period size  = 1024 frames = 4096 bytes ≈ 21.333 ms
period count = 4
buffer size  = 4096 frames = 16384 bytes ≈ 85.333 ms
```

应用首先保存`Capture Switch`当前值，将左右通道临时设置为1。退出时无论成功或失败都恢复
原值。本轮不改Capture Volume、ADC Volume、Boost、ALC或Noise Gate，保持单变量原则。

## 数据与资源生命周期

```text
打开controlC0并保存/启用Capture Switch
→ 打开pcmC0D0c
→ HW_PARAMS提交格式、采样率、period和buffer
→ SW_PARAMS设置avail_min/start_threshold/stop_threshold
→ PREPARE
→ READI_FRAMES自动启动SAI和SDMA
→ PCM先保存到应用RAM
→ DROP停止DMA
→ HW_FREE释放PCM硬件缓冲
→ 关闭PCM
→ 恢复Capture Switch并关闭controlC0
→ 最后才写WAV文件
```

先录入RAM是为了避免NFS写入或eMMC抖动阻塞实时采集。5秒PCM只占960,000字节。

## Ubuntu交叉编译

```sh
cd /home/book/Workspace/linux/nfs/project/audio_capture

make clean
make info
make

file audio_probe audio_capture
arm-linux-gnueabihf-readelf -h audio_capture | grep -E 'Class|Data|Machine'
```

## 开发板首次录音

录音过程中靠近板载麦克风说话或轻敲麦克风附近，但不要敲击连接器。

```sh
cd /home/sun/nfs/project/audio_capture

./audio_capture --help

./audio_capture /dev/snd/pcmC0D0c 5 /tmp/wm8960_5s.wav \
    /dev/snd/controlC0

echo "exit_code=$?"
ls -l /tmp/wm8960_5s.wav
wc -c /tmp/wm8960_5s.wav
od -An -tx1 -N 44 /tmp/wm8960_5s.wav

dmesg | grep -Ei 'wm8960|asoc|sai|sdma|audio|xrun|overrun|oops' | tail -n 100
```

完整5秒WAV的预期大小：

```text
44 + 48000 × 5 × 2 × 2 = 960044 bytes
```

WAV头部应以`52 49 46 46`（RIFF）开头，并在偏移8看到`57 41 56 45`（WAVE）。

程序通过条件：

- `hardware params`返回48000Hz、2声道、S16_LE。
- period为1024帧、period count为4、buffer为4096帧。
- `xruns=0`。
- 输出`[PASS]`，退出码为0。
- WAV大小为960044字节。
- peak和RMS明显大于0，且说话/敲击时peak提高。
- `clipped=0`或接近0。
- 退出时打印`DROPPED`、`HW_FREE`和Capture Switch恢复信息。
- dmesg无DMA、ASoC或Oops错误。

## 复制到PC播放

```sh
cp /tmp/wm8960_5s.wav /home/sun/nfs/project/audio_capture/
```

PC上可直接执行：

```sh
ffprobe wm8960_5s.wav
ffplay wm8960_5s.wav
```

若只有一个声道有明显波形，不立即修改驱动：板载Main MIC在设备树中连接RINPUT1/RINPUT2，
外接Mic Jack连接LINPUT2/LINPUT3。先根据左右声道RMS确认当前实际输入，再在下一轮调整
WM8960输入路由或决定项目使用单声道。

## XRUN语义

采集时应用读取过慢，ALSA环形缓冲被硬件写满，会进入XRUN并由`READI_FRAMES`返回
`EPIPE`。R2记录XRUN次数，调用`PREPARE`恢复后继续采集，但正式验收要求`xruns=0`。
