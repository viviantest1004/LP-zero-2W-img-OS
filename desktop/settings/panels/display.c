/*
 * display.c - Display: resolution, refresh rate, scale, rotation, where
 * external monitors sit, brightness, and night light.
 *
 * ── Two writes for every change ──
 *
 * A change is applied live with wlr-randr, which speaks the
 * wlr-output-management protocol that wayfire (and sway) implement, and
 * recorded in wayfire's own config as an [output:<name>] section, which
 * is what the next session starts with:
 *
 *     [output:eDP-1]
 *     mode = 3840x2160@60000        (refresh in mHz, wayfire's format)
 *     scale = 2.000000
 *     transform = normal
 *     position = 0,0
 *
 * Only wlr-randr would be gone after a reboot; only the config would
 * depend on wayfire noticing the file changed, and a monitor that is
 * rejected by the config reader fails silently. Doing both means the
 * screen changes now and stays changed.
 *
 * ── The fifteen seconds ──
 *
 * A resolution, refresh rate or rotation that the panel cannot show
 * leaves a person looking at black, with the control they need on the
 * screen they cannot see. So those three are applied live only, a dialog
 * asks "keep these settings?", and if nobody answers within fifteen
 * seconds the old mode is put back and nothing is written to the config.
 * Scale and position cannot blank a screen and are kept at once.
 *
 * ── The 4K panel ──
 *
 * The XPS 15's panel is 3840x2160 at 15.6", about 280 pixels per inch; at
 * 100% every piece of text is a third of the height it was designed for.
 * The recommended scale is worked out from the physical size wlr-randr
 * reports, and on that panel it is 200%. The shipped wayfire.ini is what
 * makes 200% the default (desktop-shell track); this panel marks it
 * "recommended" and the Reset panel puts it back.
 *
 * ── Night light ──
 *
 * wlsunset, with a fixed evening and morning rather than sunset times,
 * because sunset times need a location and this machine has no location
 * service. The choice is kept in ~/.config/lp/nightlight.conf and
 * `lp-settings --apply-night-light` (below) is the one command that turns
 * the file into a running wlsunset - the session runs it at login, this
 * panel runs the same code when a control moves, and the arguments are
 * written down in one place.
 *
 * ── Mirroring ──
 *
 * Not offered. wlroots 0.15 compositors cannot show one output's contents
 * on another without a separate mirroring client, and a "Mirror" choice
 * that produced an extended desktop would be the kind of lie the rest of
 * this app refuses to tell.
 */
#include "core.h"

#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

typedef struct { int w, h, mhz; gboolean preferred, current; } mode_t_;

typedef struct {
    char    *name, *desc;
    int      phys_w, phys_h;         /* mm */
    gboolean enabled;
    GArray  *modes;                  /* mode_t_ */
    int      x, y;
    char    *transform;
    double   scale;
    int      cur;                    /* index into modes, -1 when off */
} out_t;

typedef struct {
    GtkWidget *page;
    GPtrArray *outs;                 /* out_t */
    int        sel;
    GtkWidget *controls;             /* the group for the selected output */
    GtkWidget *arrange;              /* drawing area, when >1 output */
    int        drag;                 /* output being dragged, -1 */
    double     drag_x0, drag_y0;     /* its logical position when the drag began */
    double     view_scale, view_ox, view_oy;
    GtkWidget *res_dd, *rate_dd;
    GPtrArray *res_list;             /* "WxH" strings in the drop-down */
    GArray    *rate_list;            /* mhz values in the drop-down */
} disp_t;

static disp_t *DP;

static const double SCALES[] = { 1.0, 1.25, 1.5, 1.75, 2.0 };
static const char *const TRANSFORMS[] = { "normal", "90", "180", "270", NULL };

static void out_free(gpointer p)
{
    out_t *o = p;
    g_free(o->name); g_free(o->desc); g_free(o->transform);
    g_array_free(o->modes, TRUE);
    g_free(o);
}

static void disp_free(gpointer p)
{
    disp_t *d = p;
    g_ptr_array_free(d->outs, TRUE);
    if (d->res_list) g_ptr_array_free(d->res_list, TRUE);
    if (d->rate_list) g_array_free(d->rate_list, TRUE);
    if (DP == d) DP = NULL;
    g_free(d);
}

/* ── reading wlr-randr ──────────────────────────────────────────────── */

static GPtrArray *parse_randr(const char *text)
{
    GPtrArray *outs = g_ptr_array_new_with_free_func(out_free);
    out_t *o = NULL;
    gboolean in_modes = FALSE;
    char **l = g_strsplit(text, "\n", -1);
    for (int i = 0; l[i]; i++) {
        char *line = l[i];
        if (!*line) continue;
        if (line[0] != ' ' && line[0] != '\t') {
            o = g_new0(out_t, 1);
            o->modes = g_array_new(FALSE, TRUE, sizeof(mode_t_));
            o->scale = 1.0;
            o->cur = -1;
            o->transform = g_strdup("normal");
            char *q = strchr(line, '"');
            o->name = q ? g_strstrip(g_strndup(line, q - line)) : g_strstrip(g_strdup(line));
            if (q) {
                char *e = strrchr(q + 1, '"');
                o->desc = e ? g_strndup(q + 1, e - q - 1) : g_strdup(q + 1);
            }
            g_ptr_array_add(outs, o);
            in_modes = FALSE;
            continue;
        }
        if (!o) continue;
        char *t = g_strstrip(g_strdup(line));
        int w, h;
        double hz;
        if (in_modes && sscanf(t, "%dx%d px, %lf Hz", &w, &h, &hz) == 3) {
            mode_t_ m = { w, h, (int)lround(hz * 1000.0), strstr(t, "preferred") != NULL,
                          strstr(t, "current") != NULL };
            g_array_append_val(o->modes, m);
            if (m.current) o->cur = o->modes->len - 1;
        } else {
            in_modes = FALSE;
            if (g_str_has_prefix(t, "Modes:")) in_modes = TRUE;
            else if (g_str_has_prefix(t, "Enabled:")) o->enabled = strstr(t, "yes") != NULL;
            else if (g_str_has_prefix(t, "Physical size:")) sscanf(t, "Physical size: %dx%d", &o->phys_w, &o->phys_h);
            else if (g_str_has_prefix(t, "Position:")) sscanf(t, "Position: %d,%d", &o->x, &o->y);
            else if (g_str_has_prefix(t, "Scale:")) o->scale = g_ascii_strtod(t + 6, NULL);
            else if (g_str_has_prefix(t, "Transform:")) {
                g_free(o->transform);
                o->transform = g_strstrip(g_strdup(t + 10));
            }
        }
        g_free(t);
    }
    g_strfreev(l);
    return outs;
}

