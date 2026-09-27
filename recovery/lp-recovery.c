/* lp-recovery - LP's recovery menu, on the recovery partition.
 *
 * The boot menu's "LP Recovery" starts the same kernel with
 * root=PARTLABEL=LP-RECOVERY lp.mode=recovery. That root is a small
 * disk-rooted system (tools/mkrecovery.sh builds it: our static userland
 * plus the Debian tools a repair needs), and our init, seeing
 * lp.mode=recovery, runs this program on tty1 as root instead of a normal
 * boot. Nothing else runs: no shell on the serial port, no services.
 *
 * It is the macOS Recovery idea for this machine - one fullscreen menu
 * with big targets for the 4K touch panel, and keys:
 *
 *   Recovery shell (administrator)  a root shell with the installed
 *        system at /mnt/lp, after an administrator's password (auth.c).
 *   Reinstall LP  keep /home and the accounts, or erase everything, from
 *        the payload in /reinstall, with a typed confirmation
 *        (reinstall.c).
 *   Check and repair disks  fsck of the EFI partition and LP-ROOT, and a
 *        read-only check of LP-RECOVERY itself, with the output on
 *        screen.
 *   Exit recovery and restart  the boot menu then starts LP.
 *
 * and a status line: disk state, battery, clock.
 *
 * English by default, Korean when the boot menu was in Korean (it passes
 * lp.lang=ko), or when the installed system's locale is Korean. The text
 * shell itself is always English: the kernel console's font has no
 * Hangul.
 *
 * Motion follows COMMON.md: the focus highlight moves on the menu spring
 * (interruptible - retargeting keeps its velocity), screens crossfade in
 * the menu spring's time, a touch shows its press on touch-down with no
 * delay, and with "reduce motion" set on the installed system the
 * highlight jumps and the crossfade is 100 ms.
 *
 * The screens are drawn with recovery/ui/lp-ui.h, the same code and font
 * as the boot menu, so the two look like one product.
 */
#include "recovery.h"
#include "lp-efivar.h"

#define TIOCSCTTY 0x540E
#define SIGPIPE_  13

static lpui_canvas_t cv_old;            /* the last frame, for crossfades */
static bool reduced;

/* ── Layout ───────────────────────────────────────────────────────── */
typedef struct { int x, y, w, h; } rect_t;
static int ox, oy;
static int X(int v) { return ox + lpui_px(v); }
static int Y(int v) { return oy + lpui_px(v); }
static int P(int v) { int p = lpui_px(v); return p < 1 ? 1 : p; }
static int TXT(int v) { return lpui_text_px(v); }

enum { SC_MENU, SC_AUTH, SC_RE_CHOOSE, SC_RE_CONFIRM, SC_RE_PROGRESS, SC_RE_DONE,
       SC_CHECK, SC_RESTART };
static int screen = -1;

enum { T_ROW, T_BUTTON, T_PRIMARY, T_DANGER, T_CHIP, T_FIELD, T_KEY };
typedef struct { rect_t r; int id; int style; bool disabled; } target_t;
#define MAXT 80
static target_t tg[MAXT];
static int ntg, focus, pressed = -1;

/* target ids */
enum {
    ID_SHELL, ID_REINSTALL, ID_CHECK, ID_EXIT,
    ID_KEEP = 10, ID_ERASE,
    ID_CHIP = 100,
    ID_FIELD = 200, ID_BACK, ID_KBD, ID_OPEN, ID_GO, ID_RESTART, ID_DONE,
    ID_KEY = 300, ID_K_SHIFT = 390, ID_K_BKSP, ID_K_CLOSE, ID_K_SPACE, ID_K_ENTER,
};

/* ── State of the screens ─────────────────────────────────────────── */
static auth_info_t ai;
static int who;
static char pw[128];
static int pwlen;
static char confirm[16];
static int conflen;
static char msg[240];
static u32 msg_color;
static bool osk, osk_shift;
static int re_mode;
static re_progress_t re_prog;
static char re_result[240];
static bool re_ok;
static char version[96];
static bool interrupted;
static bool check_done;
#define OUTN 64
static char out_line[OUTN][120];
static u32 out_col[OUTN];
static int nout;
static int disk_status;                 /* LPS_ id */
static s64 last_status;

/* ── Drawing primitives in the house style ───────────────────────── */
static void card(lpui_canvas_t *c, rect_t r, int rad)
{
    lpui_rrect(c, r.x, r.y, r.w, r.h, rad, 0xffffff, LPUI_CARD_A);
    lpui_rrect_stroke(c, r.x, r.y, r.w, r.h, rad, P(3), 0xffffff, LPUI_LINE_A);
}

static void button(lpui_canvas_t *c, const target_t *t, const char *label)
{
    rect_t r = t->r;
    int rad = r.h / 2;
    u32 a = t->disabled ? 110 : 256;
    if (t->style == T_PRIMARY || t->style == T_DANGER) {
        lpui_rrect(c, r.x, r.y, r.w, r.h, rad, t->style == T_DANGER ? LPUI_DANGER : LPUI_ACCENT,
                   t->disabled ? 90 : 256);
    } else {
        lpui_rrect(c, r.x, r.y, r.w, r.h, rad, 0xffffff, 36);
        lpui_rrect_stroke(c, r.x, r.y, r.w, r.h, rad, P(3), 0xffffff, LPUI_LINE_A);
    }
    int tp = TXT(t->style == T_KEY ? 56 : 60);
    int w = lpui_text_width(LPG_M, tp, label);
    lpui_text(c, LPG_M, tp, r.x + (r.w - w) / 2, r.y + r.h / 2 + lpui_ascent(LPG_M, tp) * 36 / 100,
              LPUI_INK, a, label);
}

static void chrome(lpui_canvas_t *c, int title, const char *sub)
{
    int tp = TXT(104), sp = TXT(40);
    lpui_text_center(c, LPG_T, tp, SW / 2, Y(500), LPUI_INK, 256, lpui_s(title));
    if (sub)
        lpui_text_wrap(c, LPG_S, sp, X(620), Y(590), P(2600), LPUI_INK2, 220, true, sub);
}

static void status_line(lpui_canvas_t *c)
{
    if (osk)
        return;
    int sp = TXT(40);
    int y = Y(2085);
    lpui_text(c, LPG_S, sp, X(160), y, disk_status == LPS_R_DISK_OK ? LPUI_INK2 : LPUI_ACCENT,
              200, lpui_s(disk_status));
    bool chg = false;
    int bat = battery_percent(&chg);
    if (bat >= 0) {
        char b[64];
        lpui_fmt(b, sizeof b, lpui_s(chg ? LPS_R_CHARGING : LPS_R_BATTERY), bat);
        lpui_text_center(c, LPG_S, sp, SW / 2, y, LPUI_INK2, 200, b);
    }
    char t[48];
    clock_text(t, sizeof t);
    lpui_text(c, LPG_S, sp, X(3680) - lpui_text_width(LPG_S, sp, t), y, LPUI_INK2, 200, t);
}

