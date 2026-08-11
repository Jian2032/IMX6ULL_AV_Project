# ALSA音频应用

## 当前阶段

`AUDIO-R1`已经通过Ubuntu交叉编译和开发板实测。`AUDIO-R2`已经完成5秒实机录音，
240000帧完整、XRUN为0、WAV结构正确；但PC分声道试听均无有效声音。`AUDIO-R2.1`
的两个mixer profile也均无可辨语音。`AUDIO-R2.2`定位到板载MIC物理连线与DTS DAPM
路由不一致；最小设备树补丁更新DTB后，10秒录音已能正常辨认人声，XRUN为0。

- `audio_probe.c`：不依赖libasound的ALSA UAPI只读诊断工具。
- `audio_capture.c`：48kHz双声道S16_LE采集、XRUN恢复、WAV保存和电平统计。
- `Makefile`：使用`arm-linux-gnueabihf-`交叉编译。
- `AUDIO-R1.md`：本轮原理、编译命令、板端命令和验收标准。
- `AUDIO-R2.md`：PCM状态机、首次录音命令和验收标准。
- `AUDIO-R2.1.md`：原理图与DTS路由差异、差分MIC profile及A/B测试步骤。
- `AUDIO-R2.2.md`：DAPM断路根因、DTS补丁、DTB更新与验收步骤。
- `av_audio_capture.h/.c`：AUDIO-R3可复用采集线程和环形缓冲。
- `audio_thread_test.c`：AUDIO-R3线程化采集与WAV验收程序。
- `AUDIO-R3.md`：生产消费模型、丢包策略、生命周期和实机命令。

R1只查询能力；R2临时启用Capture Switch，录音结束后恢复，并在停止DMA后写WAV。
原理图确认板载MIC接`LINPUT2/LINPUT1`，而当前DTS把Main MIC写成`RINPUT1/RINPUT2`。
机器驱动又会在未插耳机时禁用`Mic Jack`，导致真实左输入的板级DAPM路径断开。
实机已证明DTS修复有效。录音人声清晰但主观音量稍小；由于拍手峰值已达99.97%，
不继续增加模拟增益，留待后续数字增益/限幅阶段处理。

AUDIO-R3将已验证的`mainmic-route`作为可复用模块默认输入，并用独立producer线程
向16-slot环形缓冲交付PCM。首轮实机测试定位到非阻塞PCM停在`PREPARED`状态时，
先`poll()`再读会与尚未启动的DMA互相等待；R3.1已改为producer先显式`START`，
再进入poll/read循环。R3.1已通过10秒录音、Ctrl+C部分WAV、立即重启、mixer恢复和
零丢包/XRUN实机验收，AUDIO-R3正式冻结。

第3天全部代码的六轮交互学习已经完成，覆盖ALSA/ASoC、control与PCM、HW/SW参数、
WAV、DAPM设备树路由、producer/consumer ring、XRUN及退出生命周期。完整讲义见
`docs/learning/day3_alsa_complete.md`。

## 最终目标

- 查询和设置PCM参数。
- 采集48kHz、双声道、S16_LE。
- 正确处理period、buffer、短读和XRUN。
- 保存WAV。
- 计算峰值和RMS。
- 后续增加G.711网络音频。

## 设计选择

当前BusyBox RootFS没有`alsa-lib`、`arecord`和`amixer`。前两轮先直接使用Linux公开的
`<sound/asound.h>`和`/dev/snd/*`，以零运行库依赖完成底层学习与采集。后续是否移植
`alsa-lib/alsa-utils`，在基础录音通过后单独决定。
