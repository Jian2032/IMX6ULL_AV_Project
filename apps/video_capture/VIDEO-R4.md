# VIDEO-R4：LCD双缓冲与帧边界翻页

## 1. R3结论

VIDEO-R3的NEON版本已经通过性能验收：

- 300帧、`sequence_gaps=0`、输入30.01fps；
- NEON转换平均19.82ms，显存复制平均1.33ms；
- 完整流水线平均21.16ms，退出码为0；
- CSI和LCDIF没有overflow、underflow、Oops或WARNING。

颜色转换和采集链路已经正确，但直接更新LCDIF正在扫描的page 0时仍观察到整幅错位
滚动。R4不再修改转换算法，只隔离“CPU绘图”和“LCDIF扫描”两种显存访问。

## 2. R4数据通路

```text
DQBUF取得YUYV帧
  -> NEON转换到cacheable RGB565暂存区
  -> memcpy到当前未扫描的back page
  -> ioctl(FBIOPAN_DISPLAY)
  -> LCD-R5写NEXT_BUF
  -> CUR_FRAME_DONE中断
  -> ioctl返回，back page成为front page
  -> QBUF归还摄像头buffer
```

Framebuffer为1024x600可见、1024x1200虚拟分辨率：

- page 0：`yoffset=0`；
- page 1：`yoffset=600`；
- 每帧只写`front_page ^ 1`；
- `FBIOPAN_DISPLAY`成功返回后才交换front/back身份。

LCD-R5的`fb_pan_display()`已经在内核中执行`wmb()`，写`NEXT_BUF`并等待
`CUR_FRAME_DONE` completion，因此应用不需要再额外调用`FBIO_WAITFORVSYNC`。

## 3. Ubuntu构建

覆盖R4文件前先保留已经通过的R3快照：

```sh
cd /home/book/Workspace/linux/nfs/project
cp -a video_capture video_capture_R3

cd video_capture
make clean
make info
make

file v4l2_preview
```

编译必须零warning/error，`make info`必须包含`-DAV_ENABLE_NEON=1`。

## 4. 开发板测试

无需重复R1和R2。确认LCD-R5已加载且fbcon已解绑后执行：

```sh
cd /home/sun/nfs/project/video_capture

cat /sys/bus/platform/devices/21c8000.lcdif/lcdif_stats
cat /proc/interrupts | grep -Ei 'csi|lcdif|21c4000|21c8000'

./v4l2_preview /dev/video0 /dev/fb0 300
echo "exit_code=$?"

cat /sys/bus/platform/devices/21c8000.lcdif/lcdif_stats
cat /proc/interrupts | grep -Ei 'csi|lcdif|21c4000|21c8000'
dmesg | grep -Ei 'csi|lcdif|overflow|underflow|hresponse|warning|oops' | tail -n 100
```

## 5. 验收标准

- 标题为`VIDEO-R4 NEON RGB565 double-buffered LCD preview`；
- framebuffer报告`virtual=1024x1200`、`pages=2`；
- converter报告`ARM NEON, 8 pixels/iteration`；
- 日志中的page在0和1之间交替；
- 300帧、`sequence_gaps=0`、输入fps接近30、退出码0；
- `flips`和LCDIF IRQ相对运行前约增加301次：1次初始化page 0加300次视频翻页；
- `underflows=0`、`overflows=0`，内核无Oops或WARNING；
- LCD画面位置固定，不再整幅错位滚动，也不应出现可见撕裂。

如果双缓冲后仍整幅错位滚动，则现象不再能由“应用写入当前扫描页”解释。下一步保留
R4完整输出，并分别检查保存的单帧图像、`cur_buf/next_buf`以及LCDIF扫描页地址。
