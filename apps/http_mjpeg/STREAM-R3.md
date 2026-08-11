# STREAM-R3: live V4L2 to single-client HTTP MJPEG

## Scope

STREAM-R3 connects the already accepted VIDEO-R2/R5 capture layer and the
frozen STREAM-R2 JPEG encoder:

```text
OV5640 -> mx6s-csi -> four V4L2 MMAP buffers
       -> DQBUF -> vertically reversed scanline memcpy into cacheable YUYV
       -> QBUF
       -> unpack cacheable YUYV into private planar 4:2:2 storage
       -> static ARM NEON TurboJPEG
       -> multipart/x-mixed-replace
       -> browser or VLC
```

It deliberately serves only one request at a time. While `/stream.mjpg` is
active, another request waits in the listen backlog. Capture/encode/network
thread separation, bounded queues and slow-client policy belong to STREAM-R4.

## Buffer ownership

For each live frame:

1. `av_video_dequeue()` transfers one V4L2 MMAP buffer to user space.
2. Contiguous scanline `memcpy()` calls copy the packed frame into ordinary
   cacheable heap memory in reverse row order. This corrects the board's fixed
   upside-down camera orientation without adding another full-frame pass.
3. The exact dequeued index is immediately returned with `av_video_queue()`.
4. `av_jpeg_encoder_unpack_yuyv()` reads the cacheable copy and writes
   encoder-owned Y, U and V planes.
5. JPEG compression and TCP sending use only private memory.

Network blocking therefore never holds a CSI buffer. It can still make R3 send
old completed frames because there is no latest-frame thread yet; R4 solves
that behavior.

## First live result and R3.1 correction

The first live build produced correctly colored and geometrically intact
moving video, but the user later clarified that its vertical direction had
always been upside down. HTTP framing and the network were healthy: average
send time was only about 0.45 ms and `send_errors` remained zero. It
nevertheless reported 1,017 V4L2 sequence gaps in 419 transmitted frames.

Per-stage timing identified the real bottleneck:

```text
saved frame in ordinary RAM: YUYV unpack about 3.84 ms
live V4L2 MMAP buffer:        YUYV unpack about 95.2 ms
JPEG encode:                  about 19.75 ms
socket send:                  about 0.45 ms
```

The original live path performed scalar, strided reads directly from the CSI
MMAP area. On this kernel those DMA buffers are unsuitable for that access
pattern. Holding one dequeued buffer for roughly 95 ms also exhausts the four
buffer capture queue and explains the sequence gaps.

R3.1 therefore adds one reusable `sizeimage`-byte packed-YUYV staging buffer.
Each frame is copied once, requeued immediately, and then unpacked from normal
cacheable RAM. No per-frame allocation is introduced. Logs and `/status` now
separate `copy`, `unpack`, `encode` and `send` timing so the correction can be
verified on the board.

The R3.1 board regression reduced the measured stages to 12.25 ms copy,
3.91 ms unpack, 19.79 ms encode and 0.43 ms send. It delivered 1,513 frames
with 141 gaps and no camera timeout or send error. This is a major improvement,
but the 36.38 ms serial pipeline still exceeds the 33.33 ms camera period.
Residual throughput gaps therefore belong to STREAM-R4 capture/encode
threading rather than further complicating the sequential R3 design.

R3.2 fuses the required vertical flip into the staging copy. The output rows
are written in reverse order, while bytes within each YUYV row retain their
original order; this changes top/bottom orientation without mirroring left and
right or changing YUYV color pairing.

## R3.2 final board evidence

The corrected build completed two independent browser stream sessions and
1,643 transmitted frames. The user visually confirmed all four properties:

- top and bottom are now correct;
- left and right are not mirrored;
- color and geometry remain correct;
- moving video introduces no new line displacement.

The final accumulated status was:

```text
stream_sessions=2
stream_frames=1643
sequence_gaps=166
camera_timeouts=0
stale_frames_discarded=8
average_jpeg_bytes=37973.47
average_copy_ms=12.449
average_unpack_ms=3.915
average_encode_ms=19.862
average_send_ms=0.453
average_pipeline_ms=36.679
send_errors=0
```

Both browser disconnects were handled as normal `Broken pipe` events and a new
stream could be opened without restarting the server. Ctrl+C stopped the
server with exit code zero, and the kernel log contained no CSI overflow,
warning or oops. One incomplete browser request was counted in
`bad_requests`; it did not affect either stream session.

STREAM-R3 is frozen at this point. Its remaining 166 sequence gaps are an
expected, measured limitation of the 36.679 ms sequential pipeline at a
33.333 ms camera period. Eliminating those gaps is the primary STREAM-R4
threading acceptance target.

## Multipart format

The initial response is:

```text
HTTP/1.1 200 OK
Content-Type: multipart/x-mixed-replace; boundary=imx6ullframe
```

Each JPEG is framed as:

```text
--imx6ullframe
Content-Type: image/jpeg
Content-Length: <bytes>
X-Sequence: <V4L2 sequence>
X-Timestamp-Us: <V4L2 timestamp>

<JPEG bytes>
```

`Content-Length` is supplied for browser/VLC compatibility. A disconnected
client is counted as a normal stream disconnect rather than terminating the
process with SIGPIPE.

## Build

On Ubuntu:

```sh
cd /home/book/Workspace/linux/nfs/project/http_mjpeg
make clean
make info
make

file http_mjpeg
arm-linux-gnueabihf-readelf -d http_mjpeg | grep NEEDED
```

The dynamic dependencies may contain `libm`, `librt` and `libc`, but must not
contain `libturbojpeg.so` or `libjpeg.so`.

## First board test

Do not load the LCD driver; STREAM-R3 only needs the camera and Ethernet.

```sh
cd /home/sun/nfs/project/http_mjpeg

./http_mjpeg --help
./http_mjpeg 8080 0 /dev/video0 30 80
```

Before opening the stream, Windows can verify the control endpoints:

```powershell
curl.exe -i http://192.168.1.50:8080/health
curl.exe -i http://192.168.1.50:8080/status
```

Then open exactly one of:

```text
http://192.168.1.50:8080/
http://192.168.1.50:8080/stream.mjpg
```

The index page embeds the stream. VLC can use the direct stream URL.

STREAM-R3 is sequential, so `/status` will not respond while the stream is
open. Close the browser/VLC stream first, then query status again.

## Acceptance evidence to collect

Run the stream for at least 60 seconds with continuous scene movement, close
the client, query `/status`, then stop the server with Ctrl+C:

```sh
cat /proc/interrupts | grep -Ei 'csi|21c4000'
dmesg | grep -Ei 'csi|overflow|warning|oops' | tail -n 80
```

Pass conditions:

- browser/VLC continuously displays correctly oriented, correctly colored
  moving video;
- `camera_timeouts=0` and `send_errors=0`;
- residual R3 sequence gaps and its serial pipeline time are recorded as the
  baseline that STREAM-R4 must reduce to zero and below 33.33 ms respectively;
- reconnect after closing a client succeeds without restarting the process;
- Ctrl+C performs STREAMOFF, unmaps V4L2 buffers and exits with code 0;
- no CSI overflow, kernel warning or oops appears.

At quality 80, compare the reported average JPEG bytes and complete pipeline
time with the isolated STREAM-R2 result (34,725 bytes and 23.645 ms for its
saved test frame). Live frame sizes naturally vary with scene complexity.