static gboolean is_internal(const out_t *o)
{
    return g_str_has_prefix(o->name, "eDP") || g_str_has_prefix(o->name, "LVDS") ||
           g_str_has_prefix(o->name, "DSI");
}

/* The scale that makes a pixel of text about the size it was designed
 * for, from the pixel density. Laptops are looked at from closer than
 * monitors, so the same density earns a little less scaling on a
 * monitor. */
static double recommended_scale(const out_t *o)
{
    if (!o->phys_w || o->cur < 0) return 1.0;
    const mode_t_ *m = &g_array_index(o->modes, mode_t_, o->cur);
    double dpi = m->w / (o->phys_w / 25.4);
    if (!is_internal(o)) dpi *= 0.85;
    if (dpi >= 250) return 2.0;
    if (dpi >= 215) return 1.75;
    if (dpi >= 180) return 1.5;
    if (dpi >= 150) return 1.25;
    return 1.0;
}

static void logical_size(const out_t *o, double *w, double *h)
{
    int pw = 1920, ph = 1080;
    if (o->cur >= 0) {
        const mode_t_ *m = &g_array_index(o->modes, mode_t_, o->cur);
        pw = m->w; ph = m->h;
    }
    gboolean turned = !strcmp(o->transform, "90") || !strcmp(o->transform, "270") ||
                      g_str_has_suffix(o->transform, "-90") || g_str_has_suffix(o->transform, "-270");
    *w = (turned ? ph : pw) / (o->scale > 0 ? o->scale : 1);
    *h = (turned ? pw : ph) / (o->scale > 0 ? o->scale : 1);
}

/* ── applying ───────────────────────────────────────────────────────── */

static char *mode_arg(const mode_t_ *m)
{
    return g_strdup_printf("%dx%d@%.3fHz", m->w, m->h, m->mhz / 1000.0);
}

static gboolean randr(const char *const *argv)
{
    char *err = NULL, *out = NULL;
    int st = lp_run_full(argv, NULL, &out, &err);
    if (st != 0) {
        char *why = lp_first_line(err, out);
        lp_toast(TRUE, T("The display did not accept that: %s", "화면이 받아들이지 않았습니다: %s"), why);
        g_free(why);
    }
    g_free(err); g_free(out);
    return st == 0;
}

/* Everything about one output into its [output:NAME] section. */
static void persist(const out_t *o)
{
    char *ini = wayfire_ini();
    char *sec = g_strdup_printf("output:%s", o->name);
    if (!o->enabled || o->cur < 0) {
        ini_set(ini, sec, "mode", "off");
    } else {
        const mode_t_ *m = &g_array_index(o->modes, mode_t_, o->cur);
        char *mode = g_strdup_printf("%dx%d@%d", m->w, m->h, m->mhz);
        ini_set(ini, sec, "mode", mode);
        g_free(mode);
    }
    char sc[32];
    g_ascii_formatd(sc, sizeof sc, "%.6f", o->scale);
    ini_set(ini, sec, "scale", sc);
    ini_set(ini, sec, "transform", o->transform);
    char *pos = g_strdup_printf("%d,%d", o->x, o->y);
    ini_set(ini, sec, "position", pos);
    g_free(pos);
    g_free(sec);
    g_free(ini);
}

static void rebuild_controls(disp_t *d);
static void reload(disp_t *d);

/* The keep-or-revert question. */
typedef struct {
    char *name;
    char *old_mode, *old_transform;
    gboolean old_enabled;
    int secs;
    guint timer;
    lp_dialog_t *dlg;
    GtkWidget *label;
} confirm_t;

static void confirm_free(gpointer p)
{
    confirm_t *c = p;
    if (c->timer) g_source_remove(c->timer);
    g_free(c->name); g_free(c->old_mode); g_free(c->old_transform);
    g_free(c);
}

static void revert(confirm_t *c)
{
    if (c->old_enabled) {
        const char *v[] = { "wlr-randr", "--output", c->name, "--on", "--mode", c->old_mode,
                            "--transform", c->old_transform, NULL };
        randr(v);
    } else {
        const char *v[] = { "wlr-randr", "--output", c->name, "--off", NULL };
        randr(v);
    }
    lp_toast(FALSE, T("Went back to the previous display settings",
                      "이전 화면 설정으로 되돌렸습니다"));
}

static gboolean confirm_tick(gpointer p)
{
    confirm_t *c = p;
    if (--c->secs <= 0) {
        c->timer = 0;
        revert(c);
        lp_dialog_close(c->dlg);        /* frees c */
        if (DP) reload(DP);
        return G_SOURCE_REMOVE;
    }
    char *t = g_strdup_printf(T("Going back in %d seconds.", "%d초 뒤에 되돌립니다."), c->secs);
    gtk_label_set_text(GTK_LABEL(c->label), t);
    g_free(t);
    return G_SOURCE_CONTINUE;
}

