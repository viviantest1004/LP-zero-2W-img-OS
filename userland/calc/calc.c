/* calc - calculator.
 *
 *   calc "1 + 2 * 3"        evaluate once, from the argument
 *   calc 10 / 4             2.5
 *   calc                    interactive (blank line or Ctrl-D to leave)
 *
 * Supported:
 *   arithmetic  + - * / % **      (** is power)
 *   decimals    1.25, and any division that does not come out even
 *   bitwise     & | ^ ~ << >>
 *   grouping    ( )
 *   input       decimal, 0x hex, 0b binary
 *
 * A whole result is shown in decimal, hex and binary at once. This
 * project deals with addresses and bit masks constantly, and converting
 * by hand every time is worse.
 *
 * ── Fractions, not floating point ──
 *
 * It used to be integers only, so `calc 9 / 4` said 2 - right for a bit
 * mask, wrong for anybody using it as a calculator. The answer is exact
 * fractions: every number is a numerator over a denominator in 64 bits,
 * so 1/3 * 3 is 1 and 0.1 + 0.2 is 0.3, which floating point gets wrong,
 * and no floating-point code has to exist for it (our printf has none,
 * and the Pi Zero W's 32-bit build would need a runtime library for the
 * 64-bit conversions). The answer is shown as decimals - twelve places,
 * rounded, when it does not end - and as the fraction. What does not fit
 * in 64 bits says so instead of wrapping round.
 *
 * Hex, binary and the bit operators keep it in whole numbers, as before:
 * `calc 0xff >> 4`.
 *
 * A recursive descent parser, from lowest precedence down to highest:
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
    if (!ps->error) {          /* keep only the first error */
        ps->error = true;
        ps->msg = msg;
    }
}

static void skip_space(parser_t *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t') ps->p++;
}

/* Consume op and return true if it is next. Two-character operators must
 * be tested first, or ** reads as * and << reads as <. */
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
            fail(ps, "missing closing parenthesis");
        return v;
    }

    /* 0x / 0b prefixes */
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
        fail(ps, "expected a number");
        return 0;
    }
    return v;
}

