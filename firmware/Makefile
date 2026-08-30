# firmware/Makefile - LP-zero 베어메탈 펌웨어
#
# 크로스 툴체인을 따로 받지 않는다. clang 은 태생이 크로스 컴파일러라
# --target 만 주면 x86 호스트에서 aarch64 베어메탈 코드를 바로 뽑는다.

# Make 는 CC/LD 에 내장 기본값(cc/ld)을 갖고 있어서 ?= 가 먹지 않는다.
# 값의 출처가 "default" 일 때만 덮어쓰면 사용자가 준 값은 그대로 존중된다.
ifeq ($(origin CC),default)
  CC := clang
endif
ifeq ($(origin LD),default)
  LD := ld.lld
endif
OBJCOPY ?= llvm-objcopy
OBJDUMP ?= llvm-objdump
NM      ?= llvm-nm

TARGET  := aarch64-none-elf
CPU     := cortex-a53

SRCDIR  := src
INCDIR  := include
OBJDIR  := build

IMAGE   := kernel8.img
ELF     := $(OBJDIR)/kernel8.elf
MAP     := $(OBJDIR)/kernel8.map

# ── 컴파일 플래그 ────────────────────────────────────────────────
#  -mcpu=cortex-a53      Zero 2 W 코어에 정확히 맞춘 스케줄링/명령어 선택
#  -ffreestanding        libc 없음. main 이 진입점이라고 가정하지 않음
#  -nostdlibinc          시스템 헤더 배제 (stdarg.h 등 컴파일러 내장은 유지)
#  -fno-builtin          우리 memset 이 memset 호출로 변환되는 무한재귀 방지
#  -mgeneral-regs-only   FP/SIMD 명령 생성 금지. 커널 코드에서 FP 레지스터를
#                        건드리면 컨텍스트 저장이 복잡해진다
#  -fno-pic              0x80000 고정 링크. 위치 독립 코드 불필요
#  -ffunction-sections   --gc-sections 로 안 쓰는 코드 제거 (이미지 축소)
CFLAGS := --target=$(TARGET) -mcpu=$(CPU) \
          -ffreestanding -nostdlibinc -fno-builtin \
          -mgeneral-regs-only \
          -fno-pic -fno-pie -fno-common -fno-stack-protector \
          -ffunction-sections -fdata-sections \
          -std=c11 -O2 \
          -Wall -Wextra -Werror \
          -Wno-unused-command-line-argument \
          -I$(INCDIR)

ASFLAGS := --target=$(TARGET) -mcpu=$(CPU) -ffreestanding -nostdlibinc \
           -I$(INCDIR)

LDFLAGS := -T linker.ld --gc-sections -Map=$(MAP) --no-dynamic-linker

CSRCS := $(wildcard $(SRCDIR)/*.c)
ASRCS := $(wildcard $(SRCDIR)/*.S)
OBJS  := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(CSRCS)) \
         $(patsubst $(SRCDIR)/%.S,$(OBJDIR)/%.o,$(ASRCS))
DEPS  := $(OBJS:.o=.d)

.PHONY: all clean size disasm syms

all: $(IMAGE) size

$(OBJDIR):
	@mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJDIR)/%.o: $(SRCDIR)/%.S | $(OBJDIR)
	@echo "  AS      $<"
	@$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(ELF): $(OBJS) linker.ld
	@echo "  LD      $@"
	@$(LD) $(LDFLAGS) $(OBJS) -o $@

# start.elf 는 ELF 가 아니라 순수 바이너리를 기대한다.
# 이름이 헷갈리지만 kernel8.img 는 헤더 없는 raw 이미지여야 한다.
$(IMAGE): $(ELF)
	@echo "  OBJCOPY $@"
	@$(OBJCOPY) -O binary $< $@

size: $(IMAGE)
	@echo ""
	@echo "  이미지 크기: $$(stat -c%s $(IMAGE)) bytes"
	@echo ""

disasm: $(ELF)
	@$(OBJDUMP) -d --source $(ELF)

syms: $(ELF)
	@$(NM) -n $(ELF)

clean:
	@rm -rf $(OBJDIR) $(IMAGE)
	@echo "  정리 완료"

-include $(DEPS)