static void confirm_keep(lp_dialog_t *dlg, gpointer p)
{
    (void)p;
    confirm_t *c = lp_dialog_get_data(dlg, "lp-confirm");
    if (c->timer) { g_source_remove(c->timer); c->timer = 0; }
    lp_dialog_set_data(dlg, "lp-confirm", NULL, NULL);
    if (DP) {
        for (guint i = 0; i < DP->outs->len; i++) {
            out_t *o = g_ptr_array_index(DP->outs, i);
            if (!strcmp(o->name, c->name)) persist(o);
        }
    }
    lp_toast(FALSE, T("Kept the new display settings", "새 화면 설정을 유지합니다"));
    confirm_free(c);
    lp_dialog_close(dlg);
}

/* Closed by Cancel, Escape or the window going: that is a "no". */
static void confirm_gone(gpointer p)
{
    confirm_t *c = p;
    if (c->timer) {
        g_source_remove(c->timer);
        c->timer = 0;
        revert(c);
        if (DP) reload(DP);
    }
    confirm_free(c);
}

static void ask_keep(const out_t *before_o, const char *old_mode, const char *old_tf, gboolean old_en)
{
    confirm_t *c = g_new0(confirm_t, 1);
    c->name = g_strdup(before_o->name);
    c->old_mode = g_strdup(old_mode);
    c->old_transform = g_strdup(old_tf);
    c->old_enabled = old_en;
    c->secs = 15;
    c->dlg = lp_dialog_new(T("Keep these display settings?", "이 화면 설정을 유지할까요?"),
                           T("Keep", "유지"), FALSE, confirm_keep, NULL);
    lp_dialog_text(c->dlg, T("If the screen looks wrong or is blank, wait: the previous settings "
                             "come back by themselves.",
                             "화면이 이상하거나 보이지 않으면 기다리십시오. 이전 설정이 저절로 "
                             "돌아옵니다."), NULL);
    c->label = gtk_label_new(NULL);
    gtk_widget_set_halign(c->label, GTK_ALIGN_START);
    gtk_widget_add_css_class(c->label, "lp-big");
    gtk_box_append(GTK_BOX(lp_dialog_body(c->dlg)), c->label);
    lp_dialog_set_data(c->dlg, "lp-confirm", c, confirm_gone);
    c->secs++;
    confirm_tick(c);
    c->timer = g_timeout_add(1000, confirm_tick, c);
    lp_dialog_present(c->dlg);
}

static out_t *sel_out(disp_t *d)
{
    if (d->sel < 0 || d->sel >= (int)d->outs->len) return NULL;
    return g_ptr_array_index(d->outs, d->sel);
}

/* Resolution, refresh rate and rotation: live, then the question. */
static void apply_risky(disp_t *d, int new_cur, const char *new_tf)
{
    out_t *o = sel_out(d);
    if (!o || o->cur < 0) return;
    const mode_t_ *old = &g_array_index(o->modes, mode_t_, o->cur);
    char *old_mode = mode_arg(old);
    char *old_tf = g_strdup(o->transform);

    const mode_t_ *nm = &g_array_index(o->modes, mode_t_, new_cur);
    char *mode = mode_arg(nm);
    const char *v[] = { "wlr-randr", "--output", o->name, "--mode", mode,
                        "--transform", new_tf, NULL };
    if (randr(v)) {
        g_array_index(o->modes, mode_t_, o->cur).current = FALSE;
        o->cur = new_cur;
        g_array_index(o->modes, mode_t_, o->cur).current = TRUE;
        g_free(o->transform);
        o->transform = g_strdup(new_tf);
        ask_keep(o, old_mode, old_tf, TRUE);
        if (d->arrange) gtk_widget_queue_draw(d->arrange);
    } else {
        rebuild_controls(d);
    }
    g_free(mode); g_free(old_mode); g_free(old_tf);
}

static void on_resolution(GObject *dd, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    disp_t *d = DP;
    out_t *o = d ? sel_out(d) : NULL;
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (!o || i >= d->res_list->len) return;
    int w, h;
    sscanf(g_ptr_array_index(d->res_list, i), "%dx%d", &w, &h);
    /* The highest refresh rate this size offers: a person choosing a
     * resolution has not also chosen to drop to 30Hz. */
    int best = -1;
    for (guint k = 0; k < o->modes->len; k++) {
        mode_t_ *m = &g_array_index(o->modes, mode_t_, k);
        if (m->w == w && m->h == h &&
            (best < 0 || m->mhz > g_array_index(o->modes, mode_t_, best).mhz))
            best = k;
    }
    if (best >= 0) apply_risky(d, best, o->transform);
}

static void on_rate(GObject *dd, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    disp_t *d = DP;
    out_t *o = d ? sel_out(d) : NULL;
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (!o || o->cur < 0 || i >= d->rate_list->len) return;
    const mode_t_ *cur = &g_array_index(o->modes, mode_t_, o->cur);
    int mhz = g_array_index(d->rate_list, int, i);
    for (guint k = 0; k < o->modes->len; k++) {
        mode_t_ *m = &g_array_index(o->modes, mode_t_, k);
        if (m->w == cur->w && m->h == cur->h && m->mhz == mhz) {
            apply_risky(d, k, o->transform);
            return;
        }
    }
}

static void on_rotation(GObject *dd, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    disp_t *d = DP;
    out_t *o = d ? sel_out(d) : NULL;
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (!o || i >= 4) return;
    apply_risky(d, o->cur, TRANSFORMS[i]);
}

static void on_scale(GObject *dd, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    disp_t *d = DP;
    out_t *o = d ? sel_out(d) : NULL;
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (!o || i >= G_N_ELEMENTS(SCALES)) return;
    char sc[32];
    g_ascii_formatd(sc, sizeof sc, "%.2f", SCALES[i]);
    const char *v[] = { "wlr-randr", "--output", o->name, "--scale", sc, NULL };
    if (randr(v)) {
        o->scale = SCALES[i];
        persist(o);
        lp_toast(FALSE, T("%s is now at %d%%", "%s 을(를) %d%% 로 바꿨습니다"), o->name,
                 (int)lround(SCALES[i] * 100));
        if (d->arrange) gtk_widget_queue_draw(d->arrange);
    }
}

