# VIDEO-R5：线程化低延迟本地预览

## 目标

R4把采集、NEON转换、显存复制和LCD翻页串在同一个线程中。实测平均耗时为：

- NEON转换：19.83ms；
- framebuffer复制：1.33ms；
- 帧边界翻页等待：12.79ms；
- 总计：33.96ms。

30fps输入每33.33ms产生一帧，因此R4偶尔来不及把V4L2 MMAP缓冲归还CSI，300帧出现
7个sequence缺口。R5通过生产者/消费者模型保证采集循环不再等待LCD。

```text
采集线程（唯一V4L2所有者）
    DQBUF → 复制YUYV到三槽私有帧池 → QBUF → 发布READY
                                              │
                                              ▼
显示线程                              选择最新READY帧
    NEON YUYV→RGB565 → 隐藏页blit → FBIOPAN_DISPLAY → 释放帧槽
```

## 缓冲区所有权

V4L2 MMAP缓冲在`DQBUF`后暂时属于应用，但调用`QBUF`后所有权立即归还驱动，CSI可能随时
向其中写入下一帧。因此不能让显示线程持有MMAP指针并在另一个线程执行`QBUF`。

R5分配三个614,400字节的私有YUYV槽，每个槽具有以下状态：

- `FREE`：可以被采集线程使用；
- `WRITING`：采集线程正在复制，其他线程不可访问；
- `READY`：完整帧已经发布，显示线程可以选择；
- `READING`：显示线程正在转换，采集线程不可覆盖。

当多个`READY`帧积压时，显示线程只取序号最新的一帧，旧帧计入
`stale_frames_dropped`并立即释放。这是实时预览的低延迟策略，不是驱动丢帧。

## 并发与退出

- mutex保护槽状态、统计和停止标志；
- condition variable让没有可用帧时的显示线程休眠；
- 采集线程使用100ms短超时检查退出请求；连续2秒没有摄像头帧则报错退出；
- `SIGINT`和`SIGTERM`只设置`sig_atomic_t`标志，实际的join、STREAMOFF、munmap和close
  仍在普通线程上下文执行；
- 退出顺序固定为：请求采集线程停止 → `pthread_join` → `VIDIOC_STREAMOFF` → 释放
  私有帧池和V4L2/fbdev资源。

## 构建

将本目录更新后的文件同步到Ubuntu NFS目录，然后执行：

```sh
cd /home/book/Workspace/linux/nfs/project/video_capture
make clean
make info
make
file v4l2_preview
arm-linux-gnueabihf-readelf -h v4l2_preview | grep -E 'Class|Data|Machine'
```

`make info`必须显示`THREAD_FLAGS=-pthread`，编译应无warning/error，产物应为ARM ELF32、
little-endian、EABI5。

## 300帧实机验证

先确认OV5640连接线已经断电重插并固定，再加载LCD-R5：

```sh
cd /home/sun/nfs/project/lcdif_fb
insmod av_lcdif_fb.ko

for v in /sys/class/vtconsole/vtcon*; do
    if grep -qi "frame buffer" "$v/name"; then
        echo 0 > "$v/bind"
    fi
done

cd /home/sun/nfs/project/video_capture
cat /proc/interrupts | grep -Ei 'csi|lcdif|21c4000|21c8000'
./v4l2_preview /dev/video0 /dev/fb0 300
echo "exit_code=$?"
cat /proc/interrupts | grep -Ei 'csi|lcdif|21c4000|21c8000'
dmesg | grep -Ei 'csi|lcdif|overflow|underflow|hresponse|warning|oops' | tail -n 100
```

验收标准：

- 标题为`VIDEO-R5 threaded low-latency LCD preview`；
- `converter`为`ARM NEON, 8 pixels/iteration`；
- page 0和page 1持续交替；
- `driver_sequence_gaps=0`且`capture fps`接近30；
- `stale_frames_dropped`允许为少量非零，数值应与显示速度不足相符；
- 输出`[PASS]`且退出码为0；
- 不触碰连接线时画面颜色、位置、运动方向均正确；
- 内核无CSI overflow、LCDIF underflow/overflow、WARNING或Oops。

