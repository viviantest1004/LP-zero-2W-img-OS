/* splash - the boot screen, drawn on the framebuffer.
 *
 * The bare-metal firmware used to draw this: it asked the GPU for a
 * framebuffer over the mailbox and wrote pixels into it. That firmware
 * and Linux cannot both be the kernel the GPU loads, so when the board
 * started booting Linux the splash went with it.
 *
 * This is the same idea one layer higher up. Linux hands us the
 * framebuffer as /dev/fb0 and tells us its shape through two ioctls;
 * everything after that is arithmetic. It works on the board, where the
 * GPU set the mode up, and in a virtual machine or on a laptop, where
 * the emulated card or EFI did - none of them is treated differently.
 *
 * ── What it draws ──
 *
 * The LP mark and the system's name, on the desktop's own aubergine
 * gradient. The gradient is the dark wallpaper exactly - same focus, same
 * colours (logo.h carries them) - so on the desktop machine the screen
 * goes from boot to desktop without the colour changing under the user.
 *
 * ── Why it computes shapes instead of copying a picture ──
 *
 * The same program draws a 640x480 VM window, a 1080p television and the
 * XPS's 3840x2160 panel. A bitmap logo is soft at one end of that range or
 * several megabytes at the other, and there is no image decoder here to
 * unpack a compressed one. But the mark and the letters are built from
 * two shapes only - a straight stroke with round ends, and a circular
 * stroke over whole quarter-circles (desktop/branding/src/geom.py) - and
 * the distance from a pixel to either is a few integer multiplies and a
 * square root. So for every pixel near the logo we measure how far it is
 * from the nearest edge and turn that into coverage: half a pixel inside
 * is solid, half a pixel outside is clear, and in between is the
 * anti-aliased edge. That is exact enough that the result is
 * indistinguishable from the SVG rendered by librsvg, at any size.
 *
 * All of it is integer arithmetic, because this libc has no floating
 * point: positions are in 1/32nds of a pixel, colours in 8.8 fixed point.
 *
 * ── Why it dithers ──
 *
 * A dark gradient across 3840 pixels spends only a few dozen 8-bit steps,
 * so rounding each pixel paints contour lines - and on a 16-bit (565)
 * framebuffer, which the Pi can hand us, there are eight times fewer
 * steps and the bands are unmissable. Each pixel is rounded against an
 * 8x8 ordered-dither threshold instead, so every 8x8 block averages to
 * the true colour and the steps dissolve, at whatever depth the
 * framebuffer has.
 *
 * ── Why a row at a time ──
 *
 * A 4K screen is 33MB and this runs during boot, on a board with 512MB,
 * so asking for a whole-screen buffer here would be taking it from
 * everything else at exactly the wrong moment. One row is at most 32KB.
 */
#include "types.h"
#include "osname.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "logo.h"

/* ── The framebuffer ──────────────────────────────────────────────────
 * Two ioctls describe it. Rather than declare the kernel's structures -
 * they are large, and most of what is in them is for setting a mode, not
 * reading one - we read the fields we need out of a byte buffer at the
 * offsets the kernel puts them at. Those offsets are part of the ABI and
 * cannot move.
 */
#define FBIOGET_VSCREENINFO  0x4600
#define FBIOGET_FSCREENINFO  0x4602

#define VAR_SIZE        160
#define VAR_XRES          0
#define VAR_YRES          4
#define VAR_BPP          24
#define VAR_RED_OFF      32     /* struct fb_bitfield: offset, length, msb */
#define VAR_GREEN_OFF    44
#define VAR_BLUE_OFF     56

#define FIX_SIZE         80
#define FIX_LINE_LENGTH  48

/* 8192 pixels across at 32 bits is the widest row we take. */
#define MAX_ROW_BYTES    32768
#define MAX_WIDTH        8192

typedef struct {
    u32 xres, yres, bpp, line_length;
    u32 off[3], len[3];             /* red, green, blue */
} fb_t;

