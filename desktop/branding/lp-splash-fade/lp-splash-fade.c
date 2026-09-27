/* lp-splash-fade - the desktop's first frame is the boot splash, and then
 * the splash fades away into the desktop.
 *
 * The boot splash (userland/splash) holds the screen until the session
 * starts, then stops and leaves its last frame - the aubergine gradient
 * and the logo - on the framebuffer. The compositor then takes the
 * display over, and everything the session draws appears piece by piece:
 * the wallpaper, the top bar, the dock, the desktop icons. Shown as they
 * arrive, that is a second of flicker right after a boot that was meant
 * to look finished.
 *
 * So the session's first client is this: a layer-shell surface on the
 * OVERLAY layer, over everything, drawing exactly the splash's last
 * frame. The desktop assembles itself underneath, out of sight, and
 * when it is ready this fades out in 340ms (the window spring of
 * design/feel.md) and exits. To the person watching, the boot screen
 * simply dissolves into the desktop.
 *
 * "Exactly" is literal. The picture comes from userland/splash/scene.h,
 * the same integer code the splash draws with, laid out for the same
 * pixel size, so the gradient's dither and the logo's edges land on the
 * same pixels and the switch from one program to the other cannot be
 * seen. It is drawn once, into an image, before the window is shown -
 * the first frame the compositor gets from us is already the finished
 * picture, never an empty one.
 *
 *     lp-splash-fade [--timeout S] [--name NAME]
 *                         cover the screen; fade when told (or after S
 *                         seconds, 8 by default, so a shell that never
 *                         says so cannot leave the desktop hidden)
 *     lp-splash-fade done tell the running one the desktop is ready
 *
 * `done` finds the running instance through
 * $XDG_RUNTIME_DIR/lp-splash-fade.pid and sends it SIGUSR1; it is a no-op
 * when there is none, so a shell component may call it unconditionally
 * after its first frame. SIGTERM ends it at once.
 *
 * Input passes straight through (the input region is empty): if
 * anything goes wrong the worst case is a picture over the desktop for
 * the timeout, never a desktop that cannot be touched.
 *
 * Reduced motion: lp-motion's springs settle in about 90ms, so the fade
 * becomes a quick crossfade, as the design system asks.
 *
 * Cost: the one full-screen picture is drawn on four threads (about 30ms
 * at 3840x2160); each frame of the fade is that image times the current
 * opacity (see fade_direct). Nothing moves, so there is nothing to
 * relayout. The compositor's side is one full-screen blend a frame -
 * nothing for a GPU, and the reason this is best not run under a
 * software renderer at 4K (see the branding README).
 */
#include <gtk/gtk.h>
#include <gtk-layer-shell.h>
#include <glib-unix.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lp-motion.h"

/* scene.h is written for the splash's own libc; it needs only these. */
typedef uint8_t  u8;
typedef int8_t   s8;
typedef uint16_t u16;
typedef int16_t  s16;
typedef uint32_t u32;
typedef int32_t  s32;
typedef uint64_t u64;
typedef int64_t  s64;
#include "scene.h"

#define DEFAULT_TIMEOUT 8

typedef struct {
    GtkWidget       *win;
    cairo_surface_t *img;           /* the splash, at the output's pixel size */
    LpSpring         fade;          /* 1 = the splash, 0 = gone */
    LpMotion        *motion;
    int              scale;
} Cover;

static GPtrArray  *covers;
static const char *os_name = "linux-LP";
static char       *pid_path;
static gboolean    fading;
static gboolean    trace;           /* LP_MOTION_TRACE: time each frame */

/* ── the picture ──────────────────────────────────────────────────── */

typedef struct {
    const scene_t *sc;
    u32           *px;
    int            stride;          /* in u32 */
    u32            y0, y1;
} Band;

/* One band of rows, exactly as splash.c draws a 32-bit framebuffer:
 * the gradient, the logo over it, each channel dithered to 8 bits. */
static void *draw_band(void *arg)
{
    Band *b = arg;
    const scene_t *s = b->sc;
    for (u32 y = b->y0; y < b->y1; y++) {
        u32 rsq = scene_rowsq(s, y);
        u32 *row = b->px + (size_t)y * (size_t)b->stride;
        for (u32 x = 0; x < s->W; x++) {
            u32 c[3];
            scene_bg_row(s, x, rsq, c);
            scene_logo(s, x, y, c);
            row[x] = 0xFF000000u |
                     scene_quantise(c[0], 8, x, y, 0) << 16 |
                     scene_quantise(c[1], 8, x, y, 1) << 8 |
                     scene_quantise(c[2], 8, x, y, 2);
        }
    }
    return NULL;
}