## 信号退出验证

```sh
./v4l2_preview /dev/video0 /dev/fb0 54000
# 运行数秒后按Ctrl+C
echo "exit_code=$?"
```

应输出`[STOP] Signal requested a clean shutdown`并返回0；随后应能立即再次启动预览，
证明V4L2缓冲、线程和设备文件均已完整释放。

## 稳定性验证

短测全部通过后执行约30分钟：

```sh
./v4l2_preview /dev/video0 /dev/fb0 54000
echo "exit_code=$?"
```

最终必须为`driver_sequence_gaps=0`、`[PASS]`和退出码0。若显示速度略低于30fps，允许
`stale_frames_dropped`随运行时间增长；它保证画面始终接近摄像头最新时刻，而不会形成
越来越长的历史帧队列。

## 300帧实机证据

- 采集301帧，sequence从5964连续到6264，`driver_sequence_gaps=0`、`timeouts=0`；
- 采集30.01fps，显示300帧、29.90fps，`stale_frames_dropped=0`；
- NEON转换平均8.32ms、最大10.48ms；
- framebuffer复制平均1.26ms、最大1.50ms；
- 翻页等待平均10.23ms、最大23.78ms；
- 完整显示流水线平均19.82ms、最大33.38ms；
- CSI IRQ从12088增加到12692，LCDIF IRQ从3746增加到4047；
- 输出`[PASS]`且退出码0；内核筛选无overflow、underflow、WARNING或Oops。

R5相较R4的NEON转换耗时由约19.8ms降到8.32ms。转换算法没有改变，原因是R5先把
V4L2 MMAP数据复制到普通可缓存内存，NEON随后从cacheable私有帧槽连续读取；这同时
解决了MMAP缓冲归还驱动后的所有权问题，并改善了CPU读带宽。

## 信号退出实机证据

- 目标54,000帧，运行到268帧时按Ctrl+C；
- capture sequence 6265～6532连续，`driver_sequence_gaps=0`、`timeouts=0`；
- 采集30.01fps、显示29.88fps，`stale_frames_dropped=0`；
- 输出`[STOP] Signal requested a clean shutdown after 268 frames`，退出码0；
- 随后立即重新运行60帧，采集sequence 6534～6594连续；
- 重启测试采集30.00fps、显示29.46fps，`driver_sequence_gaps=0`、`timeouts=0`、
  `stale_frames_dropped=0`，最终输出`[PASS]`。

STREAMOFF期间摄像头没有向用户空间交付sequence 6533，下一次STREAMON从6534开始是
跨会话的正常现象；每个采集会话内部均连续。立即重启成功证明pthread、V4L2队列、
MMAP缓冲和设备文件均已完整释放。

## 54,000帧长测与未关闭问题

- 完成54,000个采集和显示帧，sequence 8060～62059连续；
- `driver_sequence_gaps=0`、`timeouts=0`、`stale_frames_dropped=0`；
- 采集和显示均为30.02fps，退出码0并输出`[PASS]`；
- 转换平均8.28ms、blit平均1.27ms、flip平均10.27ms、流水线平均19.83ms；
- 最大流水线33.48ms，没有形成队列积压；内核日志无WARNING或Oops。

但是固定连接线运行时仍观察到画面错位。因此以上`[PASS]`只代表控制流、sequence和
性能通过，不能证明像素内容与视觉质量正确。下一步必须在不重载模块的故障现场读取
`lcdif_stats`和fbdev当前页，再用静态棋盘判断错误位于显存内容之前还是LCD扫描之后。
问题关闭前不把VIDEO-R5或第2天模块标记为完成。

## 故障帧三级快照

运行时提供第4个应用参数作为快照前缀：

```sh
./v4l2_preview /dev/video0 /dev/fb0 54000 fault
```

发现错位后按Ctrl+C。程序先停止采集，再写出三个不会干扰实时路径的文件：

