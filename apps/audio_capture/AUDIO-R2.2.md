# AUDIO-R2.2：修正 ALPHA 板载 MIC 的 DAPM 路由

## A/B 结果与新结论

`mainmic-route` 和 `mainmic-gain` 都完成了 8 秒录音，XRUN 为 0，mixer 也已按预期
恢复。左声道 RMS 从 AUDIO-R2 的约 233 上升到约 2000，峰值接近满幅，但两个
WAV 均听不到人声。这说明增益带来的是无效输入噪声或振荡，不是有效麦克风信号。

进一步核对 Linux 4.1.15 机器驱动发现：

1. ALPHA V2.4 板载 MIC 物理连接为 `MIC+ -> LINPUT2`、`MIC- -> LINPUT1`。
2. 当耳机未插入时，`imx-wm8960.c` 会禁用 `Mic Jack` DAPM pin，启用 `Main MIC`。
3. 当前 DTS 却把 `LINPUT2/LINPUT3` 归给 `Mic Jack`，把 `RINPUT1/RINPUT2` 归给
   `Main MIC`。
4. 因此应用层仅写 WM8960 mixer 寄存器不够；板级 DAPM 图仍将真实的左输入
   连在被禁用的端点上。

本轮先修正 DTS，不再增加模拟增益。

## DTS 修正

项目补丁：

```text
config/device-tree/0001-audio-fix-alpha-main-mic-routing.patch
```

修正后的输入路由：

```dts
"LINPUT3", "Mic Jack",
"RINPUT2", "Mic Jack",
"LINPUT1", "Main MIC",
"LINPUT2", "Main MIC",
"Mic Jack", "MICB",
"Main MIC", "MICB",
```

`audio-routing` 描述的是电路板上的固定连线，而 `mainmic-route` profile 描述的是
WM8960 内部可编程开关。两者必须同时正确。

## Ubuntu 打补丁与编译 DTB

先保存当前 DTS，再使用 `--dry-run` 检查：

```sh
cd /home/book/Workspace/nxp_linux/linux-imx-rel_imx_4.1.15_2.1.0_ga

cp arch/arm/boot/dts/imx6ull-alientek-emmc.dts \
   arch/arm/boot/dts/imx6ull-alientek-emmc.dts.before_audio_r2_2

patch --dry-run -p1 < \
  /home/book/Workspace/linux/nfs/project/audio_capture/0001-audio-fix-alpha-main-mic-routing.patch

patch -p1 < \
  /home/book/Workspace/linux/nfs/project/audio_capture/0001-audio-fix-alpha-main-mic-routing.patch

make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- \
  imx6ull-alientek-emmc.dtb

cp arch/arm/boot/dts/imx6ull-alientek-emmc.dtb \
  /home/book/Workspace/linux/nfs/project/audio_capture/
```

若 `patch --dry-run` 失败，不要执行正式 `patch`，先返回完整错误。

## 更新启动 DTB

先确认 U-Boot 实际加载的文件名和 eMMC FAT 分区内容，不要盲目覆盖：

```sh
printenv fdt_file
printenv fdtfile
printenv bootcmd
```

若 U-Boot 确认从 eMMC 第 1 分区读取 `imx6ull-alientek-emmc.dtb`，在 Linux 中可按以下
方式先备份再替换：

```sh
mkdir -p /mnt/boot
mount /dev/mmcblk1p1 /mnt/boot
ls -l /mnt/boot

cp /mnt/boot/imx6ull-alientek-emmc.dtb \
   /mnt/boot/imx6ull-alientek-emmc.dtb.before_audio_r2_2
cp /home/sun/nfs/project/audio_capture/imx6ull-alientek-emmc.dtb \
   /mnt/boot/imx6ull-alientek-emmc.dtb
sync
umount /mnt/boot
reboot
```

如果 U-Boot 读取的不是该路径或文件名，以 `printenv` 和 FAT 分区实际内容为准。

## 重启后验证

先确认声卡正常，再只跑不加 13 dB 的 profile：

```sh
cat /proc/asound/cards
cat /proc/asound/pcm

cd /home/sun/nfs/project/audio_capture
./audio_capture /dev/snd/pcmC0D0c 10 /tmp/mainmic_dts.wav \
    /dev/snd/controlC0 mainmic-route

echo "capture_exit=$?"
cp /tmp/mainmic_dts.wav /home/sun/nfs/project/audio_capture/
sync
```

录音时靠近板载 MIC 连续说话，中间拍两次手，便于从波形和听感中识别真实信号。
PC 端只播放左声道：

```sh
ffplay -nodisp -autoexit -af "pan=mono|c0=c0" mainmic_dts.wav
```

验收标准：

- 能辨认语音和拍手声；
- `xruns=0`；
- 左声道 RMS 会随安静/说话而变化，而不是只有持续噪声；
- 不要先测 `mainmic-gain`，因为上轮峰值已达 99.97%。

## 若修正 DTS 后仍无人声

下一步才进入硬件层测量，在录音运行期间测量 MICBIAS 对 AGND 的直流电压：

- 接近 0 V：MICBIAS 没有上电，继续查 DAPM/寄存器。
- 有约 2.1 V 或 3.0 V：偏置基本正常，检查 MIC、C40/C41 和焊接。

不要在不确定测试点时用表笔直接接触 WM8960 密脚，优先使用图纸中的
MICBIAS 网络或麦克风附近可见元件端点。

## 实机验收结果

DTB 替换并重启后，使用 `mainmic-route` 完成 10 秒录音：

```text
frames        = 480000
PCM bytes     = 1920000
WAV bytes     = 1920044
xruns         = 0
elapsed       = 10.006 s
measured rate = 47972.68 frames/s
left peak     = 32758, 99.97%
left RMS      = 1882.12, 5.74%
right RMS     = 17.04, 0.05%
```

PC 左声道试听已能正常辨认人声，仅主观音量稍小。这证明修复的是真实
根因，而不是通过增益掩盖问题。

全局 RMS 约为 `20 * log10(1882.12 / 32768) = -24.8 dBFS`，对未做压缩的语音仍属
可用范围。由于峰值已接近 0 dBFS，不固化 `mainmic-gain` 的 +13 dB 档，否则拍手或
近距离说话会明显削波。后续网络音频阶段优先用可控数字增益/限幅处理主观音量，
保留当前模拟采集余量。
