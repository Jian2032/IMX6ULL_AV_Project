# AUDIO-R1：ALSA与WM8960只读诊断

## 本轮目标

在不开始录音、不启动SAI DMA、不修改mixer的前提下确认：

1. `/dev/snd/controlC0`属于`wm8960-audio`声卡。
2. 声卡注册了哪些播放和采集PCM节点。
3. `hw:0,0`采集端支持的访问方式、采样格式、通道数和采样率。
4. `48000 Hz + S16_LE + 双声道 + RW_INTERLEAVED`这一参数组合可以成立。
5. WM8960当前有哪些mixer控件，麦克风、ADC、Capture、Boost、ALC相关控件当前是什么值。

R1只调用查询类ALSA ioctl。`HW_REFINE`仅计算参数约束，不等于`HW_PARAMS`，不会配置硬件。

## 已确认的内核和设备树基础

- 声卡：`wm8960-audio`
- PCM 0：`HiFi wm8960-hifi-0`，支持playback/capture
- PCM 1：`HiFi-ASRC-FE`，支持playback/capture
- CPU DAI：SAI2
- Codec：I2C地址`0x1a`的WM8960
- SAI2 MCLK：12.288 MHz
- `CONFIG_SND_PCM=y`
- `CONFIG_SND_SOC=y`
- `CONFIG_SND_SOC_FSL_SAI=y`
- `CONFIG_SND_SOC_FSL_ASRC=y`
- `CONFIG_SND_IMX_SOC=y`
- `CONFIG_SND_SOC_WM8960=y`

现有设备树和ASoC驱动已经成功注册声卡，因此本轮不修改设备树、不重新编写WM8960驱动。

## 为什么暂时不使用libasound

BusyBox RootFS当前没有`arecord`、`amixer`和`libasound`。`audio_probe`直接包含
`<sound/asound.h>`并访问ALSA字符设备，运行时不依赖`libasound.so`。这样可以先完成底层
链路验证，避免把“声卡问题”和“用户库移植问题”混在一起。

这不是绕过ALSA：`/dev/snd/*`、`SNDRV_CTL_IOCTL_*`和`SNDRV_PCM_IOCTL_*`就是ALSA内核
提供给用户空间的正式UAPI。

## Ubuntu交叉编译

将本目录内容放到：

```text
/home/book/Workspace/linux/nfs/project/audio_capture
```

执行：

```sh
cd /home/book/Workspace/linux/nfs/project/audio_capture
make clean
make info
make

file audio_probe
arm-linux-gnueabihf-readelf -h audio_probe | grep -E 'Class|Data|Machine'
```

预期`audio_probe`为ARM、ELF32、little-endian、EABI5可执行文件。链接命令中不需要
`-lasound`。

如果编译器报告找不到`sound/asound.h`，执行：

```sh
arm-linux-gnueabihf-gcc -print-sysroot
find "$(arm-linux-gnueabihf-gcc -print-sysroot)/usr/include" \
    -path '*/sound/asound.h' -print
```

此时先把完整输出发回来，不要手工复制内核私有头文件。

## 开发板验证

```sh
cd /home/sun/nfs/project/audio_capture

./audio_probe --help

cat /proc/asound/cards
cat /proc/asound/pcm
ls -l /dev/snd

./audio_probe /dev/snd/controlC0 /dev/snd/pcmC0D0c \
    > /tmp/audio_r1.log 2>&1

echo "exit_code=$?"
cat /tmp/audio_r1.log

dmesg | grep -Ei 'wm8960|asoc|sai|asrc|audio|xrun|underrun|overrun|oops' | tail -n 100
```

## 判定标准

通过时应满足：

- 最后一行出现`[PASS]`，退出码为0。
- Card name或long name能识别`wm8960-audio`。
- 至少枚举到一个capture endpoint。
- 选中的`pcmC0D0c`方向为capture。
- 支持`RW_INTERLEAVED`和`S16_LE`。
- 通道区间包含2，采样率区间包含48000。
- 组合测试明确报告`48k/S16_LE/2ch RW_INTERLEAVED: supported`。
- mixer列表存在，并能看到带`*`标记的Capture/ADC/Input/Mic/Boost/ALC候选控件。
- `dmesg`没有Oops、DMA错误或ASoC报错。

`MMAP_INTERLEAVED`是否支持只做记录，不作为R1硬性失败条件。AUDIO-R2首先采用
`RW_INTERLEAVED`，因为它更适合用ALSA UAPI清晰演示period、短读和XRUN恢复。

## 失败时不要立即改设备树

- `ENOENT`：检查`/dev/snd`和devtmpfs。
- `EBUSY`：可能有其他进程占用PCM，先用`fuser /dev/snd/pcmC0D0c`检查；BusyBox没有
  `fuser`时可重启后只运行R1。
- `EACCES`：检查设备节点权限；当前以root运行通常不会出现。
- Card查询成功但PCM打开失败：优先检查节点选择和占用情况。
- 48k组合不支持：保留完整`HW_REFINE`输出，再判断是SAI、codec还是machine约束。
- mixer控件为空：检查`controlC0`是否真的是WM8960声卡，而不是先修改DTS。

## R1之后

R1实测通过后再生成AUDIO-R2：

```text
open capture PCM
→ HW_REFINE确定能力
→ HW_PARAMS提交48k/S16_LE/2ch与period/buffer
→ SW_PARAMS设置唤醒和停止阈值
→ PREPARE
→ READI_FRAMES循环采集
→ 处理短读、EINTR、EAGAIN和-EPIPE XRUN
→ DROP/HW_FREE
→ 写入标准WAV并统计peak/RMS
```
