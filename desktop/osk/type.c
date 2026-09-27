#define _GNU_SOURCE 1
/*
 * type.c - where the keys' text goes.
 *
 * There are three ways out of lp-osk, and each exists because the others
 * cannot do its job:
 *
 * 1. The input method (zwp_input_method_v2). When a text field that speaks
 *    text-input-v3 has focus - GTK 3 and 4, foot, Firefox - the compositor
 *    hands us that field. Hangul composition goes through it as a real
 *    preedit: set_preedit_string shows the syllable being built, underlined,
 *    in the application, and commit_string makes it text. Nothing is typed
 *    and deleted again, so an application that reacts to every keystroke
 *    (a shell's history search, a web page's autocomplete) never sees the
 *    intermediate syllables, and undo takes back words, not jamo.
 *
 * 2. A virtual keyboard of our own (vk_osk, zwp_virtual_keyboard_v1) for
 *    everything that is a key rather than text: Enter, Tab, Esc, arrows,
 *    BackSpace outside composition, Ctrl and Alt combinations, and the
 *    ASCII keys, which go out as the same key events a physical keyboard
 *    sends so that shortcuts and terminals behave exactly as they would.
 *    Its keymap is ours (wtype's approach, read from the base's wtype):
 *    a US layout on the real evdev codes, plus a pool of spare keycodes
 *    that are re-pointed at whatever Unicode character is needed next,
 *    uploaded again only when a character is not in it yet. That makes
 *    any character typeable into an application with no text-input
 *    support at all (Xwayland), and there Hangul is composed the old way:
 *    type the syllable, and replace it with BackSpace + the new one as it
 *    grows.
 *
 *    The pool stays below keycode 256 because Xwayland drops anything
 *    above it, and it skips every code a compositor might bind without a
 *    modifier - wayfire matches its bindings on keycodes, not keysyms, so
 *    a Hangul syllable parked on KEY_SYSRQ would take a screenshot.
 *
 * 3. A second virtual keyboard (vk_pass) that carries the laptop's own
 *    keyboard. Only one input method may exist per seat, so there is no
 *    room for fcitx5 next to us and lp-osk has to be the Korean input
 *    method for the physical keys as well. While a text field is focused we
 *    hold the input method's keyboard grab, run the same automaton on the
 *    keys that spell jamo, and send every other key back out through
 *    vk_pass unchanged - same keymap (the grab's own, forwarded fd and
 *    all), same modifiers - so shortcuts and key repeat are untouched. The
 *    grab is dropped the moment the field loses focus, and every key we
 *    forwarded as held is released then, because a key left down on a
 *    keyboard nobody types on repeats forever.
 *
 *    sway 1.7 routes physical keys to the grab. wayfire 0.7.4 accepts the
 *    grab but never sends it a key (it has no call to
 *    wlr_input_method_keyboard_grab_v2_send_key), so there the laptop
 *    keyboard stays English-only and the grab is inert, not harmful.
 */
#include "osk.h"

#include <errno.h>
#include <gdk/gdkwayland.h>
#include <linux/input-event-codes.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include "input-method-unstable-v2-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

/* text-input-v3 values the input method protocol passes through */
#define PURPOSE_PASSWORD 8
#define PURPOSE_PIN      9
#define CAUSE_OTHER      1

static struct wl_display *dpy;
static struct wl_seat *seat;
static struct zwp_virtual_keyboard_manager_v1 *vkm;
static struct zwp_input_method_manager_v2 *imm;
static struct zwp_input_method_v2 *im;
static struct zwp_input_method_keyboard_grab_v2 *grab;
static struct zwp_virtual_keyboard_v1 *vk_osk, *vk_pass;
static struct xkb_context *xkb;

static HangulIC hic;

static struct {
    int      pending;          /* -1 nothing, 0 deactivate, 1 activate */
    gboolean active;
    gboolean unavailable;
    uint32_t serial;           /* done events seen; commit() echoes it */
    uint32_t purpose, pending_purpose;
    uint32_t cause;
    gboolean preedit_sent;     /* the application shows a preedit of ours */
    char    *surr, *pending_surr;   /* surrounding text (without preedit) */
    uint32_t cursor, pending_cursor;
    gboolean surr_changed;     /* this done batch moved the text or cursor */
} ims = { .pending = -1 };

/* A syllable in the making when its field was deactivated, kept for a
 * moment in case the same field comes straight back (see im_done). */
static struct {
    gboolean valid;
    HangulIC ic;
    char    *surr;
    uint32_t cursor;
    gint64   at;
} bounce;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void flush(void)
{
    if (dpy)
        wl_display_flush(dpy);
}

/* ── the on-screen keyboard's own keymap ────────────────────────── */

typedef struct { uint16_t code; const char *sym, *shifted; } BaseKey;

/* The US layout on the evdev codes a real keyboard uses. Letters come from
 * letter_code below. The three modifiers are here so the keymap can say
 * which real modifier each one is (Shift, Control, Mod1); we never press
 * them - modifiers go out as modifier state, as wtype sends them. */
