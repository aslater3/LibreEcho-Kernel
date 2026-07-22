MTK_PLATFORM := $(subst ",,$(CONFIG_MTK_PLATFORM))
subdir-ccflags-y += -Werror -I$(srctree)/drivers/misc/mediatek/include
subdir-ccflags-y += -Werror -I$(srctree)/drivers/misc/mediatek/include/mt-plat/$(MTK_PLATFORM)/include
subdir-ccflags-y += -Werror -I$(srctree)/drivers/misc/mediatek/include/mt-plat
ccflags-y += -Werror -I$(srctree)/drivers/misc/mediatek/base/power/$(MTK_PLATFORM)

amzn-mt-spi-objs := amzn-mt-spi-pcm.o

# This vendor logging file uses legacy %lu formats for 32-bit size_t and
# counter values.  The MTK subtree enables -Werror globally; keep the
# diagnostic-only logging intact while allowing the Amazon driver to build
# with the current ARM32 compiler.
CFLAGS_amzn-mt-spi-pcm.o += -Wno-format

obj-y	+= amzn-mt-spi.o
