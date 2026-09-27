/* screen.c - lp-recovery's framebuffer, VT and input.
 *
 * ── The picture ──
 *
 * Drawing happens in three canvases the size of the screen (lp-ui.h):
 * the gradient and mark (cv_bg, made once), the screen's content on top
 * of it (cv_base, remade when the content changes) and the frame that
 * goes out (cv_frame: base plus the moving focus highlight). Only the
 * rectangle that changed is sent to the screen, a row at a time with
 * write(): on simpledrm and the DRM drivers' fbdev emulation - which is
 * what i915 gives us on the XPS - a write() reaches the panel at once,
 * while stores through mmap() wait for deferred I/O (the boot splash
 * found this first; userland/splash/splash.c).
 *
 * The VT is put in KD_GRAPHICS while the menu is up, so the kernel's
 * console does not draw text or a cursor over it, and back in KD_TEXT for
 * the recovery shell, which is a text console.
 *
 * ── Input ──
 *
 * evdev, not the tty: the menu needs touch, and a key's press and
 * release, neither of which a tty carries. Every /dev/input/event* is
 * classified by what it reports - keys, absolute position (a touch screen
 * or a tablet), relative motion (a mouse). Keyboards are GRABBED while
 * the menu owns the screen. Without that the kernel's keyboard handler
 * also feeds every key to tty1, and the password typed into the menu
 * would be sitting in tty1's input queue when the shell starts - and the
 * shell would run it as a command, echoing it on the screen. The grab is
 * released only for the shell, and the queue is flushed before it starts.
 *
 * Devices are looked for again every two seconds, so a USB keyboard
 * plugged in after the menu came up works.
 */
#include "recovery.h"

int SW, SH;
lpui_canvas_t cv_bg, cv_base, cv_frame;
u8 *logo_scratch;

/* ── The framebuffer ──────────────────────────────────────────────── */
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602
#define KDSETMODE           0x4B3A
#define KD_TEXT             0
#define KD_GRAPHICS         1
#define TCFLSH              0x540B

static int fb_fd = -1;
static u32 fb_bpp, fb_line, fb_xoff, fb_yoff;
static u32 fb_off[3], fb_len[3];
static u8 *rowbuf;

bool scr_open(void)
{
    long fd = lp_open("/dev/fb0", O_RDWR | O_CLOEXEC, 0);
    if (fd < 0)
        return false;
    u8 var[160], fix[80];
    memset(var, 0, sizeof var);
    memset(fix, 0, sizeof fix);
    if (lp_ioctl((int)fd, FBIOGET_VSCREENINFO, var) < 0 ||
        lp_ioctl((int)fd, FBIOGET_FSCREENINFO, fix) < 0) {
        lp_close((int)fd);
        return false;
    }
#define U32AT(b, o) (*(u32 *)((b) + (o)))
    SW = (int)U32AT(var, 0);
    SH = (int)U32AT(var, 4);
    fb_xoff = U32AT(var, 16);
    fb_yoff = U32AT(var, 20);
    fb_bpp = U32AT(var, 24);
    for (int c = 0; c < 3; c++) {
        fb_off[c] = U32AT(var, 32 + 12 * c);
        fb_len[c] = U32AT(var, 36 + 12 * c);
    }
    fb_line = U32AT(fix, 48);
    if ((fb_bpp != 32 && fb_bpp != 16 && fb_bpp != 24) || SW < 320 || SH < 200) {
        lp_close((int)fd);
        return false;
    }
    fb_fd = (int)fd;
    size_t px = (size_t)SW * (size_t)SH;
    cv_bg = (lpui_canvas_t){ malloc(px * 4), SW, SH, SW };
    cv_base = (lpui_canvas_t){ malloc(px * 4), SW, SH, SW };
    cv_frame = (lpui_canvas_t){ malloc(px * 4), SW, SH, SW };
    logo_scratch = malloc(LPUI_LOGO_SCRATCH);
    rowbuf = malloc((size_t)SW * 4);
    if (!cv_bg.px || !cv_base.px || !cv_frame.px || !logo_scratch || !rowbuf)
        return false;
    rlog("screen %dx%d, %u bpp", SW, SH, fb_bpp);
    return true;
}