static bool fb_query(int fd, fb_t *fb)
{
    u8 var[VAR_SIZE], fix[FIX_SIZE];
    memset(var, 0, sizeof(var));
    memset(fix, 0, sizeof(fix));

    if (lp_ioctl(fd, FBIOGET_VSCREENINFO, var) < 0) return false;
    if (lp_ioctl(fd, FBIOGET_FSCREENINFO, fix) < 0) return false;

    fb->xres        = *(u32 *)(var + VAR_XRES);
    fb->yres        = *(u32 *)(var + VAR_YRES);
    fb->bpp         = *(u32 *)(var + VAR_BPP);
    fb->off[0]      = *(u32 *)(var + VAR_RED_OFF);
    fb->len[0]      = *(u32 *)(var + VAR_RED_OFF + 4);
    fb->off[1]      = *(u32 *)(var + VAR_GREEN_OFF);
    fb->len[1]      = *(u32 *)(var + VAR_GREEN_OFF + 4);
    fb->off[2]      = *(u32 *)(var + VAR_BLUE_OFF);
    fb->len[2]      = *(u32 *)(var + VAR_BLUE_OFF + 4);
    fb->line_length = *(u32 *)(fix + FIX_LINE_LENGTH);

    if (fb->xres == 0 || fb->yres == 0 || fb->line_length == 0)
        return false;
    if (fb->xres > MAX_WIDTH || fb->line_length > MAX_ROW_BYTES)
        return false;               /* wider than we are prepared for */
    if (fb->bpp != 16 && fb->bpp != 24 && fb->bpp != 32)
        return false;               /* 8-bit palettes need a colour map */
    for (int c = 0; c < 3; c++)
        if (fb->len[c] == 0 || fb->len[c] > 16)
            return false;
    return true;
}

/* ── Integer helpers ──────────────────────────────────────────────── */

static u32 isqrt32(u32 v)
{
    u32 r = 0, b = 1u << 30;
    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else
            r >>= 1;
        b >>= 2;
    }
    return r;
}

static s32 iabs(s32 v) { return v < 0 ? -v : v; }

/* ── The shapes, placed on this screen ────────────────────────────────
 * logo.h has them in design units; place() turns each into 1/32-pixel
 * coordinates for this screen once, with its pixel bounding box, so the
 * per-pixel work is only the distance itself. */
#define SUB      32                 /* sub-pixel units per pixel */
#define MAXPRIM  96

enum { G_RING, G_ELL, G_WORD, G_DOT, G_COUNT };

typedef struct {
    u8  kind, quads, group;
    s32 ax, ay, bx, by;             /* SEG: the ends. ARC: centre in a, b unused */
    s32 r, hw;                      /* ARC radius; half the stroke width */
    s32 len;                        /* SEG: length of b - a */
    s32 e0x, e0y, e1x, e1y;         /* ARC: where an open arc ends */
    s32 x0, y0, x1, y1;             /* pixels this can touch, inclusive */
} placed_t;

static placed_t prims[MAXPRIM];
static int      nprims;

/* Where the start of an arc's run of quadrants is: the quadrant whose
 * clockwise neighbour is not in the run. */
static void arc_ends(placed_t *p)
{
    int start = 0, count = 0;
    for (int q = 0; q < 4; q++)
        if (p->quads & (1u << q)) {
            count++;
            if (!(p->quads & (1u << ((q + 3) & 3))))
                start = q;
        }
    /* quadrant q starts at angle 90*q: east, north, west, south */
    static const s8 dx[4] = { 1, 0, -1, 0 }, dy[4] = { 0, -1, 0, 1 };
    int end = (start + count) & 3;
    p->e0x = p->ax + dx[start] * p->r;
    p->e0y = p->ay + dy[start] * p->r;
    p->e1x = p->ax + dx[end] * p->r;
    p->e1y = p->ay + dy[end] * p->r;
}

/* Add one primitive: design coordinates (1/LP_UNIT units) scaled by
 * num/den into sub-pixels and moved to (ox, oy) sub-pixels. */