static void on_enabled(GObject *sw, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    disp_t *d = DP;
    out_t *o = d ? sel_out(d) : NULL;
    if (!o) return;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    int enabled = 0;
    for (guint i = 0; i < d->outs->len; i++)
        enabled += ((out_t *)g_ptr_array_index(d->outs, i))->enabled;
    if (!on && enabled <= 1) {
        lp_toast(TRUE, T("This is the only display that is on; it stays on.",
                         "켜져 있는 화면이 이것 하나라서 끌 수 없습니다."));
        LP_QUIET(gtk_switch_set_active(GTK_SWITCH(sw), TRUE));
        return;
    }
    const char *v[] = { "wlr-randr", "--output", o->name, on ? "--on" : "--off", NULL };
    if (randr(v)) {
        o->enabled = on;
        if (on) {
            persist(o);
            lp_toast(FALSE, T("%s is on", "%s 을(를) 켰습니다"), o->name);
        } else {
            persist(o);
            lp_toast(FALSE, T("%s is off", "%s 을(를) 껐습니다"), o->name);
        }
        reload(d);
    } else {
        LP_QUIET(gtk_switch_set_active(GTK_SWITCH(sw), !on));
    }
}

/* ── arrangement ────────────────────────────────────────────────────── */

static void arrange_draw(GtkDrawingArea *a, cairo_t *cr, int W, int H, gpointer p)
{
    (void)a; (void)p;
    disp_t *d = DP;
    if (!d) return;
    double minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
    for (guint i = 0; i < d->outs->len; i++) {
        out_t *o = g_ptr_array_index(d->outs, i);
        if (!o->enabled) continue;
        double w, h;
        logical_size(o, &w, &h);
        minx = MIN(minx, o->x); miny = MIN(miny, o->y);
        maxx = MAX(maxx, o->x + w); maxy = MAX(maxy, o->y + h);
    }
    if (maxx <= minx) return;
    double pad = 24;
    d->view_scale = MIN((W - 2 * pad) / (maxx - minx), (H - 2 * pad) / (maxy - miny)) * 0.8;
    d->view_ox = (W - (maxx - minx) * d->view_scale) / 2 - minx * d->view_scale;
    d->view_oy = (H - (maxy - miny) * d->view_scale) / 2 - miny * d->view_scale;

    GdkRGBA fg;
    gtk_style_context_get_color(gtk_widget_get_style_context(GTK_WIDGET(a)), &fg);
    for (guint i = 0; i < d->outs->len; i++) {
        out_t *o = g_ptr_array_index(d->outs, i);
        if (!o->enabled) continue;
        double w, h;
        logical_size(o, &w, &h);
        double x = d->view_ox + o->x * d->view_scale, y = d->view_oy + o->y * d->view_scale;
        double rw = w * d->view_scale, rh = h * d->view_scale;
        cairo_rectangle(cr, x + 2, y + 2, rw - 4, rh - 4);
        if ((int)i == d->sel)
            cairo_set_source_rgba(cr, 0.91, 0.33, 0.13, 0.85);
        else
            cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.18);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.8);
        cairo_set_line_width(cr, 2);
        cairo_stroke(cr);

        PangoLayout *pl = gtk_widget_create_pango_layout(GTK_WIDGET(a), NULL);
        char *t = g_strdup_printf("%u\n%s", i + 1, o->name);
        pango_layout_set_text(pl, t, -1);
        pango_layout_set_alignment(pl, PANGO_ALIGN_CENTER);
        int tw, th;
        pango_layout_get_pixel_size(pl, &tw, &th);
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 1);
        cairo_move_to(cr, x + (rw - tw) / 2, y + (rh - th) / 2);
        pango_cairo_show_layout(cr, pl);
        g_object_unref(pl);
        g_free(t);
    }
}

static int hit(disp_t *d, double px, double py)
{
    for (guint i = 0; i < d->outs->len; i++) {
        out_t *o = g_ptr_array_index(d->outs, i);
        if (!o->enabled) continue;
        double w, h;
        logical_size(o, &w, &h);
        double x = d->view_ox + o->x * d->view_scale, y = d->view_oy + o->y * d->view_scale;
        if (px >= x && px <= x + w * d->view_scale && py >= y && py <= y + h * d->view_scale)
            return i;
    }
    return -1;
}

static void drag_begin(GtkGestureDrag *g, double x, double y, gpointer p)
{
    (void)g; (void)p;
    disp_t *d = DP;
    d->drag = hit(d, x, y);
    if (d->drag >= 0) {
        out_t *o = g_ptr_array_index(d->outs, d->drag);
        d->drag_x0 = o->x;
        d->drag_y0 = o->y;
        if (d->sel != d->drag) {
            d->sel = d->drag;
            rebuild_controls(d);
        }
    }
}

static void drag_update(GtkGestureDrag *g, double dx, double dy, gpointer p)
{
    (void)g; (void)p;
    disp_t *d = DP;
    if (d->drag < 0) return;
    out_t *o = g_ptr_array_index(d->outs, d->drag);
    o->x = (int)(d->drag_x0 + dx / d->view_scale);
    o->y = (int)(d->drag_y0 + dy / d->view_scale);
    gtk_widget_queue_draw(d->arrange);
}

/* Where it was dropped becomes "left of / right of / above / below" the
 * nearest other display, edges touching, tops or left edges aligned -
 * wlroots leaves a gap between outputs as a gap the pointer cannot cross,
 * so a drop is never kept as dropped. Then everything is shifted so the
 * top-left display is at 0,0. */
