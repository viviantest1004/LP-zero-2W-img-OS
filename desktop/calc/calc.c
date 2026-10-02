/*
 * calc.c - lp-calc, LP's own calculator: the four operations, % and ±,
 * in a window - and the same arithmetic at a terminal prompt.
 *
 *   lp-calc                 the window
 *   calc 10/3               one answer, at a terminal
 *   calc                    a prompt: one expression a line, a blank line
 *                           or Ctrl-D to leave; "ans" is the last answer
 *
 * + - * / (also × ÷ x), % (50% is 0.5; 10%3 is the remainder), ^ or **
 * for powers, parentheses.
 *
 * Why there is one of our own: the dock's calculator was GNOME's, a
 * Debian package, and Software removes packages - removed, the dock had a
 * slot that opened nothing and the machine had no calculator at all. This
 * one is part of LP (/usr/local/bin, no package), so Software lists it as
 * a system app it does not remove. GNOME's, when it is installed, is
 * there as the scientific one.
 *
 * The terminal's `calc` was the boards' integer calculator (userland/calc:
 * hex and binary, no fractions - 10/3 was 3). On the desktop
 * /usr/local/bin comes before /bin, and `calc` there is this program
 * (tools/mkdesktop.sh links it): decimals, the way a person expects.
 *
 * Arithmetic is in long double and shown to 12 significant digits, so
 * 0.1 + 0.2 is 0.3 and 1/3*3 is 1.
 */
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define T(en, ko) (calc_korean ? (ko) : (en))
static gboolean calc_korean;

/* ── the arithmetic ─────────────────────────────────────────────────── */

typedef struct {
    const char *p;
    const char *err;        /* the first thing that went wrong, or NULL */
    long double ans;
} Parser;

static void skip_space(Parser *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t')
        ps->p++;
}

/* The signs a person types or the keypad writes, all to one byte:
 * × x * for times, ÷ / : for divided by, − - for minus. */
static int peek_op(Parser *ps, int *len)
{
    skip_space(ps);
    const unsigned char *u = (const unsigned char *)ps->p;
    *len = 1;
    if (u[0] == 0xC3 && u[1] == 0x97) { *len = 2; return '*'; }          /* × */
    if (u[0] == 0xC3 && u[1] == 0xB7) { *len = 2; return '/'; }          /* ÷ */
    if (u[0] == 0xE2 && u[1] == 0x88 && u[2] == 0x92) { *len = 3; return '-'; } /* − */
    switch (u[0]) {
    case '+': case '-': case '*': case '/': case '%': case '(': case ')':
        return u[0];
    case 'x': case 'X':
        return '*';
    case ':':
        return '/';
    }
    return 0;
}

static long double expr(Parser *ps);

static long double atom(Parser *ps)
{
    int len;
    int op = peek_op(ps, &len);
    if (op == '-') {
        ps->p += len;
        return -atom(ps);
    }
    if (op == '+') {
        ps->p += len;
        return atom(ps);
    }
    if (op == '(') {
        ps->p += len;
        long double v = expr(ps);
        if (peek_op(ps, &len) == ')')
            ps->p += len;
        else if (!ps->err)
            ps->err = T("a ( is not closed", "괄호가 닫히지 않았습니다");
        return v;
    }
    skip_space(ps);
    if (!strncmp(ps->p, "ans", 3)) {
        ps->p += 3;
        return ps->ans;
    }
    /* A number: digits with one point (a comma is a point too). */
    char buf[64];
    int n = 0, points = 0;
    while (n < (int)sizeof buf - 1 &&
           ((*ps->p >= '0' && *ps->p <= '9') || *ps->p == '.' || *ps->p == ',')) {
        char c = *ps->p == ',' ? '.' : *ps->p;
        if (c == '.' && ++points > 1)
            break;
        buf[n++] = c;
        ps->p++;
    }
    buf[n] = 0;
    if (n == 0 || (n == 1 && buf[0] == '.')) {
        if (!ps->err)
            ps->err = *ps->p ? T("that is not a number", "숫자가 아닌 것이 있습니다")
                             : T("the expression is not finished", "식이 끝나지 않았습니다");
        return 0;
    }
    long double v = strtold(buf, NULL);
    /* 50% is 0.5, as on any calculator's keypad. */
    if (peek_op(ps, &len) == '%') {
        const char *after = ps->p + len;
        while (*after == ' ')
            after++;
        /* "%" followed by a number is the remainder, not a percent. */
        if (!(*after >= '0' && *after <= '9') && *after != '(' && *after != '.') {
            ps->p += len;
            v /= 100;
        }
    }
    return v;
}