static const BaseKey base_keys[] = {
    {KEY_ESC, "Escape", 0},
    {KEY_1, "1", "exclam"}, {KEY_2, "2", "at"}, {KEY_3, "3", "numbersign"},
    {KEY_4, "4", "dollar"}, {KEY_5, "5", "percent"},
    {KEY_6, "6", "asciicircum"}, {KEY_7, "7", "ampersand"},
    {KEY_8, "8", "asterisk"}, {KEY_9, "9", "parenleft"},
    {KEY_0, "0", "parenright"}, {KEY_MINUS, "minus", "underscore"},
    {KEY_EQUAL, "equal", "plus"}, {KEY_BACKSPACE, "BackSpace", 0},
    {KEY_TAB, "Tab", "ISO_Left_Tab"},
    {KEY_LEFTBRACE, "bracketleft", "braceleft"},
    {KEY_RIGHTBRACE, "bracketright", "braceright"},
    {KEY_ENTER, "Return", 0}, {KEY_LEFTCTRL, "Control_L", 0},
    {KEY_SEMICOLON, "semicolon", "colon"},
    {KEY_APOSTROPHE, "apostrophe", "quotedbl"},
    {KEY_GRAVE, "grave", "asciitilde"}, {KEY_LEFTSHIFT, "Shift_L", 0},
    {KEY_BACKSLASH, "backslash", "bar"}, {KEY_COMMA, "comma", "less"},
    {KEY_DOT, "period", "greater"}, {KEY_SLASH, "slash", "question"},
    {KEY_LEFTALT, "Alt_L", 0}, {KEY_SPACE, "space", 0},
    {KEY_F1, "F1", 0}, {KEY_F2, "F2", 0}, {KEY_F3, "F3", 0},
    {KEY_F4, "F4", 0}, {KEY_F5, "F5", 0}, {KEY_F6, "F6", 0},
    {KEY_F7, "F7", 0}, {KEY_F8, "F8", 0}, {KEY_F9, "F9", 0},
    {KEY_F10, "F10", 0}, {KEY_F11, "F11", 0}, {KEY_F12, "F12", 0},
    {KEY_HOME, "Home", 0}, {KEY_UP, "Up", 0}, {KEY_PAGEUP, "Prior", 0},
    {KEY_LEFT, "Left", 0}, {KEY_RIGHT, "Right", 0}, {KEY_END, "End", 0},
    {KEY_DOWN, "Down", 0}, {KEY_PAGEDOWN, "Next", 0},
    {KEY_INSERT, "Insert", 0}, {KEY_DELETE, "Delete", 0},
};
#define N_BASE (int)(sizeof base_keys / sizeof base_keys[0])

static const uint16_t letter_code[26] = {
    KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
    KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
    KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
};

/* Codes the pool never uses: modifiers and lock keys, and everything a
 * compositor, a hotkey daemon or the firmware might act on by keycode -
 * media, power, brightness, radio, print. What is left is function keys
 * past F12, the keypad, the Japanese and "office" keys: codes that exist
 * on no keyboard this machine has, or that nothing binds. */
static const uint16_t pool_deny[] = {
    KEY_RIGHTCTRL, KEY_RIGHTSHIFT, KEY_RIGHTALT, KEY_LEFTMETA,
    KEY_RIGHTMETA, KEY_COMPOSE, KEY_MENU, KEY_CAPSLOCK, KEY_NUMLOCK,
    KEY_SCROLLLOCK, KEY_SYSRQ, KEY_PRINT, KEY_PAUSE, KEY_HANGEUL,
    KEY_HANJA, KEY_SCALE, KEY_MUTE, KEY_VOLUMEDOWN, KEY_VOLUMEUP,
    KEY_POWER, KEY_SLEEP, KEY_WAKEUP, KEY_SUSPEND, KEY_BRIGHTNESSDOWN,
    KEY_BRIGHTNESSUP, KEY_BRIGHTNESS_CYCLE, KEY_BRIGHTNESS_AUTO,
    KEY_DISPLAY_OFF, KEY_SWITCHVIDEOMODE, KEY_KBDILLUMTOGGLE,
    KEY_KBDILLUMDOWN, KEY_KBDILLUMUP, KEY_PLAYPAUSE, KEY_PLAYCD,
    KEY_PAUSECD, KEY_PLAY, KEY_STOPCD, KEY_NEXTSONG, KEY_PREVIOUSSONG,
    KEY_REWIND, KEY_FASTFORWARD, KEY_RECORD, KEY_EJECTCD,
    KEY_EJECTCLOSECD, KEY_CLOSECD, KEY_CALC, KEY_WWW, KEY_MAIL, KEY_EMAIL,
    KEY_SEARCH, KEY_HOMEPAGE, KEY_BOOKMARKS, KEY_COMPUTER, KEY_FILE,
    KEY_CONFIG, KEY_DASHBOARD, KEY_CAMERA, KEY_SCREENLOCK,
    KEY_ROTATE_DISPLAY, KEY_CYCLEWINDOWS, KEY_BACK, KEY_FORWARD,
    KEY_REFRESH, KEY_BLUETOOTH, KEY_WLAN, KEY_UWB, KEY_WWAN, KEY_RFKILL,
    KEY_BATTERY, KEY_MEDIA, KEY_SOUND, KEY_PROG1, KEY_PROG2, KEY_PROG3,
    KEY_PROG4, KEY_PHONE, KEY_EXIT, KEY_CLOSE, KEY_STOP, KEY_HELP,
};

#define POOL_MAX 128
static uint16_t pool_code[POOL_MAX];
static uint32_t pool_cp[POOL_MAX];
static uint32_t pool_stamp[POOL_MAX];
static int      pool_n;
static uint32_t pool_clock;