- `fault_last_yuyv.raw`：最后显示帧的640×480 YUYV，614,400字节；
- `fault_last_rgb565.raw`：同一帧经NEON转换后的640×480 RGB565，614,400字节；
- `fault_last_fb_rgb565.raw`：最终可见页1024×600 RGB565，1,228,800字节。

PC上转换为PNG：

```sh
ffmpeg -y -f rawvideo -pixel_format yuyv422 -video_size 640x480 \
  -i fault_last_yuyv.raw -frames:v 1 fault_yuyv.png

ffmpeg -y -f rawvideo -pixel_format rgb565le -video_size 640x480 \
  -i fault_last_rgb565.raw -frames:v 1 fault_rgb565.png

ffmpeg -y -f rawvideo -pixel_format rgb565le -video_size 1024x600 \
  -i fault_last_fb_rgb565.raw -frames:v 1 fault_fb.png
```

判断边界：

- `fault_yuyv.png`已经错位：错误来自OV5640、DVP物理信号或CSI输入；
- YUYV正确、`fault_rgb565.png`错误：错误首次出现在NEON转换；
- 前两张正确、`fault_fb.png`错误：错误首次出现在居中blit或页管理；
- 三张均正确但LCD仍错位：重新检查LCDIF/面板瞬态扫描状态。

## 三级快照实测结论

实测生成的三张PNG都存在相同的画面错位，不能把“三级图彼此一致”误判为“图像正确”。
进一步逐像素比较得到：最终1024×600 Framebuffer的中央640×480区域与应用转换后的
RGB565图完全相同；YUYV经FFmpeg转换后的图也呈现同一错位，仅有正常的颜色量化差异。

因此首个已知错误边界是`fault_last_yuyv.raw`，也就是NEON转换之前。当前可以排除：

- YUYV→RGB565 NEON算法；
- 640×480到1024×600的居中blit；
- fbdev page选择、DMA扫描地址和LCDIF面板时序。

剩余范围是OV5640输出、DVP排线/连接器信号完整性，以及`mx6s-csi`对PCLK、HREF和
VSYNC的采样与帧同步。V4L2 sequence连续只证明DMA帧完成顺序连续，不验证每行收到的
像素字节及同步边沿正确。

## OV5640内部彩条隔离测试

内核未启用`CONFIG_VIDEO_ADV_DEBUG`，但已启用`CONFIG_I2C_CHARDEV`。新增
`ov5640_diag`，它只允许读取诊断寄存器以及对`0x503d`执行受限的彩条开关，不开放
任意寄存器写入。运行前必须停止所有V4L2采集进程：

```sh
cd /home/sun/nfs/project/video_capture

./ov5640_diag /dev/i2c-1 status
./ov5640_diag /dev/i2c-1 pattern on
./v4l2_preview /dev/video0 /dev/fb0 300 sensor_tpg_static
./ov5640_diag /dev/i2c-1 pattern rolling
./v4l2_preview /dev/video0 /dev/fb0 300 sensor_tpg_rolling
./ov5640_diag /dev/i2c-1 pattern off
```

`status`应报告芯片ID`0x5640`、输出`640x480`、格式`0x4300=0x30`，同时保留
`0x4740`极性和`0x503d`状态作为证据。判断规则：

- 内部彩条仍错位：故障位于OV5640 DVP输出引脚之后，优先A/B测试CSI PCLK采样沿，
  同时检查HREF/VSYNC和排线信号完整性；
- 内部彩条稳定、自然画面错位：DVP传输和CSI成帧基本正确，转查OV5640 ISP、缩放器
  与自然图像输出配置。

## 内部彩条实测与15/30fps单变量实验

静态彩条和带滚动条的彩条均完成300帧：每次采集301帧、显示300帧，采集30.02fps、
显示约29.9fps，`driver_sequence_gaps=0`、`timeouts=0`、
`stale_frames_dropped=0`，两种画面均完全正确。测试前后寄存器为：

- 芯片ID`0x5640`；
- 输出`640x480`，HTS=1896、VTS=984；
- `0x4300=0x30`，YUYV；
- `0x4740=0x20`；
- 最终`0x503d=0x00`，测试图已关闭。