static void drag_end(GtkGestureDrag *g, double dx, double dy, gpointer p)
{
    (void)g; (void)dx; (void)dy; (void)p;
    disp_t *d = DP;
    if (d->drag < 0) return;
    out_t *m = g_ptr_array_index(d->outs, d->drag);
    double mw, mh;
    logical_size(m, &mw, &mh);

    out_t *best = NULL;
    double bestd = 1e18;
    for (guint i = 0; i < d->outs->len; i++) {
        out_t *o = g_ptr_array_index(d->outs, i);
        if ((int)i == d->drag || !o->enabled) continue;
        double ow, oh;
        logical_size(o, &ow, &oh);
        double cx = (o->x + ow / 2) - (m->x + mw / 2), cy = (o->y + oh / 2) - (m->y + mh / 2);
        if (cx * cx + cy * cy < bestd) { bestd = cx * cx + cy * cy; best = o; }
    }
    if (best) {
        double ow, oh;
        logical_size(best, &ow, &oh);
        double cx = (m->x + mw / 2) - (best->x + ow / 2);
        double cy = (m->y + mh / 2) - (best->y + oh / 2);
        if (fabs(cx) / (ow + mw) >= fabs(cy) / (oh + mh)) {
            m->x = cx > 0 ? best->x + (int)ow : best->x - (int)mw;
            m->y = best->y;
        } else {
            m->y = cy > 0 ? best->y + (int)oh : best->y - (int)mh;
            m->x = best->x;
        }
    }
    int minx = G_MAXINT, miny = G_MAXINT;
    for (guint i = 0; i < d->outs->len; i++) {
        out_t *o = g_ptr_array_index(d->outs, i);
        if (!o->enabled) continue;
        minx = MIN(minx, o->x); miny = MIN(miny, o->y);
    }
    gboolean ok = TRUE;
    for (guint i = 0; i < d->outs->len; i++) {
        out_t *o = g_ptr_array_index(d->outs, i);
        if (!o->enabled) continue;
        o->x -= minx; o->y -= miny;
        char *pos = g_strdup_printf("%d,%d", o->x, o->y);
        const char *v[] = { "wlr-randr", "--output", o->name, "--pos", pos, NULL };
        ok &= randr(v);
        g_free(pos);
        persist(o);
    }
    if (ok)
        lp_toast(FALSE, T("Arranged the displays", "화면 배치를 바꿨습니다"));
    d->drag = -1;
    gtk_widget_queue_draw(d->arrange);
}

/* ── brightness ─────────────────────────────────────────────────────── */

static void on_brightness(GtkRange *r, gpointer p)
{
    (void)p;
    char v[16];
    g_snprintf(v, sizeof v, "%d", (int)lround(gtk_range_get_value(r)));
    /* lp-tune is the only program that writes the backlight (COMMON.md):
     * it clamps, it knows which of the two backlight devices on an Optimus
     * laptop is the real one, it glides to the new level by itself, and it
     * remembers the level across boots. The slider calls this at most 30
     * times a second while the finger moves (ui.c), and lp_run_latest keeps
     * one lp-tune running at a time with only the newest value waiting. */
    const char *a[] = { "lp-tune", "brightness", "set", v, NULL };
    lp_run_latest("brightness", a);
}

static char *pct(double v) { return g_strdup_printf("%d%%", (int)lround(v)); }

/* ── night light ────────────────────────────────────────────────────── */

static void stop_wlsunset(void)
{
    /* Only this user's, found by name in /proc rather than pkill, so that
     * nobody else's process with a similar command line is touched. */
    GDir *dir = g_dir_open("/proc", 0, NULL);
    if (!dir) return;
    const char *n;
    while ((n = g_dir_read_name(dir))) {
        if (!g_ascii_isdigit(n[0])) continue;
        char *p = g_strdup_printf("/proc/%s/comm", n);
        char *comm = lp_slurp(p);
        g_free(p);
        if (comm && !strcmp(comm, "wlsunset")) {
            char *sp = g_strdup_printf("/proc/%s", n);
            GStatBuf st;
            if (g_stat(sp, &st) == 0 && st.st_uid == getuid())
                kill((pid_t)atoi(n), SIGTERM);
            g_free(sp);
        }
        g_free(comm);
    }
    g_dir_close(dir);
}

static char **night_argv(void)
{
    char *conf = lp_config_path("nightlight.conf");
    char *en = kv_get(conf, "enabled");
    int temp = kv_get_int(conf, "temperature", 4000);
    char *from = kv_get(conf, "from");
    char *to = kv_get(conf, "to");
    char **argv = NULL;
    if (en && !strcmp(en, "yes")) {
        char *t = g_strdup_printf("%d", CLAMP(temp, 1000, 6400));
        argv = g_new0(char *, 10);
        int i = 0;
        argv[i++] = g_strdup("wlsunset");
        argv[i++] = g_strdup("-t"); argv[i++] = t;
        argv[i++] = g_strdup("-T"); argv[i++] = g_strdup("6500");
        /* -s is when it gets warm (sunset), -S when it goes back (sunrise). */
        argv[i++] = g_strdup("-s"); argv[i++] = g_strdup(from ? from : "20:00");
        argv[i++] = g_strdup("-S"); argv[i++] = g_strdup(to ? to : "07:00");
    }
    g_free(en); g_free(from); g_free(to); g_free(conf);
    return argv;
}

static void night_apply(void)
{
    stop_wlsunset();
    char **argv = night_argv();
    if (argv) {
        if (lp_spawn_bg((const char *const *)argv))
            lp_toast(FALSE, T("Night light is on", "야간 모드를 켰습니다"));
        g_strfreev(argv);
    } else {
        lp_toast(FALSE, T("Night light is off", "야간 모드를 껐습니다"));
    }
}

/* `lp-settings --apply-night-light`: becomes wlsunset, or exits 0 when
 * night light is off. */
