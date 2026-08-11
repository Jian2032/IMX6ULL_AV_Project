# AV-INTEGRATION-R2：单摄像头生产者的LCD/JPEG分发

## 本轮目标

AV-R2在已冻结的AV-R1上增加LCD预览，但不允许再打开一次`/dev/video0`。

```text
                              +-> encoder -> JPEG槽
OV5640 -> CSI -> V4L2 -> raw槽+
                              +-> LCD私有YUYV -> RGB565 -> fbdev双缓冲

WM8960 -> SAI/SDMA -> ALSA -> audio ring -> monitor
```

## 为什么不能同时运行`v4l2_preview`和`http_mjpeg`

两个旧程序都会独立打开、配置并启动`/dev/video0`。即使驱动允许两次`open()`，它们也会竞争同一CSI硬件、格式和vb2队列，无法证明buffer归属正确。

AV-R2保留唯一V4L2 capture线程，其他功能只读取已离开CSI MMAP的cacheable完整帧。

## raw旁路快照API

`av_mjpeg_pipeline_copy_latest_raw()`可以复制最新的`READY`或`READING` raw槽：

- `READY`表示完整帧已发布但编码线程尚未取走。
- `READING`表示编码线程正在只读访问，数据仍然不可变。
- 复制时持有pipeline mutex，所以encoder不能在`memcpy()`完成前把该槽变回`FREE`。
- LCD拿到的是自己的YUYV buffer，后续NEON转换和等待VSYNC不占用raw槽。
- LCD不改变raw槽状态，不会抢走encoder的帧。

raw新帧发布从`pthread_cond_signal()`改为`pthread_cond_broadcast()`，使encoder和LCD两个等待者都有机会观察新serial。

## LCD线程

LCD线程每轮执行：

```text
copy_latest_raw
    -> 私有cacheable YUYV
    -> NEON BT.601 YUYV-to-RGB565
    -> 顺序blit到隐藏fb页
    -> FBIOPAN_DISPLAY
    -> LCD-R5帧边界completion
```

pipeline交付的YUYV已完成180度方向校正，因此LCD和后续HTTP JPEG看到的方向应完全一致。

## Ubuntu交叉编译

```bash
cd /home/book/Workspace/linux/nfs/project/av_terminal
make clean
make info
make

file av_terminal
arm-linux-gnueabihf-readelf -h av_terminal | grep -E 'Class|Data|Machine'
```

## 开发板准备

先加载LCD-R5并解绑fbcon：

```sh
cd /home/sun/nfs/project/lcdif_fb
insmod av_lcdif_fb.ko

for v in /sys/class/vtconsole/vtcon*; do
    if grep -qi "frame buffer" "$v/name"; then
        echo 0 > "$v/bind"
    fi
done

ls -l /dev/fb0
```

不要同时运行`v4l2_preview`、`http_mjpeg`、`audio_thread_test`或另一个`av_terminal`。

## 30秒验收

```sh
cd /home/sun/nfs/project/av_terminal

./av_terminal --help

./av_terminal 30 \
    /dev/video0 \
    /dev/snd/pcmC0D0c \
    /dev/snd/controlC0 \
    /dev/fb0

echo "av_r2_exit=$?"
```

同时观察LCD：

- 上下方向正确。
- 左右无镜像。
- 颜色与几何结构正确。
- 动态画面无错行和擕裂。
- 640×480居中显示，四周为黑边。

## 通过标准

- `/dev/video0`只打开一次，唯一capture线程负责`DQBUF/QBUF`。
- 视频采集仍尽量保持30fps，`driver_gaps=0`、`timeouts=0`。
- LCD `displayed > 0`、`failed=0`，page 0/1持续交替。
- LCD或JPEG来不及时允许跳过旧帧，但不允许破坏CSI buffer归还。
- 音频约48,000 frame/s，`xruns=0`、`dropped=0`、`sequence_gaps=0`。
- 程序输出`[PASS]`且退出码0。
- `lcdif_stats`中underflow/overflow为0，内核无CSI/ALSA异常。

AV-R2仍不启动HTTP。HTTP接入和慢客户端隔离留到AV-R3。

## AV-R2.1 单核带宽修正

首轮10秒实测中，LCD视觉效果、双缓冲、ALSA和LCDIF统计均正常，但同时运行
LCD与JPEG后视频仅约23fps，`driver_gaps=68`，因此该轮不能通过。

R2.1针对采集热路径作两项修正：

- 将“垂直倒序复制 + cacheable帧水平翻转”合并为一次180度旋转复制；每个
  YUYV宏像素只从CSI MMAP读取一次，不再对整帧做第二次读写遍历。
- LCD复制raw快照前增加短期只读引用，然后释放pipeline mutex再复制614400字节；
  capture发布和槽选择不再被整帧`memcpy()`串行阻塞。

