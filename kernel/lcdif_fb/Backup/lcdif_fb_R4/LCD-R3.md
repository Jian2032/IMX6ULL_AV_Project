# LCD-R3：注册/dev/fb0与用户空间mmap绘图

## R2实机结论

- 1024×600时序解析正确。
- DMA显存地址为`0x8c200000`，大小1,228,800字节。
- `CUR_BUF`和`NEXT_BUF`都等于DMA显存地址，硬件正在扫描该内存。
- 屏幕正确显示白、黄、青、绿、品红、红、蓝、黑色条。
- 实际像素时钟49,972,866Hz，画面稳定。
- 控制器停止、模块卸载和显存释放正常。

## R3目标

- 初始化`struct fb_info`。
- 提供固定1024×600 RGB565模式。
- 注册`/dev/fb0`。
- 支持`FBIOGET_FSCREENINFO`和`FBIOGET_VSCREENINFO`。
- 使用`dma_mmap_writecombine()`把DMA显存映射给用户程序。
- 使用`fb_test`从用户空间绘制色条、渐变和棋盘图。

## R3仍然不做

- 只有一帧显存。
- 不支持`FBIOPAN_DISPLAY`。
- 不支持VSYNC中断。
- 不支持blank、动态改分辨率或RGB888。

## 保存R2

```sh
cd /home/book/Workspace/linux/nfs/project
cp -a lcdif_fb lcdif_fb_R2
```

再用Windows项目中的R3文件覆盖`lcdif_fb`。

## Ubuntu编译

R3的`make`会同时构建内核模块和用户空间测试程序：

```sh
cd /home/book/Workspace/linux/nfs/project/lcdif_fb

make clean
make info
make

modinfo av_lcdif_fb.ko | grep -E 'description|version'
file av_lcdif_fb.ko fb_test
```

预期：

```text
version: R3
av_lcdif_fb.ko: ELF 32-bit ... ARM ...
fb_test: ELF 32-bit ... ARM ...
```

## 开发板测试

```sh
cd /home/sun/nfs/project/lcdif_fb

insmod av_lcdif_fb.ko
dmesg | tail -n 60

ls -l /dev/fb*
cat /proc/fb
cat /sys/class/graphics/fb0/name
```

预期：

```text
/dev/fb0
0 av-lcdif
av-lcdif
```

读取fbdev信息：

```sh
./fb_test info
```

依次改变画面：

```sh
./fb_test checker
./fb_test gradient
./fb_test bars
```

每次执行都应立即改变LCD内容，并打印：

```text
pattern '...' written successfully
```

`fb_test info`的关键预期值：

```text
visible: 1024x600
virtual: 1024x600
offset: x=0 y=0
bits_per_pixel: 16
line_length: 2048 bytes
smem_len: 1228800 bytes
red: offset=11 length=5
green: offset=5 length=6
blue: offset=0 length=5
```

## 卸载

确保`fb_test`已经退出，然后执行：

```sh
rmmod av_lcdif_fb
dmesg | tail -n 30
ls -l /dev/fb* 2>/dev/null
lsmod | grep av_lcdif
```

预期：

```text
LCDIF controller stopped
LCD-R3 remove complete
```

`/dev/fb0`随驱动注销而消失。

### 如果rmmod提示Resource temporarily unavailable

若日志出现：

```text
Console: switching to colour frame buffer device ...
```

说明内核`fbcon`已经绑定`fb0`。`fb_ops.owner = THIS_MODULE`会让fbcon持有
模块引用，`lsmod`最后一列显示为1，所以内核拒绝卸载正在使用的驱动。
这不是应用程序文件描述符泄漏。

先查看并解绑名称包含`frame buffer`的虚拟控制台：

```sh
for v in /sys/class/vtconsole/vtcon*; do
    echo "== $v =="
    cat "$v/name"
    cat "$v/bind"
done

for v in /sys/class/vtconsole/vtcon*; do
    if grep -qi "frame buffer" "$v/name"; then
        echo 0 > "$v/bind"
    fi
done
```

然后重新卸载：

```sh
lsmod | grep av_lcdif
rmmod av_lcdif_fb
dmesg | tail -n 20
```

本项目最终由视频预览和GUI独占LCD，不需要fbcon长期占用`fb0`。串口
`ttymxc0`控制台不受上述解绑操作影响。

## 反馈内容

提供：

1. `make`完整输出。
2. `modinfo`和`file`结果。
3. `dmesg | tail -n 60`。
4. `/proc/fb`和`fb0/name`。
5. `./fb_test info`完整输出。
6. 三种图案的实际显示结果。
7. `rmmod`日志和`/dev/fb0`是否消失。