static cairo_surface_t *render(int w, int h, int scale)
{
    static scene_t sc;              /* 60KB of tables: not on the stack */
    if (w <= 0 || h <= 0 || w > SCENE_MAX_W)
        return NULL;
    scene_init(&sc, (u32)w, (u32)h, os_name);

    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS)
        return NULL;
    cairo_surface_flush(img);
    u32 *px = (u32 *)cairo_image_surface_get_data(img);
    int stride = cairo_image_surface_get_stride(img) / 4;

    enum { N = 4 };
    pthread_t th[N];
    Band band[N];
    for (int i = 0; i < N; i++) {
        band[i] = (Band){ &sc, px, stride, (u32)h * i / N, (u32)h * (i + 1) / N };
        if (pthread_create(&th[i], NULL, draw_band, &band[i]) != 0) {
            draw_band(&band[i]);
            th[i] = 0;
        }
    }
    for (int i = 0; i < N; i++)
        if (th[i])
            pthread_join(th[i], NULL);

    cairo_surface_mark_dirty(img);
    cairo_surface_set_device_scale(img, scale, scale);
    return img;
}

/* ── fading, fast ─────────────────────────────────────────────────────
 * Each frame of the fade is the picture times the fade's alpha, over
 * the whole screen. cairo does that correctly but, at 3840x2160, in
 * 15-25ms of one core (60 through its general path, SOURCE with an
 * alpha). So when GTK hands us a plain ARGB32 image the size of the
 * picture - which is what a Wayland window's buffer is - the multiply
 * is done here instead, two channels per multiply, on four threads. */
typedef struct {
    const u32 *src;
    u32       *dst;
    int        sstride, dstride, w; /* strides in u32 */
    int        y0, y1;
    u32        a;                   /* 0..256 */
} Fade;

static void *fade_band(void *arg)
{
    const Fade *f = arg;
    for (int y = f->y0; y < f->y1; y++) {
        const u32 *s = f->src + (size_t)y * (size_t)f->sstride;
        u32 *d = f->dst + (size_t)y * (size_t)f->dstride;
        for (int x = 0; x < f->w; x++) {
            u32 p = s[x];
            u32 rb = ((p & 0x00FF00FFu) * f->a >> 8) & 0x00FF00FFu;
            u32 ag = (((p >> 8) & 0x00FF00FFu) * f->a) & 0xFF00FF00u;
            d[x] = rb | ag;
        }
    }
    return NULL;
}

static gboolean fade_direct(cairo_t *cr, cairo_surface_t *img, double a)
{
    cairo_surface_t *t = cairo_get_target(cr);
    double ox, oy;
    cairo_surface_get_device_offset(t, &ox, &oy);
    if (cairo_surface_get_type(t) != CAIRO_SURFACE_TYPE_IMAGE ||
        cairo_image_surface_get_format(t) != CAIRO_FORMAT_ARGB32 ||
        cairo_image_surface_get_width(t) != cairo_image_surface_get_width(img) ||
        cairo_image_surface_get_height(t) != cairo_image_surface_get_height(img) ||
        ox != 0 || oy != 0)
        return FALSE;
    cairo_surface_flush(t);
    enum { N = 4 };
    pthread_t th[N];
    Fade f[N];
    int h = cairo_image_surface_get_height(img);
    for (int i = 0; i < N; i++) {
        f[i] = (Fade){ (const u32 *)cairo_image_surface_get_data(img),
                       (u32 *)cairo_image_surface_get_data(t),
                       cairo_image_surface_get_stride(img) / 4,
                       cairo_image_surface_get_stride(t) / 4,
                       cairo_image_surface_get_width(img),
                       h * i / N, h * (i + 1) / N, (u32)(a * 256.0 + 0.5) };
        if (pthread_create(&th[i], NULL, fade_band, &f[i]) != 0) {
            fade_band(&f[i]);
            th[i] = 0;
        }
    }
    for (int i = 0; i < N; i++)
        if (th[i])
            pthread_join(th[i], NULL);
    cairo_surface_mark_dirty(t);
    return TRUE;
}

/* ── the surface ──────────────────────────────────────────────────── */

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    Cover *c = data;
    (void)w;
    gint64 t = g_get_monotonic_time();
    double a = CLAMP(c->fade.x, 0.0, 1.0);
    /* Three ways to the same pixels, fastest first. Mid-fade, the
     * multiply above when the target is a plain image of our size (the
     * usual case). At full strength, a plain copy. Otherwise cairo: a
     * clear, then OVER at the fade's alpha - not SOURCE with an alpha,
     * which is the same result but has no fast path in pixman (60ms a
     * frame at 4K against 8 for this). */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    if (c->img && a > 0.0 && a < 1.0 && fade_direct(cr, c->img, a)) {
        /* done above */
    } else if (c->img && a >= 1.0) {
        cairo_set_source_surface(cr, c->img, 0, 0);
        cairo_paint(cr);
    } else {
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
        if (c->img && a > 0.0) {
            cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
            cairo_set_source_surface(cr, c->img, 0, 0);
            cairo_paint_with_alpha(cr, a);
        }
    }
    if (trace)
        fprintf(stderr, "lp-splash-fade: draw alpha=%.3f in %.2f ms\n", a,
                (g_get_monotonic_time() - t) / 1000.0);
    return TRUE;
}