R2.1状态行新增`capture_copy`，用于直接比较修正前后的采集复制耗时。视觉方向必须
仍为上下正确、左右无镜像；正式通过条件仍保持`driver_gaps=0`，不降低验收标准。

## AV-R2.1实测结论与AV-R2.2

R2.1开发板10秒实测：

- LCD显示227帧，约22.72fps，`failed=0`，实际画面正常。
- ALSA采集与消费均为480256帧，`dropped=0`、`xruns=0`、`sequence_gaps=0`。
- 视频仅采集229帧，`driver_gaps=68`，`capture_copy=36.21ms`。
- 30fps每帧只有33.33ms，因此在CSI DMA映射上逐宏像素旋转本身已经超时。
- `imx-uart ... Rx FIFO overrun`发生在程序运行时继续向串口粘贴诊断命令，与
  CSI/LCDIF/ALSA数据路径无关。

AV-R2.2重新划分方向校正位置：

```text
CSI MMAP --contiguous memcpy--> cacheable raw --immediate QBUF
                                      |-> rotate180 planar unpack -> JPEG
                                      `-> cacheable rotate180 copy -> LCD NEON RGB565
```

采集线程不再逐像素处理DMA映射。`av_jpeg_encoder_unpack_yuyv_rotate_180()`在本来
就必须执行的YUYV到planar 4:2:2解包中完成旋转；LCD快照仍向调用者交付方向正确的
私有YUYV，但旋转只访问普通cacheable内存。4×2确定性样本已验证旋转后的Y/U/V平面。

## AV-R2.2实测结论与AV-R2.3资源预算

R2.2开发板10秒实测：视频采集273帧、约27.39fps，`driver_gaps=26`，连续
`memcpy`的平均墙钟时间仍为33.84ms；LCD约20.92fps且`failed=0`，ALSA和LCDIF
仍无错误。说明方向处理位置已经正确，但采集线程在三路CPU竞争中仍被抢占到超过
33.33ms帧预算。

AV-R2.3不降低CSI验收标准，而是明确区分硬件采集率与消费者输出率：

- CSI/V4L2继续配置30fps，`driver_gaps`必须为0。
- LCD最多15fps，每次醒来只处理最新完整raw帧；跳过旧帧换取低延迟。
- AV-R2尚未启用HTTP，JPEG只以5fps持续验证解包、旋转和压缩支路。
- encoder节流只发生在cacheable消费者线程，绝不阻塞`DQBUF/QBUF`。
- `raw_frames_dropped`和LCD `skipped`允许增长，它们表示主动丢弃过期应用帧，
  不能与CSI驱动丢帧混为一谈。

该预算为单核Cortex-A7保留确定性的采集时间。AV-R3接入HTTP后再根据网络需求决定
LCD/JPEG预算，不应让所有消费者无上限争抢CPU。

## AV-R2.3十秒实机结果

- 视频：`captured=300`，30.02fps，`driver_gaps=0`，`timeouts=0`。
- 采集复制：平均18.49ms，已明显低于33.33ms帧周期。
- JPEG：`encoded=52`，稳定约5fps。
- LCD：`displayed=150`，稳定约15fps，`failed=0`。
- 应用策略：`raw_dropped=248`、LCD `skipped=149`，属于主动淘汰过期帧。
- 音频：采集/消费均480256帧，`dropped=0`、`xruns=0`、队列0、sequence gap 0。
- LCDIF：`underflows=0`、`overflows=0`、controller running。
- 内核日志无CSI、ALSA、LCDIF运行时错误；程序输出`[PASS]`，退出码0。

统计验收已经通过。最终LCD视觉检查确认上下方向、左右镜像、颜色与几何、动态画面、
居中位置和四周黑边全部正常。

## Ctrl+C与立即重启结果

运行约15.4秒后按Ctrl+C：

- 视频采集461帧、30.02fps，`driver_gaps=0`、`timeouts=0`。
- JPEG编码78帧；LCD显示230帧，`failed=0`。
- 音频采集/消费均738304帧，零丢弃、零XRUN、队列归零。
- 输出`[STOP] Signal requested a clean A/V shutdown.`，退出码0。

不重启内核、不重载驱动，立即运行3秒：

- 视频90帧、30.02fps，`driver_gaps=0`。
- JPEG 17帧，LCD 45帧，音频144384帧。
- 所有错误统计为0，输出`[PASS]`，退出码0。

以上证明capture/encoder/LCD/audio线程均可唤醒和join，V4L2 stream、ALSA/SDMA、
fbdev映射、用户ring、mixer状态和同步对象均已正确清理，可立即重新申请。结合最终
LCD视觉检查，AV-R2.3全部验收项目通过，AV-R2正式冻结，后续只在AV-R3增加HTTP消费者。