static u32 chan(u32 v8, u32 len)
{
    return len >= 8 ? v8 << (len - 8) : v8 >> (8 - len);
}

void scr_present(const lpui_canvas_t *c, int x, int y, int w, int h)
{
    if (fb_fd < 0)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SW) w = SW - x;
    if (y + h > SH) h = SH - y;
    if (w <= 0 || h <= 0)
        return;
    u32 bytes = fb_bpp / 8;
    bool native = fb_bpp == 32 && fb_off[0] == 16 && fb_off[1] == 8 && fb_off[2] == 0;
    for (int j = 0; j < h; j++) {
        const u32 *src = c->px + (u64)(y + j) * c->stride + x;
        const u8 *out;
        if (native)
            out = (const u8 *)src;
        else {
            for (int i = 0; i < w; i++) {
                u32 p = src[i];
                u32 v = chan((p >> 16) & 255, fb_len[0]) << fb_off[0] |
                        chan((p >> 8) & 255, fb_len[1]) << fb_off[1] |
                        chan(p & 255, fb_len[2]) << fb_off[2];
                memcpy(rowbuf + (size_t)i * bytes, &v, bytes);
            }
            out = rowbuf;
        }
        s64 off = (s64)(y + j + (int)fb_yoff) * fb_line + (s64)(x + (int)fb_xoff) * bytes;
        lp_lseek(fb_fd, off, 0);
        lp_write(fb_fd, out, (size_t)w * bytes);
    }
}

/* ── Input devices ────────────────────────────────────────────────── */
#define MAXDEV 24
enum { D_KBD = 1, D_ABS = 2, D_MT = 4, D_REL = 8 };
typedef struct {
    int  fd, num, kind;
    int  minx, maxx, miny, maxy;
    int  x, y;                  /* last absolute position, device units */
    bool down, was_down, moved;
    int  slot;                  /* current multitouch slot */
} dev_t_;
static dev_t_ devs[MAXDEV];
static int ndev;
static bool grabbed;
static s64 last_scan;
static bool shift_l, shift_r, caps;
static int mouse_x = -1, mouse_y = -1;

#define EVIOCGBIT(ev, len) _LP_IOC(2u, 'E', 0x20 + (ev), (len))
#define EVIOCGABS(abs)     _LP_IOC(2u, 'E', 0x40 + (abs), 24)
#define EVIOCGPROP(len)    _LP_IOC(2u, 'E', 0x09, (len))
#define EVIOCGRAB          _LP_IOC(1u, 'E', 0x90, sizeof(int))
#define TEST(bits, n)      ((bits)[(n) / 8] & (1u << ((n) % 8)))

