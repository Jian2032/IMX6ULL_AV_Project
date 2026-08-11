# VIDEO-R5：OV5640 DVP驱动强度1×正式修复

## 1. 修复结论

最终问题不是LCDIF、NEON转换、V4L2缓冲队列、普通滚动快门或CSI采样边沿。
根因是当前摄像头板级DVP连接在约56MHz PCLK、VGA 30fps下使用较高输出驱动强度时，
信号完整性裕量不足。OV5640寄存器`0x302c[7:6]`的实机单变量结果为：

| 驱动强度 | 30fps动态画面 |
|---:|---|
| 1× | 完全正确，无错位 |
| 2× | 出现分段错位 |
| 3× | 仍然错位 |
| 4× | 错位位移最快 |

驱动越强、故障越严重，符合边沿过快导致过冲、振铃、串扰或地弹噪声加重的特征。
下降沿CSI采样曾直接产生乱码，因此PCLK接收边沿必须继续保持原来的上升沿。

## 2. 长时间实机证据

在运行时将`0x302c[7:6]`设为1×后，以动态自然画面完成54,000帧测试：

- 采集54,001帧，显示54,000帧；
- sequence从3128连续到57128；
- 采集和显示均为30.02fps；
- `driver_sequence_gaps=0`；
- `timeouts=0`；
- `stale_frames_dropped=0`；
- NEON转换平均8.31ms；
- 完整流水线平均20.10ms、最大32.13ms；
- 程序输出`[PASS]`并返回0；
- 动态画面全程无错位；
- `dmesg`没有CSI、overflow、WARNING或Oops；
- 结束后读回`0x302c=0x02 (1x)`，确认采集过程没有覆盖实验值。

## 3. 补丁内容

补丁文件：

```text
src/kernel/advanced/csi_capture/0002-video-r5-ov5640-dvp-drive-1x.patch
```

它只修改`drivers/media/platform/mxc/capture/ov5640.c`中的一处板级参数：

```c
ov5640_driver_capability(2);
```

改为：

```c
ov5640_driver_capability(1);
```

修改位置保持在模式寄存器表下载完成之后。这样每次OV5640重新初始化模式时，驱动都会
重新写入已经验证的1×值，而不是依赖用户空间诊断工具临时改寄存器。补丁不改变分辨率、
帧率、YUYV格式、HTS/VTS、PCLK极性、CSI寄存器或设备树。

## 4. Ubuntu应用补丁并编译

```sh
cd /home/book/Workspace/nxp_linux/linux-imx-rel_imx_4.1.15_2.1.0_ga

cp drivers/media/platform/mxc/capture/ov5640.c \
   drivers/media/platform/mxc/capture/ov5640.c.before_video_r5_drive_1x

patch --dry-run -p1 < \
  /home/book/Workspace/linux/nfs/project/video_capture/0002-video-r5-ov5640-dvp-drive-1x.patch

patch -p1 < \
  /home/book/Workspace/linux/nfs/project/video_capture/0002-video-r5-ov5640-dvp-drive-1x.patch

grep -n -A12 -B3 "ALIENTEK i.MX6ULL parallel DVP" \
  drivers/media/platform/mxc/capture/ov5640.c

make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j4 zImage
```

将新的`arch/arm/boot/zImage`更新到开发板启动介质并重启。

## 5. 正式内核回归

重启后不要先执行`drive 1`命令。先确认内核本身已经写入1×：

```sh
uname -a
cd /home/sun/nfs/project/video_capture

./v4l2_probe /dev/video0 >/tmp/probe.log
./ov5640_diag /dev/i2c-1 status | grep -Ei 'drive|302c'

./v4l2_preview /dev/video0 /dev/fb0 900 kernel_drive1_30 30
echo "exit_code=$?"

./ov5640_diag /dev/i2c-1 status | grep -Ei 'drive|302c'
dmesg | grep -Ei 'csi|overflow|warning|oops' | tail -n 80
```

验收标准：

- 预览前后均报告`drive 0x302c : 0x02 (1x)`；
- 900帧动态画面无错位；
- `driver_sequence_gaps=0`、`timeouts=0`；
- 程序输出`[PASS]`且退出码为0；
- 内核筛选没有新增异常。

## 6. 回退方法

如果需要回到NXP原始值：

```sh
cd /home/book/Workspace/nxp_linux/linux-imx-rel_imx_4.1.15_2.1.0_ga

patch -R --dry-run -p1 < \
  /home/book/Workspace/linux/nfs/project/video_capture/0002-video-r5-ov5640-dvp-drive-1x.patch

patch -R -p1 < \
  /home/book/Workspace/linux/nfs/project/video_capture/0002-video-r5-ov5640-dvp-drive-1x.patch
```

也可以在确认备份文件存在后恢复`ov5640.c.before_video_r5_drive_1x`。回退后需要重新编译、
部署zImage并重启，不能只替换源码。

## 7. 面试表达

可以这样描述故障定位：

> 我先通过三级快照把错误边界定位到原始YUYV输入，再用静态/滚动彩条、15/30fps、
> CSI采样边沿以及OV5640输出驱动强度做单变量实验。下降沿直接乱码，说明原上升沿正确；
> 驱动强度从2×提高到4×时错位逐渐加重，降低到1×后完成54,000帧动态长测且无错位。
> 最终将`0x302c[7:6]`的板级默认值固化为1×，判断根因为高速DVP链路的信号完整性裕量。

这个结论包含“现象、隔离方法、单变量证据、最终修改、长稳验证”五个部分，比只说“改了
一个寄存器后正常”更能体现嵌入式音视频调试能力。
