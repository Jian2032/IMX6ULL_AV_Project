# 音视频终端主程序

该目录用于第6天音视频集成。主程序只负责编排模块、统一状态和管理生命周期，不重复实现已验收的V4L2、ALSA、JPEG和fbdev底层逻辑。

## 五轮计划

1. `AV-R1`：同一进程并行运行V4L2/JPEG与ALSA，验证统计、时间戳和退出。
2. `AV-R2`：同一视频producer同时向LCD预览和JPEG编码分发，不重复打开`/dev/video0`。
3. `AV-R3`：接入HTTP MJPEG服务和控制端点。
4. `AV-R4`：统一音视频时基、健康统计和状态快照。
5. `AV-R5`：长时稳定性、慢客户端、Ctrl+C、故障注入和最终冻结。

## 当前版本

`AV-R1`已完成ARM交叉编译、30秒并行采集、Ctrl+C和立即重启验收，已冻结。

`AV-R2.3`已通过10秒实机统计、Ctrl+C干净退出、立即重启和LCD视觉验收：CSI为
30.02fps且`driver_gaps=0`，LCD约15fps，后台JPEG约5fps，ALSA零丢包零XRUN，
LCDIF零underflow/overflow；方向、镜像、颜色、几何、动态画面和居中黑边全部正常，
AV-R2正式冻结。

- `av_mjpeg_pipeline_copy_latest_raw()`：从唯一视频producer复制最新完整YUYV帧。
- `av_lcd_preview.*`：私有YUYV快照、NEON RGB565转换、居中blit和fbdev双缓冲翻页。
- `av_terminal.c`：统一启动LCD、JPEG和ALSA，每秒输出三条数据路径的状态。

AV-R2编译与开发板验收见[AV-R2.md](AV-R2.md)。

`AV-R3`已经完成HTTP控制端点、单客户端MJPEG、视觉、Ctrl+C和立即重启验收并冻结，
详细证据见[AV-R3.md](AV-R3.md)。

`AV-R4`已经完成统一主时基、快照序号、健康位图、活动流、Ctrl+C和立即重启验收并
冻结，详细证据见[AV-R4.md](AV-R4.md)。

最终`AV-R5`已经冻结：保持30/15/5fps与48kHz预算，新增进程资源遥测和只影响健康状态的
安全合成故障。30分钟长稳、慢客户端隔离、健康降级、故障后恢复和干净退出均已通过；
第7天只把同一冻结版本扩展到一小时正式交付验收。编译和证据见[AV-R5.md](AV-R5.md)。

AV-R1复用的基础层包括：

- `../http_mjpeg/av_mjpeg_pipeline.*`：V4L2采集、方向校正与JPEG编码。
- `../audio_capture/av_audio_capture.*`：ALSA采集、用户ring与mixer生命周期。
- `av_terminal.c`：音频monitor消费者、统一统计、信号和退出顺序。

历史验收记录见[AV-R1.md](AV-R1.md)。
