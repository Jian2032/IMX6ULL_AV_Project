# AV-R5：最终长稳、慢客户端和故障恢复验收

## 本轮目标

AV-R5是第6天集成模块的最终冻结轮次。它不改变已经通过的媒体路径和预算，只增加：

- `getrusage(RUSAGE_SELF)`资源遥测：峰值RSS、用户/内核CPU时间和上下文切换数；
- 可选的合成健康故障，用于确定性验证`/health`、`/status`和最终退出判据；
- 30分钟稳定性、慢客户端隔离、Ctrl+C和故障后正常重启验收；
- 第7天继续使用同一冻结代码完成至少1小时正式交付长稳。

合成故障只把指定bit并入`health_flags`，不会操作硬件、停止线程、伪造媒体数据或修改
真实模块统计。`driver_sequence_gaps`、`audio_xruns`等原始计数仍保持真实值。

## 命令行

```text
./av_terminal [seconds] [video] [pcm] [control] [fb] [port]
              [fault-mode] [fault-after-seconds]
```

`fault-mode`支持：

```text
none            正常模式，默认值
video-gap       合成0x00000002
video-timeout   合成0x00000004
audio-xrun      合成0x00000020
audio-drop      合成0x00000040
http-send       合成0x00000400
```

## Ubuntu交叉编译

```bash
cd /home/book/Workspace/linux/nfs/project/av_terminal

make clean
make info
make

file av_terminal
arm-linux-gnueabihf-readelf -h av_terminal | grep -E 'Class|Data|Machine'
grep -n 'AV-R5' av_terminal.c
```

## 第一阶段：正常模式和资源基线

开发板运行：

```sh
cd /home/sun/nfs/project/av_terminal

./av_terminal 0 \
    /dev/video0 \
    /dev/snd/pcmC0D0c \
    /dev/snd/controlC0 \
    /dev/fb0 \
    8080
```

Windows读取两次状态并记录：

```powershell
curl.exe http://192.168.1.50:8080/status
Start-Sleep -Seconds 60
curl.exe http://192.168.1.50:8080/status
```

正常模式应显示：

```text
version=AV-R5
health_flags=0
fault_injection=none
fault_triggered=false
```

`peak_rss_kb`是Linux进程历史峰值，不要求下降；长测中应进入平台期而不是持续线性增长。

## 第二阶段：慢客户端隔离

不要同时打开浏览器。在Windows PowerShell窗口A运行：

```powershell
curl.exe --limit-rate 50k --max-time 30 `
  http://192.168.1.50:8080/stream.mjpg `
  -o NUL
```

在窗口B于慢流活动期间运行：

```powershell
curl.exe http://192.168.1.50:8080/health
curl.exe http://192.168.1.50:8080/status
```

预期控制端点立即响应、`health_flags=0`，CSI仍约30fps、ALSA无drop/XRUN；
`client_frames_skipped`允许增长，`average_send_ms`允许显著变大。这证明慢socket只阻塞
私有network worker，不阻塞采集、LCD、JPEG编码或音频。

慢curl以28超时属于测试主动结束。随后浏览器应能重新连接并正常显示。

## 第三阶段：30分钟长稳

保持正常模式运行至少1800秒，每5～10分钟保存一次`/status`。最终要求：

- 所有快照`health_flags=0`；
- CSI driver gap/timeout、ALSA drop/XRUN、HTTP send error均为0；
- 视频约30fps、LCD约15fps、JPEG约5fps、音频约48kHz；
- `peak_rss_kb`热身后没有持续线性增长；
- A/V head/span有界振荡，不持续向单一方向发散；
- 浏览器断线后可重新连接，LCD始终正常。

按Ctrl+C后保存最终统计并确认退出码0。

## 第四阶段：健康故障注入与恢复

先运行15秒合成video-gap：

```sh
./av_terminal 15 \
    /dev/video0 \
    /dev/snd/pcmC0D0c \
    /dev/snd/controlC0 \
    /dev/fb0 \
    8080 \
    video-gap 3

echo "fault_exit=$?"
```

3秒后Windows应观察到：

```powershell
curl.exe -i http://192.168.1.50:8080/health
curl.exe http://192.168.1.50:8080/status
```

预期`/health`返回503，状态为`system=degraded`、`health_flags=2`、
`fault_injection=video-gap`、`fault_triggered=true`。媒体原始统计仍正常。15秒结束时程序
应输出`[FAIL]`并返回1，这是验收预期，不是程序崩溃。

随后不带fault参数立即恢复：

```sh
./av_terminal 3 \
    /dev/video0 \
    /dev/snd/pcmC0D0c \
    /dev/snd/controlC0 \
    /dev/fb0 \
    8080

echo "recovery_exit=$?"
```

预期`health_flags=0`、`fault_injection=none`、`[PASS]`且`recovery_exit=0`。

## 已完成实机结果

AV-R5已经完成第6天冻结验收：

- 30分钟运行时间1800.262秒，所有周期快照和最终快照健康位为0；
- 视频采集54,037帧、30.02fps，`driver_gaps=0`、`timeouts=0`；
- JPEG编码8,989帧、4.99fps，LCD显示26,949帧、14.97fps；
- 音频86,410,240帧、48,000 frame/s，drop/XRUN/sequence gap全部为0；
- HTTP活动流发送8,924帧，`send_errors=0`，平均发送约0.47ms；
- 运行期`peak_rss_kb`进入约4652KiB平台，没有持续线性增长；
- 慢客户端50KiB/s测试只增加客户端skip和send耗时，视频与音频生产链保持健康；
- `video-gap`、`video-timeout`、`audio-xrun`、`audio-drop`和`http-send`故障位均能
  产生预期非零健康结果，正常模式随后恢复PASS。

## 第7天一小时正式结果

冻结代码已完成3600.665秒正式长稳：

- 最终健康位`0x00000000`，程序和外层脚本均输出`[PASS]`，程序返回0；
- 视频采集108,080帧、30.02fps，编码17,977帧、4.99fps，策略丢弃90,103帧，严格满足
  `108080 = 17977 + 90103`；driver gap和timeout均为0；
- LCD显示53,908帧、约14.97fps，`failed=0`；
- 音频采集与消费均为172,828,672帧，drop、XRUN、queue和sequence gap均为0；
- HTTP发送17,942帧，`send_errors=0`，平均JPEG 35,059.46字节，累计631,822,258字节，
  估算payload为1.400Mbit/s；
- 最终A/V head为-1.522ms，span为-12.126ms，没有持续单向发散；
- Windows在0～3500秒完成7次状态采样，健康位均为0，RSS每次均为4592KiB；
- 进程最终历史峰值RSS为5520KiB，累计用户/系统CPU为2585.610/99.040秒，折合约74.6%单核。

一小时日志中的`stream_sessions=1`只证明一条流持续稳定；随后已用120秒短测补齐重连证据：
两次独立curl会话均主动在20秒超时断开，最终`accepted=completed=streams=2`、`send_errors=0`，
健康位0、程序及脚本均PASS、退出码0。外部同屏时间码已记录10个端到端样本：最小350ms、
平均489.6ms、中位数516ms、最大606ms，nearest-rank P95为606ms。测量使用Edge
151.0.4129.72、手机60fps和100Mbps有线直连。第7天全部技术验收已经完成，只剩开发板
原始证据归档。

第7天只增加`average_jpeg_bytes`、`estimated_mjpeg_mbps`和`http_bytes_sent`只读遥测，
没有改变buffer、线程、编码或发送策略；浏览器真实端到端延迟使用外部时间码同屏拍摄法测量。
