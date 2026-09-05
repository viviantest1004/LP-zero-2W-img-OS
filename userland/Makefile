# userland/Makefile - 자체 libc 위에 올린 init 과 셸.
#
# 외부 libc 를 전혀 링크하지 않는다. crt0 부터 printf 까지 전부 우리 코드다.
# 정적 링크이므로 동적 링커(ld-linux-aarch64.so)도 필요 없다 -
# initramfs 에 바이너리만 넣으면 그대로 돈다.

ifeq ($(origin CC),default)
  CC := clang
endif
ifeq ($(origin LD),default)
  LD := ld.lld
endif
STRIP   ?= llvm-strip
OBJDUMP ?= llvm-objdump

# ARCH picks the machine. clang is a cross compiler for everything it
# supports, so this is a target triple and a CPU name and nothing else -
# no second toolchain to install.
#
#   make            -> arm64, the Pi Zero 2 W and arm64 VMs
#   make ARCH=amd64 -> x86-64, an ordinary desktop or VM
ARCH ?= arm64

ifeq ($(ARCH),amd64)
  TARGET   := x86_64-linux-gnu
  CPUFLAGS := -march=x86-64
  OBJDIR   := build-amd64
  BINDIR   := bin-amd64
  # The same system on an ordinary desktop, and it says so rather than
  # claiming to be a Raspberry Pi.
  OSNAME   ?= linux-LP
  HOSTNAME ?= linux-lp
  OSTITLE  ?= linux-LP
  OSMACHINE ?= amd64 PC or virtual machine  -  built from scratch
else ifeq ($(ARCH),arm64)
  TARGET   := aarch64-linux-gnu
  CPUFLAGS := -mcpu=cortex-a53
  OBJDIR   := build
  BINDIR   := bin
  OSNAME   ?= LP-zero
  HOSTNAME ?= lpzero
  OSTITLE  ?= test_a_123_LPzero2W_img
  OSMACHINE ?= Raspberry Pi Zero 2 W  -  built from scratch
else
  $(error ARCH must be arm64 or amd64, not "$(ARCH)")
endif

LIBC_DIR := libc

#  -ffreestanding   표준 라이브러리 가정 없음
#  -nostdlibinc     시스템 헤더 배제 (stdarg.h 등 컴파일러 내장은 유지)
#  -fno-builtin     우리 memset 이 memset 호출로 바뀌는 무한재귀 방지
#  -static          동적 링커 없이 단독 실행
CFLAGS := --target=$(TARGET) $(CPUFLAGS) \
          -ffreestanding -nostdlibinc -fno-builtin \
          -fno-stack-protector -fno-pic -fno-pie \
          -ffunction-sections -fdata-sections \
          -std=c11 -Os \
          -Wall -Wextra -Werror \
          -DLP_OS_NAME='"$(OSNAME)"' \
          -DLP_OS_TITLE='"$(OSTITLE)"' \
          -DLP_OS_MACHINE='"$(OSMACHINE)"' \
          -I$(LIBC_DIR)/include

ASFLAGS := --target=$(TARGET) $(CPUFLAGS) -ffreestanding -nostdlibinc

LDFLAGS := -static --gc-sections -e _start

LIBC_SRCS := $(wildcard $(LIBC_DIR)/src/*.c)
LIBC_ASM  := $(wildcard $(LIBC_DIR)/src/*.S)
LIBC_OBJS := $(patsubst $(LIBC_DIR)/src/%.c,$(OBJDIR)/libc/%.o,$(LIBC_SRCS)) \
             $(patsubst $(LIBC_DIR)/src/%.S,$(OBJDIR)/libc/%.o,$(LIBC_ASM))

PROGS := init sh cat ls cp mv rm touch edit help more dhcp ntp date mount zram guard bootcount expandfs mkdir sleep kill top watchdog logd sysinfo calc clear wc head tail free df grep ps find du chmod splash pkg dmesg usage ping nslookup wget ifconfig route ln uname hostname uptime whoami sort uniq cut tee sha256sum reset stat fsck authkey integrity tar run ipconfig chattr dropprivs firewall disk part datadisk net beacon update chown id useradd su service pidof persist poweroff automount storage apt which basename dirname seq rev yes nl cmp tr printf xargs sed
BINS  := $(addprefix $(BINDIR)/,$(PROGS))

# libc 오브젝트는 중간 파일이 아니다. 이것이 없으면 make 가 패턴 규칙
# 사슬 끝에서 build/libc/*.o 를 지워버리고, 다음 빌드 때 다시 짓는다.
.SECONDARY:

.PHONY: all clean size test

all: $(BINS) size

$(OBJDIR)/libc $(OBJDIR)/prog $(BINDIR):
	@mkdir -p $@

$(OBJDIR)/libc/%.o: $(LIBC_DIR)/src/%.c | $(OBJDIR)/libc
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJDIR)/libc/%.o: $(LIBC_DIR)/src/%.S | $(OBJDIR)/libc
	@echo "  AS      $<"
	@$(CC) $(ASFLAGS) -c $< -o $@

$(OBJDIR)/prog/%.o: %/%.c | $(OBJDIR)/prog
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# 각 프로그램은 자기 소스 하나 + libc 전체
.SECONDEXPANSION:
$(BINDIR)/%: $$*/$$*.c $(LIBC_OBJS) | $(BINDIR) $(OBJDIR)/prog
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $(OBJDIR)/prog/$*.o
	@echo "  LD      $@"
	@$(LD) $(LDFLAGS) $(OBJDIR)/prog/$*.o $(LIBC_OBJS) -o $@
	@$(STRIP) $@

size: $(BINS)
	@echo ""
	@for b in $(BINS); do \
	    printf "  %-12s %8s bytes\n" "$$b" "$$(stat -c%s $$b)"; \
	done
	@echo ""

clean:
	@rm -rf $(OBJDIR) $(BINDIR)
	@echo "  정리 완료"

-include $(shell find $(OBJDIR) -name '*.d' 2>/dev/null)