/* An ASCII character -> the key and shift level that types it. */
static struct { uint16_t code; uint8_t shift; } ascii[128];

static gboolean code_is_base(uint16_t c)
{
    for (int i = 0; i < N_BASE; i++)
        if (base_keys[i].code == c)
            return TRUE;
    for (int i = 0; i < 26; i++)
        if (letter_code[i] == c)
            return TRUE;
    return FALSE;
}

static void keymap_tables(void)
{
    for (uint16_t c = 1; c < 248 && pool_n < POOL_MAX; c++) {
        gboolean denied = code_is_base(c);
        for (size_t i = 0; !denied && i < G_N_ELEMENTS(pool_deny); i++)
            denied = pool_deny[i] == c;
        if (!denied)
            pool_code[pool_n++] = c;
    }
    for (int i = 0; i < N_BASE; i++)
        for (int lvl = 0; lvl < 2; lvl++) {
            const char *name = lvl ? base_keys[i].shifted : base_keys[i].sym;
            if (!name)
                continue;
            uint32_t cp = xkb_keysym_to_utf32(
                xkb_keysym_from_name(name, XKB_KEYSYM_NO_FLAGS));
            if (cp > 0 && cp < 128 && !ascii[cp].code) {
                ascii[cp].code = base_keys[i].code;
                ascii[cp].shift = (uint8_t)lvl;
            }
        }
    for (int i = 0; i < 26; i++) {
        ascii['a' + i].code = ascii['A' + i].code = letter_code[i];
        ascii['A' + i].shift = 1;
    }
    ascii['\n'].code = KEY_ENTER;
    ascii['\t'].code = KEY_TAB;
}

static void vk_upload(void)
{
    GString *s = g_string_sized_new(12000);
    g_string_append(s, "xkb_keymap {\nxkb_keycodes \"lp-osk\" {\n"
                       " minimum = 8;\n maximum = 255;\n");
    for (int i = 0; i < N_BASE; i++)
        g_string_append_printf(s, " <K%d> = %d;\n", base_keys[i].code + 8,
                               base_keys[i].code + 8);
    for (int i = 0; i < 26; i++)
        g_string_append_printf(s, " <K%d> = %d;\n", letter_code[i] + 8,
                               letter_code[i] + 8);
    for (int i = 0; i < pool_n; i++)
        if (pool_cp[i])
            g_string_append_printf(s, " <K%d> = %d;\n", pool_code[i] + 8,
                                   pool_code[i] + 8);
    /* The compositor resolves these includes against its own XKB data
     * before it hands the keymap on, so clients get it whole. */
    g_string_append(s, "};\nxkb_types \"lp-osk\" { include \"complete\" };\n"
                       "xkb_compatibility \"lp-osk\" { include \"complete\" };\n"
                       "xkb_symbols \"lp-osk\" {\n");
    for (int i = 0; i < N_BASE; i++) {
        if (base_keys[i].shifted)
            g_string_append_printf(s, " key <K%d> { [ %s, %s ] };\n",
                                   base_keys[i].code + 8, base_keys[i].sym,
                                   base_keys[i].shifted);
        else
            g_string_append_printf(s, " key <K%d> { [ %s ] };\n",
                                   base_keys[i].code + 8, base_keys[i].sym);
    }
    for (int i = 0; i < 26; i++)
        g_string_append_printf(s, " key <K%d> { [ %c, %c ] };\n",
                               letter_code[i] + 8, 'a' + i, 'A' + i);
    g_string_append_printf(s,
        " modifier_map Shift { <K%d> };\n modifier_map Control { <K%d> };\n"
        " modifier_map Mod1 { <K%d> };\n",
        KEY_LEFTSHIFT + 8, KEY_LEFTCTRL + 8, KEY_LEFTALT + 8);
    for (int i = 0; i < pool_n; i++) {
        if (!pool_cp[i])
            continue;
        /* Latin-1 keysyms are their code points. Above that, always the
         * direct Unicode keysym and never a legacy one: the legacy Korean
         * keysyms (Hangul_Kiyeog, 0xea1...) are what IME frameworks watch
         * for, and some map them to the final-consonant jamo instead. */
        char name[64];
        xkb_keysym_get_name(pool_cp[i] < 0x100 ? pool_cp[i]
                                               : 0x01000000u | pool_cp[i],
                            name, sizeof name);
        g_string_append_printf(s, " key <K%d> { [ %s ] };\n",
                               pool_code[i] + 8, name);
    }
    g_string_append(s, "};\n};\n");

    /* The size handed over includes the terminating NUL: the compositor
     * maps the fd and parses it as a C string. */
    int fd = memfd_create("lp-osk-keymap", MFD_CLOEXEC);
    if (fd >= 0) {
        size_t len = s->len + 1, off = 0;
        while (off < len) {
            ssize_t w = write(fd, s->str + off, len - off);
            if (w <= 0)
                break;
            off += (size_t)w;
        }
        if (off == len)
            zwp_virtual_keyboard_v1_keymap(vk_osk,
                WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)len);
        close(fd);
    }
    g_string_free(s, TRUE);
}

static gboolean vk_ready(void)
{
    if (!vkm || !seat)
        return FALSE;
    if (!vk_osk) {
        vk_osk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vkm, seat);
        vk_upload();
    }
    return TRUE;
}

/* Called when the keyboard is shown. A compositor that reloads its config
 * may put its own keymap back on every keyboard it has, ours included, and
 * says nothing to us; from then on a pool key would type whatever the US
 * layout has on that code. Uploading again on show costs one keymap and
 * puts it right before anybody types. */