/* Powers, right to left: 2^3^2 is 2^9. "**" is the same. */
static long double power(Parser *ps)
{
    long double v = atom(ps);
    skip_space(ps);
    int len = 0;
    if (*ps->p == '^')
        len = 1;
    else if (ps->p[0] == '*' && ps->p[1] == '*')
        len = 2;
    if (!len)
        return v;
    ps->p += len;
    return powl(v, power(ps));
}

static long double term(Parser *ps)
{
    long double v = power(ps);
    for (;;) {
        int len, op = peek_op(ps, &len);
        if (op != '*' && op != '/' && op != '%')
            return v;
        ps->p += len;
        long double r = power(ps);
        if (op == '*') {
            v *= r;
        } else if (r == 0) {
            if (!ps->err)
                ps->err = T("cannot divide by zero", "0 으로 나눌 수 없습니다");
            return 0;
        } else if (op == '/') {
            v /= r;
        } else {
            v = fmodl(v, r);
        }
    }
}

static long double expr(Parser *ps)
{
    long double v = term(ps);
    for (;;) {
        int len, op = peek_op(ps, &len);
        if (op != '+' && op != '-')
            return v;
        ps->p += len;
        long double r = term(ps);
        v = op == '+' ? v + r : v - r;
    }
}

/* Evaluates `s`; on an error returns FALSE with *why set. */
static gboolean evaluate(const char *s, long double ans, long double *out, const char **why)
{
    Parser ps = { s, NULL, ans };
    long double v = expr(&ps);
    skip_space(&ps);
    if (!ps.err && *ps.p)
        ps.err = T("something after the end of the expression", "식 뒤에 알 수 없는 것이 있습니다");
    if (!ps.err && !isfinite((double)v))
        ps.err = T("the number is too large", "수가 너무 큽니다");
    if (ps.err) {
        *why = ps.err;
        return FALSE;
    }
    *out = v;
    return TRUE;
}

/* 12 significant digits, no trailing zeros, no "-0"; large and tiny
 * numbers in exponent form. */
static void format(long double v, char *out, size_t n)
{
    if (fabsl(v) < 1e-12L)
        v = 0;
    snprintf(out, n, "%.12Lg", v);
}

/* ── at a terminal ──────────────────────────────────────────────────── */

static int run_cli(int argc, char **argv)
{
    long double ans = 0, v;
    const char *why;
    char text[96];
    if (argc > 1) {
        GString *e = g_string_new(NULL);
        for (int i = 1; i < argc; i++)
            g_string_append_printf(e, "%s%s", i > 1 ? " " : "", argv[i]);
        gboolean ok = evaluate(e->str, ans, &v, &why);
        g_string_free(e, TRUE);
        if (!ok) {
            fprintf(stderr, "calc: %s\n", why);
            return 1;
        }
        format(v, text, sizeof text);
        printf("%s\n", text);
        return 0;
    }
    gboolean tty = isatty(0);
    if (tty)
        printf("%s\n", T("Calculator: type an expression and press Enter (+ - * / %, "
                         "parentheses; \"ans\" is the last answer). An empty line leaves.",
                         "계산기: 식을 입력하고 Enter 를 누르세요 (+ - * / %, 괄호, "
                         "ans 는 직전 답). 빈 줄을 입력하면 끝납니다."));
    char line[1024];
    for (;;) {
        if (tty) {
            fputs("> ", stdout);
            fflush(stdout);
        }
        if (!fgets(line, sizeof line, stdin))
            break;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0])
            break;
        if (evaluate(line, ans, &v, &why)) {
            ans = v;
            format(v, text, sizeof text);
            printf("= %s\n", text);
        } else {
            printf("%s\n", why);
        }
    }
    return 0;
}

