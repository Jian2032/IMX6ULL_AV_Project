# i.MX6ULL LCDIF外部内核模块Makefile
#
# 默认值对应当前项目环境，仍允许命令行覆盖：
#   make KDIR=/other/kernel CROSS_COMPILE=/other/toolchain-prefix-

ifneq ($(KERNELRELEASE),)

obj-m += av_lcdif_fb.o

else

KDIR ?= /home/book/Workspace/nxp_linux/linux-imx-rel_imx_4.1.15_2.1.0_ga
ARCH ?= arm
CROSS_COMPILE ?= arm-linux-gnueabihf-
MODULE_DIR := $(shell pwd)
APP := fb_test
APP_CC ?= $(CROSS_COMPILE)gcc
APP_CFLAGS ?= -O2 -Wall -Wextra -std=gnu99 \
	-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard

.PHONY: all module app clean info

all: module app

module:
	$(MAKE) -C $(KDIR) M=$(MODULE_DIR) ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) modules

app: $(APP)

$(APP): fb_test.c
	$(APP_CC) $(APP_CFLAGS) -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(MODULE_DIR) ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) clean
	$(RM) $(APP)

info:
	@echo "KDIR=$(KDIR)"
	@echo "ARCH=$(ARCH)"
	@echo "CROSS_COMPILE=$(CROSS_COMPILE)"
	@echo "MODULE_DIR=$(MODULE_DIR)"
	@echo "APP_CC=$(APP_CC)"
	@echo "APP_CFLAGS=$(APP_CFLAGS)"

endif