static rect_t status_band(void) { return (rect_t){ 0, Y(2010), SW, SH - Y(2010) }; }

static void field(lpui_canvas_t *c, const target_t *t, const char *text, bool secret)
{
    rect_t r = t->r;
    lpui_rrect(c, r.x, r.y, r.w, r.h, P(28), 0x000000, 70);
    lpui_rrect_stroke(c, r.x, r.y, r.w, r.h, P(28), P(3), 0xffffff, 70);
    int tp = TXT(60);
    int base = r.y + r.h / 2 + lpui_ascent(LPG_M, tp) * 36 / 100;
    int x = r.x + P(40);
    char shown[140];
    int n = 0;
    if (secret) {
        /* bullets, as many as fit */
        int bw = lpui_text_width(LPG_M, tp, "•");
        int max = bw ? (r.w - P(100)) / bw : 20;
        int k = (int)strlen(text);
        if (k > max) k = max;
        for (int i = 0; i < k && n + 3 < (int)sizeof shown; i++) {
            memcpy(shown + n, "•", 3);
            n += 3;
        }
        shown[n] = 0;
    } else
        strlcpy(shown, text, sizeof shown);
    int w = lpui_text(c, LPG_M, tp, x, base, LPUI_INK, 256, shown);
    /* the caret */
    lpui_rrect(c, x + w + P(8), r.y + r.h / 5, P(6), r.h * 3 / 5, 0, LPUI_ACCENT, 256);
}

/* ── The on-screen keyboard ───────────────────────────────────────────
 * The owner uses the touch panel without a keyboard attached, and a
 * password cannot be typed with a finger otherwise. It opens from the
 * "Keyboard" button and has a close key of its own (×) - the owner's
 * one explicit request about on-screen keyboards is that closing one is
 * never forgotten. */