/* ── the window ─────────────────────────────────────────────────────── */

static GtkWidget *win, *line_expr, *line_result;
static GString *cur;            /* what has been typed */
static long double last_ans;
static gboolean just_answered;  /* the display shows a result of "=" */
static char asked[160];         /* what that result answered */

static void show(void)
{
    /* After "=": the question small above, the answer large below. It
     * was the other way round - the answer took the question's place in
     * the small grey line and the large one went blank. */
    if (just_answered) {
        char q[176];
        g_snprintf(q, sizeof q, "%s =", asked);
        gtk_label_set_text(GTK_LABEL(line_expr), q);
        gtk_label_set_text(GTK_LABEL(line_result), cur->str);
        gtk_widget_remove_css_class(line_result, "error");
        return;
    }
    gtk_label_set_text(GTK_LABEL(line_expr), cur->len ? cur->str : "0");
    long double v;
    const char *why;
    char text[96] = "";
    if (cur->len && evaluate(cur->str, last_ans, &v, &why)) {
        format(v, text, sizeof text);
        if (!strcmp(text, cur->str))
            text[0] = 0;            /* "7" needs no "= 7" under it */
    }
    gtk_label_set_text(GTK_LABEL(line_result), text[0] ? text : " ");
    gtk_widget_remove_css_class(line_result, "error");
}

static gboolean ends_with_op(void)
{
    if (!cur->len)
        return TRUE;
    const char *e = cur->str + cur->len;
    return g_str_has_suffix(cur->str, "+") || g_str_has_suffix(cur->str, "−") ||
           g_str_has_suffix(cur->str, "×") || g_str_has_suffix(cur->str, "÷") ||
           g_str_has_suffix(cur->str, "(") || e[-1] == ' ';
}

static void drop_last_char(void)
{
    if (!cur->len)
        return;
    const char *prev = g_utf8_find_prev_char(cur->str, cur->str + cur->len);
    g_string_truncate(cur, prev ? (gsize)(prev - cur->str) : 0);
}

static void press(const char *k)
{
    if (!strcmp(k, "C")) {
        g_string_truncate(cur, 0);
        just_answered = FALSE;
    } else if (!strcmp(k, "back")) {
        drop_last_char();
        just_answered = FALSE;
    } else if (!strcmp(k, "=")) {
        long double v;
        const char *why;
        if (!cur->len)
            return;
        if (evaluate(cur->str, last_ans, &v, &why)) {
            char text[96];
            format(v, text, sizeof text);
            last_ans = v;
            g_strlcpy(asked, cur->str, sizeof asked);
            g_string_assign(cur, text);
            just_answered = TRUE;
            show();
        } else {
            gtk_label_set_text(GTK_LABEL(line_result), why);
            gtk_widget_add_css_class(line_result, "error");
        }
        return;
    } else if (!strcmp(k, "±")) {
        /* The sign of the number being typed - the last one in the line:
         * a "-" just before it is taken away, or put there. The keypad's
         * minus is "−"; this "-" is the number's own sign. */
        gssize i = (gssize)cur->len;
        while (i > 0 && (g_ascii_isdigit(cur->str[i - 1]) || cur->str[i - 1] == '.'))
            i--;
        if (i > 0 && cur->str[i - 1] == '-')
            g_string_erase(cur, i - 1, 1);
        else
            g_string_insert(cur, i, "-");
        just_answered = FALSE;
    } else if (!strcmp(k, "+") || !strcmp(k, "−") || !strcmp(k, "×") || !strcmp(k, "÷")) {
        just_answered = FALSE;
        if (!cur->len && strcmp(k, "−") != 0)
            g_string_assign(cur, "0");
        /* A second operator replaces the first, as on any keypad. */
        if (cur->len && ends_with_op() && !g_str_has_suffix(cur->str, "("))
            drop_last_char();
        g_string_append(cur, k);
    } else {
        /* A digit, ".", "%" or a parenthesis. After "=", a digit starts
         * a new calculation; an operator went on from the answer above. */
        if (just_answered && (g_ascii_isdigit(k[0]) || k[0] == '.' || k[0] == '('))
            g_string_truncate(cur, 0);
        just_answered = FALSE;
        if (k[0] == '.') {
            gssize i = (gssize)cur->len;
            while (i > 0 && g_ascii_isdigit(cur->str[i - 1]))
                i--;
            if (i > 0 && cur->str[i - 1] == '.')
                return;             /* one point per number */
            if (ends_with_op())
                g_string_append(cur, "0");
        }
        g_string_append(cur, k);
    }
    show();
}