int lp_night_light_exec(void)
{
    stop_wlsunset();
    char **v = night_argv();
    if (!v) return 0;
    execvp(v[0], v);
    g_printerr("lp-settings: cannot run wlsunset: %s\n", g_strerror(errno));
    return 1;
}

/* At login (lp-settings --restore): start it in the background. */
static void restore(void)
{
    stop_wlsunset();
    char **v = night_argv();
    if (v) {
        GError *e = NULL;
        if (!g_spawn_async(NULL, v, NULL, G_SPAWN_SEARCH_PATH |
                           G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                           (GSpawnChildSetupFunc)(void (*)(void))setsid, NULL, NULL, &e)) {
            g_printerr("lp-settings: cannot run wlsunset: %s\n", e->message);
            g_error_free(e);
        }
        g_strfreev(v);
    }
}

static void on_night(GObject *sw, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    char *conf = lp_config_path("nightlight.conf");
    kv_set(conf, "enabled", gtk_switch_get_active(GTK_SWITCH(sw)) ? "yes" : "no");
    g_free(conf);
    night_apply();
}

static void on_night_temp(GtkRange *r, gpointer p)
{
    (void)p;
    char *conf = lp_config_path("nightlight.conf");
    char v[16];
    g_snprintf(v, sizeof v, "%d", (int)(lround(gtk_range_get_value(r) / 100.0) * 100));
    kv_set(conf, "temperature", v);
    char *en = kv_get(conf, "enabled");
    if (en && !strcmp(en, "yes")) night_apply();
    g_free(en);
    g_free(conf);
}

static char *kelvin(double v) { return g_strdup_printf("%dK", (int)(lround(v / 100.0) * 100)); }

static const char *const HOURS[] = { "18:00", "19:00", "20:00", "21:00", "22:00", "23:00", NULL };
static const char *const MORNINGS[] = { "05:00", "06:00", "07:00", "08:00", "09:00", NULL };

static void on_night_time(GObject *dd, GParamSpec *ps, gpointer p)
{
    (void)ps;
    const char *key = p;
    const char *const *list = !strcmp(key, "from") ? HOURS : MORNINGS;
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    char *conf = lp_config_path("nightlight.conf");
    kv_set(conf, key, list[i]);
    char *en = kv_get(conf, "enabled");
    if (en && !strcmp(en, "yes")) night_apply();
    else lp_toast(FALSE, T("Saved", "저장했습니다"));
    g_free(en);
    g_free(conf);
}

static guint index_of(const char *const *list, const char *v, guint dflt)
{
    for (guint i = 0; list[i]; i++)
        if (!g_strcmp0(list[i], v)) return i;
    return dflt;
}

/* ── building ───────────────────────────────────────────────────────── */

static void on_pick_output(GObject *dd, GParamSpec *ps, gpointer p)
{
    (void)ps; (void)p;
    if (!DP) return;
    DP->sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    rebuild_controls(DP);
    if (DP->arrange) gtk_widget_queue_draw(DP->arrange);
}

static gint by_size_desc(gconstpointer a, gconstpointer b)
{
    int aw, ah, bw, bh;
    sscanf(*(char *const *)a, "%dx%d", &aw, &ah);
    sscanf(*(char *const *)b, "%dx%d", &bw, &bh);
    return (bw * bh) - (aw * ah);
}

static void rebuild_controls(disp_t *d)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(d->controls)))
        gtk_list_box_remove(GTK_LIST_BOX(d->controls), c);
    out_t *o = sel_out(d);
    if (!o) return;

    if (d->outs->len > 1)
        row_switch(d->controls, T("Use this display", "이 화면 사용"), NULL, o->enabled,
                   G_CALLBACK(on_enabled), NULL);
    if (!o->enabled || o->cur < 0)
        return;

    const mode_t_ *cur = &g_array_index(o->modes, mode_t_, o->cur);

    /* Resolutions: every distinct size, largest first. */
    if (d->res_list) g_ptr_array_free(d->res_list, TRUE);
    d->res_list = g_ptr_array_new_with_free_func(g_free);
    for (guint k = 0; k < o->modes->len; k++) {
        mode_t_ *m = &g_array_index(o->modes, mode_t_, k);
        char *s = g_strdup_printf("%dx%d", m->w, m->h);
        gboolean dup = FALSE;
        for (guint j = 0; j < d->res_list->len && !dup; j++)
            dup = !strcmp(g_ptr_array_index(d->res_list, j), s);
        if (dup) g_free(s); else g_ptr_array_add(d->res_list, s);
    }
    g_ptr_array_sort(d->res_list, by_size_desc);
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    guint rsel = 0;
    for (guint j = 0; j < d->res_list->len; j++) {
        int w, h;
        sscanf(g_ptr_array_index(d->res_list, j), "%dx%d", &w, &h);
        gboolean pref = FALSE;
        for (guint k = 0; k < o->modes->len; k++) {
            mode_t_ *m = &g_array_index(o->modes, mode_t_, k);
            if (m->w == w && m->h == h && m->preferred) pref = TRUE;
        }
        g_ptr_array_add(labels, g_strdup_printf(pref ? T("%d × %d (recommended)", "%d × %d (권장)")
                                                     : "%d × %d", w, h));
        if (w == cur->w && h == cur->h) rsel = j;
    }
    g_ptr_array_add(labels, NULL);
    GtkWidget *r = row_choice(d->controls, T("Resolution", "해상도"), NULL,
                              (const char *const *)labels->pdata, rsel,
                              G_CALLBACK(on_resolution), NULL);
    d->res_dd = row_control(r);
    g_ptr_array_free(labels, TRUE);

    /* Refresh rates for the current size. */
    if (d->rate_list) g_array_free(d->rate_list, TRUE);
    d->rate_list = g_array_new(FALSE, FALSE, sizeof(int));
    labels = g_ptr_array_new_with_free_func(g_free);
    guint fsel = 0;
    for (guint k = 0; k < o->modes->len; k++) {
        mode_t_ *m = &g_array_index(o->modes, mode_t_, k);
        if (m->w != cur->w || m->h != cur->h) continue;
        gboolean dup = FALSE;
        for (guint j = 0; j < d->rate_list->len && !dup; j++)
            dup = g_array_index(d->rate_list, int, j) == m->mhz;
        if (dup) continue;
        if (m->mhz == cur->mhz) fsel = d->rate_list->len;
        g_array_append_val(d->rate_list, m->mhz);
        g_ptr_array_add(labels, g_strdup_printf("%.2f Hz", m->mhz / 1000.0));
    }
    g_ptr_array_add(labels, NULL);
    r = row_choice(d->controls, T("Refresh rate", "주사율"), NULL,
                   (const char *const *)labels->pdata, fsel, G_CALLBACK(on_rate), NULL);
    d->rate_dd = row_control(r);
    g_ptr_array_free(labels, TRUE);

    /* Scale. */
    double rec = recommended_scale(o);
    labels = g_ptr_array_new_with_free_func(g_free);
    guint ssel = 0;
    for (guint k = 0; k < G_N_ELEMENTS(SCALES); k++) {
        int pc = (int)lround(SCALES[k] * 100);
        g_ptr_array_add(labels, fabs(SCALES[k] - rec) < 0.01
                                ? g_strdup_printf(T("%d%% (recommended)", "%d%% (권장)"), pc)
                                : g_strdup_printf("%d%%", pc));
        if (fabs(SCALES[k] - o->scale) < fabs(SCALES[ssel] - o->scale)) ssel = k;
    }
    g_ptr_array_add(labels, NULL);
    row_choice(d->controls, T("Scale", "배율"),
               T("How big text and controls are", "글자와 단추의 크기"),
               (const char *const *)labels->pdata, ssel, G_CALLBACK(on_scale), NULL);
    g_ptr_array_free(labels, TRUE);

    const char *rot[] = { T("Landscape", "가로"), T("Portrait (right)", "세로 (오른쪽)"),
                          T("Landscape (flipped)", "가로 (뒤집기)"), T("Portrait (left)", "세로 (왼쪽)"),
                          NULL };
    row_choice(d->controls, T("Rotation", "방향"), NULL, rot,
               index_of(TRANSFORMS, o->transform, 0), G_CALLBACK(on_rotation), NULL);
}

