KERNELDIR := /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)
INSTALL_MOD_PATH :=

include Makefile.local

default:
		$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

install:
		$(MAKE) INSTALL_MOD_PATH="$(INSTALL_MOD_PATH)" -C $(KERNELDIR) modules_install

.PHONY: clean
clean:
	   $(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	   rm -f cscope.out tags nvmev.S

.PHONY: cscope
cscope:
		cscope -b -R
		ctags *.[ch]

.PHONY: tags
tags: cscope

.PHONY: format
format:
	clang-format -i *.[ch]

.PHONY: dis
dis:
	objdump -d -S nvmev.ko > nvmev.S

load:
	sudo insmod ./nvmev.ko \
	memmap_start=22G \
	memmap_size=10G \
	cpus=7,8,9,10

unload:
	sudo rmmod nvmev

fio_write:
	sudo fio --name=test --ioengine=libaio --iodepth=1 --rw=write --bs=4k --size=8G --numjobs=1 \
	--filename=/dev/nvme2n1 --direct=1 --group_reporting --norandommap
# fio:
# 	./fio.sh