static void place(const lp_prim_t *s, int group, s32 ox, s32 oy, s32 num, s32 den)
{
    if (nprims >= MAXPRIM)
        return;
    placed_t *p = &prims[nprims++];
    p->kind  = s->kind;
    p->quads = s->quads;
    p->group = (u8)group;
    p->ax = ox + s->a * num / den;
    p->ay = oy + s->b * num / den;
    p->hw = s->w * num / den / 2;

    s32 lo_x, lo_y, hi_x, hi_y;
    if (s->kind == LP_SEG) {
        p->bx = ox + s->c * num / den;
        p->by = oy + s->d * num / den;
        s32 dx = p->bx - p->ax, dy = p->by - p->ay;
        p->len = (s32)isqrt32((u32)(dx * dx + dy * dy));
        lo_x = p->ax < p->bx ? p->ax : p->bx;
        hi_x = p->ax < p->bx ? p->bx : p->ax;
        lo_y = p->ay < p->by ? p->ay : p->by;
        hi_y = p->ay < p->by ? p->by : p->ay;
        lo_x -= p->hw; hi_x += p->hw; lo_y -= p->hw; hi_y += p->hw;
    } else {
        p->r = s->c * num / den;
        arc_ends(p);
        lo_x = p->ax - p->r - p->hw; hi_x = p->ax + p->r + p->hw;
        lo_y = p->ay - p->r - p->hw; hi_y = p->ay + p->r + p->hw;
    }
    /* one pixel of margin for the anti-aliased fringe */
    p->x0 = lo_x / SUB - 1; p->x1 = hi_x / SUB + 1;
    p->y0 = lo_y / SUB - 1; p->y1 = hi_y / SUB + 1;
}

/* Distance from (px, py) to the primitive's centreline, in sub-pixels. */
static s32 distance(const placed_t *p, s32 px, s32 py)
{
    if (p->kind == LP_SEG) {
        s32 dx = p->bx - p->ax, dy = p->by - p->ay;
        s32 qx = px - p->ax, qy = py - p->ay;
        s32 dot = qx * dx + qy * dy;
        if (p->len > 0 && dot > 0 && dot < p->len * p->len)
            return iabs(qx * dy - qy * dx) / p->len;     /* beside it */
        if (p->len > 0 && dot >= p->len * p->len) {      /* past the far end */
            qx = px - p->bx;
            qy = py - p->by;
        }
        return (s32)isqrt32((u32)(qx * qx + qy * qy));   /* round end */
    }

    s32 dx = px - p->ax, dy = py - p->ay;
    int q = dy <= 0 ? (dx >= 0 ? 0 : 1) : (dx <= 0 ? 2 : 3);
    if (p->quads & (1u << q))
        return iabs((s32)isqrt32((u32)(dx * dx + dy * dy)) - p->r);
    /* outside the swept quadrants: the nearest point is a round end */
    s32 ax = px - p->e0x, ay = py - p->e0y, bx = px - p->e1x, by = py - p->e1y;
    u32 da = (u32)(ax * ax + ay * ay), db = (u32)(bx * bx + by * by);
    return (s32)isqrt32(da < db ? da : db);
}

/* ── The background ───────────────────────────────────────────────────
 * The colour depends only on the distance from the focus, so it is
 * looked up rather than computed: the table is indexed by the squared
 * distance (no square root per pixel) and each entry holds the colour at
 * the square root of its index, interpolated from logo.h's 65 samples. */
#define LUT_N 4096
static u16 lut[LUT_N][3];
static u32 colsq[MAX_WIDTH];

static void build_lut(void)
{
    for (u32 i = 0; i < LUT_N; i++) {
        /* t = sqrt(i / (LUT_N-1)) in 0.16 fixed point: the square root of
         * the ratio scaled by 2^32, which for the last entry is exactly
         * 2^32 and has to be held just under it to fit. */
        u64 ratio = ((u64)i << 32) / (LUT_N - 1);
        if (ratio > 0xFFFFFFFFull)
            ratio = 0xFFFFFFFFull;
        u32 t = isqrt32((u32)ratio);
        u32 pos = t * (LP_GRAD_N - 1);          /* 16.16, 0 .. 64.0 */
        u32 k = pos >> 16, f = pos & 0xFFFF;
        if (k >= LP_GRAD_N - 1) { k = LP_GRAD_N - 2; f = 0xFFFF; }
        for (int c = 0; c < 3; c++) {
            s32 a = LP_GRAD[k][c], b = LP_GRAD[k + 1][c];
            lut[i][c] = (u16)(a + (s32)(((s64)(b - a) * f) >> 16));
        }
    }
}

static const u8 BAYER[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 }, { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 }, { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 }, { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 }, { 63, 31, 55, 23, 61, 29, 53, 21 },
};

/* One 8.8 channel value to `len` bits, rounded against the dither
 * threshold (0..255, as a fraction of one output step). */