static void on_randr(int st, const char *out, const char *err, gpointer p)
{
    disp_t *d = p;
    GtkWidget *page = d->page;
    if (st != 0) {
        char *why = st == -1 ? g_strdup(T("wlr-randr is not installed", "wlr-randr 가 설치되어 있지 않습니다"))
                             : lp_first_line(err, out);
        page_set_subtitle(page, T("Could not read the displays", "화면 정보를 읽지 못했습니다"));
        row_value(d->controls, T("Displays", "화면"), why, NULL);
        g_free(why);
        return;
    }
    g_ptr_array_free(d->outs, TRUE);
    d->outs = parse_randr(out);
    if (d->sel >= (int)d->outs->len) d->sel = 0;
    /* The internal panel first when nothing was chosen. */
    for (guint i = 0; d->sel < 0 && i < d->outs->len; i++)
        if (is_internal(g_ptr_array_index(d->outs, i))) d->sel = i;
    if (d->sel < 0) d->sel = 0;

    GString *sub = g_string_new(NULL);
    for (guint i = 0; i < d->outs->len; i++) {
        out_t *o = g_ptr_array_index(d->outs, i);
        if (!o->enabled || o->cur < 0) continue;
        mode_t_ *m = &g_array_index(o->modes, mode_t_, o->cur);
        if (sub->len) g_string_append(sub, "  ·  ");
        g_string_append_printf(sub, "%s %d × %d, %.0f Hz, %d%%", o->name, m->w, m->h,
                               m->mhz / 1000.0, (int)lround(o->scale * 100));
    }
    page_set_subtitle(page, sub->str);
    g_string_free(sub, TRUE);

    GtkWidget *pick = g_object_get_data(G_OBJECT(page), "lp-pick");
    if (d->outs->len > 1 && pick) {
        GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
        for (guint i = 0; i < d->outs->len; i++) {
            out_t *o = g_ptr_array_index(d->outs, i);
            g_ptr_array_add(names, g_strdup_printf("%u · %s%s%s", i + 1, o->name,
                                                   is_internal(o) ? T(" (built-in)", " (내장)") : "",
                                                   o->enabled ? "" : T(" - off", " - 꺼짐")));
        }
        g_ptr_array_add(names, NULL);
        GtkWidget *list = gtk_widget_get_parent(pick);
        (void)list;
        GtkWidget *row = row_choice(NULL, T("Display", "화면"), NULL,
                                    (const char *const *)names->pdata, d->sel,
                                    G_CALLBACK(on_pick_output), NULL);
        gtk_list_box_append(GTK_LIST_BOX(pick), row);
        g_ptr_array_free(names, TRUE);
        gtk_widget_set_visible(pick, TRUE);
        gtk_widget_set_visible(d->arrange, TRUE);
        GtkWidget *head = g_object_get_data(G_OBJECT(page), "lp-arr-head");
        gtk_widget_set_visible(head, TRUE);
    }
    rebuild_controls(d);
    gtk_widget_queue_draw(d->arrange);
}

static void reload(disp_t *d)
{
    GtkWidget *pick = g_object_get_data(G_OBJECT(d->page), "lp-pick");
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(pick)))
        gtk_list_box_remove(GTK_LIST_BOX(pick), c);
    static const char *const v[] = { "wlr-randr", NULL };
    lp_run_async(v, NULL, d->page, on_randr, d);
}

