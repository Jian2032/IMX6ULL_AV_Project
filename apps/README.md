# 用户空间应用

计划组件：

- `common/`：日志、时间、队列和公共错误处理。
- `video_capture/`：V4L2设备查询和MMAP采集。
- `local_preview/`：PXP/软件转换和LCD显示。
- `audio_capture/`：ALSA录音、WAV和电平统计。
- `http_mjpeg/`：JPEG编码和HTTP流。
- `av_terminal/`：最终多线程集成程序。

所有应用以BusyBox RootFS和GCC 4.9.4为兼容基线。

