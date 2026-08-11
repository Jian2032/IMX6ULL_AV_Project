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

.PHONY: all clean info

all:
	$(MAKE) -C $(KDIR) M=$(MODULE_DIR) ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(MODULE_DIR) ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) clean

info:
	@echo "KDIR=$(KDIR)"
	@echo "ARCH=$(ARCH)"
	@echo "CROSS_COMPILE=$(CROSS_COMPILE)"
	@echo "MODULE_DIR=$(MODULE_DIR)"

endif

