# VIDEO-R5：CSI PCLK 采样边沿 A/B 实验

## 1. 当前证据与判断

实机对比结果是：

- 固定场景：15 fps、30 fps 都正确；
- 移动场景：30 fps 出现整幅错位，15 fps 恢复正确；
- OV5640 静态彩条、滚动彩条在 30 fps 都正确；
- 故障帧在原始 YUYV 中已经错位；
- `fb_test checker`、LCDIF 翻页、Framebuffer 地址和 LCD 时序都正确。

这排除了固定的分辨率、stride、RGB565 转换、Framebuffer 翻页和 LCDIF 扫描错误。现有
OV5640 VGA 表在 30 fps 使用 `0x3035=0x11`，15 fps 使用 `0x3035=0x21`；HTS/VTS 等
关键 VGA 时序保持不变，因此该实验主要把像素时钟从约 56 MHz 降到约 28 MHz。

“30 fps 错、15 fps 对”更符合高速 DVP 接收裕量不足。它不能单独证明一定是采样边沿，
所以本轮只进行一次受控 A/B：把 i.MX6ULL CSI 从上升沿采样改成下降沿采样，其他配置全部
保持不变。

## 2. 补丁改变了什么

补丁文件：

`0001-video-r5-csi-sample-pclk-falling.patch`

原驱动在 `csi_init_interface()` 中执行：

```c
val |= BIT_REDGE;
```

B 版改为：

```c
val &= ~BIT_REDGE;
```

补丁还覆盖了 `csi_tvdec_enable(false)` 的恢复路径，并在每次打开 `/dev/video0` 时打印：

```text
VIDEO-R5 A/B: parallel PCLK sampling edge = falling
```

本次不修改 OV5640 的 `0x4740`、格式、HTS/VTS、分辨率、pinctrl 或 LCD 驱动。

## 3. Ubuntu 应用补丁并编译 zImage

先进入当前运行内核对应的源码：

```sh
cd /home/book/Workspace/nxp_linux/linux-imx-rel_imx_4.1.15_2.1.0_ga

cp drivers/media/platform/mxc/subdev/mx6s_capture.c \
   drivers/media/platform/mxc/subdev/mx6s_capture.c.before_video_r5_pclk_ab

patch --dry-run -p1 < \
  /home/book/Workspace/linux/nfs/project/video_capture/0001-video-r5-csi-sample-pclk-falling.patch

patch -p1 < \
  /home/book/Workspace/linux/nfs/project/video_capture/0001-video-r5-csi-sample-pclk-falling.patch

make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j4 zImage
```

编译产物位于：

```text
arch/arm/boot/zImage
```

先备份开发板当前可启动的 zImage，再按现有可靠流程安装新 zImage。这个驱动由内核内建，
仅重新编译应用程序或复制 `.c` 文件到开发板不会生效；必须启动新内核。设备树本轮不改。

## 4. 启动后确认 B 版真正生效

```sh
uname -a
dmesg | grep -F "VIDEO-R5 A/B"
```

随后先确认测试图已经关闭：

```sh
cd /home/sun/nfs/project/video_capture
./ov5640_diag /dev/i2c-1 status
```

`0x503d` 必须为 `0x00`。若不是，先执行：

```sh
./ov5640_diag /dev/i2c-1 pattern off
```

## 5. 严格的实机对比顺序

先不触碰排线，只移动镜头前的物体或平稳转动整块开发板，避免把接触不良混入采样边沿实验：

```sh
./v4l2_preview /dev/video0 /dev/fb0 900 falling_30 30
echo "falling_30_exit=$?"

./v4l2_preview /dev/video0 /dev/fb0 450 falling_15 15
echo "falling_15_exit=$?"

dmesg | grep -Ei 'VIDEO-R5 A/B|csi|overflow|warning|oops' | tail -n 80
```

每项都记录：固定是否正确、移动是否错位、是否滚动、`driver_sequence_gaps`、采集帧率和退出码。

| 采样边沿 | 帧率 | 固定场景 | 移动场景 | 用途 |
|---|---:|---|---|---|
| 上升沿 A（原内核） | 30 | 正确 | 错位 | 已有基线 |
| 上升沿 A（原内核） | 15 | 正确 | 正确 | 已有基线 |
| 下降沿 B（新内核） | 30 | 待测 | 待测 | 关键结论 |
| 下降沿 B（新内核） | 15 | 待测 | 待测 | 回归检查 |

## 6. 如何解释结果

- **下降沿下 30 fps 恢复，15 fps 仍正确**：采样边沿就是主要根因。保留测试证据，下一轮把
  硬编码改成设备树属性，形成可维护的正式修复。
- **下降沿下 30 fps 更差或无法成帧**：立即恢复上升沿；根因更可能是 DVP 排线、连接器、
  IO 驱动强度/边沿速度或板级信号完整性。下一步只检查 CSI pinctrl pad 参数和物理连接。
- **两个边沿都只在 30 fps 移动时出错**：边沿选择不是根因，但 28/56 MHz 对比仍证明问题
  与高速链路裕量相关；恢复原驱动后进入 pad drive/slew 和示波器测量。

滚动快门通常表现为移动物体倾斜、弯曲，而不是 YUYV 行起点持续错位。15 fps 行读出更慢却
恢复正确，也不支持把本现象简单归因于普通滚动快门。

## 7. 恢复原始上升沿版本

若 B 版没有改善，在 Ubuntu 内核源码根目录执行：

```sh
patch --dry-run -R -p1 < \
  /home/book/Workspace/linux/nfs/project/video_capture/0001-video-r5-csi-sample-pclk-falling.patch

patch -R -p1 < \
  /home/book/Workspace/linux/nfs/project/video_capture/0001-video-r5-csi-sample-pclk-falling.patch

make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j4 zImage
```

也可以用 `mx6s_capture.c.before_video_r5_pclk_ab` 对照确认恢复结果，但不要在未核对路径时覆盖
内核源码。重新安装恢复后的 zImage 并重启，原上升沿配置才真正恢复。
