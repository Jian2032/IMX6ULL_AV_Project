# VIDEO-R4连续帧隔离诊断

## 已排除的路径

- 单帧显示完全正确；
- `fb_test flip 300`以58.66次/秒运行，画面无错位滚动；
- flips从432增加到732，underflow和overflow均为0。

因此OV5640单帧格式、NEON转换、Framebuffer布局、LCDIF静态扫描和LCD连续翻页均可
冻结。后续对比发现：不触碰OV5640连接线时预览稳定，只有触碰连接线才出现整幅错位
滚动。故障被定位为DVP连接器/排线接触不稳定，尤其可能影响PCLK、HREF或VSYNC；它
不再作为LCDIF或预览软件缺陷处理。

## 录像方式

`v4l2_record`连续捕获60帧，但采集期间不操作LCD，也不写NFS。程序先在RAM中保存
36,864,000字节，STREAMOFF以后再写文件，避免网络文件系统速度干扰30fps采集。

开发板执行：

```sh
cd /home/sun/nfs/project/video_capture

./v4l2_record /dev/video0 60 record_640x480_yuyv_60f.raw
echo "exit_code=$?"
ls -l record_640x480_yuyv_60f.raw
```

必须为60帧、sequence gaps 0、约30fps、文件36,864,000字节、退出码0。

Ubuntu播放：

```sh
cd /home/book/Workspace/linux/nfs/project/video_capture

ffplay -f rawvideo \
  -pixel_format yuyv422 \
  -video_size 640x480 \
  -framerate 30 \
  record_640x480_yuyv_60f.raw
```

- PC播放也错位滚动：故障已存在于OV5640、排线或CSI输入侧；
- PC播放完全正常：继续检查实时预览时序。当前实机证据已经进一步指向物理连接。

处理时必须先断电，再重插或更换排线、检查锁扣和焊点，并对排线做应力固定；禁止
带电插拔摄像头。
