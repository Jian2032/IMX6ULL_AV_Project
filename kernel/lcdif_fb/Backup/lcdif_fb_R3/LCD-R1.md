# LCD-R1：platform资源探测

## 本轮目标

验证自写驱动可以绑定现有LCDIF设备树节点，并取得：

- 寄存器区`0x021c8000～0x021cbfff`
- LCDIF中断
- `pix`、`axi`、`disp_axi`时钟
- 默认pinctrl状态
- `display` phandle
- `bus-width=24`
- `bits-per-pixel=16`

## 本轮明确不做

- 不写LCDIF寄存器
- 不设置51.2MHz像素时钟
- 不申请DMA显存
- 不注册framebuffer
- 不生成`/dev/fb0`
- 不注册中断处理函数

## Ubuntu编译

把本目录复制到Ubuntu的NFS共享目录后执行：

```sh
cd /home/book/Workspace/linux/nfs/project/lcdif_fb

make info
make

file av_lcdif_fb.ko
arm-linux-gnueabihf-readelf -h av_lcdif_fb.ko | head
```

预期生成`av_lcdif_fb.ko`。如果内核源码路径改变，可以覆盖：

```sh
make KDIR=/home/book/Workspace/nxp_linux/linux-imx-rel_imx_4.1.15_2.1.0_ga
```

## 开发板测试

```sh
cd /home/sun/nfs/project/lcdif_fb

insmod av_lcdif_fb.ko
dmesg | tail -n 30
lsmod | grep av_lcdif
ls -l /sys/bus/platform/drivers/av-lcdif-fb
```

成功日志应包含：

```text
LCD-R1 probe begin
display node ...: bus-width=24, bpp=16
clock rates: pix=... Hz, axi=... Hz, disp_axi=... Hz
LCD-R1 ready: regs=0x021c8000-0x021cbfff, irq=...
resource validation passed; LCD registers were not modified
```

R1阶段`/dev/fb0`仍然不存在，这是预期行为。

卸载测试：

```sh
rmmod av_lcdif_fb
dmesg | tail -n 10
lsmod | grep av_lcdif
```

预期出现`LCD-R1 remove`，并且`lsmod`不再显示该模块。

## 需要反馈的结果

请原样提供：

```sh
make 2>&1
insmod av_lcdif_fb.ko
dmesg | tail -n 40
ls -l /sys/bus/platform/drivers/av-lcdif-fb
rmmod av_lcdif_fb
dmesg | tail -n 10
```

R1通过后再进入LCD-R2，解析完整1024×600时序并申请DMA显存。

