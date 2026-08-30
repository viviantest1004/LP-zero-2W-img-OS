/* main.c - LP-zero 펌웨어 Phase 1 진입점.
 *
 * boot.S 가 EL1 / 스택 / BSS 를 준비한 뒤 여기로 들어온다.
 * 하는 일:
 *   1) UART 를 올려 시리얼 콘솔 확보 (모든 디버깅의 기반)
 *   2) 메일박스로 GPU 에게 보드/메모리/클럭/온도 조회
 *   3) 대화형 모니터 - 하드웨어 브링업 중 손으로 찔러볼 수 있게 */
#include "types.h"
#include "bcm2710.h"
#include "mmio.h"
#include "uart.h"
#include "printf.h"
#include "gpio.h"
#include "timer.h"
#include "mbox.h"
#include "board.h"

#define FW_NAME     "LP-zero"
#define FW_VERSION  "0.1.0-phase1"

/* 링커가 채워주는 이미지 경계 */
extern char __image_start[];
extern char __image_end[];
extern char __bss_start[];
extern char __bss_end[];

static u32 current_el(void)
{
    u64 el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return (u32)(el >> 2) & 3u;
}

static void print_banner(void)
{
    kprintf("\n\n");
    kprintf("  _     ____                     \n");
    kprintf(" | |   |  _ \\    ___ _ __ ___    \n");
    kprintf(" | |   | |_) |  |_ /| '__/ _ \\   \n");
    kprintf(" | |___|  __/    / / | | | (_) |  \n");
    kprintf(" |_____|_|      /___||_|  \\___/   \n");
    kprintf("\n");
    kprintf(" %s firmware %s\n", FW_NAME, FW_VERSION);
    kprintf(" Raspberry Pi Zero 2 W / BCM2710A1 / Cortex-A53\n");
    kprintf("================================================\n\n");
}

static void print_kb(const char *label, u32 bytes)
{
    kprintf("  %s%u KiB (%u bytes)\n", label, bytes / 1024, bytes);
}

static void dump_system_info(void)
{
    /* ── 실행 컨텍스트 ── */
    kprintf("[cpu]\n");
    kprintf("  Exception level  : EL%u\n", current_el());

    u64 midr;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    kprintf("  MIDR_EL1         : 0x%08x  (part 0x%03x, rev %u)\n",
            (u32)midr, (u32)((midr >> 4) & 0xFFF), (u32)(midr & 0xF));

    u64 mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    kprintf("  MPIDR_EL1        : 0x%08x  (core %u)\n",
            (u32)mpidr, (u32)(mpidr & 0xFF));

    /* ── 이미지 배치 ── */
    kprintf("\n[image]\n");
    kprintf("  load address     : %p\n", (void *)__image_start);
    kprintf("  end address      : %p\n", (void *)__image_end);
    kprintf("  total size       : %u bytes\n",
            (u32)(__image_end - __image_start));
    kprintf("  bss              : %p .. %p (%u bytes)\n",
            (void *)__bss_start, (void *)__bss_end,
            (u32)(__bss_end - __bss_start));
    kprintf("  stack top        : 0x%08x (grows down)\n",
            (u32)KERNEL_LOAD_ADDR);

    /* ── 보드 정보 (메일박스) ── */
    kprintf("\n[board]\n");
    u32 rev = mbox_get_board_revision();
    if (rev) {
        board_info_t bi;
        board_decode_revision(rev, &bi);
        kprintf("  revision code    : 0x%08x\n", bi.raw);
        kprintf("  model            : Raspberry Pi %s (rev 1.%u)\n",
                bi.model_name, bi.revision);
        kprintf("  processor        : %s\n", bi.processor_name);
        kprintf("  manufacturer     : %s\n", bi.manufacturer_name);
        kprintf("  memory (label)   : %u MB\n", bi.memory_mb);
    } else {
        kprintf("  revision code    : <mailbox failed>\n");
    }

    u64 serial = mbox_get_board_serial();
    kprintf("  serial           : %08x%08x\n",
            (u32)(serial >> 32), (u32)serial);
    kprintf("  firmware rev     : 0x%08x\n", mbox_get_firmware_revision());

    /* ── 메모리 분할 ── */
    kprintf("\n[memory split]\n");
    u32 base, size;
    if (mbox_get_arm_memory(&base, &size)) {
        kprintf("  ARM   base 0x%08x  ", base);
        print_kb("size ", size);
    } else {
        kprintf("  ARM   <mailbox failed>\n");
    }
    if (mbox_get_vc_memory(&base, &size)) {
        kprintf("  GPU   base 0x%08x  ", base);
        print_kb("size ", size);
    } else {
        kprintf("  GPU   <mailbox failed>\n");
    }

    /* ── 클럭 ── */
    kprintf("\n[clocks]  (current / max)\n");
    static const struct { u32 id; const char *name; } clocks[] = {
        { MBOX_CLOCK_ARM,   "arm  " },
        { MBOX_CLOCK_CORE,  "core " },
        { MBOX_CLOCK_SDRAM, "sdram" },
        { MBOX_CLOCK_UART,  "uart " },
        { MBOX_CLOCK_EMMC,  "emmc " },
        { MBOX_CLOCK_V3D,   "v3d  " },
    };
    for (u32 i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
        u32 cur = mbox_get_clock_rate(clocks[i].id);
        u32 max = mbox_get_max_clock_rate(clocks[i].id);
        kprintf("  %s : %4u MHz / %4u MHz\n",
                clocks[i].name, cur / 1000000, max / 1000000);
    }

    /* ── 온도 ── */
    s32 temp = mbox_get_temperature_mc();
    kprintf("\n[thermal]\n");
    if (temp >= 0)
        kprintf("  SoC temperature  : %u.%u C\n",
                (u32)temp / 1000, ((u32)temp % 1000) / 100);
    else
        kprintf("  SoC temperature  : <mailbox failed>\n");

    kprintf("\n");
}