void type_refresh_keymap(void)
{
    if (vk_osk) {
        vk_upload();
        flush();
    }
}

static uint32_t xmods(uint32_t mods)
{
    /* Our keymap's modifier_map makes these the real modifier bits. */
    return (mods & MOD_SHIFT ? 1u : 0) | (mods & MOD_CTRL ? 4u : 0) |
           (mods & MOD_ALT ? 8u : 0);
}

static void vk_press(struct zwp_virtual_keyboard_v1 *vk, uint32_t code,
                     uint32_t mask)
{
    uint32_t t = now_ms();
    if (mask)
        zwp_virtual_keyboard_v1_modifiers(vk, mask, 0, 0, 0);
    zwp_virtual_keyboard_v1_key(vk, t, code, WL_KEYBOARD_KEY_STATE_PRESSED);
    zwp_virtual_keyboard_v1_key(vk, t, code, WL_KEYBOARD_KEY_STATE_RELEASED);
    if (mask)
        zwp_virtual_keyboard_v1_modifiers(vk, 0, 0, 0, 0);
}

/* Type text with vk_osk, re-pointing pool keys as needed. Done in chunks
 * of at most half the pool so one chunk never evicts its own characters. */
static void vk_type(const char *utf8)
{
    if (!vk_ready() || !utf8 || !*utf8)
        return;
    glong n = 0;
    gunichar *cps = g_utf8_to_ucs4_fast(utf8, -1, &n);
    for (glong start = 0; start < n; start += POOL_MAX / 2) {
        glong end = MIN(n, start + POOL_MAX / 2);
        int slot[POOL_MAX / 2];
        gboolean dirty = FALSE;
        pool_clock++;
        for (glong i = start; i < end; i++) {
            uint32_t c = cps[i];
            slot[i - start] = -1;
            if (c < 128 && ascii[c].code)
                continue;
            for (int k = 0; k < pool_n; k++)
                if (pool_cp[k] == c) {
                    slot[i - start] = k;
                    pool_stamp[k] = pool_clock;
                    break;
                }
        }
        for (glong i = start; i < end; i++) {
            uint32_t c = cps[i];
            if (slot[i - start] >= 0 || (c < 128 && ascii[c].code))
                continue;
            int best = -1;
            for (int k = 0; k < pool_n; k++) {
                if (pool_cp[k] == c) { best = k; break; }  /* earlier in chunk */
                if (pool_stamp[k] == pool_clock)
                    continue;
                if (best < 0 || pool_stamp[k] < pool_stamp[best])
                    best = k;
            }
            if (best < 0)
                continue;
            if (pool_cp[best] != c) {
                pool_cp[best] = c;
                dirty = TRUE;
            }
            pool_stamp[best] = pool_clock;
            slot[i - start] = best;
        }
        if (dirty)
            vk_upload();
        for (glong i = start; i < end; i++) {
            uint32_t c = cps[i];
            if (c < 128 && ascii[c].code)
                vk_press(vk_osk, ascii[c].code, ascii[c].shift ? 1u : 0);
            else if (slot[i - start] >= 0)
                vk_press(vk_osk, pool_code[slot[i - start]], 0);
        }
    }
    g_free(cps);
    flush();
}

/* ── composition, whichever way it goes out ─────────────────────── */

/* Without an input method the syllable being composed is real text in
 * the application; this is what we typed of it. */
static char shown[8];

static void vk_compose(const char *commit, const char *pre)
{
    char *want = g_strconcat(commit, pre, NULL);
    const char *a = shown, *b = want;
    while (*a && *b) {
        const char *na = g_utf8_next_char(a), *nb = g_utf8_next_char(b);
        if (na - a != nb - b || memcmp(a, b, (size_t)(na - a)) != 0)
            break;
        a = na;
        b = nb;
    }
    if (vk_ready()) {
        for (glong i = g_utf8_strlen(a, -1); i > 0; i--)
            vk_press(vk_osk, KEY_BACKSPACE, 0);
        vk_type(b);
        flush();
    }
    g_strlcpy(shown, pre, sizeof shown);
    g_free(want);
}

static void im_send(const char *commit, const char *pre)
{
    if (commit && *commit)
        zwp_input_method_v2_commit_string(im, commit);
    int n = (int)strlen(pre);
    zwp_input_method_v2_set_preedit_string(im, pre, n, n);
    zwp_input_method_v2_commit(im, ims.serial);
    ims.preedit_sent = n > 0;
    flush();
}

static gboolean im_route(void)
{
    return im && ims.active;
}

static void composition_changed(const char *commit)
{
    char pre[8] = "";
    uint32_t p = hangul_preedit(&hic);
    if (p)
        pre[g_unichar_to_utf8(p, pre)] = 0;
    if (im_route())
        im_send(commit, pre);
    else
        vk_compose(commit, pre);
}

/* Forget a composition without committing it: the field it belonged to is
 * gone, or the application changed the text under it. */
static void composition_drop(void)
{
    hangul_reset(&hic);
    shown[0] = 0;
    ims.preedit_sent = FALSE;
}

void type_flush(void)
{
    uint32_t c = hangul_flush(&hic);
    if (im_route() && (c || ims.preedit_sent)) {
        char s[8] = "";
        if (c)
            s[g_unichar_to_utf8(c, s)] = 0;
        im_send(s, "");
    }
    shown[0] = 0;   /* without an input method it is already typed */
}

