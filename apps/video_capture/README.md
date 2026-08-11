# V4L2视频采集与LCD预览

本目录按五轮完成第2天的“摄像头采集 → LCD本地预览”数据通路。应用仅依赖
Linux V4L2、fbdev、pthread和C库，兼容BusyBox RootFS与GCC 4.9.4。

## 五轮实现

| 轮次 | 功能 | 实机结论 |
|---|---|---|
| VIDEO-R1 | 自写V4L2只读诊断工具 | 确认`mx6s-csi`、YUYV、分辨率、帧率及MMAP能力 |
| VIDEO-R2 | 640×480 YUYV、四缓冲MMAP采集 | 120帧、30.02fps、驱动序号缺口0 |
| VIDEO-R3 | NEON YUYV→RGB565及LCD居中预览 | 转换19.82ms，采集30.01fps |
| VIDEO-R4 | fbdev双缓冲和帧完成中断同步翻页 | page 0/1交替，LCDIF异常计数0 |
| VIDEO-R5 | 采集/显示线程解耦、最新帧策略、信号退出 | 54,000帧通过；1×DVP驱动修复视觉错位 |

PXP硬件转换属于后续优化项；当前CPU NEON路径用于建立可解释、可测量的基线。

## 当前开发轮次：VIDEO-R5稳定性验收

- `v4l2_probe.c`：R1只读诊断工具。
- `v4l2_capture.c`：R2单帧保存和连续采集验证程序。
- `av_video_capture.h/.c`：可复用V4L2 MMAP采集层。
- `v4l2_preview.c`：R5线程化低延迟LCD预览程序。
- `v4l2_record.c`：先录入RAM再落盘的连续YUYV诊断工具。
- `ov5640_diag.c`：读取OV5640 DVP关键寄存器，并受限控制内部彩条和输出驱动强度。
- `VIDEO-R1.md`～`VIDEO-R5.md`：每轮设计、测试与验收记录。
- `docs/learning/day2_v4l2_complete.md`：最终代码、内核调用链、故障定位和综合答案讲义。

R4实测流水线平均33.96ms，略高于30fps的33.33ms周期，因此同步单线程程序出现7个
V4L2序号缺口。R5由独立采集线程快速执行`DQBUF → 私有帧复制 → QBUF`，显示线程只取
三槽帧池中的最新帧。显示来不及时可以主动丢弃过期帧，但CSI采集序号必须连续。

此前触碰OV5640 DVP连接线可以复现错位，曾将其归因于接触不稳定。但54,000帧固定连接
长测仍观察到错位，所以该结论不足以关闭问题。三级快照的YUYV、转换后RGB565和最终
Framebuffer三张图均已确认错位；Framebuffer中心640×480区域与转换后RGB565逐像素
相同。这证明错误最迟已存在于原始YUYV，LCDIF、居中blit和NEON转换只是忠实传递错误
输入。当前故障边界是OV5640输出、DVP物理信号或`mx6s-csi`采样/同步。

## 快速构建

```sh
cd /home/book/Workspace/linux/nfs/project/video_capture
make clean
make info
make
file v4l2_probe v4l2_capture v4l2_preview v4l2_record ov5640_diag
```

开发板执行：

```sh
cd /home/sun/nfs/project/video_capture
./v4l2_preview /dev/video0 /dev/fb0 300
echo "exit_code=$?"
```

300帧实测已经达到`driver_sequence_gaps=0`、`stale_frames_dropped=0`、采集30.01fps、
显示29.90fps、`[PASS]`和退出码0。线程版从可缓存私有YUYV槽读取，NEON转换平均耗时
由R4约19.8ms降到8.32ms，完整显示流水线由约34.0ms降到19.82ms。Ctrl+C在268帧
处干净退出且退出码0，随后立即重启完成60帧并输出`[PASS]`。54,000帧长测的控制指标
也全部通过，但视觉错位仍出现，因此模块尚未冻结，暂不开始最终代码学习。

为捕获故障现场，`v4l2_preview`新增可选的`snapshot-prefix`参数。提供该参数后，程序在
退出时保存最后显示帧的原始YUYV、NEON转换结果和可见Framebuffer；默认不提供参数时
没有额外逐帧复制开销。

下一步使用OV5640内部彩条隔离传感器ISP与DVP/CSI。必须先停止所有采集进程，再执行：

```sh
./ov5640_diag /dev/i2c-1 status
./ov5640_diag /dev/i2c-1 pattern on
./v4l2_preview /dev/video0 /dev/fb0 300 sensor_tpg_static
./ov5640_diag /dev/i2c-1 pattern rolling
./v4l2_preview /dev/video0 /dev/fb0 300 sensor_tpg_rolling
./ov5640_diag /dev/i2c-1 pattern off
```

若内部彩条也逐行错位，继续检查PCLK采样沿、HREF/VSYNC同步和DVP信号完整性；若内部
彩条完全稳定而自然画面错位，再检查OV5640 ISP/缩放和输出时序配置。

实测静态彩条和滚动彩条均正确后，不立即修改CSI采样沿。`v4l2_preview`最后增加可选
`fps`参数（只允许15或30），用于比较同一自然场景在约28MHz和56MHz DVP PCLK下的
表现：

```sh
./v4l2_preview /dev/video0 /dev/fb0 300 natural_30 30
./v4l2_preview /dev/video0 /dev/fb0 300 natural_15 15
```

分别在摄像头完全静止和受控水平移动时比较。15fps恢复而30fps错误，偏向DVP电气裕量；
移动时15fps的倾斜反而更重，偏向OV5640滚动快门效应。

下降沿CSI采样A/B已证明下降沿直接乱码、上升沿正确，因此恢复上升沿。随后增加受限的
OV5640 DVP输出驱动强度命令，只修改`0x302c[7:6]`并保留低6位：

```sh
./ov5640_diag /dev/i2c-1 drive 3
./ov5640_diag /dev/i2c-1 drive 4
./ov5640_diag /dev/i2c-1 drive 2
```

执行时必须停止全部V4L2进程；按3×、4×逐级测试30fps，结束后恢复驱动默认的2×。