这说明同一DVP输出时序和CSI接收配置能够稳定传输动态测试图，不应在没有新证据时直接
翻转`BIT_REDGE`。测试图绕过了自然光学成像/阵列读出，因此下一步比较自然图像的15与
30fps路径。`v4l2_preview`新增最后一个可选参数，只接受15或30：

```sh
./v4l2_preview /dev/video0 /dev/fb0 300 natural_30 30
./v4l2_preview /dev/video0 /dev/fb0 300 natural_15 15
```

旧驱动的15fps VGA表将`0x3035`从`0x11`改成`0x21`，即系统分频从1变2；在其他关键
VGA时序相同的条件下，PCLK约从56MHz降到28MHz，行读出时间约翻倍。判定规则：

- 静止场景30fps错、15fps正确：优先检查DVP信号完整性和PCLK采样裕量；
- 移动场景两者都有倾斜且15fps更重：符合滚动快门逐行读出；
- 静止场景两者仍有同样错位：检查OV5640阵列读出、裁剪/缩放配置和摄像头模块。

## 15/30 fps 实机结论与 PCLK 边沿 A/B

最新实测结果为：固定场景在15 fps和30 fps都正确；移动场景在30 fps出现错位，切换到
15 fps后恢复。旧OV5640 VGA配置主要通过把`0x3035`从`0x11`改为`0x21`，将PCLK
从约56 MHz降到约28 MHz。因此当前优先判断为高速DVP接收裕量或采样边沿问题，不把普通
滚动快门当作根因。

下一步采用单变量内核A/B：保持OV5640、设备树、分辨率和所有极性不变，只清除
`mx6s_capture.c`的`BIT_REDGE`，让CSI改为下降沿采样。完整补丁、编译、回退和测试矩阵见：

`src/kernel/advanced/csi_capture/VIDEO-R5-PCLK-AB.md`

下降沿B版实测直接产生乱码，恢复上升沿后画面恢复，说明`BIT_REDGE`必须保持置位；采样边沿
选择不是原30fps运动错位的修复方向。30fps故障图与15fps正确图对比确认，异常只在自然运动
画面的高速模式出现。进一步核对OV5640驱动发现：15/30fps VGA寄存器表只有`0x3035`
分别为`0x21/0x11`；驱动在初始化时把`0x302c[7:6]`设为2×，并明确注明图像不稳定时可
提高输出驱动强度。

`ov5640_diag`因此新增受限的DVP驱动强度A/B命令：

```sh
./ov5640_diag /dev/i2c-1 drive 3
./ov5640_diag /dev/i2c-1 drive 4
./ov5640_diag /dev/i2c-1 drive 2
```

每次只修改`0x302c[7:6]`、保留低6位并读回验证。较强驱动不一定更好，必须按2×、3×、4×
逐级测试，结束后恢复2×；执行命令时不得有V4L2采集进程运行。

## DVP驱动强度最终结论

实机单变量结果为：2×出现动态画面错位，3×仍然错位，4×位移最快；1×完全没有错位。
驱动越强、故障越严重，说明原先“图像不稳定就继续提高驱动”的通用注释不适合当前板级
DVP连接。该现象更符合边沿过快引起的过冲、振铃、串扰或地弹噪声，而不是驱动能力不足。

1×随后完成54,000帧动态长测：采集54,001帧、显示54,000帧，sequence 3128～57128连续，
采集和显示均为30.02fps，`driver_sequence_gaps=0`、`timeouts=0`、
`stale_frames_dropped=0`，动态画面全程无错位。程序输出`[PASS]`且退出码0，内核没有CSI、
overflow、WARNING或Oops；结束后读回`0x302c=0x02 (1x)`，证明采集初始化未覆盖实验值。

正式修复不依赖用户空间在每次启动时执行I2C写入，而是在OV5640模式初始化代码中把板级
默认值固化为1×。补丁、详细注释、编译、回退与正式内核回归步骤见：

`src/kernel/advanced/csi_capture/VIDEO-R5-DVP-DRIVE-1X.md`
