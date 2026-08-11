# AUDIO-R2.1：板载差分麦克风路由A/B诊断

## 为什么增加这一轮

AUDIO-R2已经证明以下数字通路正确：

```text
WM8960 ADC数字输出 → SAI2 → SDMA → ALSA PCM → WAV
```

5秒采集得到240000帧、XRUN为0、WAV为960044字节，但PC整段及左右声道试听均无有效
人声。Ubuntu正弦波播放正常，因此问题位于开发板的模拟麦克风输入路径。

ALPHA V2.4底板原理图显示：

```text
MIC+ → C40 → LINPUT2（左PGA同相输入）
MIC- → C41 → LINPUT1（左PGA反相输入）
MICBIAS → 板载MIC偏置
```

当前设备树却把`Main MIC`连接到`RINPUT1/RINPUT2`，与实际连线不一致。R1 mixer快照还
显示`Left Boost Mixer LINPUT2 Switch=0`、`Left Input Mixer Boost Switch=0`，所以
`LINPUT2`没有作为差分PGA同相输入，左PGA输出也没有接到ADC前Boost Mixer。

## 三个profile

`audio_capture`新增最后一个参数：

- `baseline`：保持AUDIO-R2行为，只临时解除Capture mute。
- `mainmic-route`：只修复板载MIC差分路由，不增加增益。
- `mainmic-gain`：在相同路由上，把MIC PGA到Boost Mixer的增益从0改为1，即增加13dB。

`mainmic-route`设置：

```text
Left Input Boost Mixer LINPUT2 Volume = 0  # 禁止把MIC+当独立line输入
Left Input Boost Mixer LINPUT3 Volume = 0
Left Boost Mixer LINPUT1 Switch       = 1  # LMN1，选择MIC-
Left Boost Mixer LINPUT2 Switch       = 1  # LMP2，选择MIC+
Left Boost Mixer LINPUT3 Switch       = 0  # LMP3关闭
Left Input Mixer Boost Switch         = 1  # LMIC2B，PGA送入Boost/ADC
Capture Switch                        = 1,1，最后解除mute
```

`mainmic-gain`额外设置：

```text
Left Input Boost Mixer LINPUT1 Volume = 1  # LMICBOOST=+13dB
```

程序在每次写入前保存原值，读回验证；退出或中途失败时按相反顺序恢复，因此先恢复
`Capture Switch=0,0`使输入静音，再恢复增益和路由。

## Ubuntu编译

```sh
cd /home/book/Workspace/linux/nfs/project/audio_capture

make clean
make

./audio_capture --help
```

帮助信息应列出`baseline`、`mainmic-route`和`mainmic-gain`。

## 开发板A/B测试

测试时持续对着板载MIC清楚说话，不要敲连接器。

### A：只修路由

```sh
cd /home/sun/nfs/project/audio_capture

./audio_capture /dev/snd/pcmC0D0c 8 /tmp/mainmic_route.wav \
    /dev/snd/controlC0 mainmic-route

echo "route_exit=$?"
```

### B：路由加13dB

```sh
./audio_capture /dev/snd/pcmC0D0c 8 /tmp/mainmic_gain.wav \
    /dev/snd/controlC0 mainmic-gain

echo "gain_exit=$?"

cp /tmp/mainmic_route.wav /home/sun/nfs/project/audio_capture/
cp /tmp/mainmic_gain.wav /home/sun/nfs/project/audio_capture/
sync
```

两次结束后重新运行R1工具，检查所有控制已经恢复：

```sh
./audio_probe /dev/snd/controlC0 /dev/snd/pcmC0D0c |
grep -E 'Capture Switch|Left Input|Left Boost'
```

## PC试听

```sh
cd /home/book/Workspace/linux/nfs/project/audio_capture

ffplay -nodisp -autoexit -af "pan=mono|c0=c0" mainmic_route.wav
ffplay -nodisp -autoexit -af "pan=mono|c0=c0" mainmic_gain.wav
```

验收比较：

- `xruns=0`且退出码0；
- route/gain两次均完整恢复mixer；
- route版本左声道peak/RMS应明显高于AUDIO-R2基线；
- gain版本RMS应进一步提高，理论上约13dB，但不能出现持续削波；
- 至少一个版本能清楚听见人声。

若本轮验证成功，再修改DTS的`audio-routing`并将正确输入profile固化；若仍无声，再在采集
运行期间测量MICBIAS电压并检查板载MIC硬件，不继续盲目增加增益。
