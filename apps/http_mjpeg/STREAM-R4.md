# STREAM-R4: threaded latest-frame MJPEG pipeline

## Objective

STREAM-R3 proved the complete V4L2/JPEG/HTTP path, but its sequential service
time averaged 36.679 ms against a 33.333 ms camera period. The network was not
the bottleneck; holding all stages in one loop allowed the capture queue to
fall behind.

STREAM-R4 separates ownership and scheduling:

```text
mx6s-csi V4L2 MMAP
        |
        v
capture pthread
  DQBUF -> vertically corrected scanline copy -> QBUF
        |
        v
3 cacheable YUYV latest-frame slots
        |
        v
encoder pthread
  unpack 4:2:2 -> TurboJPEG -> publish complete JPEG
        |
        v
3 complete JPEG latest-frame slots
        |
        v
network pthread
  copy newest JPEG -> blocking multipart send
```

The main thread only accepts and parses HTTP requests. `/health` and `/status`
therefore remain responsive while `/stream.mjpg` is active.

## Ownership rules

Raw YUYV slots use four states:

```text
FREE -> WRITING -> READY -> READING -> FREE
```

- only the capture thread owns `WRITING`;
- only the encoder thread owns `READING`;
- a producer may replace the oldest `READY` slot;
- neither producer may touch a `READING` or `WRITING` slot.

JPEG slots are `FREE`, `WRITING` or `READY`. The network thread copies the
newest complete JPEG into its own preallocated buffer while holding the queue
mutex, then frees old READY history. A socket send never references shared
pipeline storage.

## Latest-frame policy and statistics

The statistics deliberately distinguish three different events:

- `driver_sequence_gaps`: V4L2 did not deliver a camera sequence; R4 acceptance
  requires zero;
- `raw_frames_dropped`: capture succeeded, but the encoder deliberately chose
  a newer YUYV frame; this is allowed for bounded latency;
- `client_frames_skipped`: the network client was slower than the JPEG
  producer and skipped already encoded history; this is the slow-client policy,
  not a camera failure.

`jpeg_frames_replaced` also grows while no stream client is connected because
the encoder continues publishing fresh JPEGs and replaces old unconsumed
snapshots. It is a queue activity counter, not an error counter.

## Single-core performance boundary

i.MX6ULL has one Cortex-A7 CPU core. Threads isolate ownership and let the
capture sleeper wake promptly, but they cannot execute the copy and JPEG work
on two CPU cores. The R3.2 evidence was approximately:

```text
MMAP to RAM copy       12.449 ms
RAM YUYV unpack         3.915 ms
JPEG encode            19.862 ms
network send            0.453 ms
```

Copy plus unpack/encode consumes slightly more than one 30-fps period. R4 is
therefore expected to preserve a 30-fps, zero-gap capture thread while the
encoder presents roughly the newest 27 fps and increments `raw_frames_dropped`
when necessary. Achieving encoded 30 fps on this CPU is a separate R5 tuning
question involving quality, resolution, frame rate or a more optimized codec
path.

## HTTP concurrency policy

- one active `/stream.mjpg` client is supported;
- a second stream request receives `503 Service Unavailable` and
  `Retry-After: 1`;
- ordinary `/`, `/health` and `/status` requests remain available during the
  stream;
- disconnecting the stream resets the persistent network worker, allowing an
  immediate reconnect without restarting capture or encoding;
- a slow client owns only its private JPEG copy and cannot block capture.

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

`http_mjpeg` now links with `-pthread`. It must still have no dynamic
`libturbojpeg.so` or `libjpeg.so` dependency.

## Stage A: basic threaded regression

On the board:

```sh
cd /home/sun/nfs/project/http_mjpeg

./http_mjpeg --help
./http_mjpeg 8080 0 /dev/video0 30 80
```

The startup log must identify STREAM-R4, three raw slots, three JPEG slots and
the private-copy network policy. Open on Windows:

```text
http://192.168.1.50:8080/
```

While the moving stream is still open, run in PowerShell:

```powershell
curl.exe http://192.168.1.50:8080/health
curl.exe http://192.168.1.50:8080/status
```

Unlike R3, both requests must return immediately during streaming.

## Stage B: second-stream rejection

Keep the browser stream open and run:

```powershell
curl.exe -i --max-time 3 http://192.168.1.50:8080/stream.mjpg
```

The second stream must receive 503; the first stream must remain correct.

## Stage C: slow-client isolation

Close the browser stream, then use one PowerShell window:

```powershell
curl.exe --limit-rate 50k --max-time 20 `
  http://192.168.1.50:8080/stream.mjpg -o NUL
```

During that command, query `/status` from another PowerShell window. Expected:

- `stream_client` remains `active`;
- `captured_frames` and `encoded_frames` continue increasing;
- `driver_sequence_gaps` remains zero;
- `client_frames_skipped` eventually increases as the slow reader falls
  behind;
- `send_errors` and `camera_timeouts` remain zero.

After the slow request exits, reopen the browser stream. Reconnect must work
without restarting the server.

## Final cleanup evidence

Stop with Ctrl+C and collect:

```sh
echo "server_exit=$?"

cat /proc/interrupts | grep -Ei 'csi|21c4000'

