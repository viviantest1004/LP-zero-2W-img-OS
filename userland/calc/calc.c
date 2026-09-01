/* calc - 정수 계산기.
 *
 *   calc "1 + 2 * 3"          인자로 한 번 계산
 *   calc                      대화형 모드 (빈 줄이나 Ctrl-D 로 종료)
 *
 * 지원:
 *   산술    + - * / % **        (** 는 거듭제곱)
 *   비트    & | ^ ~ << >>
 *   괄호    ( )
 *   입력    10진수, 0x16진수, 0b2진수
 *
 * 결과를 10진수/16진수/2진수로 함께 보여준다. 이 프로젝트에서는
 * 주소나 비트마스크를 다룰 일이 많아 그때마다 변환하는 것보다 낫다.
 *
 * 64비트 정수만 다룬다. 부동소수점은 없다 - 우리 printf 가 지원하지
 * 않고, 넣으면 코드가 몇 배로 커진다.
 *
 * 재귀 하향 파서다. 우선순위가 낮은 것부터 높은 것으로 내려간다:
 *   or -> xor -> and -> shift -> add -> mul -> unary -> power -> atom
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

typedef struct {
    const char *p;
    bool        error;
    const char *msg;
} parser_t;

static s64 parse_or(parser_t *ps);

static void fail(parser_t *ps, const char *msg)
{
    if (!ps->error) {          /* 첫 오류만 기억한다 */
        ps->error = true;
        ps->msg = msg;
    }
}

static void skip_space(parser_t *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t') ps->p++;
}

/* 다음이 op 면 소비하고 true. 두 글자 연산자를 먼저 봐야 한다
 * (** 를 * 로, << 를 < 로 읽으면 안 된다). */
static bool eat(parser_t *ps, const char *op)
{
    skip_space(ps);
    size_t n = strlen(op);
    if (strncmp(ps->p, op, n) != 0)
        return false;
    ps->p += n;
    return true;
}

static s64 parse_atom(parser_t *ps)
{
    skip_space(ps);

    if (eat(ps, "(")) {
        s64 v = parse_or(ps);
        if (!eat(ps, ")"))
            fail(ps, "닫는 괄호가 없습니다");
        return v;
    }

    /* 0x / 0b 접두사 */
    int base = 10;
    if (ps->p[0] == '0' && (ps->p[1] == 'x' || ps->p[1] == 'X')) {
        base = 16; ps->p += 2;
    } else if (ps->p[0] == '0' && (ps->p[1] == 'b' || ps->p[1] == 'B')) {
        base = 2;  ps->p += 2;
    }

    const char *start = ps->p;
    s64 v = 0;

    for (;;) {
        int d;
        char c = *ps->p;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (c == '_')             { ps->p++; continue; }  /* 1_000_000 */
        else break;

        if (d >= base) break;
        v = v * base + d;
        ps->p++;
    }

    if (ps->p == start) {
        fail(ps, "숫자가 와야 합니다");
        return 0;
    }
    return v;
}

/* 거듭제곱은 오른쪽 결합이다: 2**3**2 = 2**(3**2) */
static s64 parse_power(parser_t *ps)
{
    s64 base = parse_atom(ps);
    if (!eat(ps, "**"))
        return base;

    s64 exp = parse_power(ps);
    if (exp < 0) {
        fail(ps, "음수 지수는 정수 계산에서 쓸 수 없습니다");
        return 0;
    }

    s64 r = 1;
    while (exp-- > 0) {
        r *= base;
        if (exp > 62) { fail(ps, "너무 큽니다"); return 0; }
    }
    return r;
}

static s64 parse_unary(parser_t *ps)
{
    skip_space(ps);
    if (eat(ps, "-")) return -parse_unary(ps);
    if (eat(ps, "+")) return  parse_unary(ps);
    if (eat(ps, "~")) return ~parse_unary(ps);
    return parse_power(ps);
}