static bool dev_open(int num)
{
    for (int i = 0; i < ndev; i++)
        if (devs[i].num == num)
            return true;
    if (ndev >= MAXDEV)
        return false;
    char path[40];
    snprintf(path, sizeof path, "/dev/input/event%d", num);
    long fd = lp_open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC, 0);
    if (fd < 0)
        return false;
    u8 ev[4] = { 0 }, keys[96] = { 0 }, abs[8] = { 0 }, rel[4] = { 0 };
    lp_ioctl((int)fd, EVIOCGBIT(0, sizeof ev), ev);
    lp_ioctl((int)fd, EVIOCGBIT(1, sizeof keys), keys);
    lp_ioctl((int)fd, EVIOCGBIT(3, sizeof abs), abs);
    lp_ioctl((int)fd, EVIOCGBIT(2, sizeof rel), rel);
    dev_t_ d;
    memset(&d, 0, sizeof d);
    d.fd = (int)fd;
    d.num = num;
    if (TEST(ev, 1) && TEST(keys, 30) && TEST(keys, 28))       /* KEY_A, KEY_ENTER */
        d.kind |= D_KBD;
    if (TEST(ev, 3) && TEST(abs, 0x35) && TEST(abs, 0x36))     /* ABS_MT_POSITION_X/Y */
        d.kind |= D_ABS | D_MT;
    else if (TEST(ev, 3) && TEST(abs, 0) && TEST(abs, 1))      /* ABS_X/Y */
        d.kind |= D_ABS;
    else if (TEST(ev, 2) && TEST(rel, 0) && TEST(rel, 1))      /* REL_X/Y */
        d.kind |= D_REL;
    if (!d.kind) {
        lp_close((int)fd);
        return false;
    }
    if (d.kind & D_ABS) {
        s32 info[6];
        int ax = (d.kind & D_MT) ? 0x35 : 0, ay = (d.kind & D_MT) ? 0x36 : 1;
        if (lp_ioctl((int)fd, EVIOCGABS(ax), info) == 0) { d.minx = info[1]; d.maxx = info[2]; }
        if (lp_ioctl((int)fd, EVIOCGABS(ay), info) == 0) { d.miny = info[1]; d.maxy = info[2]; }
        if (d.maxx <= d.minx) d.maxx = d.minx + 1;
        if (d.maxy <= d.miny) d.maxy = d.miny + 1;
    }
    if ((d.kind & D_KBD) && grabbed)
        lp_ioctl((int)fd, EVIOCGRAB, (void *)1);
    devs[ndev++] = d;
    rlog("input event%d:%s%s%s", num, (d.kind & D_KBD) ? " keyboard" : "",
         (d.kind & D_MT) ? " touch" : (d.kind & D_ABS) ? " tablet" : "",
         (d.kind & D_REL) ? " mouse" : "");
    return true;
}

void in_rescan(void)
{
    for (int n = 0; n < 32; n++)
        dev_open(n);
    last_scan = lp_monotonic_ms();
}

static void dev_drop(int i)
{
    lp_close(devs[i].fd);
    devs[i] = devs[--ndev];
}

void scr_graphics(bool on)
{
    lp_ioctl(STDIN_FILENO, KDSETMODE, (void *)(long)(on ? KD_GRAPHICS : KD_TEXT));
    grabbed = on;
    for (int i = 0; i < ndev; i++)
        if (devs[i].kind & D_KBD)
            lp_ioctl(devs[i].fd, EVIOCGRAB, (void *)(long)(on ? 1 : 0));
    /* Whatever reached the tty's queue before the grab (or while the
     * shell ran) must not be read by the next reader. */
    lp_ioctl(STDIN_FILENO, TCFLSH, (void *)0);
}

void in_drain(void)
{
    u8 buf[24 * 16];
    for (int i = 0; i < ndev; i++)
        while (lp_read(devs[i].fd, buf, sizeof buf) > 0)
            ;
}

/* US layout. The recovery menu types passwords and one confirmation
 * word; the owner's Korean input is a desktop feature (the OSK track's
 * input method) and passwords are ASCII. */
static const char KEYMAP[2][58] = {
    { 0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8, 9,
      'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 13, 0,
      'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
      'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ' },
    { 0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 8, 9,
      'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 13, 0,
      'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
      'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ' },
};

static u32 key_char(int code)
{
    if (code <= 0 || code >= 58)
        return 0;
    bool shift = shift_l || shift_r;
    char c = KEYMAP[0][code];
    bool letter = c >= 'a' && c <= 'z';
    int layer = (letter ? (shift != caps) : shift) ? 1 : 0;
    u8 ch = (u8)KEYMAP[layer][code];
    return ch >= 32 && ch < 127 ? ch : 0;
}

typedef struct { u16 type, code; s32 value; } ie_t;