static void print_help(void)
{
    kprintf("\n명령:\n");
    kprintf("  h  이 도움말\n");
    kprintf("  i  시스템 정보 다시 출력\n");
    kprintf("  t  SoC 온도\n");
    kprintf("  c  클럭 현황\n");
    kprintf("  u  부팅 후 경과 시간\n");
    kprintf("  b  ACT LED 10회 깜빡임\n");
    kprintf("  r  재부팅 (워치독)\n\n");
}

static void cmd_temperature(void)
{
    s32 t = mbox_get_temperature_mc();
    if (t >= 0)
        kprintf("SoC: %u.%u C\n", (u32)t / 1000, ((u32)t % 1000) / 100);
    else
        kprintf("온도 조회 실패\n");
}

static void cmd_clocks(void)
{
    kprintf("arm=%u MHz core=%u MHz sdram=%u MHz\n",
            mbox_get_clock_rate(MBOX_CLOCK_ARM)   / 1000000,
            mbox_get_clock_rate(MBOX_CLOCK_CORE)  / 1000000,
            mbox_get_clock_rate(MBOX_CLOCK_SDRAM) / 1000000);
}

static void cmd_uptime(void)
{
    u64 us = timer_get_us();
    kprintf("uptime: %u.%06u s\n",
            (u32)(us / 1000000ULL), (u32)(us % 1000000ULL));
}

static void cmd_blink(void)
{
    kprintf("ACT LED 깜빡이는 중...\n");
    for (u32 i = 0; i < 20; i++) {
        gpio_toggle(BOARD_ACT_LED_PIN);
        delay_ms(100);
    }
    kprintf("완료\n");
}

/* 대화형 모니터. 시리얼로 한 글자씩 받아서 처리한다. */
static void monitor(void) __attribute__((noreturn));
static void monitor(void)
{
    kprintf("모니터 시작. 'h' 로 도움말.\n");

    u64 last_beat = timer_get_us();
    bool led_on = false;

    for (;;) {
        /* 1초마다 ACT LED 토글 - 살아있다는 신호.
         * 극성이 어느 쪽이든 토글이므로 반드시 깜빡인다. */
        u64 now = timer_get_us();
        if (now - last_beat >= 1000000ULL) {
            last_beat = now;
            led_on = !led_on;
            gpio_write(BOARD_ACT_LED_PIN, led_on);
        }

        if (!uart_rx_ready())
            continue;

        char c = uart_getc();
        switch (c) {
        case 'h': case '?': print_help();      break;
        case 'i':           dump_system_info(); break;
        case 't':           cmd_temperature(); break;
        case 'c':           cmd_clocks();      break;
        case 'u':           cmd_uptime();      break;
        case 'b':           cmd_blink();       break;
        case 'r':
            kprintf("재부팅...\n");
            uart_flush();
            board_reset();
            /* 도달하지 않음 */
        case '\r': case '\n':
            break;
        default:
            kprintf("알 수 없는 명령 '%c'. 'h' 로 도움말.\n", c);
            break;
        }
    }
}

void kernel_main(void)
{
    /* UART 가 최우선. 이게 없으면 이후 문제를 볼 방법이 없다. */
    uart_init();

    /* ACT LED 를 출력으로. 시리얼이 안 잡혀도 LED 로 살아있는지 안다. */
    gpio_set_function(BOARD_ACT_LED_PIN, GPIO_FUNC_OUTPUT);

    print_banner();
    dump_system_info();
    print_help();

    monitor();
}
