CROSS_COMPILE ?= arm-linux-gnueabihf-
CC := $(CROSS_COMPILE)gcc

TARGETS := v4l2_probe v4l2_capture v4l2_preview v4l2_record ov5640_diag

# Keep the flags compatible with GCC 4.9.4 and the Cortex-A7 hard-float rootfs.
CFLAGS := -O2 -g -Wall -Wextra -std=gnu99 \
	-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
	-DAV_ENABLE_NEON=1
THREAD_FLAGS := -pthread

.PHONY: all clean info

all: $(TARGETS)

v4l2_probe: v4l2_probe.c
	$(CC) $(CFLAGS) -o $@ $^

v4l2_capture: v4l2_capture.c
	$(CC) $(CFLAGS) -o $@ $^

v4l2_preview: v4l2_preview.c av_video_capture.c av_video_capture.h
	$(CC) $(CFLAGS) $(THREAD_FLAGS) -o $@ v4l2_preview.c av_video_capture.c

v4l2_record: v4l2_record.c av_video_capture.c av_video_capture.h
	$(CC) $(CFLAGS) -o $@ v4l2_record.c av_video_capture.c

ov5640_diag: ov5640_diag.c
	$(CC) $(CFLAGS) -o $@ $^

%: %.c
	$(CC) $(CFLAGS) -o $@ $^

info:
	@echo "CROSS_COMPILE=$(CROSS_COMPILE)"
	@echo "CC=$(CC)"
	@echo "TARGETS=$(TARGETS)"
	@echo "CFLAGS=$(CFLAGS)"
	@echo "THREAD_FLAGS=$(THREAD_FLAGS)"

clean:
	rm -f $(TARGETS)