/* One read of one device into zero or one UI event. */
static int dev_read(dev_t_ *d, uev_t *out)
{
    u8 raw[sizeof(long) * 2 + 8];
    for (;;) {
        long n = lp_read(d->fd, raw, sizeof raw);
        if (n < (long)sizeof raw)
            return n == -11 /* EAGAIN */ ? 0 : -1;
        ie_t e;
        memcpy(&e, raw + sizeof(long) * 2, sizeof e);
        if (e.type == 1 && (d->kind & D_KBD) && e.code < 0x100) {       /* EV_KEY */
            if (e.code == 42) { shift_l = e.value != 0; continue; }
            if (e.code == 54) { shift_r = e.value != 0; continue; }
            if (e.code == 58 && e.value == 1) { caps = !caps; continue; }
            if (e.value == 0)
                continue;
            out->kind = UEV_KEY;
            out->code = e.code;
            out->ch = key_char(e.code);
            out->press = true;
            return 1;
        }
        if (e.type == 1 && (e.code == 0x14a || e.code == 0x110)) {       /* BTN_TOUCH/LEFT */
            d->down = e.value != 0;
            d->moved = true;
            continue;
        }
        if (e.type == 3 && (d->kind & D_ABS)) {
            if (d->kind & D_MT) {
                if (e.code == 0x2f) d->slot = e.value;                  /* ABS_MT_SLOT */
                else if (d->slot == 0 && e.code == 0x35) { d->x = e.value; d->moved = true; }
                else if (d->slot == 0 && e.code == 0x36) { d->y = e.value; d->moved = true; }
                else if (d->slot == 0 && e.code == 0x39) { d->down = e.value >= 0; d->moved = true; }
            } else {
                if (e.code == 0) { d->x = e.value; d->moved = true; }
                else if (e.code == 1) { d->y = e.value; d->moved = true; }
            }
            continue;
        }
        if (e.type == 2 && (d->kind & D_REL)) {
            if (mouse_x < 0) { mouse_x = SW / 2; mouse_y = SH / 2; }
            if (e.code == 0) mouse_x += e.value * (lpui_px(4) > 1 ? lpui_px(4) : 1);
            if (e.code == 1) mouse_y += e.value * (lpui_px(4) > 1 ? lpui_px(4) : 1);
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_x >= SW) mouse_x = SW - 1;
            if (mouse_y >= SH) mouse_y = SH - 1;
            d->moved = true;
            continue;
        }
        if (e.type == 0 && d->moved) {                                  /* EV_SYN */
            d->moved = false;
            out->kind = UEV_PTR;
            if (d->kind & D_REL) {
                out->x = mouse_x;
                out->y = mouse_y;
                out->mouse = true;
            } else {
                out->x = (int)((s64)(d->x - d->minx) * SW / (d->maxx - d->minx));
                out->y = (int)((s64)(d->y - d->miny) * SH / (d->maxy - d->miny));
                out->mouse = false;
            }
            out->down = d->down;
            return 1;
        }
    }
}

typedef struct { int fd; short events, revents; } pollfd_t;

int in_wait(uev_t *ev, int timeout_ms)
{
    memset(ev, 0, sizeof *ev);
    s64 now = lp_monotonic_ms();
    if (now - last_scan > 2000)
        in_rescan();
    /* Anything already queued first. */
    for (int i = 0; i < ndev; i++) {
        int r = dev_read(&devs[i], ev);
        if (r > 0)
            return 1;
        if (r < 0) { dev_drop(i); i--; }
    }
    pollfd_t pf[MAXDEV];
    for (int i = 0; i < ndev; i++)
        pf[i] = (pollfd_t){ devs[i].fd, 1 /* POLLIN */, 0 };
    struct { long s, ns; } ts = { timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000L };
    long r = sys_call5(SYS_ppoll, (long)pf, ndev, (long)&ts, 0, 8);
    if (r <= 0)
        return 0;
    for (int i = 0; i < ndev; i++) {
        if (!pf[i].revents)
            continue;
        int k = dev_read(&devs[i], ev);
        if (k > 0)
            return 1;
        if (k < 0 || (pf[i].revents & (8 | 16))) { dev_drop(i); i--; }   /* ERR, HUP */
    }
    return 0;
}