static u32 quantise(u32 c, u32 len, u32 thr)
{
    if (len >= 8) {
        u32 v = (c + thr) >> 8;
        if (v > 255) v = 255;
        return v << (len - 8);
    }
    u32 max = (1u << len) - 1;
    u32 v = (((c * max * 257) >> 16) + thr) >> 8;  /* c * max / 255, 8.8 */
    return v > max ? max : v;
}

static void put_pixel(u8 *row, const fb_t *fb, u32 x, u32 v)
{
    if (fb->bpp == 32)
        *(u32 *)(row + x * 4) = v;
    else if (fb->bpp == 24) {
        row[x * 3]     = (u8)v;
        row[x * 3 + 1] = (u8)(v >> 8);
        row[x * 3 + 2] = (u8)(v >> 16);
    } else
        *(u16 *)(row + x * 2) = (u16)v;
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/fb0";
    if (argc > 1 && strcmp(argv[1], "-h") == 0) {
        printf("usage: splash [device]\n");
        printf("  draws the boot screen on the framebuffer (default %s)\n", dev);
        return 0;
    }
    if (argc > 1)
        dev = argv[1];

    /* Wait a little for the device to appear.
     *
     * The graphics hardware is found by probing a bus, and that finishes
     * some time after init starts - so at the moment we are run there is
     * often no /dev/fb0 yet, and being early is not a reason to give up.
     * Eight seconds covers a slow probe on a cold board without holding
     * anything up: init starts this and carries straight on, and a
     * machine with no screen simply has a process asleep for a moment. */
    long fd = -1;
    for (int waited = 0; waited <= 8000; waited += 100) {
        fd = lp_open(dev, O_WRONLY, 0);
        if (fd >= 0)
            break;
        lp_sleep_ms(100);
    }
    if (fd < 0)
        return 1;                   /* no screen attached - not an error */

    fb_t fb;
    if (!fb_query((int)fd, &fb)) {
        lp_close((int)fd);
        return 1;
    }
    u32 W = fb.xres, H = fb.yres;

    /* ── Layout ──
     * The mark is 15% of the screen's height, so it is the same size to
     * the eye on every screen, but never more than 22% of the width, for
     * a portrait panel. Its centre sits a little above the middle - a
     * shape placed dead centre reads as slightly low - and the name hangs
     * under it at a quarter of its height. */
    s32 mark_h = (s32)(H * 15 / 100);
    if (mark_h > (s32)(W * 22 / 100)) mark_h = (s32)(W * 22 / 100);
    if (mark_h < 24) mark_h = 24;
    s32 cx = (s32)W * SUB / 2, cy = (s32)H * SUB * 44 / 100;

    s32 m_num = mark_h * SUB, m_den = LP_MARK_Y1 - LP_MARK_Y0;
    s32 m_ox = cx - (LP_MARK_X0 + LP_MARK_X1) * m_num / m_den / 2;
    s32 m_oy = cy - (LP_MARK_Y0 + LP_MARK_Y1) * m_num / m_den / 2;
    for (u32 i = 0; i < sizeof(LP_MARK) / sizeof(LP_MARK[0]); i++)
        place(&LP_MARK[i], LP_MARK[i].accent ? G_RING : G_ELL, m_ox, m_oy, m_num, m_den);

    /* The name, in the wordmark's letters: first measured, then placed
     * centred. A letter the wordmark does not have is left as a gap the
     * width of an n rather than guessed at. */
    const char *name = LP_OS_NAME;
    s32 w_num = mark_h * SUB * 27 / 100, w_den = LP_CAP;
    const lp_glyph_t *gl[64];
    int n = 0;
    s32 width = 0;
    for (const char *s = name; *s && n < 64; s++, n++) {
        gl[n] = NULL;
        for (u32 k = 0; k < sizeof(LP_LETTERS) / sizeof(LP_LETTERS[0]); k++)
            if (LP_LETTERS[k].ch == *s)
                gl[n] = &LP_LETTERS[k];
        if (n && gl[n]) width += gl[n]->kern;
        width += (gl[n] ? gl[n]->adv : 4 * LP_UNIT) + LP_TRACK;
    }
    if (n)
        width -= LP_TRACK;
    s32 pen = cx - width * w_num / w_den / 2;
    s32 base = cy + mark_h * SUB / 2 + mark_h * SUB * 62 / 100;
    s32 x_units = 0;
    for (int i = 0; i < n; i++) {
        if (i && gl[i]) x_units += gl[i]->kern;
        if (gl[i])
            for (int k = 0; k < gl[i]->count; k++) {
                const lp_prim_t *p = &LP_LETTER_PRIMS[gl[i]->first + k];
                place(p, p->accent ? G_DOT : G_WORD,
                      pen + x_units * w_num / w_den, base, w_num, w_den);
            }
        x_units += (gl[i] ? gl[i]->adv : 4 * LP_UNIT) + LP_TRACK;
    }

    /* the box every shape lives in: nothing outside it needs a look */
    s32 bx0 = (s32)W, by0 = (s32)H, bx1 = -1, by1 = -1;
    for (int i = 0; i < nprims; i++) {
        if (prims[i].x0 < bx0) bx0 = prims[i].x0;
        if (prims[i].y0 < by0) by0 = prims[i].y0;
        if (prims[i].x1 > bx1) bx1 = prims[i].x1;
        if (prims[i].y1 > by1) by1 = prims[i].y1;
    }

    static const u8 colour[G_COUNT][3] = {
        LP_RGB_ACCENT, LP_RGB_INK, LP_RGB_WORD, LP_RGB_ACCENT,
    };

    /* ── The gradient's geometry ──
     * Positions in 1/4096ths of the width and height, measured from the
     * focus, so the light stretches to the screen's shape. The index into
     * the table is the squared distance over the squared distance to the
     * farthest corner, taken by multiplying with a reciprocal made once. */
    build_lut();
    for (u32 x = 0; x < W; x++) {
        s32 ux = (s32)((2 * x + 1) * 2048 / W) - LP_FOCUS_X;
        colsq[x] = (u32)(ux * ux);
    }
    u32 fx = 4096 - LP_FOCUS_X, fy = 4096 - LP_FOCUS_Y;
    u64 recip = ((u64)(LUT_N - 1) << 32) / (u64)(fx * fx + fy * fy);

    static u8 row[MAX_ROW_BYTES] __attribute__((aligned(8)));

    for (u32 y = 0; y < H; y++) {
        s32 uy = (s32)((2 * y + 1) * 2048 / H) - LP_FOCUS_Y;
        u32 rowsq = (u32)(uy * uy);
        bool logo_row = (s32)y >= by0 && (s32)y <= by1;

        for (u32 x = 0; x < W; x++) {
            u32 idx = (u32)(((u64)(colsq[x] + rowsq) * recip) >> 32);
            if (idx >= LUT_N) idx = LUT_N - 1;
            u32 c[3] = { lut[idx][0], lut[idx][1], lut[idx][2] };

            if (logo_row && (s32)x >= bx0 && (s32)x <= bx1) {
                s32 px = (s32)x * SUB + SUB / 2, py = (s32)y * SUB + SUB / 2;
                s32 best[G_COUNT];
                for (int g = 0; g < G_COUNT; g++)
                    best[g] = 1 << 30;
                for (int i = 0; i < nprims; i++) {
                    const placed_t *p = &prims[i];
                    if ((s32)x < p->x0 || (s32)x > p->x1 || (s32)y < p->y0 || (s32)y > p->y1)
                        continue;
                    s32 sd = distance(p, px, py) - p->hw;
                    if (sd < best[p->group])
                        best[p->group] = sd;
                }
                /* each colour's shapes are one union (the nearest edge
                 * wins), laid over the background in drawing order */
                for (int g = 0; g < G_COUNT; g++) {
                    s32 cov = SUB / 2 - best[g];      /* 0..SUB across the edge */
                    if (cov <= 0)
                        continue;
                    if (cov > SUB) cov = SUB;
                    for (int k = 0; k < 3; k++) {
                        s32 fg = (s32)colour[g][k] << 8;
                        c[k] = (u32)((s32)c[k] + (fg - (s32)c[k]) * cov / SUB);
                    }
                }
            }

            u32 v = 0;
            for (int k = 0; k < 3; k++) {
                /* the channels read the pattern at different offsets so
                 * the dither does not line up into a grey grid */
                u32 thr = BAYER[(y + 3 * k) & 7][(x + 5 * k) & 7] * 4u + 2u;
                v |= quantise(c[k], fb.len[k], thr) << fb.off[k];
            }
            put_pixel(row, &fb, x, v);
        }

        if (lp_write((int)fd, row, fb.line_length) != (long)fb.line_length)
            break;                  /* short write: stop rather than tear */
    }

    lp_close((int)fd);
    return 0;
}
