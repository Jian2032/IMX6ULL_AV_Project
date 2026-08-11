# LCD-R5：稳定性与诊断收尾

## 本轮定位

LCD-R5是LCDIF模块第5次、也是最后一次计划内代码迭代。R1～R4已经
分别验证资源获取、点屏、fbdev和双缓冲，本轮只做工程化收尾：

1. `FBIOBLANK`停止和恢复LCDIF；
2. 通过fb notifier联动现有`pwm-backlight`；
3. 统计FIFO underflow/overflow异常；
4. 提供只读`lcdif_stats` sysfs诊断；
5. 验证长时间翻页和重复加载/卸载。

本轮不修改设备树和内核配置。DTS已经存在`pwm-backlight`节点，
`CONFIG_BACKLIGHT_PWM=y`，因此复用Linux背光框架。

## blank为何不直接操作PWM寄存器

```text
fb_test
  |
  | FBIOBLANK
  v
fbdev core
  |
  +--> av_lcdif_fb.fb_blank：停止/恢复LCDIF扫描
  |
  +--> FB_EVENT_BLANK notifier
          |
          v
       backlight core
          |
          v
       pwm-backlight：关闭/恢复背光
```

LCDIF驱动只负责显示控制器，PWM背光仍由`pwm-backlight`驱动负责。
这体现了Linux驱动框架之间通过公共接口协作，而不是互相直接访问
对方硬件寄存器。

## Linux 4.1.15的blank锁顺序

用户程序执行`FBIOBLANK`时，fbdev core已经使用以下锁顺序：

```text
console_lock
  -> fb_info lock
    -> fb_blank()
      -> av_lcdif_fb_blank()
      -> fbcon/backlight notifier
```

驱动在`probe`、`remove`和`shutdown`中也会主动发布blank事件，但这些
调用没有经过ioctl入口。因此R5使用`av_lcdif_publish_blank()`补齐相同
的锁顺序。否则Linux 4.1.15的fbcon notifier进入VT层时，会在
`do_unblank_screen()`触发`WARN_CONSOLE_UNLOCKED`。

`av_lcdif_fb_blank()`回调自身不能再获取`console_lock`，否则用户态
`FBIOBLANK`路径会对同一把不可递归锁重复加锁。

## 编译前备份R4

```sh
cd /home/book/Workspace/linux/nfs/project
cp -a lcdif_fb lcdif_fb_R4
cd lcdif_fb
```

同步R5的`av_lcdif_fb.c`、`fb_test.c`、`Makefile`和本说明后编译：

```sh
make clean
make info
make

modinfo av_lcdif_fb.ko | grep -E 'description|version'
file av_lcdif_fb.ko fb_test
```

预期模块版本为`R5`。

## 功能验收

### 1. 加载并解绑fbcon

```sh
cd /home/sun/nfs/project/lcdif_fb
insmod av_lcdif_fb.ko
dmesg | tail -n 50
dmesg | grep -E "WARNING:|do_unblank_screen|cut here"

for v in /sys/class/vtconsole/vtcon*; do
    if grep -qi "frame buffer" "$v/name"; then
        echo 0 > "$v/bind"
    fi
done
```

修订后的R5不应输出`do_unblank_screen`、`WARNING`或`cut here`。

### 2. 检查背光和诊断节点

```sh
ls -l /sys/class/backlight
cat /sys/bus/platform/devices/21c8000.lcdif/lcdif_stats
./fb_test info
```

初始诊断预期类似：

```text
version=R5
controller=running
blank_state=0
scanout_dma=0x8c200000
flips=1
vsync_waits=0
underflows=0
overflows=0
blank_events=0
```

`flips=1`通常来自fbcon注册阶段的初始翻页，不要求一定为1。

### 3. blank和背光恢复

先显示容易辨认的画面，再测试：

```sh
./fb_test gradient
./fb_test blank 2
cat /sys/bus/platform/devices/21c8000.lcdif/lcdif_stats
```

预期过程：

1. PWM背光关闭约2秒；
2. 背光恢复；
3. 恢复后仍是原渐变图，显存内容没有丢失；
4. 应用输出`blank test passed`；
5. `blank_events=2`，并新增1次VSYNC验证。

### 4. 长翻页和异常计数

```sh
./fb_test vsync 300
./fb_test flip 600
cat /proc/interrupts | grep -Ei "lcdif|21c8000"
cat /sys/bus/platform/devices/21c8000.lcdif/lcdif_stats
```

预期：

- VSYNC与翻页均无超时；
- 平均频率接近58.5Hz；
- `underflows=0`；
- `overflows=0`。

如果异常计数非零，保留统计和`dmesg`，不要直接把计数隐藏。

### 5. 卸载

```sh
rmmod av_lcdif_fb
dmesg | tail -n 30
ls -l /dev/fb* 2>/dev/null
ls -l /sys/bus/platform/devices/21c8000.lcdif/lcdif_stats 2>/dev/null
```

卸载会主动发布POWERDOWN事件，所以LCDIF停止、背光关闭、`/dev/fb0`
和`lcdif_stats`同时消失。日志应包含最终计数。

## 五轮重复加载稳定性

先确认模块已经卸载，然后执行：

```sh
i=1
while [ "$i" -le 5 ]; do
    echo "===== cycle $i ====="
    insmod ./av_lcdif_fb.ko || break

    for v in /sys/class/vtconsole/vtcon*; do
        if grep -qi "frame buffer" "$v/name"; then
            echo 0 > "$v/bind"
        fi
    done

    ./fb_test vsync 10 || break
    cat /sys/bus/platform/devices/21c8000.lcdif/lcdif_stats
    rmmod av_lcdif_fb || break

    if [ -e /dev/fb0 ]; then
        echo "ERROR: /dev/fb0 remains after unload"
        break
    fi

    i=$((i + 1))
done
echo "completed cycles: $((i - 1))"
```

预期`completed cycles: 5`，每轮：

- DMA显存成功分配；
- IRQ成功申请；
- VSYNC测试通过；
- underflow/overflow为0；
- 模块成功卸载；
- `/dev/fb0`没有残留。

## 最终实机验收结果

LCD-R5最终验收通过：

- 五轮重复加载/卸载完成，`completed cycles: 5`；
- 每轮都成功分配2,457,600字节DMA显存并注册`/dev/fb0`；
- 每轮10次VSYNC均通过，耗时0.155～0.161秒；
- 每轮统计均为`flips=1`、`vsync_waits=10`、
  `underflows=0`、`overflows=0`；
- 卸载前`blank_events=0`，卸载主动POWERDOWN后最终日志为`blank=1`；
- 每轮卸载均成功，未触发`/dev/fb0`或`lcdif_stats`残留检查；
- 最终`do_unblank_screen`、`WARNING`、`cut here`筛选无输出。

10次短样本使用`等待次数/总耗时`计算，首个VSYNC与开始计时点之间不足
一个完整周期，因此会得到62.20～64.67Hz的偏高结果。300次长样本的
58.66Hz与实际49,972,866Hz像素时钟计算值一致，应作为刷新率结论。

LCDIF驱动到此冻结，完整代码学习见
`docs/learning/day1_lcdif_complete.md`。

## 最终证据清单

- [x] `make`、`modinfo`、`file`；
- [x] 加载日志和初始`lcdif_stats`；
- [x] blank停止、恢复及恢复后VSYNC；
- [x] blank后的`lcdif_stats`；
- [x] 300次VSYNC与600帧翻页；
- [x] 最终异常计数和卸载日志；
- [x] 五轮重复加载/卸载。
