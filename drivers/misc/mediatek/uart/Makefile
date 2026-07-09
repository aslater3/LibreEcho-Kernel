#ccflags-y += -DCONFIG_MTK_FPGA
#ccflags-y += -DFIX_TO_26M

MTK_PLATFORM := $(subst ",,$(CONFIG_MTK_PLATFORM))
ccflags-y += -I$(srctree)/drivers/misc/mediatek/uart/$(MTK_PLATFORM)
ccflags-y += -I$(srctree)/drivers/misc/mediatek/uart
ccflags-y += -I$(srctree)/drivers/misc/mediatek/uart/mt8163
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/$(MTK_PLATFORM)/include

obj-$(CONFIG_MTK_SERIAL) := uart.o

obj-y += $(subst ",,$(CONFIG_MTK_PLATFORM))/