static void maybe_quit(void)
{
    for (guint i = 0; i < covers->len; i++) {
        Cover *c = covers->pdata[i];
        if (c->fade.moving || c->fade.x > 0.001)
            return;
    }
    gtk_main_quit();
}

static void on_frame(GtkWidget *w, gpointer data)
{
    (void)data;
    gtk_widget_queue_draw(w);
    if (fading)
        maybe_quit();
}

/* The size the output really has, in pixels, so the picture is laid out
 * for exactly the pixels the framebuffer had. */
static void cover_render(Cover *c, int lw, int lh, int scale)
{
    if (c->img && cairo_image_surface_get_width(c->img) == lw * scale &&
        cairo_image_surface_get_height(c->img) == lh * scale)
        return;
    if (c->img)
        cairo_surface_destroy(c->img);
    gint64 t = g_get_monotonic_time();
    c->img = render(lw * scale, lh * scale, scale);
    c->scale = scale;
    if (trace)
        fprintf(stderr, "lp-splash-fade: %dx%d rendered in %.1f ms\n", lw * scale, lh * scale,
                (g_get_monotonic_time() - t) / 1000.0);
}

static void on_size(GtkWidget *w, GdkRectangle *r, gpointer data)
{
    /* The compositor decides the size; if it is not the monitor's (a
     * scale we did not expect), draw again for what we were given. */
    cover_render(data, r->width, r->height, gtk_widget_get_scale_factor(w));
    /* Until the fade starts we are opaque, and saying so lets the
     * compositor skip drawing the desktop assembling underneath. */
    GdkWindow *gw = gtk_widget_get_window(w);
    if (gw && !fading) {
        cairo_rectangle_int_t all = { 0, 0, r->width, r->height };
        cairo_region_t *reg = cairo_region_create_rectangle(&all);
        gdk_window_set_opaque_region(gw, reg);
        cairo_region_destroy(reg);
    }
}

static void on_realize(GtkWidget *w, gpointer data)
{
    (void)data;
    /* Nothing is to be touched here: an empty input region. */
    cairo_region_t *none = cairo_region_create();
    gtk_widget_input_shape_combine_region(w, none);
    cairo_region_destroy(none);
}

static Cover *cover_new(GdkMonitor *mon)
{
    Cover *c = g_new0(Cover, 1);
    GtkWindow *win = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    c->win = GTK_WIDGET(win);

    gtk_layer_init_for_window(win);
    gtk_layer_set_namespace(win, "lp-splash-fade");
    gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_monitor(win, mon);
    for (int e = 0; e < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER; e++)
        gtk_layer_set_anchor(win, e, TRUE);
    gtk_layer_set_exclusive_zone(win, -1);      /* over the bar and dock too */
    gtk_layer_set_keyboard_interactivity(win, FALSE);

    GdkScreen *scr = gtk_widget_get_screen(c->win);
    GdkVisual *rgba = gdk_screen_get_rgba_visual(scr);
    if (rgba)
        gtk_widget_set_visual(c->win, rgba);
    gtk_widget_set_app_paintable(c->win, TRUE);

    /* Draw the picture now, before the window exists, so the first
     * buffer we ever attach is the finished one. */
    GdkRectangle g;
    gdk_monitor_get_geometry(mon, &g);
    cover_render(c, g.width, g.height, gdk_monitor_get_scale_factor(mon));
    /* and ask for that size from the start: without it GTK allocates its
     * 200x200 default before the compositor's configure arrives, and the
     * picture was drawn twice more (200x200, then full size again) -
     * 180ms of a core at 4K, just as the session is starting. */
    gtk_window_set_default_size(win, g.width, g.height);

    lp_spring_init(&c->fade, LP_SPRING_WINDOW, 1.0);
    c->motion = lp_motion_new(c->win, on_frame, c);
    lp_motion_add(c->motion, &c->fade);

    g_signal_connect(c->win, "draw", G_CALLBACK(on_draw), c);
    g_signal_connect(c->win, "size-allocate", G_CALLBACK(on_size), c);
    g_signal_connect(c->win, "realize", G_CALLBACK(on_realize), c);
    gtk_widget_show(c->win);
    return c;
}

/* ── being told ───────────────────────────────────────────────────── */

