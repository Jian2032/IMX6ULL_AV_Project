# AV-R3：共享摄像头的HTTP MJPEG音视频终端

## 本轮目标

AV-R3在已冻结的AV-R2.3上增加HTTP服务，但不增加第二个摄像头实例：

```text
                         -> LCD latest-frame consumer (15 fps)
OV5640 -> CSI/V4L2 -> raw latest-frame pool
                         -> JPEG encoder (5 fps) -> HTTP network worker

WM8960 -> SAI/SDMA -> ALSA producer -> audio monitor
```

唯一capture线程仍独占`DQBUF/QBUF`。HTTP只调用
`av_mjpeg_pipeline_copy_latest_timeout()`复制完整JPEG，socket绝不引用CSI MMAP或共享
JPEG槽。

## 新增边界

- `av_http_server.*`封装`socket/bind/listen/accept`、HTTP解析和multipart MJPEG。
- accept线程负责`/`、`/health`、`/status`等短请求。
- network线程独占一个`/stream.mjpg`连接；第二个流请求返回503。
- network线程先复制到私有JPEG buffer，再调用可能阻塞的`send()`。
- 带超时的JPEG读取使HTTP能够先于producer停止，即使摄像头不再产帧也不会无限等待。
- `/status`通过回调统一快照HTTP、V4L2/JPEG、LCD和ALSA统计。

AV-R3继续使用单核预算：CSI 30fps、LCD 15fps、JPEG/HTTP 5fps。这里优先保证CSI
零driver gap、音频零XRUN和系统确定性；提升HTTP帧率不是本轮验收条件。

## 生命周期顺序

启动顺序：

```text
create video/LCD/audio/HTTP
-> start video
-> start LCD
-> start audio producer + monitor
-> start HTTP accept thread
```

停止顺序：

```text
stop/join HTTP socket consumers
-> stop ALSA producer
-> stop shared video producer
-> join audio monitor and LCD
-> release HTTP/LCD/audio/video objects
```

HTTP必须先停，因为它依赖JPEG producer；producer不能在网络线程仍可能读取时释放。

## Ubuntu交叉编译

```bash
cd /home/book/Workspace/linux/nfs/project/av_terminal

make clean
make info
make

file av_terminal
arm-linux-gnueabihf-readelf -h av_terminal | grep -E 'Class|Data|Machine'
grep -n 'AV-R3' av_terminal.c
```

预期为32位ARM EABI5，编译过程没有warning。

## 开发板启动

先保证`/dev/fb0`存在并解绑fbcon，然后确认没有旧的摄像头、音频或8080端口程序：

```sh
cd /home/sun/nfs/project/av_terminal

./av_terminal 0 \
    /dev/video0 \
    /dev/snd/pcmC0D0c \
    /dev/snd/controlC0 \
    /dev/fb0 \
    8080
```

`seconds=0`表示运行到Ctrl+C。开发板应打印index和stream URL，LCD继续显示居中的
640x480动态画面。

## Windows验收

PowerShell先检查控制端点：

```powershell
curl.exe http://192.168.1.50:8080/health
curl.exe http://192.168.1.50:8080/status
```

浏览器打开：

```text
http://192.168.1.50:8080/
```

保持浏览器视频活动时，再次执行`/health`和`/status`，二者必须立即响应。再执行：

```powershell
curl.exe -i --max-time 3 http://192.168.1.50:8080/stream.mjpg
```

第二路流应得到503，原浏览器画面不受影响。

## 验收标准

- LCD与浏览器均为正确180度方向，颜色和几何正确，无动态错行。
- `/health`返回`ok`，`/status`显示`version=AV-R3`和`system=running`。
- `capture_fps`约30，`driver_sequence_gaps=0`、`capture_timeouts=0`。
- LCD约15fps且`failed=0`；HTTP约5fps属于本轮预算。
- `audio_xruns=0`、`audio_dropped_frames=0`，采集与消费最终相等。
- 正常网络下`send_errors=0`；第二路流增加`rejected_streams`而不破坏第一路。
- Ctrl+C输出干净`[STOP]`，退出码0；随后能够立即重新运行。

结束后保存以下信息：

```sh
echo "av_r3_exit=$?"
cat /sys/bus/platform/devices/21c8000.lcdif/lcdif_stats
dmesg | grep -Ei \
  'csi|lcdif|wm8960|sai|sdma|xrun|overrun|overflow|underflow|warning|oops' | \
  tail -n 100
```

## 最终实机验收记录（2026-08-10）

AV-R3已经完成并冻结：

- 立即重启3秒测试：视频采集90帧、JPEG 16帧，`driver_gaps=0`、
  `capture_timeouts=0`；LCD显示45帧且`failed=0`；音频采集与消费均为
  145,408帧，零drop、零XRUN、最终ring为空；进程输出`[PASS]`并返回0。
- 随后的长期并发测试运行约26.3秒：视频采集790帧、JPEG 133帧，LCD显示
  394帧；音频采集与消费均为1,264,640帧；HTTP完成1个MJPEG session、发送
  37帧，`send_errors=0`；Ctrl+C输出干净`[STOP]`。
- Windows侧`/health`与`/status`正常，浏览器流保持约5fps；用户实机确认LCD、
  浏览器上下方向、左右方向、颜色、几何和动态画面全部正常。

`raw_frames_dropped`与`lcd_source_frames_skipped`是单核预算下的最新帧策略统计，
不是CSI驱动丢帧；正式判据`driver_sequence_gaps=0`始终满足。
