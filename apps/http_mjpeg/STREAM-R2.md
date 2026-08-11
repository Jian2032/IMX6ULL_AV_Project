# STREAM-R2: YUYV single-frame JPEG encoding

## Purpose

This round isolates JPEG from the live V4L2 and HTTP paths. A known-good
640x480 packed-YUYV frame is converted to planar 4:2:2 and encoded repeatedly
with a statically linked ARM libjpeg-turbo. Only after correctness and timing
pass will STREAM-R3 connect the encoder to `/dev/video0`.

## Components

- `setup_libjpeg_turbo.sh`: private, non-root CMake and ARM static-library build.
- `av_jpeg_encoder.[ch]`: reusable encoder with one-time buffer allocation.
- `jpeg_test.c`: exact-size input validation, timing, JPEG marker/SOF validation
  and output-file generation.
- `Makefile`: links only `jpeg_test` against `libturbojpeg.a`; the R1 HTTP
  executable remains independent of JPEG.

The conversion is:

```text
YUYV input:  Y0 U0 Y1 V0
                  |
                  v
planes:      Y[640x480] + U[320x480] + V[320x480]
                  |
                  v
TurboJPEG:   4:2:2 JPEG, quality 80, fast integer DCT
```

This avoids the wasteful `YUYV -> RGB -> YCbCr` conversion. The planar working
memory and worst-case JPEG buffer are allocated in `init()` and reused, which is
required for the later real-time server.

## 1. Obtain one input frame

On the board:

```sh
cd /home/sun/nfs/project/video_capture

./v4l2_capture /dev/video0 120 \
    /home/sun/nfs/project/http_mjpeg/frame_640x480_yuyv.raw

wc -c /home/sun/nfs/project/http_mjpeg/frame_640x480_yuyv.raw
```

The exact result must be `614400` bytes. The capture tool saves one selected
frame after streaming has stopped; it does not write 120 frames to this file.

## 2. Build the dependency without sudo

On Ubuntu:

```sh
cd /home/book/Workspace/linux/nfs/project/http_mjpeg
chmod +x setup_libjpeg_turbo.sh
./setup_libjpeg_turbo.sh
```

The script pins libjpeg-turbo 3.0.4 and installs only into:

```text
/home/book/Workspace/linux/nfs/project/third_party/libjpeg-turbo-arm
```

It also uses a private CMake 3.28.6 because this Ubuntu installation has no
system CMake. The two verified archives are already provided under
`third_party/downloads`, so a normal build does not access the network. If an
archive is missing, the script uses a domestic GitHub accelerator first and the
official URL only as a fallback. It does not require administrator privileges.

The script rejects HTML error pages and corrupted downloads with pinned SHA-256
values before extracting anything:

```text
cmake-3.28.6-linux-x86_64.tar.gz
  931e3c0d546ee03ca72bb147ccd9b49e3b6252f765f66bf21b9d165519940458
libjpeg-turbo-3.0.4.tar.gz
  99130559e7d62e8d695f2c0eaeef912c5828d5b84a0537dcb24c9678c9d5b76b
```

Expected products:

```sh
ls -lh ../third_party/libjpeg-turbo-arm/lib/libturbojpeg.a
ls -lh ../third_party/libjpeg-turbo-arm/include/turbojpeg.h
```

## 3. Cross-compile

```sh
make clean
make info
make

file http_mjpeg jpeg_test
arm-linux-gnueabihf-readelf -h jpeg_test | grep -E 'Class|Data|Machine'
arm-linux-gnueabihf-readelf -d jpeg_test | grep -Ei 'jpeg|turbo'
```

Both executables must be ARM EABI5. The last command should print no JPEG
`NEEDED` entry, proving that TurboJPEG was linked statically.

## 4. Board acceptance

```sh
cd /home/sun/nfs/project/http_mjpeg

./jpeg_test --help
./jpeg_test frame_640x480_yuyv.raw 640 480 80 300 \
    frame_640x480_q80.jpg

echo "exit_code=$?"
ls -l frame_640x480_q80.jpg
od -An -tx1 -N 16 frame_640x480_q80.jpg
```

Acceptance conditions:

- program exits with `0` and prints `[PASS]`;
- JPEG reports 640x480 and valid SOI/EOI/SOF;
- no per-frame allocation occurs inside the 300-iteration loop;
- end-to-end average supports at least 15 fps (less than 66.67 ms/frame);
- preferably it supports 30 fps (less than 33.33 ms/frame).

Copy the JPEG to the shared directory and inspect it on Ubuntu:

```sh
file frame_640x480_q80.jpg
ffplay -loop 1 frame_640x480_q80.jpg
```

The picture must have the same geometry and colors as the VIDEO-R2 raw frame.

## Measured acceptance evidence (2026-08-10)

The dependency and ARM build passed:

- libjpeg-turbo 3.0.4 configured as a 32-bit ARM build;
- `HAVE_NEON` succeeded and CMake reported `SIMD extensions: arm`;
- the ARM AArch32 NEON C/assembly objects were built into
  `libturbojpeg.a`;
- `jpeg_test` is ARM ELF32, little-endian, EABI5;
- its only dynamic dependencies are `libm.so.6`, `librt.so.1` and
  `libc.so.6`, so no JPEG shared library is needed on the BusyBox rootfs.

The board encoded a 640x480 quality-80 frame 300 times after one warm-up:

```text
JPEG size       : 34,725 bytes
raw/JPEG ratio  : 17.69:1
FNV-1a          : 0x0002c930
YUYV unpack avg : 3,838.99 us
JPEG encode avg : 19,805.68 us
pipeline avg    : 23,644.68 us
theoretical rate: 42.29 fps
exit code       : 0
```

The generated file reports 640x480, has valid SOI/EOI/SOF markers and begins
with the expected JFIF bytes `ff d8 ff e0 00 10 4a 46 49 46`.  At 30 fps, this
sample would require approximately 8.33 Mbit/s before small HTTP multipart/TCP
overheads, which is well below the board's measured 100-Mbit/s Ethernet link.

The pipeline has about 9.69 ms of budget remaining in each 33.33-ms camera
period.  STREAM-R3 must measure the complete capture/encode/send path because
the 42.29-fps number is an isolated single-frame benchmark, not yet a live
streaming result.

Technical acceptance is complete.  The only remaining R2 freeze item is to
open `frame_640x480_q80.jpg` on the PC and confirm geometry and colors visually.

## Failure isolation

- input not 614400 bytes: recapture or correct width/height; do not change the
  encoder to accept a truncated frame;
- build cannot find `libturbojpeg.a`: run the setup script and inspect its first
  failed command;
- JPEG is valid but image content is wrong: compare the YUYV input with the
  already proven VIDEO-R2/VIDEO-R5 tools;
- output is correct but too slow: use the separate unpack and encode timings to
  determine whether packing or JPEG DCT is the bottleneck.
