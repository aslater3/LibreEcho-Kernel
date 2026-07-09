# BTIF driver for AD DIE
ifneq ($(KERNELRELEASE),)
	MTK_PLATFORM := $(subst ",,$(CONFIG_MTK_PLATFORM))
	subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/$(MTK_PLATFORM)/include
	subdir-ccflags-y += -I$(srctree)/arch/arm/mach-$(MTK_PLATFORM)/include/mach
    obj-y += common/
else
    KERNELDIR ?= /lib/modules/$(shell uname -r)/build
    PWD  := $(shell pwd)
default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
endif