static s64 parse_mul(parser_t *ps)
{
    s64 v = parse_unary(ps);
    for (;;) {
        skip_space(ps);
        /* ** 는 거듭제곱이므로 * 로 읽으면 안 된다 */
        if (ps->p[0] == '*' && ps->p[1] == '*') return v;

        if (eat(ps, "*")) { v *= parse_unary(ps); continue; }
        if (eat(ps, "/")) {
            s64 d = parse_unary(ps);
            if (d == 0) { fail(ps, "0 으로 나눌 수 없습니다"); return 0; }
            v /= d; continue;
        }
        if (eat(ps, "%")) {
            s64 d = parse_unary(ps);
            if (d == 0) { fail(ps, "0 으로 나눌 수 없습니다"); return 0; }
            v %= d; continue;
        }
        return v;
    }
}

static s64 parse_add(parser_t *ps)
{
    s64 v = parse_mul(ps);
    for (;;) {
        skip_space(ps);
        if (eat(ps, "+")) { v += parse_mul(ps); continue; }
        if (eat(ps, "-")) { v -= parse_mul(ps); continue; }
        return v;
    }
}

static s64 parse_shift(parser_t *ps)
{
    s64 v = parse_add(ps);
    for (;;) {
        if (eat(ps, "<<")) { v = (s64)((u64)v << (parse_add(ps) & 63)); continue; }
        if (eat(ps, ">>")) { v = (s64)((u64)v >> (parse_add(ps) & 63)); continue; }
        return v;
    }
}

/* & | ^ 는 << >> 보다 낮은 우선순위 (C 와 같다) */
static s64 parse_and(parser_t *ps)
{
    s64 v = parse_shift(ps);
    while (eat(ps, "&")) v &= parse_shift(ps);
    return v;
}

static s64 parse_xor(parser_t *ps)
{
    s64 v = parse_and(ps);
    for (;;) {
        skip_space(ps);
        if (ps->p[0] == '^' && ps->p[1] != '^') { ps->p++; v ^= parse_and(ps); continue; }
        return v;
    }
}

static s64 parse_or(parser_t *ps)
{
    s64 v = parse_xor(ps);
    while (eat(ps, "|")) v |= parse_xor(ps);
    return v;
}

/* 2진수는 앞의 0 을 떼고 4자리씩 끊어 보여준다 */
static void print_binary(u64 v)
{
    if (v == 0) { printf("0"); return; }

    int top = 63;
    while (top > 0 && !((v >> top) & 1)) top--;

    for (int i = top; i >= 0; i--) {
        printf("%c", ((v >> i) & 1) ? '1' : '0');
        if (i && i % 4 == 0) printf("_");
    }
}

static void show(s64 v)
{
    printf("  %ld\n", (long)v);
    printf("  0x%lx\n", (unsigned long)v);
    printf("  0b"); print_binary((u64)v); printf("\n");
}

static int evaluate(const char *expr, bool verbose)
{
    parser_t ps = { .p = expr, .error = false, .msg = NULL };
    s64 v = parse_or(&ps);

    skip_space(&ps);
    if (!ps.error && *ps.p)
        fail(&ps, "식 뒤에 알 수 없는 문자가 있습니다");

    if (ps.error) {
        dprintf(STDERR_FILENO, "calc: %s\n", ps.msg);
        if (*ps.p)
            dprintf(STDERR_FILENO, "      -> %s\n", ps.p);
        return 1;
    }

    if (verbose) show(v);
    else         printf("%ld\n", (long)v);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        /* 인자를 모두 이어붙인다. 셸이 공백으로 나눠 넘기기 때문이다. */
        char expr[512];
        expr[0] = '\0';
        for (int i = 1; i < argc; i++) {
            strlcpy(expr + strlen(expr), argv[i], sizeof(expr) - strlen(expr));
            if (i + 1 < argc)
                strlcpy(expr + strlen(expr), " ", sizeof(expr) - strlen(expr));
        }
        return evaluate(expr, true);
    }

    printf("calc - 정수 계산기.  빈 줄이나 Ctrl-D 로 종료\n");
    printf("  + - * / %% **   & | ^ ~ << >>   ( )   0x.. 0b..\n\n");

    char line[512];
    for (;;) {
        printf("calc> ");
        long n = readline(STDIN_FILENO, line, sizeof(line));
        if (n < 0) { printf("\n"); break; }
        if (n == 0) break;
        evaluate(line, true);
    }
    return 0;
}