gboolean type_composing(void)
{
    return hangul_preedit(&hic) != 0;
}

void type_jamo(uint32_t jamo)
{
    if (!hangul_is_jamo(jamo)) {
        char s[8];
        s[g_unichar_to_utf8(jamo, s)] = 0;
        type_text(s);
        return;
    }
    HangulOut o = {{0}, 0};
    hangul_feed(&hic, jamo, &o);
    char commit[32];
    int k = 0;
    for (int i = 0; i < o.n; i++)
        k += g_unichar_to_utf8(o.text[i], commit + k);
    commit[k] = 0;
    composition_changed(commit);
}

void type_backspace(uint32_t mods)
{
    if (!(mods & (MOD_CTRL | MOD_ALT)) && hangul_backspace(&hic)) {
        composition_changed("");
        return;
    }
    type_flush();
    if (vk_ready()) {
        vk_press(vk_osk, KEY_BACKSPACE, xmods(mods & (MOD_CTRL | MOD_ALT)));
        flush();
    }
}

void type_text(const char *utf8)
{
    type_flush();
    if (im_route())
        im_send(utf8, "");
    else
        vk_type(utf8);
}

void type_key(uint32_t code, uint32_t mods)
{
    type_flush();
    if (vk_ready()) {
        vk_press(vk_osk, code, xmods(mods));
        flush();
    }
}

/* The on-screen keys for ASCII come here rather than to type_text, so
 * that even with an input method active they arrive as the key events a
 * laptop keyboard would send: a web page's keydown handler, a game, a
 * terminal program's key bindings all see the same thing either way. */
gboolean type_ascii(char c, uint32_t mods)
{
    unsigned char u = (unsigned char)c;
    if (u >= 128 || !ascii[u].code)
        return FALSE;
    type_key(ascii[u].code, mods | (ascii[u].shift ? MOD_SHIFT : 0));
    return TRUE;
}

/* ── language ───────────────────────────────────────────────────── */

/* 2-beolsik on QWERTY positions, a..z, plain and shifted. */
static const char *const KO_PLAIN =
    "ㅁㅠㅊㅇㄷㄹㅎㅗㅑㅓㅏㅣㅡㅜㅐㅔㅂㄱㄴㅅㅕㅍㅈㅌㅛㅋ";
static const char *const KO_SHIFT =
    "ㅁㅠㅊㅇㄸㄹㅎㅗㅑㅓㅏㅣㅡㅜㅒㅖㅃㄲㄴㅆㅕㅍㅉㅌㅛㅋ";

uint32_t type_ko_jamo(char letter, gboolean shift)
{
    if (letter >= 'A' && letter <= 'Z') {
        letter = (char)(letter - 'A' + 'a');
        shift = TRUE;
    }
    if (letter < 'a' || letter > 'z')
        return 0;
    const char *p = shift ? KO_SHIFT : KO_PLAIN;
    for (int i = 0; i < letter - 'a'; i++)
        p = g_utf8_next_char(p);
    return g_utf8_get_char(p);
}

int type_lang(void)
{
    if (ims.active && (ims.purpose == PURPOSE_PASSWORD ||
                       ims.purpose == PURPOSE_PIN))
        return LANG_EN;
    return cfg.lang;
}

uint32_t type_purpose(void)
{
    return ims.active ? ims.purpose : 0;
}

void type_set_lang(int lang)
{
    type_flush();
    if (lang == LANG_KO && !(cfg.layouts & LAYOUT_KO))
        lang = LANG_EN;
    if (cfg.lang == lang)
        return;
    cfg.lang = lang;
    osk_config_save();     /* the last language used comes back at login */
    ui_relabel();
    osk_state_changed();
}

void type_toggle_lang(void)
{
    type_set_lang(cfg.lang == LANG_KO ? LANG_EN : LANG_KO);
}

const char *type_im_status(void)
{
    if (!imm)
        return "unsupported";
    if (ims.unavailable)
        return "taken";   /* another input method holds the seat */
    return ims.active ? (grab ? "active+grab" : "active") : "inactive";
}

/* ── the laptop keyboard, while a field is focused ──────────────── */

static struct xkb_keymap *gkm;
static struct xkb_state  *gst;
static uint32_t g_shift, g_ctrl, g_alt, g_logo;   /* masks in gkm */
static uint32_t g_dep;
static gboolean pass_keymap;
static uint8_t  consumed[32], pass_down[32];      /* bitsets, codes < 256 */
static int      rep_rate = 25, rep_delay = 600;
static guint    rep_timer;
static uint32_t rep_key, rep_jamo;
static gboolean ralt_pending, ralt_sent;
static uint32_t ralt_time;

#define BIT_SET(a, k)   ((k) < 256 ? ((a)[(k) >> 3] |= (uint8_t)(1u << ((k) & 7))) : 0)
#define BIT_CLR(a, k)   ((k) < 256 ? ((a)[(k) >> 3] &= (uint8_t)~(1u << ((k) & 7))) : 0)
#define BIT_GET(a, k)   ((k) < 256 && ((a)[(k) >> 3] >> ((k) & 7) & 1))

static void pass_key(uint32_t time, uint32_t key, uint32_t state)
{
    if (!vk_pass || !pass_keymap)
        return;
    zwp_virtual_keyboard_v1_key(vk_pass, time, key, state);
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
        BIT_SET(pass_down, key);
    else
        BIT_CLR(pass_down, key);
}

