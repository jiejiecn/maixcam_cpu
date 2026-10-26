# Out-of-tree kernel module: mpll_freq.ko
#
# Build with the kernel tree that matches the running kernel:
#   make -C /path/to/linux-5.10.4 M=$(pwd) ARCH=riscv \
#        CROSS_COMPILE=riscv64-linux-gnu- modules
#
# or just use ../build.sh

obj-m := mpll_freq.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=riscv \
		CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=riscv clean

.PHONY: all clean
