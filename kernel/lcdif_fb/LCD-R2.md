# LCD-R2：设备树时序、DMA显存和基础点屏

## R1实机结论

R1已经验证：

- 驱动成功绑定`21c8000.lcdif`。
- 面板节点由`display` phandle正确解析。
- `bus-width=24`、`bits-per-pixel=16`。
- LCDIF寄存器范围为`0x021c8000～0x021cbfff`。
- Linux映射后的IRQ为229。
- `pix`和`axi`时钟正常。
- `disp_axi=0 Hz`来自DTS dummy clock，不是错误。
- 模块能够干净卸载。

## R2目标

- 解析1024×600完整时序和极性。
- 把像素时钟请求为51.2MHz。
- 申请一帧1,228,800字节的DMA显存。
- 在内核中填充8条RGB565颜色条。
- 配置LCDIF v4寄存器并启动扫描。
- 卸载时安全停止DMA并释放显存。

## R2仍然不做

- 不注册`fb_info`。
- 不创建`/dev/fb0`。
- 不允许用户程序修改显存。
- 不实现双缓冲和VSYNC中断。

这些功能分别属于LCD-R3和LCD-R4。

## 先保存R1

在Ubuntu上保留已经验证的R1目录：

```sh
cd /home/book/Workspace/linux/nfs/project
cp -a lcdif_fb lcdif_fb_R1
```

然后再用Windows项目中的新版`lcdif_fb`覆盖NFS工作目录。

## Ubuntu编译

```sh
cd /home/book/Workspace/linux/nfs/project/lcdif_fb

make clean
make
file av_lcdif_fb.ko
```

编译成功后，确认模块版本：

```sh
modinfo av_lcdif_fb.ko | grep -E 'description|version'
```

预期版本为`R2`。

## 开发板加载

确保R1已经卸载：

```sh
lsmod | grep av_lcdif
```

加载R2：

```sh
cd /home/sun/nfs/project/lcdif_fb
insmod av_lcdif_fb.ko
dmesg | tail -n 50
```

屏幕预期显示8条竖直颜色条：

```text
白 | 黄 | 青 | 绿 | 品红 | 红 | 蓝 | 黑
```

日志预期包含：

```text
LCD-R2 probe begin
timing ...: 1024x600, pixelclock=51200000 Hz, refresh~59 Hz
horizontal: active=1024 front=160 sync=20 back=140 total=1344
vertical: active=600 front=12 sync=3 back=20 total=635
polarity: hsync=0 vsync=0 de=1 pixelclk=0
DMA framebuffer: cpu=..., dma=..., size=1228800 bytes
clock rates: requested pix=51200000 Hz, actual pix=...
LCDIF state: CTRL=... CUR_BUF=... NEXT_BUF=...
LCD-R2 ready: fixed RGB565 color bars are scanning out
```

`CUR_BUF`或`NEXT_BUF`应当等于日志中的DMA framebuffer地址。

## 如果背光未亮

先查看当前backlight节点，不要修改DTS：

```sh
for b in /sys/class/backlight/*; do
    echo "$b"
    cat "$b/brightness"
    cat "$b/max_brightness"
done
```

如存在节点且brightness为0，可把它恢复到DTS默认等级6：

```sh
for b in /sys/class/backlight/*; do
    echo 6 > "$b/brightness"
done
```

## 卸载

```sh
rmmod av_lcdif_fb
dmesg | tail -n 20
lsmod | grep av_lcdif
```

预期出现：

```text
LCDIF controller stopped
LCD-R2 remove complete
```

卸载后屏幕可能变黑、变白或保持最后一帧的视觉残留，这是停止RGB时钟后的
面板表现；重点是串口仍正常、模块消失且没有Oops。

## 失败时反馈

提供：

```sh
make 2>&1
insmod av_lcdif_fb.ko
dmesg | tail -n 60
cat /proc/meminfo | grep -E 'MemFree|CmaFree'
for b in /sys/class/backlight/*; do
    cat "$b/brightness"
    cat "$b/max_brightness"
done
```

并说明屏幕是：

- 完全无变化
- 背光亮但全白
- 全黑
- 有颜色但滚动
- 有色条但颜色或顺序错误
- 正确显示色条