static void rep_stop(void)
{
    if (rep_timer)
        g_source_remove(rep_timer);
    rep_timer = 0;
    rep_key = 0;
}

static gboolean rep_tick(gpointer data)
{
    (void)data;
    if (rep_jamo) {
        type_jamo(rep_jamo);
    } else if (hangul_preedit(&hic)) {
        type_backspace(0);
    } else {
        /* composition emptied under a held BackSpace: keep deleting text */
        uint32_t t = now_ms();
        pass_key(t, KEY_BACKSPACE, WL_KEYBOARD_KEY_STATE_PRESSED);
        pass_key(t, KEY_BACKSPACE, WL_KEYBOARD_KEY_STATE_RELEASED);
        flush();
    }
    return G_SOURCE_CONTINUE;
}

static gboolean rep_begin(gpointer data)
{
    (void)data;
    rep_tick(NULL);
    rep_timer = g_timeout_add((guint)(1000 / rep_rate), rep_tick, NULL);
    return G_SOURCE_REMOVE;
}

/* Keys we consume never reach the application, so its own key repeat
 * cannot run for them: holding ㅋ has to give ㅋㅋㅋ here. */
static void rep_start(uint32_t key, uint32_t jamo)
{
    rep_stop();
    if (rep_rate <= 0)
        return;
    rep_key = key;
    rep_jamo = jamo;
    rep_timer = g_timeout_add((guint)rep_delay, rep_begin, NULL);
}

static void grab_keymap(void *data, struct zwp_input_method_keyboard_grab_v2 *g,
                        uint32_t format, int32_t fd, uint32_t size)
{
    (void)data; (void)g;
    if (!vk_pass && vkm)
        vk_pass = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vkm, seat);
    if (vk_pass) {
        /* The very bytes the compositor gave us; libwayland dups the fd. */
        zwp_virtual_keyboard_v1_keymap(vk_pass, format, fd, size);
        pass_keymap = TRUE;
    }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map != MAP_FAILED) {
        struct xkb_keymap *km = xkb_keymap_new_from_buffer(xkb, map,
            strnlen(map, size), XKB_KEYMAP_FORMAT_TEXT_V1,
            XKB_KEYMAP_COMPILE_NO_FLAGS);
        munmap(map, size);
        if (km) {
            if (gst) xkb_state_unref(gst);
            if (gkm) xkb_keymap_unref(gkm);
            gkm = km;
            gst = xkb_state_new(km);
            xkb_mod_index_t i;
            g_shift = (i = xkb_keymap_mod_get_index(km, XKB_MOD_NAME_SHIFT)) != XKB_MOD_INVALID ? 1u << i : 0;
            g_ctrl  = (i = xkb_keymap_mod_get_index(km, XKB_MOD_NAME_CTRL))  != XKB_MOD_INVALID ? 1u << i : 0;
            g_alt   = (i = xkb_keymap_mod_get_index(km, XKB_MOD_NAME_ALT))   != XKB_MOD_INVALID ? 1u << i : 0;
            g_logo  = (i = xkb_keymap_mod_get_index(km, XKB_MOD_NAME_LOGO))  != XKB_MOD_INVALID ? 1u << i : 0;
        }
    }
    close(fd);
    flush();
}

static void grab_modifiers(void *data, struct zwp_input_method_keyboard_grab_v2 *g,
                           uint32_t serial, uint32_t dep, uint32_t lat,
                           uint32_t lock, uint32_t group)
{
    (void)data; (void)g; (void)serial;
    g_dep = dep | lat;
    if (gst)
        xkb_state_update_mask(gst, dep, lat, lock, 0, 0, group);
    if (vk_pass && pass_keymap) {
        zwp_virtual_keyboard_v1_modifiers(vk_pass, dep, lat, lock, group);
        flush();
    }
}

static void grab_repeat(void *data, struct zwp_input_method_keyboard_grab_v2 *g,
                        int32_t rate, int32_t delay)
{
    (void)data; (void)g;
    rep_rate = rate;
    rep_delay = delay > 0 ? delay : 600;
}

/* The keysym a key gives with no modifiers, in the layout now in use -
 * the physical position's meaning, which is what 2-beolsik is laid on. */
static xkb_keysym_t base_sym(uint32_t key)
{
    if (!gkm)
        return XKB_KEY_NoSymbol;
    const xkb_keysym_t *syms;
    xkb_layout_index_t layout = gst ? xkb_state_key_get_layout(gst, key + 8) : 0;
    if (layout == XKB_LAYOUT_INVALID)
        layout = 0;
    int n = xkb_keymap_key_get_syms_by_level(gkm, key + 8, layout, 0, &syms);
    return n > 0 ? syms[0] : XKB_KEY_NoSymbol;
}

static void consume(uint32_t key)
{
    BIT_SET(consumed, key);
}