/* Power is right associative: 2**3**2 = 2**(3**2) */
static s64 parse_power(parser_t *ps)
{
    s64 base = parse_atom(ps);
    if (!eat(ps, "**"))
        return base;

    s64 exp = parse_power(ps);
    if (exp < 0) {
        fail(ps, "a negative exponent has no integer result");
        return 0;
    }

    s64 r = 1;
    while (exp-- > 0) {
        r *= base;
        if (exp > 62) { fail(ps, "too large"); return 0; }
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
        /* ** is power, so it must not be read as * */
        if (ps->p[0] == '*' && ps->p[1] == '*') return v;

        if (eat(ps, "*")) { v *= parse_unary(ps); continue; }
        if (eat(ps, "/")) {
            s64 d = parse_unary(ps);
            if (d == 0) { fail(ps, "division by zero"); return 0; }
            v /= d; continue;
        }
        if (eat(ps, "%")) {
            s64 d = parse_unary(ps);
            if (d == 0) { fail(ps, "division by zero"); return 0; }
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

/* & | ^ bind less tightly than << >>, as in C */
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

/* ── Fractions ───────────────────────────────────────────────────── */

typedef struct { s64 n, d; } q_t;      /* d > 0, n/d in lowest terms */

#define S64_MAX 0x7fffffffffffffffLL

static s64 gcd64(s64 a, s64 b)
{
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { s64 t = a % b; a = b; b = t; }
    return a;
}

/* Overflow-checked; s64's most negative value is refused as an operand,
 * since it has no positive twin. */
static bool mul_ok(s64 a, s64 b, s64 *r)
{
    if (a == 0 || b == 0) { *r = 0; return true; }
    if (a == -S64_MAX - 1 || b == -S64_MAX - 1) return false;
    s64 aa = a < 0 ? -a : a, bb = b < 0 ? -b : b;
    if (aa > S64_MAX / bb) return false;
    *r = a * b;
    return true;
}

static bool add_ok(s64 a, s64 b, s64 *r)
{
    if ((b > 0 && a > S64_MAX - b) || (b < 0 && a < -S64_MAX - 1 - b)) return false;
    *r = a + b;
    return true;
}

static q_t q_norm(parser_t *ps, s64 n, s64 d)
{
    q_t r = { 0, 1 };
    if (d == 0) { fail(ps, "division by zero"); return r; }
    if (d < 0) {
        if (n == -S64_MAX - 1 || d == -S64_MAX - 1) { fail(ps, "too large"); return r; }
        n = -n; d = -d;
    }
    s64 g = gcd64(n, d);
    if (g > 1) { n /= g; d /= g; }
    r.n = n; r.d = d;
    return r;
}

static q_t q_add(parser_t *ps, q_t x, q_t y)
{
    s64 g = gcd64(x.d, y.d), a, b, n, d;
    if (!mul_ok(x.n, y.d / g, &a) || !mul_ok(y.n, x.d / g, &b) ||
        !add_ok(a, b, &n) || !mul_ok(x.d, y.d / g, &d)) {
        fail(ps, "the answer does not fit in 64 bits");
        return x;
    }
    return q_norm(ps, n, d);
}

static q_t q_mul(parser_t *ps, q_t x, q_t y)
{
    s64 g1 = gcd64(x.n, y.d), g2 = gcd64(y.n, x.d), n, d;
    if (g1 == 0) g1 = 1;
    if (g2 == 0) g2 = 1;
    if (!mul_ok(x.n / g1, y.n / g2, &n) || !mul_ok(x.d / g2, y.d / g1, &d)) {
        fail(ps, "the answer does not fit in 64 bits");
        return x;
    }
    return q_norm(ps, n, d);
}

static q_t q_div(parser_t *ps, q_t x, q_t y)
{
    if (y.n == 0) { fail(ps, "division by zero"); return x; }
    q_t inv = q_norm(ps, y.d, y.n);
    return q_mul(ps, x, inv);
}

/* x % y: x - y * (x / y with the fraction cut off), as C does it. */
static q_t q_mod(parser_t *ps, q_t x, q_t y)
{
    if (y.n == 0) { fail(ps, "division by zero"); return x; }
    q_t t = q_div(ps, x, y);
    if (ps->error) return x;
    q_t whole = { t.n / t.d, 1 };
    q_t m = q_mul(ps, y, whole);
    m.n = -m.n;
    return q_add(ps, x, m);
}

static q_t q_parse_or(parser_t *ps);
static q_t q_unary(parser_t *ps);

static q_t q_atom(parser_t *ps)
{
    skip_space(ps);
    if (eat(ps, "(")) {
        q_t v = q_parse_or(ps);
        if (!eat(ps, ")"))
            fail(ps, "missing closing parenthesis");
        return v;
    }
    const char *start = ps->p;
    s64 n = 0, d = 1;
    bool point = false;
    for (;;) {
        char c = *ps->p;
        if (c == '_') { ps->p++; continue; }
        if (c == '.' && !point) { point = true; ps->p++; continue; }
        if (c < '0' || c > '9') break;
        if (!mul_ok(n, 10, &n) || !add_ok(n, c - '0', &n) ||
            (point && !mul_ok(d, 10, &d))) {
            fail(ps, "too many digits for 64 bits");
            return (q_t){ 0, 1 };
        }
        ps->p++;
    }
    if (ps->p == start || (point && ps->p == start + 1)) {
        fail(ps, "expected a number");
        return (q_t){ 0, 1 };
    }
    return q_norm(ps, n, d);
}

static q_t q_power(parser_t *ps)
{
    q_t base = q_atom(ps);
    if (!eat(ps, "**"))
        return base;
    q_t e = q_unary(ps);                /* 2**-2 is a quarter */
    if (ps->error) return base;
    if (e.d != 1) { fail(ps, "only whole-number powers"); return base; }
    s64 k = e.n;
    if (k < 0) {
        if (base.n == 0) { fail(ps, "division by zero"); return base; }
        base = q_norm(ps, base.d, base.n);
        k = -k;
    }
    if (k > 4096) { fail(ps, "too large"); return base; }
    q_t r = { 1, 1 };
    while (k-- > 0 && !ps->error)
        r = q_mul(ps, r, base);
    return r;
}

static q_t q_unary(parser_t *ps)
{
    skip_space(ps);
    if (eat(ps, "-")) { q_t v = q_unary(ps); v.n = -v.n; return v; }
    if (eat(ps, "+")) return q_unary(ps);
    return q_power(ps);
}

static q_t q_mulop(parser_t *ps)
{
    q_t v = q_unary(ps);
    for (;;) {
        skip_space(ps);
        if (ps->p[0] == '*' && ps->p[1] == '*') return v;
        if (eat(ps, "*")) { v = q_mul(ps, v, q_unary(ps)); continue; }
        if (eat(ps, "/")) { v = q_div(ps, v, q_unary(ps)); continue; }
        if (eat(ps, "%")) { v = q_mod(ps, v, q_unary(ps)); continue; }
        return v;
    }
}

static q_t q_parse_or(parser_t *ps)
{
    q_t v = q_mulop(ps);
    for (;;) {
        skip_space(ps);
        if (eat(ps, "+")) { v = q_add(ps, v, q_mulop(ps)); continue; }
        if (eat(ps, "-")) { q_t w = q_mulop(ps); w.n = -w.n; v = q_add(ps, v, w); continue; }
        return v;
    }
}

/* n/d as decimals: twelve places, rounded, trailing zeros dropped.
 * Long division in unsigned 64 bits, so nothing is ever approximated
 * before the last digit. */
static void print_decimal(q_t v)
{
    u64 n = v.n < 0 ? (u64)(-(v.n + 1)) + 1 : (u64)v.n, d = (u64)v.d;
    u64 ip = n / d, r = n % d;
    char frac[14];
    int k = 0;
    for (; k < 13 && r; k++) {
        if (r > 0x7fffffffffffffffULL / 5) break;   /* r * 10 would not fit */
        r *= 10;
        frac[k] = (char)('0' + r / d);
        r %= d;
    }
    bool exact = r == 0 && k <= 12;
    if (k == 13) {                          /* round to twelve places */
        bool up = frac[12] >= '5';
        k = 12;
        for (int i = k - 1; up && i >= 0; i--) {
            if (frac[i] == '9') frac[i] = '0';
            else { frac[i]++; up = false; }
        }
        if (up) ip++;
    }
    while (k > 0 && frac[k - 1] == '0') k--;
    frac[k] = '\0';
    printf("  %s%llu%s%s%s\n", v.n < 0 ? "-" : "", (unsigned long long)ip,
           k ? "." : "", frac, exact ? "" : "...");
}

/* Binary: drop leading zeros and group in fours */
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

/* Whole numbers only: hex or binary in it, or a bit operator. */
static bool integer_only(const char *e)
{
    for (const char *p = e; *p; p++) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X' || p[1] == 'b' || p[1] == 'B'))
            return true;
        if (*p == '&' || *p == '|' || *p == '^' || *p == '~' || *p == '<' || *p == '>')
            return true;
    }
    return false;
}

static int evaluate(const char *expr, bool verbose)
{
    if (!integer_only(expr)) {
        parser_t qs = { .p = expr, .error = false, .msg = NULL };
        q_t q = q_parse_or(&qs);
        skip_space(&qs);
        if (!qs.error && *qs.p)
            fail(&qs, "unexpected characters after the expression");
        if (qs.error) {
            dprintf(STDERR_FILENO, "calc: %s\n", qs.msg);
            if (*qs.p)
                dprintf(STDERR_FILENO, "      -> %s\n", qs.p);
            return 1;
        }
        if (q.d != 1) {
            print_decimal(q);
            if (verbose)
                printf("  = %lld/%lld\n", (long long)q.n, (long long)q.d);
            return 0;
        }
        if (verbose) show(q.n);
        else         printf("%ld\n", (long)q.n);
        return 0;
    }

    parser_t ps = { .p = expr, .error = false, .msg = NULL };
    s64 v = parse_or(&ps);

    skip_space(&ps);
    if (!ps.error && *ps.p)
        fail(&ps, "unexpected characters after the expression");

    if (ps.error) {
        dprintf(STDERR_FILENO, "calc: %s\n", ps.msg);
        if (*ps.p)
            dprintf(STDERR_FILENO, "      -> %s\n", ps.p);
        return 1;
    }

    if (verbose) show(v);
    else         printf("%ld\n", (long)v);
    /* `calc 2^10` is 8, which is right in C and a surprise to everybody
     * else. Say so once rather than change what ^ means. */
    if (verbose && strchr(expr, '^'))
        printf("  (^ is xor, as in C; a power is **, 2**10 = 1024)\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        /* Join all arguments: the shell splits them on spaces. */
        char expr[512];
        expr[0] = '\0';
        for (int i = 1; i < argc; i++) {
            strlcpy(expr + strlen(expr), argv[i], sizeof(expr) - strlen(expr));
            if (i + 1 < argc)
                strlcpy(expr + strlen(expr), " ", sizeof(expr) - strlen(expr));
        }
        return evaluate(expr, true);
    }

    printf("calc - calculator.  Blank line or Ctrl-D to leave.\n");
    printf("  + - * / %% **   1.25   & | ^ ~ << >>   ( )   0x.. 0b..\n");
    printf("  ^ is xor, as in C. Powers are **, so 2**10 is 1024.\n\n");

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
