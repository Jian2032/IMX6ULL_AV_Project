CROSS_COMPILE ?= arm-linux-gnueabihf-
CC := $(CROSS_COMPILE)gcc
TARGETS := http_mjpeg jpeg_test

# In the NFS development tree this module is /project/http_mjpeg, therefore
# the default installation made by setup_libjpeg_turbo.sh is one level up.
# A different prefix can be selected with: make JPEG_PREFIX=/some/path
JPEG_PREFIX ?= ../third_party/libjpeg-turbo-arm
JPEG_INCLUDE := $(JPEG_PREFIX)/include
JPEG_STATIC := $(JPEG_PREFIX)/lib/libturbojpeg.a

# Keep the flags compatible with GCC 4.9.4 and the Cortex-A7 hard-float rootfs.
CFLAGS := -O2 -g -Wall -Wextra -std=gnu99 \
	-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
THREAD_FLAGS := -pthread
JPEG_CFLAGS := -I$(JPEG_INCLUDE)
JPEG_LIBS := $(JPEG_STATIC) -lm -lrt

.PHONY: all clean info

all: $(TARGETS)

VIDEO_DIR := ../video_capture

http_mjpeg: http_mjpeg.c av_mjpeg_pipeline.c av_mjpeg_pipeline.h \
		av_jpeg_encoder.c av_jpeg_encoder.h \
		$(VIDEO_DIR)/av_video_capture.c \
		$(VIDEO_DIR)/av_video_capture.h $(JPEG_STATIC)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $(JPEG_CFLAGS) -I$(VIDEO_DIR) -o $@ \
		http_mjpeg.c av_mjpeg_pipeline.c av_jpeg_encoder.c \
		$(VIDEO_DIR)/av_video_capture.c $(JPEG_LIBS)

jpeg_test: jpeg_test.c av_jpeg_encoder.c av_jpeg_encoder.h $(JPEG_STATIC)
	$(CC) $(CFLAGS) $(JPEG_CFLAGS) -o $@ \
		jpeg_test.c av_jpeg_encoder.c $(JPEG_LIBS)

$(JPEG_STATIC):
	@echo "ERROR: $(JPEG_STATIC) does not exist."
	@echo "Run ./setup_libjpeg_turbo.sh first, then run make again."
	@false

info:
	@echo "CROSS_COMPILE=$(CROSS_COMPILE)"
	@echo "CC=$(CC)"
	@echo "TARGETS=$(TARGETS)"
	@echo "CFLAGS=$(CFLAGS)"
	@echo "THREAD_FLAGS=$(THREAD_FLAGS)"
	@echo "JPEG_PREFIX=$(JPEG_PREFIX)"
	@echo "JPEG_STATIC=$(JPEG_STATIC)"
	@echo "DEPENDENCIES=POSIX sockets/pthreads; static libturbojpeg; V4L2 UAPI"

clean:
	rm -f $(TARGETS)