static void grab_key(void *data, struct zwp_input_method_keyboard_grab_v2 *g,
                     uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    (void)data; (void)g; (void)serial;

    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (key == rep_key)
            rep_stop();
        if (key == KEY_RIGHTALT && ralt_pending) {
            /* Right Alt pressed and let go on its own: 한/영. A long hold
             * is somebody changing their mind, not a toggle. */
            ralt_pending = FALSE;
            if (time - ralt_time < 1000)
                type_toggle_lang();
            return;
        }
        if (key == KEY_RIGHTALT && ralt_sent) {
            ralt_sent = FALSE;
            pass_key(time, key, state);
            flush();
            return;
        }
        if (BIT_GET(consumed, key)) {
            BIT_CLR(consumed, key);
            return;
        }
        pass_key(time, key, state);
        flush();
        return;
    }

    xkb_keysym_t sym = base_sym(key);
    gboolean shift = (g_dep & g_shift) != 0;
    gboolean command = (g_dep & (g_ctrl | g_alt | g_logo)) != 0;

    if (ralt_pending && key != KEY_RIGHTALT) {
        /* Right Alt is being used as Alt after all: let the application
         * see it go down before the key it modifies. */
        ralt_pending = FALSE;
        ralt_sent = TRUE;
        pass_key(time, KEY_RIGHTALT, WL_KEYBOARD_KEY_STATE_PRESSED);
    }

    /* 한/영: the Hangul key, or Right Alt tapped on its own (below) on a
     * keyboard like this laptop's, which has no Hangul key. Shift+Space,
     * which some Korean IMEs also take, is deliberately not one: English
     * is the default here, and a capital typed a moment before a space
     * would switch the language under an English typist's fingers. */
    if (key == KEY_HANGEUL || sym == XKB_KEY_Hangul) {
        consume(key);
        type_toggle_lang();
        return;
    }
    if (key == KEY_RIGHTALT && !(g_dep & (g_ctrl | g_logo))) {
        ralt_pending = TRUE;
        ralt_time = time;
        return;
    }
    if (!command && type_lang() == LANG_KO) {
        uint32_t j = 0;
        if (sym >= XKB_KEY_a && sym <= XKB_KEY_z)
            j = type_ko_jamo((char)('a' + (sym - XKB_KEY_a)), shift);
        else if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z)
            j = type_ko_jamo((char)('a' + (sym - XKB_KEY_A)), TRUE);
        if (j) {
            consume(key);
            type_jamo(j);
            rep_start(key, j);
            return;
        }
        if (sym == XKB_KEY_BackSpace && type_composing()) {
            consume(key);
            type_backspace(0);
            rep_start(key, 0);
            return;
        }
    }
    /* Everything else is the application's, exactly as it was pressed -
     * after whatever was being composed has been handed over, so the
     * text lands before the key that ended it. */
    type_flush();
    pass_key(time, key, state);
    flush();
}

static const struct zwp_input_method_keyboard_grab_v2_listener grab_listener = {
    .keymap = grab_keymap,
    .key = grab_key,
    .modifiers = grab_modifiers,
    .repeat_info = grab_repeat,
};

static void grab_start(void)
{
    if (grab || !im || !vkm || !cfg.ime)
        return;
    grab = zwp_input_method_v2_grab_keyboard(im);
    zwp_input_method_keyboard_grab_v2_add_listener(grab, &grab_listener, NULL);
    flush();
}

static void grab_stop(void)
{
    rep_stop();
    if (grab) {
        zwp_input_method_keyboard_grab_v2_release(grab);
        grab = NULL;
    }
    if (vk_pass && pass_keymap) {
        uint32_t t = now_ms();
        for (uint32_t k = 0; k < 256; k++)
            if (BIT_GET(pass_down, k))
                pass_key(t, k, WL_KEYBOARD_KEY_STATE_RELEASED);
        zwp_virtual_keyboard_v1_modifiers(vk_pass, 0, 0, 0, 0);
    }
    memset(consumed, 0, sizeof consumed);
    memset(pass_down, 0, sizeof pass_down);
    ralt_pending = ralt_sent = FALSE;
    g_dep = 0;
    flush();
}

/* ── the input method ───────────────────────────────────────────── */

static void im_activate(void *d, struct zwp_input_method_v2 *m)
{
    (void)d; (void)m;
    ims.pending = 1;
    ims.pending_purpose = 0;
}

static void im_deactivate(void *d, struct zwp_input_method_v2 *m)
{
    (void)d; (void)m;
    ims.pending = 0;
}

static void im_surrounding(void *d, struct zwp_input_method_v2 *m,
                           const char *text, uint32_t cursor, uint32_t anchor)
{
    (void)d; (void)m; (void)anchor;
    g_free(ims.pending_surr);
    ims.pending_surr = g_strdup(text ? text : "");
    ims.pending_cursor = cursor;
}

static void im_cause(void *d, struct zwp_input_method_v2 *m, uint32_t cause)
{
    (void)d; (void)m;
    ims.cause = cause;
}

static void im_content(void *d, struct zwp_input_method_v2 *m,
                       uint32_t hint, uint32_t purpose)
{
    (void)d; (void)m; (void)hint;
    ims.pending_purpose = purpose;
}