static void on_key_button(GtkButton *b, gpointer d)
{
    (void)b;
    press(d);
}

static gboolean on_key(GtkEventControllerKey *c, guint kv, guint code,
                       GdkModifierType mods, gpointer d)
{
    (void)c; (void)code; (void)d;
    if (mods & GDK_CONTROL_MASK) {
        if (kv == GDK_KEY_c || kv == GDK_KEY_C) {
            const char *r = gtk_label_get_text(GTK_LABEL(line_result));
            const char *text = (r && r[0] != ' ' && !gtk_widget_has_css_class(line_result, "error"))
                               ? r : cur->str;
            gdk_clipboard_set_text(gtk_widget_get_clipboard(win), text);
            return TRUE;
        }
        return FALSE;
    }
    guint32 uc = gdk_keyval_to_unicode(kv);
    switch (kv) {
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_equal:
        press("="); return TRUE;
    case GDK_KEY_BackSpace:
        press("back"); return TRUE;
    case GDK_KEY_Escape: case GDK_KEY_Delete:
        press("C"); return TRUE;
    case GDK_KEY_KP_Add: press("+"); return TRUE;
    case GDK_KEY_KP_Subtract: press("−"); return TRUE;
    case GDK_KEY_KP_Multiply: press("×"); return TRUE;
    case GDK_KEY_KP_Divide: press("÷"); return TRUE;
    case GDK_KEY_KP_Decimal: press("."); return TRUE;
    }
    static char one[2];
    if (uc >= '0' && uc <= '9') {
        one[0] = (char)uc; one[1] = 0;
        press(g_intern_string(one));
        return TRUE;
    }
    switch (uc) {
    case '+': press("+"); return TRUE;
    case '-': press("−"); return TRUE;
    case '*': case 'x': press("×"); return TRUE;
    case '/': press("÷"); return TRUE;
    case '.': case ',': press("."); return TRUE;
    case '%': press("%"); return TRUE;
    case '(': press("("); return TRUE;
    case ')': press(")"); return TRUE;
    }
    return FALSE;
}

static const char CSS[] =
    "window.lp-calc { background-color: #1c1c1c; }\n"
    ".calc-expr { font-size: 22px; color: #9a9a9a; }\n"
    ".calc-result { font-size: 46px; font-weight: 300; color: #f2f2f2;"
    "  font-feature-settings: 'tnum'; }\n"
    ".calc-result.error { font-size: 18px; color: #f0b350; }\n"
    "button.calc-key { min-height: 64px; min-width: 64px; border-radius: 32px;"
    "  font-size: 24px; background: #333333; color: #f2f2f2; border: none;"
    "  box-shadow: none; }\n"
    "button.calc-key:hover { background: #3f3f3f; }\n"
    "button.calc-key:active { background: #5a5a5a; }\n"
    "button.calc-key.fn { background: #a5a5a5; color: #111111; }\n"
    "button.calc-key.fn:hover { background: #b8b8b8; }\n"
    "button.calc-key.op { background: #f28c28; color: #ffffff; }\n"
    "button.calc-key.op:hover { background: #f5a04c; }\n"
    "button.calc-key.op:active { background: #d4741a; }\n";

static GtkWidget *key(GtkWidget *grid, const char *label, const char *press_as,
                      const char *css, int col, int row, int w)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(b, "calc-key");
    if (css)
        gtk_widget_add_css_class(b, css);
    gtk_widget_set_focusable(b, FALSE);     /* keys go to the window */
    gtk_widget_set_hexpand(b, TRUE);
    gtk_widget_set_vexpand(b, TRUE);
    g_signal_connect(b, "clicked", G_CALLBACK(on_key_button), (gpointer)press_as);
    gtk_grid_attach(GTK_GRID(grid), b, col, row, w, 1);
    return b;
}

