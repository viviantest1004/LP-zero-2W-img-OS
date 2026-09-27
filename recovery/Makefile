# recovery/Makefile - build lp-recovery, the recovery menu that runs on
# the LP-RECOVERY partition.
#
#   make -C recovery                 -> recovery/build/amd64/lp-recovery
#   make -C recovery ARCH=arm64      (and armv6: it builds everywhere the
#                                     userland does, though only the amd64
#                                     laptop has a recovery partition)
#
# lp-recovery is a static program on our own libc, like everything in
# userland/, but it is several source files and userland's Makefile
# builds one file per program - so it has this small Makefile of its own,
# with the same compiler flags (warnings are errors) and its own copy of
# the libc objects under build/<arch>/, so a build here never races a
# userland build in the same tree. build/ is in .gitignore;
# tools/mkrecovery.sh runs this and copies the result into the recovery
# root as /recovery/bin/lp-recovery.

ifeq ($(origin CC),default)
  CC := clang
endif
ifeq ($(origin LD),default)
  LD := ld.lld
endif
STRIP ?= llvm-strip

ARCH ?= amd64
ifeq ($(ARCH),amd64)
  TARGET   := x86_64-linux-gnu
  CPUFLAGS := -march=x86-64
else ifeq ($(ARCH),arm64)
  TARGET   := aarch64-linux-gnu
  CPUFLAGS := -mcpu=cortex-a53
else ifeq ($(ARCH),armv6)
  TARGET   := arm-linux-gnueabihf
  CPUFLAGS := -march=armv6kz -mfpu=vfp -mfloat-abi=hard -marm
else
  $(error ARCH must be amd64, arm64 or armv6, not "$(ARCH)")
endif

LIBC   := ../userland/libc
OUT    := build/$(ARCH)
CFLAGS := --target=$(TARGET) $(CPUFLAGS) \
          -ffreestanding -nostdlibinc -fno-builtin \
          -fno-stack-protector -fno-pic -fno-pie \
          -ffunction-sections -fdata-sections \
          -std=c11 -O2 -Wall -Wextra -Werror \
          -DLP_OS_NAME='"linux-LP"' -DLP_OS_TITLE='"linux-LP"' \
          -DLP_OS_MACHINE='"LP Recovery"' \
          -I$(LIBC)/include -I.
ASFLAGS := --target=$(TARGET) $(CPUFLAGS) -ffreestanding -nostdlibinc
LDFLAGS := -static --gc-sections -e _start

LIBC_SRCS := $(wildcard $(LIBC)/src/*.c)
LIBC_ASM  := $(wildcard $(LIBC)/src/*.S)
LIBC_OBJS := $(patsubst $(LIBC)/src/%.c,$(OUT)/libc/%.o,$(LIBC_SRCS)) \
             $(patsubst $(LIBC)/src/%.S,$(OUT)/libc/%.o,$(LIBC_ASM))

SRCS := lp-recovery.c screen.c system.c auth.c reinstall.c osk.c term.c
OBJS := $(patsubst %.c,$(OUT)/%.o,$(SRCS))

all: $(OUT)/lp-recovery

$(OUT)/libc/%.o: $(LIBC)/src/%.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OUT)/libc/%.o: $(LIBC)/src/%.S
	@mkdir -p $(dir $@)
	@echo "  AS      $<"
	@$(CC) $(ASFLAGS) -c $< -o $@

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OUT)/lp-recovery: $(OBJS) $(LIBC_OBJS)
	@echo "  LD      $@"
	@$(LD) $(LDFLAGS) $(OBJS) $(LIBC_OBJS) -o $@
	@$(STRIP) $@
	@echo "  $$(stat -c%s $@) bytes  $@"

clean:
	rm -rf build

.PHONY: all clean
.SECONDARY:
-include $(shell find $(OUT) -name '*.d' 2>/dev/null)