static void im_done(void *d, struct zwp_input_method_v2 *m)
{
    (void)d; (void)m;
    ims.serial++;
    int pend = ims.pending;
    ims.pending = -1;
    /* Surrounding text is double-buffered like everything else: it only
     * counts once done says so. */
    ims.surr_changed = FALSE;
    if (ims.pending_surr) {
        ims.surr_changed = g_strcmp0(ims.surr, ims.pending_surr) != 0 ||
                           ims.cursor != ims.pending_cursor;
        g_free(ims.surr);
        ims.surr = ims.pending_surr;
        ims.cursor = ims.pending_cursor;
        ims.pending_surr = NULL;
    }
    g_debug("im done: serial %u pending %d active %d cause %u purpose %u "
            "composing %d surrounding \"%s\" cursor %u%s",
            ims.serial, pend, ims.active, ims.cause, ims.pending_purpose,
            type_composing(), ims.surr ? ims.surr : "", ims.cursor,
            ims.surr_changed ? " (changed)" : "");

    if (pend == 1) {
        gboolean fresh = !ims.active;
        if (fresh) {
            type_flush();         /* a fallback composition is already text */
            composition_drop();
            ims.active = TRUE;
        }
        uint32_t old = ims.purpose;
        ims.purpose = ims.pending_purpose;
        grab_start();
        if (fresh || old != ims.purpose)
            ui_field_purpose(ims.purpose);
        /* The same field back within a moment, text and cursor exactly as
         * they were: a focus bounce, not a person moving on. Seen when a
         * keyboard device comes or goes (a USB keyboard unplugged, a
         * virtual one closed) - the compositor deactivates and activates
         * the field in one breath and the half-typed syllable would
         * otherwise be lost. Put it back and carry on. */
        if (fresh && bounce.valid &&
            g_get_monotonic_time() - bounce.at < 300000 &&
            !g_strcmp0(bounce.surr, ims.surr) && bounce.cursor == ims.cursor) {
            hic = bounce.ic;
            composition_changed("");
            g_debug("im: focus bounce, composition restored");
        }
        bounce.valid = FALSE;
        osk_im_activated(fresh);
        osk_state_changed();
    } else if (pend == 0) {
        bounce.valid = type_composing() && ims.surr;
        if (bounce.valid) {
            bounce.ic = hic;
            g_free(bounce.surr);
            bounce.surr = g_strdup(ims.surr);
            bounce.cursor = ims.cursor;
            bounce.at = g_get_monotonic_time();
        }
        grab_stop();
        ims.active = FALSE;
        ims.purpose = 0;
        composition_drop();
        ui_field_purpose(0);
        osk_im_deactivated();
        osk_state_changed();
    } else if (ims.active && ims.cause == CAUSE_OTHER && ims.surr_changed &&
               type_composing()) {
        /* The application changed its text without us - a tap moved the
         * cursor, a shortcut cut a word. Whatever it did with our preedit,
         * the syllable we were building no longer sits where we think it
         * does; continuing it would put half a syllable somewhere else.
         * (GTK also sends "other" with nothing changed, after a refocus;
         * that is no reason to throw the syllable away.) */
        composition_drop();
    }
    ims.cause = 0;
}

static void im_unavailable(void *d, struct zwp_input_method_v2 *m)
{
    (void)d;
    g_message("another input method holds the seat; no auto-show, and the "
              "laptop keyboard stays with it");
    ims.unavailable = TRUE;
    ims.active = FALSE;
    zwp_input_method_v2_destroy(m);
    im = NULL;
    osk_state_changed();
}

static const struct zwp_input_method_v2_listener im_listener = {
    .activate = im_activate,
    .deactivate = im_deactivate,
    .surrounding_text = im_surrounding,
    .text_change_cause = im_cause,
    .content_type = im_content,
    .done = im_done,
    .unavailable = im_unavailable,
};

/* ── setup ──────────────────────────────────────────────────────── */

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t version)
{
    (void)d; (void)version;
    if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name))
        vkm = wl_registry_bind(r, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    else if (!strcmp(iface, zwp_input_method_manager_v2_interface.name))
        imm = wl_registry_bind(r, name, &zwp_input_method_manager_v2_interface, 1);
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{
    (void)d; (void)r; (void)name;
}

static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

gboolean type_init(void)
{
    GdkDisplay *gd = gdk_display_get_default();
    if (!gd || !GDK_IS_WAYLAND_DISPLAY(gd))
        return FALSE;
    dpy = gdk_wayland_display_get_wl_display(gd);
    seat = gdk_wayland_seat_get_wl_seat(gdk_display_get_default_seat(gd));
    xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    keymap_tables();
    hangul_reset(&hic);

    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(dpy);

    if (!vkm)
        g_warning("the compositor has no zwp_virtual_keyboard_manager_v1: "
                  "the keys cannot type anything");
    /* The virtual keyboard exists from the start, not from the first key,
     * and the compositor has seen it before any text field can activate
     * us. sway 1.7 answers a keyboard grab by reading the modifiers of the
     * seat's current keyboard without checking there is one, and a seat
     * with no keyboard at all (a tablet, the headless test session) dies
     * there - taking the whole desktop with it. With ours on the seat
     * there always is one. It also takes the keymap upload off the first
     * keystroke. */
    if (vk_ready())
        wl_display_roundtrip(dpy);
    if (imm && seat) {
        im = zwp_input_method_manager_v2_get_input_method(imm, seat);
        zwp_input_method_v2_add_listener(im, &im_listener, NULL);
    } else {
        g_message("no zwp_input_method_manager_v2: the keyboard will not "
                  "show itself on text focus; the button and gesture still work");
    }
    flush();
    return vkm != NULL;
}

void type_shutdown(void)
{
    grab_stop();
    if (im) {
        zwp_input_method_v2_destroy(im);
        im = NULL;
    }
    if (vk_osk) zwp_virtual_keyboard_v1_destroy(vk_osk);
    if (vk_pass) zwp_virtual_keyboard_v1_destroy(vk_pass);
    vk_osk = vk_pass = NULL;
    flush();
}
