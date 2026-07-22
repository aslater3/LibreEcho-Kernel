SOURCE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
REPO_ROOT := $(abspath $(SOURCE_DIR)/../../..)

OUT_DIR ?= /tmp/libreecho-mt8163-connectivity
ARM_CC ?= arm-linux-gnueabihf-gcc
QEMU_ARM ?= qemu-arm-static
READELF ?= readelf
FILE ?= file

ARM_CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror -Os \
	-march=armv7-a -mfloat-abi=hard -mfpu=vfpv3-d16 -fno-ident
ARM_LDFLAGS := -static -Wl,--build-id=none

PROGRAMS := wmt_responder wmt_configure wmt_bt_on
OUTPUTS := $(addprefix $(OUT_DIR)/,$(PROGRAMS))

.PHONY: all check clean

all: $(OUTPUTS)

$(OUT_DIR):
	mkdir -p $@

$(OUT_DIR)/%: $(SOURCE_DIR)/%.c | $(OUT_DIR)
	$(ARM_CC) $(ARM_CFLAGS) $(ARM_LDFLAGS) -o $@ $<

check: all
	@set -eu; \
	for binary in $(OUTPUTS); do \
		$(FILE) "$$binary" | grep -q 'ELF 32-bit LSB executable, ARM'; \
		! $(READELF) -l "$$binary" | grep -q 'INTERP'; \
		$(QEMU_ARM) "$$binary" --help >/dev/null; \
	done
	@! $(QEMU_ARM) $(OUT_DIR)/wmt_responder --unknown \
		>$(OUT_DIR)/responder-negative.out 2>&1
	@grep -q 'unknown or incomplete option' $(OUT_DIR)/responder-negative.out
	@! $(QEMU_ARM) $(OUT_DIR)/wmt_bt_on \
		>$(OUT_DIR)/bt-no-gate.out 2>&1
	@grep -q -- '--execute-bt-only-once' $(OUT_DIR)/bt-no-gate.out
	@! $(QEMU_ARM) -strace $(OUT_DIR)/wmt_bt_on --execute-bt-only-once \
		--device /dev/null >$(OUT_DIR)/bt-null.out 2>$(OUT_DIR)/bt-null.strace
	@test "$$(grep -c 'ioctl(' $(OUT_DIR)/bt-null.strace)" -eq 1
	@grep -Eq '0x4004a006|1074044934' $(OUT_DIR)/bt-null.strace
	@grep -Eq '0x80000000|2147483648' $(OUT_DIR)/bt-null.strace
	@$(QEMU_ARM) $(OUT_DIR)/wmt_configure --inspect-patches \
		--firmware-dir $(REPO_ROOT)/firmware >$(OUT_DIR)/patch-inspection.out
	@test "$$(grep -c '^patch_info ' $(OUT_DIR)/patch-inspection.out)" -eq 2
	@grep -q 'seq=2 size=128720 header=8a:00 route=22:00:06:00 address=00:00:06:00' \
		$(OUT_DIR)/patch-inspection.out
	@grep -q 'seq=1 size=50148 header=8a:00 route=21:00:0e:f0 address=00:00:0e:f0' \
		$(OUT_DIR)/patch-inspection.out
	@grep -q 'device_opened=no' $(OUT_DIR)/patch-inspection.out
	@python3 -B $(SOURCE_DIR)/test_wmt_configure.py \
		--binary $(OUT_DIR)/wmt_configure \
		--firmware $(REPO_ROOT)/firmware --qemu $(QEMU_ARM)
	@! grep -E 'FUNC_ON|0x4004a006|WMTDRV_TYPE|--no-function-on' \
		$(SOURCE_DIR)/wmt_configure.c
	@test "$$(grep -c 'result = ioctl(fd, WMT_BT_ONLY_REQUEST' \
		$(SOURCE_DIR)/wmt_bt_on.c)" -eq 1
	@printf '%s\n' 'connectivity_helper_contract=PASS architecture=ARM32 static=yes device_touched=no'

clean:
	rm -f $(OUTPUTS) \
		$(OUT_DIR)/responder-negative.out \
		$(OUT_DIR)/bt-no-gate.out \
		$(OUT_DIR)/bt-null.out \
		$(OUT_DIR)/bt-null.strace \
		$(OUT_DIR)/patch-inspection.out
