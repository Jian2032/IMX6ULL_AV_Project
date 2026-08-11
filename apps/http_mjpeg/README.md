# HTTP MJPEG 服务

目标数据通路：

```text
OV5640 -> V4L2 MMAP -> YUYV -> JPEG -> HTTP multipart -> 浏览器/VLC
```

五轮实现：

- `STREAM-R1`：无第三方依赖的 TCP/HTTP 服务骨架、状态页和断线处理。
- `STREAM-R2`：YUYV 单帧 JPEG 编码、质量参数和编码性能统计。
- `STREAM-R3`：接入 V4L2 最新帧，完成单客户端 MJPEG 实时输出。
- `STREAM-R4/R4.1`：采集、编码和网络线程解耦，处理慢客户端与重连；
  R4.1补充YUYV宏像素级水平翻转，与已有垂直翻转共同完成180度方向校正。
- `STREAM-R5`：浏览器/VLC 兼容、FPS/码率统计、长稳测试和文档冻结。

`STREAM-R1`已经冻结：开发板到PC的TCP、HTTP请求解析、响应、客户端断开、
单请求自动退出和服务立即重启均已通过。当前进入`STREAM-R2`，准备ARM静态
JPEG编码器并验证640×480 YUYV单帧编码。

R1构建和测试记录见 [STREAM-R1.md](STREAM-R1.md)。

R2的静态 `libjpeg-turbo`、YUYV 4:2:2编码和性能验收见
[STREAM-R2.md](STREAM-R2.md)。

R2已完成PC视觉确认并正式冻结。当前实现为 `STREAM-R3`：复用
`../video_capture/av_video_capture` 完成V4L2实时采集、单客户端HTTP multipart
MJPEG及浏览器/VLC播放。R3.1使用可缓存packed-YUYV暂存区缩短V4L2 DMA buffer
持有时间，并分别统计copy、unpack、encode和send耗时；R3.2在同一次逐行暂存复制中
完成上下翻转，不改变行内像素顺序。设计、首次性能故障证据与
验收清单见 [STREAM-R3.md](STREAM-R3.md)。

R3.2已完成开发板与浏览器验收并冻结：方向、颜色、几何、断线重连和清理路径均通过。
串行流水线平均36.679ms并保留166个sequence gap，作为下一轮STREAM-R4线程解耦的
定量优化基线。

当前进入 `STREAM-R4`：新增可复用 `av_mjpeg_pipeline`，使用采集线程、编码线程、
三槽cacheable YUYV池和三槽完整JPEG池；持久网络线程只发送客户端私有JPEG副本。
主线程可在视频流活动期间继续响应 `/health` 和 `/status`，第二个流请求返回503，慢客户
端通过丢弃旧JPEG保持低延迟。设计、统计语义和分阶段验收命令见
[STREAM-R4.md](STREAM-R4.md)。

R4压力验收已经证明30fps采集、零driver gap、慢客户端跳帧隔离、第二路503和断线重连
均正常。R4.1在QBUF之后对cacheable YUYV槽执行宏像素级水平翻转，最终视觉回归确认
上下、左右、颜色、几何、动态画面和重连全部正确。最终实测采集30.017fps、编码20.906fps，
`driver_sequence_gaps/capture_timeouts/send_errors`均为0；STREAM-R4.1已冻结为第5天基线。