static void on_tune(int st, const char *out, const char *err, gpointer p)
{
    (void)err;
    GtkWidget *row = p;
    jnode_t *j = st == 0 ? json_parse(out) : NULL;
    double b = json_num(j, "brightness", -1);
    GtkWidget *sc = row_control(row);
    if (b >= 0) {
        LP_QUIET(gtk_range_set_value(GTK_RANGE(sc), b));
        gtk_widget_set_sensitive(sc, TRUE);
        row_set_detail(row, NULL);
    } else {
        row_set_detail(row, st == -1 ? T("lp-tune is not installed", "lp-tune 이 설치되어 있지 않습니다")
                                     : T("This display has no adjustable backlight",
                                         "이 화면에는 조절할 수 있는 백라이트가 없습니다"));
    }
    json_free(j);
}

static GtkWidget *build(void)
{
    disp_t *d = g_new0(disp_t, 1);
    DP = d;
    d->sel = -1;
    d->drag = -1;
    d->outs = g_ptr_array_new_with_free_func(out_free);
    d->page = page_new(T("Display", "화면"), T("Reading the displays…", "화면을 읽는 중…"));
    g_object_set_data_full(G_OBJECT(d->page), "lp-display", d, disp_free);

    GtkWidget *head = gtk_label_new(T("Arrangement - drag a display to where it sits on the desk",
                                      "배치 - 책상 위에 놓인 자리로 화면을 끌어 옮기십시오"));
    gtk_widget_add_css_class(head, "lp-heading");
    gtk_widget_set_halign(head, GTK_ALIGN_START);
    gtk_widget_set_margin_top(head, 22);
    gtk_widget_set_margin_bottom(head, 8);
    gtk_widget_set_visible(head, FALSE);
    gtk_box_append(GTK_BOX(d->page), head);
    g_object_set_data(G_OBJECT(d->page), "lp-arr-head", head);

    d->arrange = gtk_drawing_area_new();
    gtk_widget_add_css_class(d->arrange, "lp-arrange");
    gtk_widget_set_size_request(d->arrange, -1, 240);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(d->arrange), arrange_draw, NULL, NULL);
    GtkGesture *g = gtk_gesture_drag_new();
    g_signal_connect(g, "drag-begin", G_CALLBACK(drag_begin), NULL);
    g_signal_connect(g, "drag-update", G_CALLBACK(drag_update), NULL);
    g_signal_connect(g, "drag-end", G_CALLBACK(drag_end), NULL);
    gtk_widget_add_controller(d->arrange, GTK_EVENT_CONTROLLER(g));
    gtk_widget_set_visible(d->arrange, FALSE);
    gtk_box_append(GTK_BOX(d->page), d->arrange);

    GtkWidget *pick = group_new(d->page, NULL);
    gtk_widget_set_visible(pick, FALSE);
    g_object_set_data(G_OBJECT(d->page), "lp-pick", pick);

    d->controls = group_new(d->page, NULL);
    page_note(d->page, T("Mirroring one display onto another is not available: this compositor "
                         "can only extend the desktop across displays.",
                         "화면 복제는 지원하지 않습니다. 이 컴포지터는 화면을 이어 붙이는 것만 "
                         "할 수 있습니다."));

    GtkWidget *g2 = group_new(d->page, T("Brightness and colour", "밝기와 색"));
    GtkWidget *br = row_scale(g2, T("Brightness", "밝기"), NULL, 1, 100, 1, 50,
                              G_CALLBACK(on_brightness), NULL);
    g_object_set_data(G_OBJECT(row_control(br)), "lp-fmt", (gpointer)pct);
    gtk_widget_set_sensitive(row_control(br), FALSE);
    static const char *const tv[] = { "lp-tune", "status", "--json", NULL };
    lp_run_async(tv, NULL, br, on_tune, br);

    char *conf = lp_config_path("nightlight.conf");
    char *en = kv_get(conf, "enabled");
    int temp = kv_get_int(conf, "temperature", 4000);
    char *from = kv_get(conf, "from"), *to = kv_get(conf, "to");
    row_switch(g2, T("Night light", "야간 모드"),
               T("Warmer colours in the evening, easier on the eyes at night",
                 "저녁에 화면 색을 따뜻하게 해서 밤에 눈이 덜 피로합니다"),
               en && !strcmp(en, "yes"), G_CALLBACK(on_night), NULL);
    GtkWidget *tr = row_scale(g2, T("Night light warmth", "야간 모드 색온도"),
                              T("Lower is warmer", "낮을수록 따뜻합니다"),
                              2500, 6000, 100, temp, G_CALLBACK(on_night_temp), NULL);
    g_object_set_data(G_OBJECT(row_control(tr)), "lp-fmt", (gpointer)kelvin);
    LP_QUIET(gtk_range_set_value(GTK_RANGE(row_control(tr)), temp + 1));
    LP_QUIET(gtk_range_set_value(GTK_RANGE(row_control(tr)), temp));
    row_choice(g2, T("Night light from", "야간 모드 시작"), NULL, HOURS,
               index_of(HOURS, from, 2), G_CALLBACK(on_night_time), (gpointer)"from");
    row_choice(g2, T("Night light until", "야간 모드 끝"), NULL, MORNINGS,
               index_of(MORNINGS, to, 2), G_CALLBACK(on_night_time), (gpointer)"to");
    g_free(en); g_free(from); g_free(to); g_free(conf);

    reload(d);
    return d->page;
}

static const char *const KEYS[] = {
    "Resolution", "해상도",
    "Refresh rate", "주사율",
    "Scale", "배율",
    "Rotation", "방향",
    "Use this display", "이 화면 사용",
    "Brightness", "밝기",
    "Night light", "야간 모드",
    "Monitor", "모니터",
    "Arrangement", "배치",
    NULL
};

const lp_panel_t lp_panel_display = {
    "display", "Display", "화면", "video-display-symbolic", build, KEYS, restore
};
