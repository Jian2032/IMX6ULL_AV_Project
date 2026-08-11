CROSS_COMPILE ?= arm-linux-gnueabihf-
CC := $(CROSS_COMPILE)gcc

TARGETS := audio_probe audio_capture audio_thread_test
CFLAGS := -O2 -g -Wall -Wextra -std=gnu99 \
	-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
THREAD_FLAGS := -pthread

.PHONY: all clean info

all: $(TARGETS)

audio_probe: audio_probe.c
	$(CC) $(CFLAGS) -o $@ $<

audio_capture: audio_capture.c
	$(CC) $(CFLAGS) -o $@ $<

audio_thread_test: audio_thread_test.c av_audio_capture.c av_audio_capture.h
	$(CC) $(CFLAGS) $(THREAD_FLAGS) -o $@ \
		audio_thread_test.c av_audio_capture.c

clean:
	rm -f $(TARGETS)

info:
	@echo "CROSS_COMPILE=$(CROSS_COMPILE)"
	@echo "CC=$(CC)"
	@echo "TARGETS=$(TARGETS)"
	@echo "CFLAGS=$(CFLAGS)"
	@echo "THREAD_FLAGS=$(THREAD_FLAGS)"
	@echo "DEPENDENCIES=Linux ALSA UAPI only; no libasound"