static gboolean start_fade(gpointer data)
{
    (void)data;
    if (fading)
        return G_SOURCE_REMOVE;
    fading = TRUE;
    for (guint i = 0; i < covers->len; i++) {
        Cover *c = covers->pdata[i];
        /* The fade out is the brief's 340ms: the window spring's full
         * time, not its 0.7x exit - this is the boot finishing, not a
         * window being dismissed, and it is the one moment worth seeing. */
        lp_spring_set_target(&c->fade, 0.0);
        lp_motion_kick(c->motion);
        GdkWindow *gw = gtk_widget_get_window(c->win);
        if (gw)
            gdk_window_set_opaque_region(gw, NULL);
    }
    return G_SOURCE_REMOVE;
}

static gboolean on_term(gpointer data)
{
    (void)data;
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

static int send_done(void)
{
    const char *rt = g_getenv("XDG_RUNTIME_DIR");
    if (!rt)
        return 0;
    g_autofree char *path = g_build_filename(rt, "lp-splash-fade.pid", NULL);
    g_autofree char *text = NULL;
    if (!g_file_get_contents(path, &text, NULL, NULL))
        return 0;                   /* nothing covering the screen */
    long pid = strtol(text, NULL, 10);
    if (pid <= 1)
        return 0;
    /* a pid file can outlive its process: check it is still us */
    g_autofree char *commp = g_strdup_printf("/proc/%ld/comm", pid);
    g_autofree char *comm = NULL;
    if (!g_file_get_contents(commp, &comm, NULL, NULL) ||
        strncmp(comm, "lp-splash-fade", 14) != 0)
        return 0;
    return kill((pid_t)pid, SIGUSR1) == 0 ? 0 : 1;
}

/* NAME= from os-release: the same name the splash drew under the logo
 * (its build's LP_OS_NAME, which desktop/branding/os-release is made to
 * match). Only when os-release is ours (ID=lp): on a root whose
 * os-release is still Debian's, "Debian GNU/Linux" is not what the boot
 * screen said, and the wordmark has no letters for it anyway. */
static void read_os_name(void)
{
    g_autofree char *text = NULL;
    if (!g_file_get_contents("/etc/os-release", &text, NULL, NULL))
        return;
    char *name = NULL;
    gboolean ours = FALSE;
    for (char *l = strtok(text, "\n"); l; l = strtok(NULL, "\n")) {
        if (strcmp(l, "ID=lp") == 0 || strcmp(l, "ID=\"lp\"") == 0)
            ours = TRUE;
        if (strncmp(l, "NAME=", 5) == 0) {
            char *v = l + 5;
            size_t n = strlen(v);
            if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
                v[n - 1] = 0;
                v++;
            }
            name = v;
        }
    }
    if (ours && name && *name)
        os_name = g_strdup(name);
}

int main(int argc, char **argv)
{
    int timeout = DEFAULT_TIMEOUT;
    trace = g_getenv("LP_MOTION_TRACE") != NULL;
    read_os_name();
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "done") == 0)
            return send_done();
        else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
            timeout = atoi(argv[++i]);
        else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc)
            os_name = argv[++i];
        else {
            printf("usage: lp-splash-fade [--timeout S] [--name NAME]   cover the screen with the splash\n"
                   "       lp-splash-fade done                          the desktop is ready: fade out\n");
            return strcmp(argv[i], "--help") == 0 ? 0 : 2;
        }
    }

    gtk_init(&argc, &argv);
    if (!gtk_layer_is_supported()) {
        fprintf(stderr, "lp-splash-fade: the compositor has no layer-shell; nothing to cover\n");
        return 0;
    }

    const char *rt = g_getenv("XDG_RUNTIME_DIR");
    if (rt) {
        pid_path = g_build_filename(rt, "lp-splash-fade.pid", NULL);
        g_autofree char *me = g_strdup_printf("%ld\n", (long)getpid());
        g_file_set_contents(pid_path, me, -1, NULL);    /* /run: tmpfs, no fsync needed */
    }
    g_unix_signal_add(SIGUSR1, start_fade, NULL);
    g_unix_signal_add(SIGTERM, on_term, NULL);
    g_unix_signal_add(SIGINT, on_term, NULL);
    if (timeout > 0)
        g_timeout_add_seconds((guint)timeout, start_fade, NULL);

    covers = g_ptr_array_new();
    GdkDisplay *dpy = gdk_display_get_default();
    for (int i = 0; i < gdk_display_get_n_monitors(dpy); i++)
        g_ptr_array_add(covers, cover_new(gdk_display_get_monitor(dpy, i)));
    if (covers->len == 0)
        return 0;

    gtk_main();

    if (pid_path)
        unlink(pid_path);
    return 0;
}