static void on_activate(GtkApplication *app, gpointer d)
{
    (void)d;
    if (win) {
        gtk_window_present(GTK_WINDOW(win));
        return;
    }
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, CSS, -1);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    win = gtk_application_window_new(app);
    gtk_widget_add_css_class(win, "lp-calc");
    gtk_window_set_title(GTK_WINDOW(win), T("Calculator", "계산기"));
    gtk_window_set_default_size(GTK_WINDOW(win), 340, 540);
    gtk_window_set_icon_name(GTK_WINDOW(win), "accessories-calculator");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 16);

    /* Neither line is selectable: a selectable label takes the focus when
     * the window opens - "0" came up selected, and a caret stood by the
     * answer. Ctrl+C copies the answer (on_key). */
    line_expr = gtk_label_new("0");
    gtk_widget_add_css_class(line_expr, "calc-expr");
    gtk_label_set_xalign(GTK_LABEL(line_expr), 1.0);
    gtk_label_set_wrap(GTK_LABEL(line_expr), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(line_expr), PANGO_WRAP_CHAR);
    gtk_widget_set_vexpand(line_expr, TRUE);
    gtk_widget_set_valign(line_expr, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(box), line_expr);

    line_result = gtk_label_new(" ");
    gtk_widget_add_css_class(line_result, "calc-result");
    gtk_label_set_xalign(GTK_LABEL(line_result), 1.0);
    gtk_label_set_ellipsize(GTK_LABEL(line_result), PANGO_ELLIPSIZE_START);
    gtk_widget_set_margin_bottom(line_result, 12);
    gtk_box_append(GTK_BOX(box), line_result);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_row_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    key(grid, "C", "C", "fn", 0, 0, 1);
    key(grid, "±", "±", "fn", 1, 0, 1);
    key(grid, "%", "%", "fn", 2, 0, 1);
    key(grid, "÷", "÷", "op", 3, 0, 1);
    key(grid, "7", "7", NULL, 0, 1, 1);
    key(grid, "8", "8", NULL, 1, 1, 1);
    key(grid, "9", "9", NULL, 2, 1, 1);
    key(grid, "×", "×", "op", 3, 1, 1);
    key(grid, "4", "4", NULL, 0, 2, 1);
    key(grid, "5", "5", NULL, 1, 2, 1);
    key(grid, "6", "6", NULL, 2, 2, 1);
    key(grid, "−", "−", "op", 3, 2, 1);
    key(grid, "1", "1", NULL, 0, 3, 1);
    key(grid, "2", "2", NULL, 1, 3, 1);
    key(grid, "3", "3", NULL, 2, 3, 1);
    key(grid, "+", "+", "op", 3, 3, 1);
    key(grid, "0", "0", NULL, 0, 4, 1);
    key(grid, ".", ".", NULL, 1, 4, 1);
    key(grid, "⌫", "back", NULL, 2, 4, 1);
    key(grid, "=", "=", "op", 3, 4, 1);
    gtk_widget_set_vexpand(grid, TRUE);
    gtk_box_append(GTK_BOX(box), grid);
    gtk_window_set_child(GTK_WINDOW(win), box);

    GtkEventController *kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key), NULL);
    gtk_widget_add_controller(win, kc);

    show();
    gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char **argv)
{
    const char *lang = g_getenv("LANG");
    calc_korean = lang && g_str_has_prefix(lang, "ko");
    const char *me = strrchr(argv[0], '/');
    me = me ? me + 1 : argv[0];
    /* "calc" is the terminal's; lp-calc with an expression answers it. */
    if (!strcmp(me, "calc") ||
        (argc > 1 && (argv[1][0] != '-' || g_ascii_isdigit(argv[1][1]) ||
                      argv[1][1] == '.' || argv[1][1] == '(')))
        return run_cli(argc, argv);
    cur = g_string_new(NULL);
    GtkApplication *app = gtk_application_new("org.lpzero.Calculator",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
