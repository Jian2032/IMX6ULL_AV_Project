# LCD-R4：双缓冲、翻页与帧同步

## 本轮目标

在LCD-R3已经验证的`/dev/fb0`基础上增加：

1. 两帧连续RGB565 DMA显存；
2. `yres_virtual=1200`与`ypanstep=1`；
3. `FBIOPAN_DISPLAY`切换`NEXT_BUF`；
4. `CUR_FRAME_DONE`中断确认翻页完成；
5. 标准`FBIO_WAITFORVSYNC`和VSYNC边沿中断；
6. 用户空间双缓冲移动色块测试。

本轮不修改设备树和内核`.config`。中断号、时钟、LCD时序仍来自
已经通过R1～R3验证的现有设备树。

## 翻页数据流

```text
CPU绘制隐藏页
    |
    v
FBIOPAN_DISPLAY(yoffset=0或600)
    |
    v
驱动计算DMA地址并写NEXT_BUF
    |
    v
LCDIF在帧边界装载为CUR_BUF
    |
    v
CUR_FRAME_DONE IRQ -> complete()
    |
    v
ioctl返回，原显示页成为下一张隐藏页
```

这里没有复制整帧数据。两页在同一块DMA内存中，翻页只是改变LCDIF
下一帧读取的起始地址。

## 编译前保留R3

在Ubuntu中先备份已经通过验收的R3：

```sh
cd /home/book/Workspace/linux/nfs/project
cp -a lcdif_fb lcdif_fb_R3
cd lcdif_fb
```

再把本轮的`av_lcdif_fb.c`、`fb_test.c`、`Makefile`和`LCD-R4.md`
同步到该目录。

## 交叉编译

```sh
cd /home/book/Workspace/linux/nfs/project/lcdif_fb
make clean
make info
make

modinfo av_lcdif_fb.ko | grep -E 'description|version'
file av_lcdif_fb.ko fb_test
```

预期模块版本是`R4`，两个产物都应为32位ARM EABI5。

## 开发板测试

### 1. 加载和解绑fbcon

```sh
cd /home/sun/nfs/project/lcdif_fb
insmod av_lcdif_fb.ko
dmesg | tail -n 40

for v in /sys/class/vtconsole/vtcon*; do
    if grep -qi "frame buffer" "$v/name"; then
        echo 0 > "$v/bind"
    fi
done
```

解绑fbcon可以避免控制台同时修改双缓冲页面，也便于最后卸载模块；
串口控制台不受影响。

### 2. 检查模式

```sh
./fb_test info
cat /proc/interrupts | grep -Ei "lcdif|21c8000"
```

关键预期值：

```text
visible: 1024x600
virtual: 1024x1200
bits_per_pixel: 16
line_length: 2048 bytes
pan steps: x=0 y=1 ywrap=0
smem_len: 2457600 bytes
```

### 3. 测量VSYNC

```sh
./fb_test vsync 180
cat /proc/interrupts | grep -Ei "lcdif|21c8000"
```

180次等待约需3秒。根据当前实际像素时钟49,972,866Hz和总时序
1344×635，预期测量值约为58.5Hz：

```text
refresh = 49,972,866 / (1344 * 635) = 58.56 Hz
```

### 4. 双缓冲动画

```sh
./fb_test flip 180
./fb_test info
```

预期看到亮色色块在深蓝背景上平滑水平移动，没有明显上下错开的撕裂
线。测试通常接近3秒，平均翻页速率应接近实际刷新率；如果CPU绘制
速度较慢，数值可以低于58.5次/秒，但不应发生超时。

`info`最后的`yoffset`应为0或600，取决于翻页次数和起始页。

### 5. 回归R3绘图功能

```sh
./fb_test checker
./fb_test gradient
./fb_test bars
```

三种静态图仍应正确显示。

### 6. 卸载

```sh
lsmod | grep av_lcdif
rmmod av_lcdif_fb
dmesg | tail -n 30
ls -l /dev/fb* 2>/dev/null
```

预期卸载日志包含：

```text
LCDIF controller stopped
LCD-R4 remove complete: flips=180, vsync_waits=180
```

实际计数可能包含其他fbdev用户触发的翻页或同步等待，因此允许略大于
命令参数。卸载后`/dev/fb0`应消失。

## 超时排查

若出现`wait for VSYNC timeout`或`pan timeout`，不要重复快速加载模块。
保留现场并提供：

```sh
dmesg | tail -n 80
cat /proc/interrupts
devmem 0x021c8010 32
devmem 0x021c8040 32
devmem 0x021c8050 32
```

三个寄存器分别是`CTRL1`、`CUR_BUF`和`NEXT_BUF`。若BusyBox没有
`devmem`，只提供前两项即可。

## 需要反馈的结果

1. `make`、`modinfo`和`file`输出；
2. R4加载日志；
3. `./fb_test info`；
4. `./fb_test vsync 180`及中断计数；
5. `./fb_test flip 180`及实际画面；
6. 三种静态图回归结果；
7. 卸载日志和`/dev/fb0`是否消失。
