# VIDEO-R3：CPU转换RGB565与LCD单缓冲预览

## 1. 本轮目标

```text
OV5640 YUYV 640×480
  → CSI/videobuf2 MMAP
  → CPU使用BT.601整数公式转换RGB565
  → mmap写入/dev/fb0当前显示页
  → 1024×600 LCD居中显示
```

R3不缩放图像。640×480原尺寸画面放在1024×600中央：左右各192像素黑边，
上下各60像素黑边。这样先验证采集、颜色转换和fbdev写入，不让缩放算法干扰。

本轮直接写LCD正在扫描的page 0，画面可能存在轻微撕裂；R4将改为隐藏页绘制、
`FBIOPAN_DISPLAY`和VSYNC同步。

## 2. 新增代码结构

- `av_video_capture.h/.c`：从R2流程抽出的可复用V4L2 MMAP接口；
- `v4l2_preview.c`：fbdev初始化、YUYV转RGB565、居中布局和预览统计；
- R1的`v4l2_probe`与R2的`v4l2_capture`保持不变，作为独立诊断工具。

每组`Y0 U Y1 V`生成两个RGB565像素，两个像素共享U/V。转换采用BT.601
limited-range整数公式，输出与LCD驱动报告的R5/G6/B5位域一致。

修订版不再逐像素直接写write-combine framebuffer：

1. ARM目标使用NEON指令一次并行转换8个YUYV像素，标量查表代码仅作为尾部回退；
2. 在普通cacheable内存中完成YUYV到RGB565转换；
3. 通过逐行大块`memcpy`把完整RGB565画面写入framebuffer；
4. 分别统计`convert`、`blit`和端到端`total`耗时。

这样既保留了容易学习的CPU转换基线，也避免Cortex-A7对WC显存执行大量零散16位写入。

## 3. 首次实测与R3修订依据

首次实现已经证明颜色格式、取帧和LCD布局正确，但性能未通过：

- 300帧输出的sequence从120增长到1076，缺失657个输入帧；
- 平均转换耗时107.31ms，实际输入处理速率只有9.39fps；
- CSI IRQ持续增长，内核无CSI overflow、Hresponse、LCD underflow、Oops或WARNING；
- LCD颜色正确，但出现撕裂、错位和向右滚动。

第一次修订把转换和显存写入拆开后，第二次实测得到更准确的瓶颈证据：

- `convert avg=102.93ms`，`blit avg=1.32ms`，`pipeline avg=104.25ms`；
- 300帧仍缺失630帧，实际处理速率9.66fps；
- 批量显存复制已经足够快，真正瓶颈是逐像素标量BT.601计算与通道裁剪。

30fps输入每33.3ms产生一帧，而标量转换超过100ms。处理期间LCD会反复扫描正在修改的
page 0，因此持续出现错位滚动。当前修订使用i.MX6ULL Cortex-A7的NEON SIMD一次计算
8个像素，先解决转换瓶颈；R4再通过隐藏页绘制和VSYNC翻页消除单缓冲固有撕裂。

## 4. Ubuntu编译

建议先保留R2目录快照，再覆盖R3文件：

```sh
cd /home/book/Workspace/linux/nfs/project
cp -a video_capture video_capture_R2

cd video_capture
make clean
make info
make

file v4l2_probe v4l2_capture v4l2_preview
arm-linux-gnueabihf-readelf -h v4l2_preview | grep -E 'Class|Data|Machine'
```

三个程序都应无warning/error，`v4l2_preview`应为ARM ELF32 EABI5。

运行时还必须打印：

```text
converter      : ARM NEON, 8 pixels/iteration
```

如果打印`scalar lookup fallback`，说明交叉编译器没有启用Makefile中的NEON选项，
不要继续做性能验收。

## 5. 准备LCD framebuffer

如果`/dev/fb0`不存在，加载已经通过验收的LCD-R5模块：

```sh
cd /home/sun/nfs/project/lcdif_fb
insmod av_lcdif_fb.ko
ls -l /dev/fb0
cat /sys/class/graphics/fb0/name
```

为防止fbcon覆盖预览画面，解绑framebuffer console：

```sh
for v in /sys/class/vtconsole/vtcon*; do
    if grep -qi "frame buffer" "$v/name"; then
        echo 0 > "$v/bind"
    fi
done
```

预期串口显示切换到dummy console，`lsmod`中`av_lcdif_fb`引用计数归零。

## 6. 开发板预览测试

```sh
cd /home/sun/nfs/project/video_capture

./v4l2_preview --help
cat /proc/interrupts | grep -Ei 'csi|lcdif|21c4000|21c8000'

./v4l2_preview /dev/video0 /dev/fb0 300
echo "exit_code=$?"

cat /proc/interrupts | grep -Ei 'csi|lcdif|21c4000|21c8000'
dmesg | tail -n 100
```

运行约10秒。必须确认：

- video格式为640×480 YUYV、30fps、line 1280、size 614400；
- framebuffer为`av-lcdif`、1024×600、virtual 1024×1200、RGB565；
- placement为`(192,60)`；
- LCD中央出现连续摄像头画面，四周黑边尺寸合理；
- 亮度、红绿蓝颜色基本正确，没有明显偏紫、偏绿或像素错位；
- 完成300帧，sequence gaps为0，input fps接近30；
- 输出分别包含`convert`、`blit`和`total`耗时；
- 平均`total`低于33.3ms，证明完整CPU转换与显存拷贝可以跟上30fps输入；
- 最后打印`[PASS]`、退出码为0；
- 内核无CSI overflow、Hresponse、LCD underflow、Oops或WARNING。

单缓冲阶段出现一条移动的横向撕裂线不算R3失败，只要画面内容、颜色、固定位置和帧率
正确；如果仍有整幅错位、持续向右滚动或大量sequence gap，则继续停留在R3定位。
R4专门解决单缓冲剩余撕裂。

## 7. 测试后状态

程序退出后最后一帧会留在LCD上，这是正常的；应用已关闭video和fb文件并释放全部
MMAP。LCD模块可继续留给R4使用。如需卸载：

```sh
rmmod av_lcdif_fb
```

## 8. 失败时保留的信息

请完整粘贴程序输出，并执行：

```sh
./fb_test info
cat /proc/interrupts | grep -Ei 'csi|lcdif|21c4000|21c8000'
dmesg | tail -n 150
```

同时描述LCD现象：全黑、花屏、颜色错误、画面错位、卡住还是只有撕裂。
