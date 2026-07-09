ifneq ($(KERNELRELEASE),)
	ccflags-y += -I$(src)/inc
	ccflags-y += -I$(src)/plat_inc
	ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat
	ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/mt8163/include

	obj-y += btif.o
	btif-y	:= mtk_btif.o mtk_btif_exp.o btif_dma_plat.o btif_plat.o
	btif-$(CONFIG_AMAZON_A2DP_TS) += btif_audio_ts.o
else
    KERNELDIR ?= /lib/modules/$(shell uname -r)/build
    PWD  := $(shell pwd)
default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
endif