static const char *const OSK_ROWS[2][4] = {
    { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm." },
    { "!@#$%^&*()", "QWERTYUIOP", "ASDFGHJKL_", "ZXCVBNM," },
};
#define OSK_TOP 1545

static void osk_layout(void)
{
    if (!osk)
        return;
    int gap = 16, kw = (2600 - 9 * gap) / 10, kh = 100, x0 = 620;
    for (int row = 0; row < 5; row++) {
        int y = OSK_TOP + 20 + row * (kh + 14);
        if (row < 3) {
            for (int i = 0; i < 10; i++)
                tg[ntg++] = (target_t){ { X(x0 + i * (kw + gap)), Y(y), P(kw), P(kh) },
                                        ID_KEY + row * 16 + i, T_KEY, false };
        } else if (row == 3) {
            tg[ntg++] = (target_t){ { X(x0), Y(y), P(kw), P(kh) }, ID_K_SHIFT, T_KEY, false };
            for (int i = 0; i < 8; i++)
                tg[ntg++] = (target_t){ { X(x0 + (i + 1) * (kw + gap)), Y(y), P(kw), P(kh) },
                                        ID_KEY + 3 * 16 + i, T_KEY, false };
            tg[ntg++] = (target_t){ { X(x0 + 9 * (kw + gap)), Y(y), P(kw), P(kh) }, ID_K_BKSP, T_KEY, false };
        } else {
            tg[ntg++] = (target_t){ { X(x0), Y(y), P(kw), P(kh) }, ID_K_CLOSE, T_KEY, false };
            tg[ntg++] = (target_t){ { X(x0 + (kw + gap)), Y(y), P(7 * kw + 6 * gap), P(kh) },
                                    ID_K_SPACE, T_KEY, false };
            tg[ntg++] = (target_t){ { X(x0 + 8 * (kw + gap)), Y(y), P(2 * kw + gap), P(kh) },
                                    ID_K_ENTER, T_KEY, false };
        }
    }
}

static void osk_draw(lpui_canvas_t *c)
{
    if (!osk)
        return;
    lpui_rrect(c, X(580), Y(OSK_TOP), P(2680), P(600), P(40), 0x000000, 90);
    for (int i = 0; i < ntg; i++) {
        const target_t *t = &tg[i];
        if (t->style != T_KEY)
            continue;
        char lab[8];
        int id = t->id;
        if (id >= ID_KEY && id < ID_KEY + 64) {
            int row = (id - ID_KEY) / 16, col = (id - ID_KEY) % 16;
            lab[0] = OSK_ROWS[osk_shift][row][col];
            lab[1] = 0;
        } else
            strlcpy(lab, id == ID_K_SHIFT ? "⇧" : id == ID_K_BKSP ? "⌫" : id == ID_K_CLOSE ? "×" :
                         id == ID_K_ENTER ? "⏎" : "", sizeof lab);
        target_t k = *t;
        if (id == ID_K_SHIFT && osk_shift)
            k.style = T_PRIMARY;
        if (id == ID_K_ENTER)
            k.style = T_PRIMARY;
        button(c, &k, lab);
    }
}

/* ── Screens: layout ──────────────────────────────────────────────── */
static void add(int x, int y, int w, int h, int id, int style)
{
    if (ntg < MAXT)
        tg[ntg++] = (target_t){ { X(x), Y(y), P(w), P(h) }, id, style, false };
}

/* Buttons centred in a row. */
static void add_buttons(int y, const int *ids, const int *styles, int n)
{
    int w = 560, gap = 60, total = n * w + (n - 1) * gap, x = 1920 - total / 2;
    for (int i = 0; i < n; i++)
        add(x + i * (w + gap), y, w, 160, ids[i], styles[i]);
}

static int rows_top(void) { return interrupted ? 790 : 720; }

static void layout(void)
{
    ntg = 0;
    switch (screen) {
    case SC_MENU:
        for (int i = 0; i < 4; i++)
            add(770, rows_top() + i * 264, 2300, 230, ID_SHELL + i, T_ROW);
        break;
    case SC_AUTH: {
        int chips = ai.mode == AUTH_ADMINS ? ai.nusers : ai.mode == AUTH_RECOVERY ? 1 : 0;
        int cw[AUTH_MAX_USERS], total = 0;
        for (int i = 0; i < chips; i++) {
            const char *name = ai.mode == AUTH_RECOVERY ? lpui_s(LPS_R_AUTH_RECNAME) : ai.users[i];
            cw[i] = lpui_text_width(LPG_M, 60, name) + 160;
            if (cw[i] < 420) cw[i] = 420;
            total += cw[i] + (i ? 40 : 0);
        }
        int x = 1920 - total / 2;
        for (int i = 0; i < chips; i++) {
            add(x, 760, cw[i], 150, ID_CHIP + i, T_CHIP);
            x += cw[i] + 40;
        }
        if (chips)
            add(1120, 1030, 1600, 170, ID_FIELD, T_FIELD);
        if (chips) {
            int ids[] = { ID_BACK, ID_KBD, ID_OPEN };
            int st[] = { T_BUTTON, T_BUTTON, T_PRIMARY };
            add_buttons(1360, ids, st, 3);
        } else {
            int ids[] = { ID_BACK };
            int st[] = { T_BUTTON };
            add_buttons(1360, ids, st, 1);
        }
        osk_layout();
        break;
    }
    case SC_RE_CHOOSE:
        if (version[0]) {
            add(770, 720, 2300, 260, ID_KEEP, T_ROW);
            add(770, 1014, 2300, 260, ID_ERASE, T_ROW);
        }
        {
            int ids[] = { ID_BACK };
            int st[] = { T_BUTTON };
            add_buttons(1400, ids, st, 1);
        }
        break;
    case SC_RE_CONFIRM: {
        add(1320, 1170, 1200, 170, ID_FIELD, T_FIELD);
        int ids[] = { ID_BACK, ID_KBD, ID_GO };
        int st[] = { T_BUTTON, T_BUTTON, re_mode == RE_ERASE ? T_DANGER : T_PRIMARY };
        add_buttons(1380, ids, st, 3);
        tg[ntg - 1].disabled = strcmp(confirm, "REINSTALL") != 0;
        osk_layout();
        break;
    }
    case SC_RE_DONE: {
        int ids[] = { re_ok ? ID_RESTART : ID_BACK };
        int st[] = { re_ok ? T_PRIMARY : T_BUTTON };
        add_buttons(1250, ids, st, 1);
        break;
    }
    case SC_CHECK:
        if (check_done) {
            int ids[] = { ID_DONE };
            int st[] = { T_PRIMARY };
            add_buttons(1840, ids, st, 1);
        }
        break;
    }
    if (focus >= ntg)
        focus = ntg ? ntg - 1 : 0;
}

static const target_t *find(int id)
{
    for (int i = 0; i < ntg; i++)
        if (tg[i].id == id)
            return &tg[i];
    return 0;
}

/* ── Screens: drawing ─────────────────────────────────────────────── */
static void draw_menu(lpui_canvas_t *c)
{
    chrome(c, LPS_R_TITLE, lpui_s(LPS_R_CHOOSE));
    if (interrupted)
        lpui_text_wrap(c, LPG_S, TXT(40), X(620), Y(700), P(2600), LPUI_ACCENT, 256, true,
                       lpui_s(LPS_R_INTERRUPTED));
    static const int T[4][2] = {
        { LPS_R_SHELL, LPS_R_SHELL_SUB }, { LPS_R_REINSTALL, LPS_R_REINSTALL_SUB },
        { LPS_R_CHECK, LPS_R_CHECK_SUB }, { LPS_R_EXIT, LPS_R_EXIT_SUB },
    };
    for (int i = 0; i < ntg; i++) {
        rect_t r = tg[i].r;
        card(c, r, P(44));
        int k = tg[i].id - ID_SHELL;
        lpui_text(c, LPG_M, TXT(60), r.x + P(90), r.y + P(108), LPUI_INK, 256, lpui_s(T[k][0]));
        lpui_text(c, LPG_S, TXT(40), r.x + P(90), r.y + P(180), LPUI_INK2, 210, lpui_s(T[k][1]));
    }
    lpui_text_center(c, LPG_S, TXT(40), SW / 2, Y(1960), LPUI_INK2, 170, lpui_s(LPS_R_HINT));
}

static void draw_auth(lpui_canvas_t *c)
{
    chrome(c, LPS_R_AUTH_TITLE,
           lpui_s(ai.mode == AUTH_RECOVERY ? LPS_R_AUTH_RECPW : LPS_R_AUTH_WHO));
    for (int i = 0; i < ntg; i++) {
        const target_t *t = &tg[i];
        if (t->style == T_CHIP) {
            int k = t->id - ID_CHIP;
            const char *name = ai.mode == AUTH_RECOVERY ? lpui_s(LPS_R_AUTH_RECNAME) : ai.users[k];
            target_t ch = *t;
            ch.style = k == who ? T_PRIMARY : T_BUTTON;
            button(c, &ch, name);
        } else if (t->style == T_FIELD) {
            lpui_text(c, LPG_S, TXT(40), t->r.x, t->r.y - P(24), LPUI_INK2, 220,
                      lpui_s(LPS_R_AUTH_PASSWORD));
            field(c, t, pw, true);
        } else if (t->style != T_KEY) {
            int id = t->id;
            button(c, t, lpui_s(id == ID_BACK ? LPS_R_BACK : id == ID_KBD ? LPS_R_KEYBOARD :
                                LPS_R_AUTH_OPEN));
        }
    }
    const char *m = ai.mode == AUTH_NOBODY ? lpui_s(LPS_R_AUTH_NOBODY) : msg;
    if (m[0])
        lpui_text_wrap(c, LPG_S, TXT(40), X(620), Y(1290), P(2600),
                       ai.mode == AUTH_NOBODY ? LPUI_DANGER : msg_color, 256, true, m);
    osk_draw(c);
}

static void draw_re_choose(lpui_canvas_t *c)
{
    char sub[160];
    if (version[0])
        lpui_fmt(sub, sizeof sub, lpui_s(LPS_R_RE_PAYLOAD), version);
    else
        strlcpy(sub, lpui_s(LPS_R_NO_PAYLOAD), sizeof sub);
    chrome(c, LPS_R_RE_TITLE, sub);
    for (int i = 0; i < ntg; i++) {
        const target_t *t = &tg[i];
        if (t->style == T_ROW) {
            bool erase = t->id == ID_ERASE;
            card(c, t->r, P(44));
            lpui_text(c, LPG_M, TXT(60), t->r.x + P(90), t->r.y + P(112),
                      erase ? LPUI_DANGER : LPUI_INK, 256,
                      lpui_s(erase ? LPS_R_RE_ERASE : LPS_R_RE_KEEP));
            lpui_text_wrap(c, LPG_S, TXT(40), t->r.x + P(90), t->r.y + P(188), t->r.w - P(180),
                           LPUI_INK2, 210, false,
                           lpui_s(erase ? LPS_R_RE_ERASE_SUB : LPS_R_RE_KEEP_SUB));
        } else
            button(c, t, lpui_s(LPS_R_BACK));
    }
}

static void draw_re_confirm(lpui_canvas_t *c)
{
    chrome(c, LPS_R_RE_TITLE, 0);
    int sp = TXT(40), lh = lpui_line(LPG_S, sp);
    lpui_text_center(c, LPG_M, TXT(60), SW / 2, Y(640),
                     re_mode == RE_ERASE ? LPUI_DANGER : LPUI_INK, 256,
                     lpui_s(re_mode == RE_ERASE ? LPS_R_RE_ERASE : LPS_R_RE_KEEP));
    int y = Y(760);
    char b[200];
    if (re_mode == RE_KEEP) {
        y += lh * lpui_text_wrap(c, LPG_S, sp, X(620), y, P(2600), LPUI_INK, 256, true,
                                 lpui_s(LPS_R_RE_KEPT));
        y += lh * lpui_text_wrap(c, LPG_S, sp, X(620), y, P(2600), LPUI_INK, 256, true,
                                 lpui_s(LPS_R_RE_REPLACED));
    } else
        y += lh * lpui_text_wrap(c, LPG_S, sp, X(620), y, P(2600), LPUI_DANGER, 256, true,
                                 lpui_s(LPS_R_RE_ERASED));
    lpui_fmt(b, sizeof b, lpui_s(LPS_R_RE_PAYLOAD), version);
    y += lh * lpui_text_wrap(c, LPG_S, sp, X(620), y, P(2600), LPUI_INK2, 220, true, b);
    lpui_text_wrap(c, LPG_S, sp, X(620), y, P(2600), LPUI_INK2, 190, true, lpui_s(LPS_R_RE_AGAIN));
    lpui_text_center(c, LPG_S, sp, SW / 2, Y(1130), LPUI_INK, 256, lpui_s(LPS_R_RE_TYPE));
    for (int i = 0; i < ntg; i++) {
        const target_t *t = &tg[i];
        if (t->style == T_FIELD)
            field(c, t, confirm, false);
        else if (t->style != T_KEY)
            button(c, t, lpui_s(t->id == ID_BACK ? LPS_R_CANCEL : t->id == ID_KBD ?
                                LPS_R_KEYBOARD : LPS_R_RE_GO));
    }
    osk_draw(c);
}

static rect_t bar_rect(void) { return (rect_t){ X(720), Y(980), P(2400), P(44) }; }

static void draw_re_progress(lpui_canvas_t *c)
{
    chrome(c, LPS_R_RE_TITLE, 0);
    int sp = TXT(40);
    lpui_text_center(c, LPG_M, TXT(60), SW / 2, Y(880), LPUI_INK, 256,
                     lpui_s(re_prog.step ? re_prog.step : LPS_R_RE_VERIFY));
    rect_t b = bar_rect();
    lpui_rrect(c, b.x, b.y, b.w, b.h, b.h / 2, 0xffffff, 40);
    int fw = (int)((s64)b.w * re_prog.permille / 1000);
    if (fw > b.h)
        lpui_rrect(c, b.x, b.y, fw, b.h, b.h / 2, LPUI_ACCENT, 256);
    char pct[16];
    lpui_fmt(pct, sizeof pct, "%d%%", re_prog.permille / 10);
    lpui_text_center(c, LPG_S, sp, SW / 2, Y(1120), LPUI_INK2, 220, pct);
    lpui_text_wrap(c, LPG_S, sp, X(620), Y(1260), P(2600), LPUI_INK2, 190, true,
                   lpui_s(LPS_R_RE_AGAIN));
}

static void draw_re_done(lpui_canvas_t *c)
{
    chrome(c, LPS_R_RE_TITLE, 0);
    if (re_ok)
        lpui_text_center(c, LPG_M, TXT(60), SW / 2, Y(950), LPUI_INK, 256, lpui_s(LPS_R_RE_DONE));
    else
        lpui_text_wrap(c, LPG_S, TXT(40), X(620), Y(900), P(2600), LPUI_DANGER, 256, true, re_result);
    for (int i = 0; i < ntg; i++)
        button(c, &tg[i], lpui_s(tg[i].id == ID_RESTART ? LPS_R_RESTART : LPS_R_BACK));
}

static rect_t out_rect(void) { return (rect_t){ X(420), Y(640), P(3000), P(1140) }; }

static void draw_check(lpui_canvas_t *c)
{
    chrome(c, LPS_R_CK_TITLE, 0);
    rect_t r = out_rect();
    lpui_rrect(c, r.x, r.y, r.w, r.h, P(36), 0x000000, 80);
    int sp = TXT(36), lh = lpui_line(LPG_S, sp);
    int fit = (r.h - P(60)) / (lh ? lh : 1);
    int first = nout > fit ? nout - fit : 0;
    for (int i = first; i < nout; i++)
        lpui_text(c, LPG_S, sp, r.x + P(50), r.y + P(40) + lpui_ascent(LPG_S, sp) + (i - first) * lh,
                  out_col[i], 256, out_line[i]);
    for (int i = 0; i < ntg; i++)
        button(c, &tg[i], lpui_s(LPS_R_CK_DONE));
}

static void draw_restart(lpui_canvas_t *c)
{
    chrome(c, LPS_R_TITLE, 0);
    lpui_text_center(c, LPG_M, TXT(60), SW / 2, Y(1000), LPUI_INK, 256, lpui_s(LPS_R_RESTARTING));
}

static void draw_base(void)
{
    lpui_copy_rect(&cv_base, &cv_bg, 0, 0, SW, SH);
    lpui_canvas_t *c = &cv_base;
    switch (screen) {
    case SC_MENU:        draw_menu(c); break;
    case SC_AUTH:        draw_auth(c); break;
    case SC_RE_CHOOSE:   draw_re_choose(c); break;
    case SC_RE_CONFIRM:  draw_re_confirm(c); break;
    case SC_RE_PROGRESS: draw_re_progress(c); break;
    case SC_RE_DONE:     draw_re_done(c); break;
    case SC_CHECK:       draw_check(c); break;
    case SC_RESTART:     draw_restart(c); break;
    }
    status_line(c);
}

/* ── Overlays: focus highlight, press, mouse pointer ──────────────── */
static lpui_spring_t hx, hy, hw, hh;
static bool hl_on;
static rect_t ov_last;                  /* what the overlays covered last time */
static int cur_x = -1, cur_y = -1;
static bool anim;

static rect_t hl_now(void)
{
    return (rect_t){ (int)(hx.x >> 16), (int)(hy.x >> 16), (int)(hw.x >> 16), (int)(hh.x >> 16) };
}

static rect_t grow(rect_t r, int d) { return (rect_t){ r.x - d, r.y - d, r.w + 2 * d, r.h + 2 * d }; }

static rect_t unite(rect_t a, rect_t b)
{
    if (a.w <= 0 || a.h <= 0) return b;
    if (b.w <= 0 || b.h <= 0) return a;
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return (rect_t){ x0, y0, x1 - x0, y1 - y0 };
}

static rect_t overlays_bound(void)
{
    rect_t b = { 0, 0, 0, 0 };
    if (hl_on)
        b = grow(hl_now(), P(24));
    if (pressed >= 0 && pressed < ntg)
        b = unite(b, tg[pressed].r);
    if (cur_x >= 0) {
        int s = P(40) < 12 ? 12 : P(40);
        b = unite(b, (rect_t){ cur_x - s, cur_y - s, 2 * s, 2 * s });
    }
    return b;
}

/* Draw the overlays into `c`, a window onto the frame whose top-left is
 * screen position (dx, dy) - clipping to the window keeps a translucent
 * overlay from being blended twice where two updates overlap. */
static void draw_overlays(lpui_canvas_t *c, int dx, int dy)
{
    if (hl_on && focus < ntg) {
        rect_t r = hl_now();
        const target_t *t = &tg[focus];
        int rad = t->style == T_ROW ? P(44) : t->style == T_FIELD ? P(28) : r.h / 2;
        if (t->style == T_ROW)
            lpui_rrect(c, r.x - dx, r.y - dy, r.w, r.h, rad, 0xffffff, LPUI_SEL_A - LPUI_CARD_A);
        int th = P(8) < 2 ? 2 : P(8);
        lpui_rrect_stroke(c, r.x - dx - th, r.y - dy - th, r.w + 2 * th, r.h + 2 * th, rad + th,
                          th, LPUI_ACCENT, 256);
    }
    if (pressed >= 0 && pressed < ntg) {
        rect_t r = tg[pressed].r;
        int rad = tg[pressed].style == T_ROW ? P(44) : tg[pressed].style == T_FIELD ? P(28) : r.h / 2;
        lpui_rrect(c, r.x - dx, r.y - dy, r.w, r.h, rad, 0xffffff, 60);
    }
    if (cur_x >= 0) {
        int s = P(36) < 10 ? 10 : P(36);
        lpui_rrect(c, cur_x - s / 2 - dx, cur_y - s / 2 - dy, s, s, s / 2, 0xffffff, 230);
        lpui_rrect_stroke(c, cur_x - s / 2 - dx, cur_y - s / 2 - dy, s, s, s / 2,
                          s / 6 ? s / 6 : 1, 0x2a0f27, 200);
    }
}

static void compose_rect(rect_t d)
{
    if (d.x < 0) { d.w += d.x; d.x = 0; }
    if (d.y < 0) { d.h += d.y; d.y = 0; }
    if (d.x + d.w > SW) d.w = SW - d.x;
    if (d.y + d.h > SH) d.h = SH - d.y;
    if (d.w <= 0 || d.h <= 0)
        return;
    lpui_copy_rect(&cv_frame, &cv_base, d.x, d.y, d.w, d.h);
    lpui_canvas_t win = { cv_frame.px + (u64)d.y * cv_frame.stride + d.x, d.w, d.h, cv_frame.stride };
    draw_overlays(&win, d.x, d.y);
    scr_present(&cv_frame, d.x, d.y, d.w, d.h);
}

/* After the overlays moved: redraw where they were and where they are. */
static void present_overlays(void)
{
    rect_t now = overlays_bound();
    compose_rect(unite(ov_last, now));
    ov_last = now;
}

/* After part of the base changed. */
static void present_rect(rect_t r)
{
    rect_t now = overlays_bound();
    compose_rect(unite(r, unite(ov_last, now)));
    ov_last = now;
}

static void present_all(void)
{
    ov_last = overlays_bound();
    compose_rect((rect_t){ 0, 0, SW, SH });
}

static void set_focus(int i, bool jump)
{
    if (i < 0 || i >= ntg) {
        hl_on = false;
        return;
    }
    focus = i;
    rect_t r = tg[i].r;
    if (!hl_on || jump || reduced) {
        lpui_spring_init(&hx, LPUI_SPRING_MENU, (s64)r.x << 16);
        lpui_spring_init(&hy, LPUI_SPRING_MENU, (s64)r.y << 16);
        lpui_spring_init(&hw, LPUI_SPRING_MENU, (s64)r.w << 16);
        lpui_spring_init(&hh, LPUI_SPRING_MENU, (s64)r.h << 16);
        hl_on = true;
        return;
    }
    /* Retarget: position and velocity carry on from where they are. */
    hx.target = (s64)r.x << 16;
    hy.target = (s64)r.y << 16;
    hw.target = (s64)r.w << 16;
    hh.target = (s64)r.h << 16;
    anim = true;
}

static bool springs_step(int ms)
{
    bool a = lpui_spring_step(&hx, ms);
    a |= lpui_spring_step(&hy, ms);
    a |= lpui_spring_step(&hw, ms);
    a |= lpui_spring_step(&hh, ms);
    return a;
}

/* ── Screen changes ───────────────────────────────────────────────────
 * The new screen fades in over the old one in the menu spring's time
 * (180 ms), eased out; 100 ms when motion is reduced. */
static void scr_present_mix(const lpui_canvas_t *a, const lpui_canvas_t *b, u32 wa)
{
    /* cv_old is overwritten row by row with the mix, then presented. */
    for (int y = 0; y < SH; y++) {
        u32 *d = cv_old.px + (u64)y * cv_old.stride;
        const u32 *s = a->px + (u64)y * a->stride;
        const u32 *t = b->px + (u64)y * b->stride;
        for (int x = 0; x < SW; x++)
            d[x] = lpui_mix(t[x], s[x], wa);
    }
    scr_present(&cv_old, 0, 0, SW, SH);
}

static void go(int sc, bool from_black)
{
    /* What is on the screen now. */
    if (from_black)
        memset(cv_old.px, 0, (size_t)SW * SH * 4);
    else
        lpui_copy_rect(&cv_old, &cv_frame, 0, 0, SW, SH);
    screen = sc;
    pressed = -1;
    focus = 0;
    hl_on = false;
    layout();
    draw_base();
    set_focus(ntg ? 0 : -1, true);
    /* the finished frame */
    lpui_copy_rect(&cv_frame, &cv_base, 0, 0, SW, SH);
    ov_last = overlays_bound();
    lpui_canvas_t full = cv_frame;
    draw_overlays(&full, 0, 0);
    /* keep a copy of the old picture for the mix (cv_old is reused) */
    int dur = reduced ? 100 : 180;
    s64 t0 = lp_monotonic_ms();
    static lpui_canvas_t oldpic;
    if (!oldpic.px) {
        oldpic = cv_old;
        oldpic.px = malloc((size_t)SW * SH * 4);
    }
    if (oldpic.px) {
        lpui_copy_rect(&oldpic, &cv_old, 0, 0, SW, SH);
        for (;;) {
            s64 t = lp_monotonic_ms() - t0;
            if (t >= dur)
                break;
            u32 f = (u32)(t * 256 / dur);
            u32 e = 256 - ((256 - f) * (256 - f) >> 8);
            scr_present_mix(&cv_frame, &oldpic, e);
        }
    }
    scr_present(&cv_frame, 0, 0, SW, SH);
    last_status = lp_monotonic_ms();
}

static void redraw(void)
{
    draw_base();
    present_all();
}

/* ── The recovery shell ───────────────────────────────────────────── */
static void open_shell(const char *who_name)
{
    sys_root_remount(true);
    rlog("recovery shell opened (administrator: %s)", who_name);
    scr_graphics(false);
    static const char hello[] =
        "\033[2J\033[H\033[?25h\r\n"
        "  You are root on the recovery system. The installed system is at /mnt/lp.\r\n"
        "  Type exit to return.\r\n\r\n";
    lp_write(STDOUT_FILENO, hello, sizeof hello - 1);
    pid_t pid = lp_fork();
    if (pid == 0) {
        lp_setsid();
        long fd = lp_open("/dev/tty1", O_RDWR, 0);
        if (fd >= 0) {
            lp_dup2((int)fd, 0);
            lp_dup2((int)fd, 1);
            lp_dup2((int)fd, 2);
            if (fd > 2)
                lp_close((int)fd);
        }
        lp_ioctl(0, TIOCSCTTY, (void *)1);
        lp_term_sane(0);
        lp_term_set_utf8(0);
        for (int s = 1; s < 32; s++)
            if (s != 9 && s != 19)
                lp_signal_default(s);
        lp_chdir("/root");
        const char *sh = lp_exists("/bin/lpsh") ? "/bin/lpsh" : "/bin/sh";
        char *argv[] = { (char *)sh, 0 };
        char *envp[] = { "HOME=/root", "PATH=/bin:/sbin:/usr/bin:/usr/sbin", "TERM=linux",
                         "USER=root", "LOGNAME=root", "SHELL=/bin/lpsh", "LANG=C.UTF-8", 0 };
        lp_execve(sh, argv, envp);
        lp_exit(127);
    }
    int status = 0;
    if (pid > 0)
        while (lp_waitpid(pid, &status, 0) != pid)
            ;
    rlog("recovery shell closed (status %d)", LP_WIFEXITED(status) ? LP_WEXITSTATUS(status) : -1);
    lp_sync();
    sys_root_remount(false);
    scr_graphics(true);
    in_drain();
    msg[0] = 0;
    go(SC_MENU, true);
}

/* ── Exit ─────────────────────────────────────────────────────────── */
static void restart(void)
{
    go(SC_RESTART, false);
    rlog("exit recovery: restarting");
    if (lp_efivars_ready()) {
        long c = lp_bootcount_get();
        /* A fresh start for LP - unless a reinstall is unfinished, which
         * keeps the menu pointing here until it is done. */
        if (c >= 0 && c < (long)LP_BOOT_REINSTALL_MARK)
            lp_bootcount_set(0);
        lp_efivar_delete("LPBootNext");
    }
    lp_sync();
    sys_root_umount();
    lp_umount(MNT_ESP, 0);
    lp_sync();
    /* init stops everything, unmounts and reboots (SIGUSR2). If it has
     * not after 15 s, do it here. */
    lp_kill(1, SIGUSR2);
    lp_sleep_ms(15000);
    lp_sync();
    lp_reboot(LINUX_REBOOT_CMD_RESTART);
    for (;;)
        lp_sleep_ms(1000);
}

/* ── Check and repair disks ───────────────────────────────────────── */
static void out_add(const char *s, u32 col)
{
    if (nout == OUTN) {
        memmove(out_line[0], out_line[1], sizeof out_line[0] * (OUTN - 1));
        memmove(&out_col[0], &out_col[1], sizeof out_col[0] * (OUTN - 1));
        nout--;
    }
    strlcpy(out_line[nout], s, sizeof out_line[0]);
    out_col[nout++] = col;
    draw_base();
    present_rect(out_rect());
}

static void on_fsck_line(const char *s, void *ctx)
{
    (void)ctx;
    out_add(s, LPUI_INK2);
}

static void check_one(const char *name, const part_t *p, char *const argv[], bool readonly)
{
    char b[160];
    if (!p->found) {
        lpui_fmt(b, sizeof b, lpui_s(LPS_R_CK_MISSING), name);
        out_add(b, LPUI_DANGER);
        return;
    }
    lpui_fmt(b, sizeof b, lpui_s(LPS_R_CK_RUNNING), name);
    out_add(b, LPUI_INK);
    int r = run(argv, on_fsck_line, 0);
    rlog("check %s: exit %d", name, r);
    if (r == 0)
        lpui_fmt(b, sizeof b, lpui_s(LPS_R_CK_CLEAN), name);
    else if (!readonly && (r == 1 || r == 2 || r == 3))
        lpui_fmt(b, sizeof b, lpui_s(LPS_R_CK_FIXED), name);
    else
        lpui_fmt(b, sizeof b, lpui_s(LPS_R_CK_LEFT), name, r);
    out_add(b, r == 0 ? LPUI_INK : (!readonly && r <= 3) ? LPUI_ACCENT : LPUI_DANGER);
    out_add("", LPUI_INK2);
}

extern void sys_log_pause(bool pause);

static void check_disks(void)
{
    nout = 0;
    check_done = false;
    go(SC_CHECK, false);
    rlog("check and repair disks");
    sys_root_umount();
    lp_umount(MNT_ESP, 0);
    char *esp[] = { "/usr/sbin/fsck.vfat", "-a", "-w", p_esp.dev, 0 };
    check_one("LP-ESP", &p_esp, esp, false);
    char *root[] = { "/usr/sbin/e2fsck", "-f", "-y", p_root.dev, 0 };
    check_one("LP-ROOT", &p_root, root, false);
    /* LP-RECOVERY is the running system: checked, never repaired, and
     * made read-only for the check so the answer is not about a
     * filesystem changing under it. */
    sys_log_pause(true);
    bool ro = lp_mount(p_rec.dev, "/", "ext4", MS_REMOUNT | MS_RDONLY, NULL) == 0;
    char *rec[] = { "/usr/sbin/e2fsck", "-f", "-n", p_rec.dev, 0 };
    check_one("LP-RECOVERY", &p_rec, rec, true);
    if (ro)
        lp_mount(p_rec.dev, "/", "ext4", MS_REMOUNT, NULL);
    sys_log_pause(false);
    sys_root_mount(false);
    int st = sys_root_state();
    disk_status = !p_root.found ? LPS_R_DISK_NOROOT : st == 1 ? LPS_R_DISK_CHECK : LPS_R_DISK_OK;
    check_done = true;
    layout();
    set_focus(0, true);
    redraw();
}

/* ── Reinstall ────────────────────────────────────────────────────── */
static void on_progress(const re_progress_t *p, void *ctx)
{
    (void)ctx;
    re_prog = *p;
    draw_base();
    present_rect((rect_t){ 0, Y(780), SW, Y(1160) - Y(780) });
}

static void reinstall_go(void)
{
    re_prog = (re_progress_t){ LPS_R_RE_VERIFY, 0 };
    go(SC_RE_PROGRESS, false);
    bool damaged = false;
    const char *err = re_run(re_mode, on_progress, 0, &damaged);
    re_ok = err == 0;
    if (err) {
        lpui_fmt(re_result, sizeof re_result,
                 lpui_s(damaged ? LPS_R_RE_DAMAGED : LPS_R_RE_FAILED), err);
        rlog("reinstall failed: %s", err);
    }
    interrupted = re_interrupted();
    int st = sys_root_state();
    disk_status = !p_root.found ? LPS_R_DISK_NOROOT : st == 1 ? LPS_R_DISK_CHECK : LPS_R_DISK_OK;
    go(SC_RE_DONE, false);
}

/* ── Actions ──────────────────────────────────────────────────────── */
static void set_msg(u32 col, const char *fmt, int v, const char *s)
{
    if (s)
        lpui_fmt(msg, sizeof msg, fmt, s);
    else
        lpui_fmt(msg, sizeof msg, fmt, v);
    msg_color = col;
}

static void auth_submit(void)
{
    if (ai.mode == AUTH_NOBODY)
        return;
    int wait = auth_lockout_left();
    if (wait > 0) {
        set_msg(LPUI_DANGER, lpui_s(LPS_R_AUTH_WAIT), wait, 0);
        redraw();
        return;
    }
    set_msg(LPUI_INK2, lpui_s(LPS_R_AUTH_CHECKING), 0, 0);
    redraw();
    const char *why = "";
    int r = auth_check(&ai, who, pw, &wait, &why);
    memset(pw, 0, sizeof pw);
    pwlen = 0;
    switch (r) {
    case 0: {
        char name[40];
        strlcpy(name, ai.mode == AUTH_RECOVERY ? "recovery password" : ai.users[who], sizeof name);
        msg[0] = 0;
        open_shell(name);
        return;
    }
    case 1:
        if (wait > 0)
            set_msg(LPUI_DANGER, lpui_s(LPS_R_AUTH_WAIT), wait, 0);
        else
            set_msg(LPUI_DANGER, lpui_s(LPS_R_AUTH_WRONG), auth_tries_left(), 0);
        break;
    case 2:
        set_msg(LPUI_DANGER, lpui_s(LPS_R_AUTH_WAIT), wait, 0);
        break;
    case 3:
        set_msg(LPUI_DANGER, lpui_s(LPS_R_AUTH_FORMAT), 0, why);
        break;
    default:
        set_msg(LPUI_DANGER, lpui_s(LPS_R_AUTH_LOCKED), 0, 0);
        break;
    }
    redraw();
}

static void type_char(u32 ch)
{
    if (screen == SC_AUTH && ai.mode != AUTH_NOBODY) {
        if (auth_lockout_left() > 0)
            return;
        if (pwlen < (int)sizeof pw - 1) {
            pw[pwlen++] = (char)ch;
            pw[pwlen] = 0;
        }
        if (msg_color == LPUI_DANGER)
            msg[0] = 0;
    } else if (screen == SC_RE_CONFIRM) {
        if (ch >= 'a' && ch <= 'z')
            ch -= 32;
        if (conflen < (int)sizeof confirm - 1 && ch > ' ') {
            confirm[conflen++] = (char)ch;
            confirm[conflen] = 0;
        }
        layout();
    } else
        return;
    redraw();
}

static void backspace(void)
{
    if (screen == SC_AUTH && pwlen > 0)
        pw[--pwlen] = 0;
    else if (screen == SC_RE_CONFIRM && conflen > 0) {
        confirm[--conflen] = 0;
        layout();
    } else
        return;
    redraw();
}

static void toggle_osk(bool on)
{
    osk = on;
    osk_shift = false;
    int keep = tg[focus].id;
    layout();
    focus = 0;
    for (int i = 0; i < ntg; i++)
        if (tg[i].id == keep)
            focus = i;
    set_focus(focus, true);
    redraw();
}

static void back(void)
{
    switch (screen) {
    case SC_AUTH:
    case SC_RE_CHOOSE:
    case SC_CHECK:
        memset(pw, 0, sizeof pw);
        pwlen = 0;
        osk = false;
        go(SC_MENU, false);
        break;
    case SC_RE_CONFIRM:
        osk = false;
        go(SC_RE_CHOOSE, false);
        break;
    case SC_RE_DONE:
        if (!re_ok)
            go(SC_MENU, false);
        break;
    }
}

static void activate(int id)
{
    if (id >= ID_KEY && id < ID_KEY + 64) {
        int row = (id - ID_KEY) / 16, col = (id - ID_KEY) % 16;
        type_char((u8)OSK_ROWS[osk_shift][row][col]);
        return;
    }
    switch (id) {
    case ID_SHELL:
        msg[0] = 0;
        memset(pw, 0, sizeof pw);
        pwlen = 0;
        who = 0;
        osk = false;
        auth_prepare(&ai);
        go(SC_AUTH, false);
        if (ai.mode != AUTH_NOBODY && find(ID_FIELD)) {
            for (int i = 0; i < ntg; i++)
                if (tg[i].id == ID_FIELD)
                    set_focus(i, true);
            int wait = auth_lockout_left();
            if (wait > 0)
                set_msg(LPUI_DANGER, lpui_s(LPS_R_AUTH_WAIT), wait, 0);
            redraw();
        }
        return;
    case ID_REINSTALL:
        if (!re_payload_present(version, sizeof version))
            version[0] = 0;
        go(SC_RE_CHOOSE, false);
        return;
    case ID_CHECK:
        check_disks();
        return;
    case ID_EXIT:
    case ID_RESTART:
        restart();
        return;
    case ID_KEEP:
    case ID_ERASE:
        re_mode = id == ID_KEEP ? RE_KEEP : RE_ERASE;
        confirm[0] = 0;
        conflen = 0;
        osk = false;
        go(SC_RE_CONFIRM, false);
        for (int i = 0; i < ntg; i++)
            if (tg[i].id == ID_FIELD)
                set_focus(i, true);
        present_all();
        return;
    case ID_BACK:
        back();
        return;
    case ID_DONE:
        go(SC_MENU, false);
        return;
    case ID_KBD:
        toggle_osk(!osk);
        return;
    case ID_K_CLOSE:
        toggle_osk(false);
        return;
    case ID_K_SHIFT:
        osk_shift = !osk_shift;
        redraw();
        return;
    case ID_K_BKSP:
        backspace();
        return;
    case ID_K_SPACE:
        type_char(' ');
        return;
    case ID_K_ENTER:
    case ID_FIELD:
    case ID_OPEN:
    case ID_GO:
        if (screen == SC_AUTH && id != ID_FIELD)
            auth_submit();
        else if (screen == SC_AUTH && id == ID_FIELD && pwlen > 0)
            auth_submit();
        else if (screen == SC_RE_CONFIRM && !strcmp(confirm, "REINSTALL") && id != ID_FIELD)
            reinstall_go();
        else if (screen == SC_RE_CONFIRM && id == ID_FIELD && !strcmp(confirm, "REINSTALL"))
            reinstall_go();
        return;
    }
    if (id >= ID_CHIP && id < ID_CHIP + AUTH_MAX_USERS) {
        who = id - ID_CHIP;
        redraw();
    }
}

/* ── Input ────────────────────────────────────────────────────────── */
static int hit(int x, int y)
{
    for (int i = ntg - 1; i >= 0; i--) {
        rect_t r = tg[i].r;
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
            return i;
    }
    return -1;
}

static bool ptr_down;

static void on_ptr(const uev_t *e)
{
    if (e->mouse) {
        cur_x = e->x;
        cur_y = e->y;
    } else
        cur_x = cur_y = -1;
    int t = hit(e->x, e->y);
    if (e->down && !ptr_down) {
        ptr_down = true;
        pressed = t;                    /* feedback on touch-down, no delay */
        if (t >= 0 && !tg[t].disabled)
            set_focus(t, false);
        else
            pressed = -1;
        present_overlays();
    } else if (!e->down && ptr_down) {
        ptr_down = false;
        int p = pressed;
        pressed = -1;
        present_overlays();
        if (p >= 0 && p == t)
            activate(tg[p].id);
    } else if (e->down) {
        if (t != pressed && pressed >= 0) {
            pressed = -1;               /* slid off: cancel */
            present_overlays();
        }
    } else {
        if (e->mouse && t >= 0 && t != focus && !tg[t].disabled)
            set_focus(t, false);
        present_overlays();
    }
}

static void move_focus(int d)
{
    if (!ntg)
        return;
    int i = focus;
    for (int k = 0; k < ntg; k++) {
        i = (i + d + ntg) % ntg;
        if (!tg[i].disabled)
            break;
    }
    set_focus(i, false);
    if (screen == SC_AUTH && tg[i].style == T_CHIP) {
        who = tg[i].id - ID_CHIP;
        redraw();
    }
}

static void on_key(const uev_t *e)
{
    switch (e->code) {
    case K_TAB: case K_DOWN: case K_RIGHT:
        move_focus(1);
        return;
    case K_UP: case K_LEFT:
        move_focus(-1);
        return;
    case K_HOME:
        set_focus(0, false);
        return;
    case K_END:
        set_focus(ntg - 1, false);
        return;
    case K_ESC:
        if (osk)
            toggle_osk(false);
        else
            back();
        return;
    case K_BACKSPACE:
        backspace();
        return;
    case K_ENTER: case K_KPENTER:
        if (ntg && focus < ntg && !tg[focus].disabled)
            activate(tg[focus].id);
        else if (screen == SC_AUTH)
            auth_submit();
        return;
    }
    if (e->ch >= 32 && e->ch < 127) {
        /* On screens without a text field, digits pick menu rows. */
        if (screen == SC_MENU && e->ch >= '1' && e->ch <= '4') {
            set_focus(e->ch - '1', false);
            activate(ID_SHELL + (int)(e->ch - '1'));
            return;
        }
        type_char(e->ch);
    }
}

/* ── Setup ────────────────────────────────────────────────────────── */
static void choose_language(void)
{
    char cmd[1024], loc[256];
    proc_read("/proc/cmdline", cmd, sizeof cmd);
    if (strstr(cmd, "lp.lang=ko"))
        lpui_korean = true;
    else if (strstr(cmd, "lp.lang=en"))
        lpui_korean = false;
    else if (sys_root_mounted() && file_read(MNT_ROOT "/etc/default/locale", loc, sizeof loc) &&
             strstr(loc, "LANG=ko"))
        lpui_korean = true;
    else if (file_read(PAYLOAD_DIR "/manifest", loc, sizeof loc) && strstr(loc, "\nlang ko"))
        lpui_korean = true;
    rlog("language %s", lpui_korean ? "Korean" : "English");
}

/* The status line's clock in the installed system's time zone. Its
 * /etc/localtime is usually an absolute link into its own /usr/share/
 * zoneinfo, which from here is under /mnt/lp. */
static void choose_timezone(void)
{
    if (!sys_root_mounted())
        return;
    char link[200], path[260];
    long n = lp_readlink(MNT_ROOT "/etc/localtime", link, sizeof link - 1);
    if (n > 0) {
        link[n] = 0;
        if (link[0] == '/')
            snprintf(path, sizeof path, MNT_ROOT "%s", link);
        else
            snprintf(path, sizeof path, MNT_ROOT "/etc/%s", link);
    } else
        strlcpy(path, MNT_ROOT "/etc/localtime", sizeof path);
    if (lp_exists(path)) {
        /* Copied, because LP-ROOT is unmounted for checks and reinstalls
         * and the zone file must not vanish with it. */
        if (file_copy(path, "/etc/localtime", 0644))
            unsetenv("TZ");
    }
}

int main(void)
{
    lp_signal_ignore(SIGPIPE_);
    lp_signal_ignore(SIGINT);
    lp_signal_ignore(SIGQUIT);
    lp_signal_ignore(SIGHUP);
    lp_signal_ignore(SIGTSTP);
    sys_init();
    rlog("LP Recovery starting");
    sys_find_partitions();
    sys_root_mount(false);
    choose_language();
    choose_timezone();
    int st = sys_root_state();
    disk_status = !p_root.found ? LPS_R_DISK_NOROOT : st == 1 ? LPS_R_DISK_CHECK : LPS_R_DISK_OK;
    interrupted = re_interrupted();
    reduced = motion_reduced();

    if (!scr_open()) {
        static const char no[] =
            "\r\nLP Recovery: this machine has no framebuffer (/dev/fb0), so the\r\n"
            "recovery menu cannot be shown. Nothing has been changed.\r\n";
        lp_write(STDOUT_FILENO, no, sizeof no - 1);
        rlog("no framebuffer; waiting");
        for (;;)
            lp_sleep_ms(60000);
    }
    cv_old = cv_frame;
    cv_old.px = malloc((size_t)SW * SH * 4);
    if (!cv_old.px)
        return 1;
    lpui_set_screen(SW, SH);
    ox = (SW - lpui_px(3840)) / 2;
    oy = (SH - lpui_px(2160)) / 2;
    lpui_gradient(&cv_bg);
    lpui_logo(&cv_bg, SW / 2, Y(250), P(210), logo_scratch);
    in_rescan();
    scr_graphics(true);
    in_drain();
    go(SC_MENU, true);
    if (interrupted)
        set_focus(1, true), present_all();   /* Reinstall LP, to finish it */

    s64 last = lp_monotonic_ms();
    for (;;) {
        uev_t e;
        int got = in_wait(&e, anim ? 12 : 250);
        s64 now = lp_monotonic_ms();
        int dt = (int)(now - last);
        last = now;
        if (got) {
            if (e.kind == UEV_KEY)
                on_key(&e);
            else if (e.kind == UEV_PTR)
                on_ptr(&e);
        }
        if (anim) {
            anim = springs_step(dt > 0 ? dt : 1);
            present_overlays();
        }
        if (now - last_status >= 1000) {
            last_status = now;
            if (screen == SC_AUTH) {
                int w = auth_lockout_left();
                if (w > 0) {
                    set_msg(LPUI_DANGER, lpui_s(LPS_R_AUTH_WAIT), w, 0);
                    draw_base();
                    present_rect((rect_t){ 0, Y(1220), SW, Y(1360) - Y(1220) });
                } else if (msg_color == LPUI_DANGER && strstr(msg, "s.") && pwlen == 0 &&
                           auth_tries_left() == 3) {
                    msg[0] = 0;
                    draw_base();
                    present_rect((rect_t){ 0, Y(1220), SW, Y(1360) - Y(1220) });
                }
            }
            if (!osk) {
                draw_base();
                present_rect(status_band());
            }
        }
    }
}
