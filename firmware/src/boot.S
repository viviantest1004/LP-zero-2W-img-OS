/* boot.S - LP-zero OS 최초 진입점.
 *
 * ── 부팅 사슬 ─────────────────────────────────────────────────────
 *  1) BCM2710 실리콘 내부 부트ROM (GPU/VideoCore가 먼저 깨어난다)
 *  2) SD카드의 bootcode.bin          <- Broadcom 클로즈드 블롭
 *  3) SD카드의 start.elf + fixup.dat <- Broadcom 클로즈드 블롭
 *       · SDRAM 초기화, config.txt 파싱, ARM 코어 릴리즈
 *  4) kernel8.img 를 0x80000 에 적재하고 여기로 점프  <-- 우리 코드 시작
 *
 * 여기부터는 전부 우리가 쓴다.
 *
 * ── 진입 상태 ─────────────────────────────────────────────────────
 *  · config.txt 에 arm_64bit=1 이므로 AArch64
 *  · 기본 armstub 을 거쳐 보통 EL2 로 진입한다 (EL3/EL1 도 방어적으로 처리)
 *  · 코어 4개가 전부 같은 주소로 들어올 수 있다 -> 0번만 살리고 나머지 파킹
 *  · MMU/캐시 꺼짐, 스택 없음, BSS 미초기화
 */

.section ".text.boot"
.global _start

_start:
    /* ── 1. 나는 몇 번 코어인가? MPIDR_EL1[7:0] = Aff0 ──────────── */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbz     x0, primary_core

    /* 보조 코어(1~3)는 여기서 잠든다.
     * WFE 는 SEV 가 오기 전까지 저전력 대기. Phase 2 에서 SMP 를
     * 붙일 때 스핀테이블로 깨울 예정. */
secondary_park:
    wfe
    b       secondary_park

primary_core:
    /* ── 2. 지금 어느 예외 레벨인가? ────────────────────────────── */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #3
    b.eq    drop_from_el3
    cmp     x0, #2
    b.eq    drop_from_el2
    b       el1_entry               /* 이미 EL1 이면 바로 진행 */

    /* ── 3a. EL3 -> EL2 강하 ────────────────────────────────────── */
drop_from_el3:
    mrs     x0, scr_el3
    orr     x0, x0, #(1 << 10)      /* RW  : 하위 EL 은 AArch64      */
    orr     x0, x0, #(1 << 0)       /* NS  : Non-secure              */
    msr     scr_el3, x0

    mov     x0, #0x3C9              /* DAIF 마스크 + M=0b01001(EL2h) */
    msr     spsr_el3, x0
    adr     x0, drop_from_el2
    msr     elr_el3, x0
    eret

    /* ── 3b. EL2 -> EL1 강하 ────────────────────────────────────── */
drop_from_el2:
    /* EL1 을 AArch64 로 (HCR_EL2.RW = 1) */
    mrs     x0, hcr_el2
    orr     x0, x0, #(1 << 31)
    msr     hcr_el2, x0

    /* EL2 에서 FP/SIMD·트레이스 트랩 해제.
     * 0x33FF 는 CPTR_EL2 의 RES1 비트만 세운 값 = 아무것도 트랩 안 함. */
    mov     x0, #0x33FF
    msr     cptr_el2, x0
    msr     hstr_el2, xzr           /* CP15 트랩 없음 */

    /* EL1 이 물리 카운터/타이머를 직접 볼 수 있게 허용 */
    mrs     x0, cnthctl_el2
    orr     x0, x0, #3              /* EL1PCTEN | EL1PCEN */
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr        /* 가상 카운터 오프셋 0 */

    /* EL1 SCTLR 초기값: MMU/캐시/정렬검사 전부 끄고 RES1 비트만 세움.
     * 0x30D00800 = bit 11,20,22,23,28,29 (아키텍처가 1을 요구) */
    mov     x0, #0x0800
    movk    x0, #0x30D0, lsl #16
    msr     sctlr_el1, x0

    /* EL1 에서 FP/SIMD 접근 허용 (CPACR_EL1.FPEN = 0b11) */
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0

    mov     x0, #0x3C5              /* DAIF 마스크 + M=0b00101(EL1h) */
    msr     spsr_el2, x0
    adr     x0, el1_entry
    msr     elr_el2, x0
    eret

    /* ── 4. EL1 에서 C 실행 환경 만들기 ─────────────────────────── */
el1_entry:
    /* 스택 포인터. 우리 이미지는 0x80000 에 실리고 위로 자라므로
     * 스택은 0x80000 에서 아래로 내려간다 (약 512KB 여유).
     * 저주소 0x00~0xFF 의 펌웨어 스핀테이블까지는 절대 못 닿는다. */
    movz    x0, #0x8, lsl #16       /* 0x00080000 */
    mov     sp, x0

    /* BSS 를 0 으로. 링커가 16바이트 정렬을 보장하므로 8바이트씩 쓴다. */
    adrp    x0, __bss_start
    add     x0, x0, :lo12:__bss_start
    adrp    x1, __bss_end
    add     x1, x1, :lo12:__bss_end
1:
    cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b
2:
    /* ── 5. 예외 벡터를 먼저 건다 ───────────────────────────────
     *
     * kernel_main 안에서 걸지 않고 여기서 거는 이유: 그 사이에 나는
     * 예외를 놓치기 때문이다. UART 초기화 자체가 잘못된 주소를
     * 건드리는 버그였던 적이 있는데, 벡터가 없으면 그 순간 보드가
     * 조용해질 뿐이고 - 하필 아직 UART 가 없어서 아무 말도 못 하는
     * 구간이라 - 증상만 보면 원인을 짐작할 방법이 전혀 없다.
     *
     * 벡터를 먼저 걸어두면 최소한 CPU 가 우리 핸들러까지는 온다.
     * 거기서 UART 를 직접 깨워 사정을 말할 수 있다. */
    adrp    x0, vector_table
    add     x0, x0, :lo12:vector_table
    msr     vbar_el1, x0
    isb

    /* C 로 진입. 돌아오지 않는다. */
    bl      kernel_main

    /* 혹시 돌아오면 안전하게 정지 */
halt:
    wfe
    b       halt

.size   _start, . - _start
.type   _start, %function
