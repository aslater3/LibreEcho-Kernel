#ccflags-y += -DCONFIG_MTK_FPGA
#ccflags-y += -DFIX_TO_26M

ccflags-y += -I$(srctree)/drivers/misc/mediatek/uart
ccflags-y += -I$(srctree)/drivers/misc/mediatek/uart/mt8163
ccflags-y += -I$(srctree)/drivers/misc/mediatek/uart/include
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/$(MTK_PLATFORM)/include
ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/$(subst ",,$(CONFIG_MTK_PLATFORM))

obj-$(CONFIG_MTK_SERIAL) := platform_uart.o
