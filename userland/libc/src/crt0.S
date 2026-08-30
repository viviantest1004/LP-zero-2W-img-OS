/* crt0.S - 프로세스 진입점.
 *
 * 커널은 execve 직후 스택을 이렇게 만들어두고 여기로 점프한다:
 *
 *   sp + 0                  argc
 *   sp + 8                  argv[0]
 *   ...                     argv[argc-1]
 *   sp + 8 + argc*8         NULL
 *   sp + 8 + argc*8 + 8     envp[0]
 *   ...                     NULL
 *   ...                     auxv[]
 *
 * 동적 링커가 없으므로(정적 링크) 우리가 직접 argc/argv/envp 를 꺼내
 * main 으로 넘긴다. */

.section .text._start, "ax"
.global _start
.type _start, %function

_start:
    /* 스택 되감기(unwind)가 여기서 멈추도록 프레임/링크 레지스터를 0 으로 */
    mov     x29, xzr
    mov     x30, xzr

    ldr     x0, [sp]                /* x0 = argc                       */
    add     x1, sp, #8              /* x1 = argv                       */

    /* envp = argv + (argc+1)*8   ... argv 종료 NULL 다음 */
    add     x2, x0, #1
    add     x2, x1, x2, lsl #3      /* x2 = envp                       */

    /* environ 전역에 저장해 getenv 등이 쓸 수 있게 한다 */
    adrp    x3, environ
    add     x3, x3, :lo12:environ
    str     x2, [x3]

    /* AAPCS64 는 호출 시점 sp 가 16바이트 정렬일 것을 요구한다.
     * AArch64 는 SP 를 논리연산의 피연산자로 못 쓰므로 레지스터를 경유한다.
     * (커널이 이미 정렬해서 주지만 방어적으로 맞춘다) */
    mov     x9, sp
    and     x9, x9, #-16
    mov     sp, x9

    bl      main

    /* main 의 반환값이 종료 코드. exit_group 은 스레드 그룹 전체를 끝낸다. */
    mov     x8, #94                 /* SYS_exit_group */
    svc     #0

    /* 도달하지 않지만 방어적으로 */
1:  b       1b

.size _start, . - _start
