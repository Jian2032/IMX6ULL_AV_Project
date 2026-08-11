CROSS_COMPILE ?= arm-linux-gnueabihf-
CC := $(CROSS_COMPILE)gcc
TARGETS := av_terminal

# The live NFS tree keeps each learning module as a sibling directory.
VIDEO_DIR ?= ../video_capture
AUDIO_DIR ?= ../audio_capture
MJPEG_DIR ?= ../http_mjpeg
JPEG_PREFIX ?= ../third_party/libjpeg-turbo-arm
JPEG_INCLUDE := $(JPEG_PREFIX)/include
JPEG_STATIC := $(JPEG_PREFIX)/lib/libturbojpeg.a

CFLAGS := -O2 -g -Wall -Wextra -std=gnu99 \
	-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
THREAD_FLAGS := -pthread
NEON_FLAGS := -DAV_ENABLE_NEON=1
CPPFLAGS := -I$(VIDEO_DIR) -I$(AUDIO_DIR) -I$(MJPEG_DIR) \
	-I$(JPEG_INCLUDE)
JPEG_LIBS := $(JPEG_STATIC) -lm -lrt

AV_SOURCES := av_terminal.c av_lcd_preview.c av_http_server.c \
	$(VIDEO_DIR)/av_video_capture.c \
	$(AUDIO_DIR)/av_audio_capture.c \
	$(MJPEG_DIR)/av_mjpeg_pipeline.c \
	$(MJPEG_DIR)/av_jpeg_encoder.c

.PHONY: all clean info

all: $(TARGETS)

av_terminal: $(AV_SOURCES) \
		$(VIDEO_DIR)/av_video_capture.h \
		$(AUDIO_DIR)/av_audio_capture.h \
		av_lcd_preview.h av_http_server.h \
		$(MJPEG_DIR)/av_mjpeg_pipeline.h \
		$(MJPEG_DIR)/av_jpeg_encoder.h $(JPEG_STATIC)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $(NEON_FLAGS) $(CPPFLAGS) -o $@ \
		$(AV_SOURCES) $(JPEG_LIBS)

$(JPEG_STATIC):
	@echo "ERROR: $(JPEG_STATIC) does not exist."
	@echo "Build the ARM static libjpeg-turbo package before AV-R2."
	@false

clean:
	rm -f $(TARGETS)

info:
	@echo "CROSS_COMPILE=$(CROSS_COMPILE)"
	@echo "CC=$(CC)"
	@echo "TARGETS=$(TARGETS)"
	@echo "CFLAGS=$(CFLAGS)"
	@echo "THREAD_FLAGS=$(THREAD_FLAGS)"
	@echo "NEON_FLAGS=$(NEON_FLAGS)"
	@echo "VIDEO_DIR=$(VIDEO_DIR)"
	@echo "AUDIO_DIR=$(AUDIO_DIR)"
	@echo "MJPEG_DIR=$(MJPEG_DIR)"
	@echo "JPEG_PREFIX=$(JPEG_PREFIX)"
	@echo "JPEG_STATIC=$(JPEG_STATIC)"
	@echo "DEPENDENCIES=V4L2/ALSA/fbdev UAPI; pthreads; static libturbojpeg"