dmesg |
grep -Ei 'csi|overflow|warning|oops' |
tail -n 80
```

R4 acceptance requires:

- correct orientation, color and geometry;
- concurrent `/status` while streaming;
- a second stream receives 503 without disturbing the first;
- slow-client and reconnect tests pass;
- `driver_sequence_gaps=0`, `capture_timeouts=0`, `send_errors=0`;
- `raw_frames_dropped` and `client_frames_skipped` are interpreted according
  to the latest-frame policy rather than treated as driver loss;
- Ctrl+C exits with code zero and no kernel warning/oops appears.

## Measured board evidence

The staged acceptance tests on the i.MX6ULL produced the following evidence.

### Normal client and concurrent HTTP requests

```text
capture                 30.017 fps
encoder                 25.440 fps
driver_sequence_gaps    0
capture_timeouts        0
send_errors             0
```

While the browser stream remained active, `/health` and `/status` returned
immediately. A second `/stream.mjpg` request received `503 Service
Unavailable`, `Retry-After: 1`, and incremented `rejected_streams` without
interrupting the first client.

At clean shutdown, frame accounting was exact:

```text
captured_frames         1224
encoded_frames          1038
raw_frames_dropped       186
```

Thus `captured = encoded + raw_dropped`; the application deliberately replaced
old raw frames while the V4L2 driver delivered a continuous sequence.

### Slow client and reconnect

A Windows curl client was limited to 50 KiB/s for 20 seconds. During the slow
stream, the main HTTP thread remained responsive and the camera remained at
30.017 fps with zero driver gaps and zero timeouts. Two live status snapshots
showed:

```text
uptime                 stream frames   client frames skipped   average send
18.877 s                    23                  223              382.465 ms
26.008 s                    34                  398              457.996 ms
```

The client exited by its configured timeout and the server reported a normal
peer reset. Two later browser connections were accepted without restarting the
server. Final slow-client/reconnect accounting was:

```text
stream_sessions          3
stream_frames          459
client_frames_skipped  473
disconnects              2
driver_gaps              0
capture_timeouts         0
send_errors              0
capture rate        30.017 fps
encode rate         26.069 fps
```

The kernel log contained no CSI overflow, warning or oops. The final serial
console output included the clean `[STOP]` path. A later shell status of 127 was
caused by accidentally pasting a log line as a command after the server had
already stopped; it was not the server process exit status.

These measurements validate the queue ownership, latest-frame policy,
slow-client isolation, second-client rejection and reconnect paths. Final R4
freeze additionally requires the user's visual confirmation of orientation,
color, geometry and motion after reconnect.

## R4.1 horizontal-mirror correction

The final visual target test reported:

```text
vertical direction      correct
horizontal direction    mirrored
color and geometry      correct
moving image            no line displacement
reconnect               successful
```

The existing reverse-row copy had corrected the vertical direction, but it did
not change the pixel order within each row. R4.1 keeps that contiguous CSI
MMAP-to-cacheable-RAM copy, returns the V4L2 buffer with QBUF, and then mirrors
the private cacheable row horizontally.

Packed YUYV cannot be reversed byte by byte. Each four-byte macropixel is
`Y0 U Y1 V`; U and V are shared by two adjacent pixels. R4.1 reverses the
macropixel order and exchanges Y0/Y1 inside every macropixel:

```text
left  [LY0 LU LY1 LV] ... right [RY0 RU RY1 RV]

becomes

left  [RY1 RU RY0 RV] ... right [LY1 LU LY0 LV]
```

This preserves each source pair's chroma while reversing its two luma samples.
Together with the existing vertical flip, the published orientation is a
180-degree rotation. `/status` therefore reports `STREAM-R4.1` and
`orientation=rotate-180`.

The private-RAM transformation is deliberately performed after QBUF. It does
not extend CSI DMA-buffer ownership and avoids the severe cost previously
measured when unpacking pixels directly from the CSI MMAP mapping.

R4.1 passed host `-Wall -Wextra -Werror` compilation and GCC `-fanalyzer`.
Board acceptance must confirm readable, non-mirrored text and re-check capture
rate, driver gaps, raw drops and the new average copy/orientation time.

## R4.1 final acceptance and freeze

The ARM build identified itself as `STREAM-R4.1`, and `/status` reported
`orientation=rotate-180`. The user verified all visual requirements after a
disconnect/reconnect cycle:

```text
vertical direction      correct
horizontal mirror       corrected; text reads normally
color and geometry      correct
moving image            correct; no line displacement
reconnect               successful
```

The final run produced:

```text
captured_frames         1548
encoded_frames          1079
raw_frames_dropped       469
stream_frames            796
driver_sequence_gaps       0
capture_timeouts           0
send_errors                0
capture_fps           30.017
encode_fps            20.906
average_copy_ms       27.497
average_unpack_ms      6.503
average_encode_ms     37.866
average_send_ms        0.501
```

Frame accounting is again exact: `1548 = 1079 + 469`. The horizontal
cacheable-RAM pass increased the measured copy/orientation stage from about
21.0 ms to 27.5 ms. Together with scene-dependent JPEG work, the single-core
Cortex-A7 encoded about 20.9 fps, while the isolated capture thread still
accepted every 30-fps driver sequence. This is the intended latest-frame
tradeoff: application-level raw replacement grows, but camera DMA continuity
and live latency remain bounded.

The program followed the clean `[STOP]` path and the kernel log contained no
CSI overflow, warning or oops. STREAM-R4.1 is therefore frozen as the Day 5
baseline. A future performance revision may use a NEON horizontal-mirror path,
lower JPEG quality/resolution, or a lower stream frame rate; those changes are
optimization work, not correctness fixes required by this baseline.
