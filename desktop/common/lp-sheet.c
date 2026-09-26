/*
 * lp-sheet.c - see lp-sheet.h. An edge sheet that slides on a spring.
 *
 * The surface is created once at its open size and never resized. The
 * content inside it is a windowless GtkBox whose "draw" we own: it
 * cairo_translate()s by the current spring offset and propagates the draw
 * to the real content, so the pixels move without a single layer-shell
 * configure. The only per-frame change to the compositor is the input
 * region, which is a client-side region op with no round-trip.
 *
 * The travel is one LP_SPRING_SHEET. Because a spring keeps its velocity
 * across a target change, a half-open sheet that is told to close turns
 * round from where it is; a finger can fling it (lp_sheet_release), and a
 * drag pins it frame by frame (lp_sheet_drag). When it settles shut the
 * window is hidden, so a closed sheet captures no input and, with the
 * tick callback gone, costs nothing.
 *
 * Input mid-slide: the content keeps its allocation at the open position
 * while it is drawn shifted, so a touch lands on the widget under the
 * finger only once the sheet is at rest (offset 0). That is deliberate -
 * allocating the content afresh each frame to keep input pixel-true would
 * be the per-frame relayout COMMON.md's Motion section forbids, and a
 * 260 ms slide is not something you aim at.
 */
#include "lp-sheet.h"
#include "lp-shell.h"
#include "lp-motion.h"

struct _LpSheet {
    GtkWindow    *win;
    GtkWidget    *slider;      /* windowless box; we own its draw          */
    GtkWidget    *content;
    int           edge;        /* one LP_EDGE_* : the edge it slides from   */
    LpSpring      open;        /* 0 hidden .. 1 shown                       */
    LpMotion     *motion;
    gboolean      want_shown;  /* the last target the caller asked for      */
    gboolean      pending_closed;  /* fire closed_cb once when it next hides */
    LpSheetClosed closed_cb;
    gpointer      closed_data;
};

static gboolean sheet_vertical(const LpSheet *sh)
{
    return sh->edge == LP_EDGE_TOP || sh->edge == LP_EDGE_BOTTOM;
}

/* The surface's size along the slide axis: how far the content travels. */
static int sheet_extent(LpSheet *sh)
{
    return sheet_vertical(sh)
        ? gtk_widget_get_allocated_height(sh->slider)
        : gtk_widget_get_allocated_width(sh->slider);
}

/* Pixels to translate the content by for open fraction x. Under reduced
 * motion the content does not travel (it fades instead), so the offset is
 * zero and the sheet appears in place. */
static void sheet_offset(LpSheet *sh, double x, double *ox, double *oy)
{
    *ox = 0.0;
    *oy = 0.0;
    if (lp_motion_reduced())
        return;
    double e = sheet_extent(sh);
    switch (sh->edge) {
    case LP_EDGE_TOP:    *oy = (x - 1.0) * e; break;
    case LP_EDGE_BOTTOM: *oy = (1.0 - x) * e; break;
    case LP_EDGE_LEFT:   *ox = (x - 1.0) * e; break;
    case LP_EDGE_RIGHT:  *ox = (1.0 - x) * e; break;
    default: break;
    }
}

static gboolean on_slider_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    LpSheet *sh = data;
    double ox, oy;
    sheet_offset(sh, sh->open.x, &ox, &oy);
    cairo_translate(cr, ox, oy);
    if (sh->content)
        gtk_container_propagate_draw(GTK_CONTAINER(w), sh->content, cr);
    return TRUE;   /* the child has been drawn; skip the default box draw */
}

/* Accept touches only on the part of the surface the content actually
 * covers right now. Cheap - no configure round-trip - unlike moving the
 * layer-shell margin would be. */
static void sheet_apply_input(LpSheet *sh)
{
    GtkWidget *win = GTK_WIDGET(sh->win);
    if (!gtk_widget_get_realized(win))
        return;
    int W = gtk_widget_get_allocated_width(win);
    int H = gtk_widget_get_allocated_height(win);
    cairo_rectangle_int_t r = { 0, 0, W, H };
    if (!lp_motion_reduced()) {
        double x = sh->open.x < 0.0 ? 0.0 : (sh->open.x > 1.0 ? 1.0 : sh->open.x);
        int e = sheet_extent(sh);
        int vis = (int)(x * e + 0.5);
        switch (sh->edge) {
        case LP_EDGE_TOP:    r.x = 0;     r.y = 0;     r.width = W;   r.height = vis; break;
        case LP_EDGE_BOTTOM: r.x = 0;     r.y = H-vis; r.width = W;   r.height = vis; break;
        case LP_EDGE_LEFT:   r.x = 0;     r.y = 0;     r.width = vis; r.height = H;   break;
        case LP_EDGE_RIGHT:  r.x = W-vis; r.y = 0;     r.width = vis; r.height = H;   break;
        default: break;
        }
    }
    cairo_region_t *reg = cairo_region_create_rectangle(&r);
    gtk_widget_input_shape_combine_region(win, reg);
    cairo_region_destroy(reg);
}

/* One tick callback drives the whole sheet: redraw the shifted content,
 * retrack the input region, and when it has come fully to rest shut, hide
 * the window and tell whoever asked to be told. */
static void on_frame(GtkWidget *w, gpointer data)
{
    (void)w;
    LpSheet *sh = data;
    if (lp_motion_reduced())
        gtk_widget_set_opacity(GTK_WIDGET(sh->win), sh->open.x);
    gtk_widget_queue_draw(sh->slider);
    sheet_apply_input(sh);
    if (!sh->open.moving && sh->open.x <= 0.001 &&
        gtk_widget_get_visible(GTK_WIDGET(sh->win))) {
        gtk_widget_hide(GTK_WIDGET(sh->win));
        if (sh->pending_closed && sh->closed_cb)
            sh->closed_cb(sh, sh->closed_data);
        sh->pending_closed = FALSE;
    }
}

LpSheet *lp_sheet_new(const char *ns, int edge, GtkWidget *content)
{
    LpSheet *sh = g_new0(LpSheet, 1);
    sh->edge = edge;
    sh->content = content;
    sh->win = lp_layer_window(ns, GTK_LAYER_SHELL_LAYER_OVERLAY, edge);

    sh->slider = gtk_box_new(sheet_vertical(sh) ? GTK_ORIENTATION_VERTICAL
                                                : GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_app_paintable(sh->slider, TRUE);
    g_signal_connect(sh->slider, "draw", G_CALLBACK(on_slider_draw), sh);
    gtk_box_pack_start(GTK_BOX(sh->slider), content, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(sh->win), sh->slider);

    lp_spring_init(&sh->open, LP_SPRING_SHEET, 0.0);
    sh->motion = lp_motion_new(sh->slider, on_frame, sh);
    lp_motion_add(sh->motion, &sh->open);
    return sh;
}

GtkWindow *lp_sheet_window(LpSheet *sh) { return sh->win; }

gboolean lp_sheet_shown(LpSheet *sh) { return sh->want_shown; }

void lp_sheet_show(LpSheet *sh)
{
    sh->want_shown = TRUE;
    sh->pending_closed = TRUE;   /* so the eventual hide reports closed */
    gtk_widget_set_opacity(GTK_WIDGET(sh->win),
                           lp_motion_reduced() ? sh->open.x : 1.0);
    gtk_widget_show_all(GTK_WIDGET(sh->win));
    lp_spring_set_target(&sh->open, 1.0);
    lp_motion_kick(sh->motion);
}

void lp_sheet_hide(LpSheet *sh)
{
    sh->want_shown = FALSE;
    lp_spring_set_target_out(&sh->open, 0.0);
    lp_motion_kick(sh->motion);
}

void lp_sheet_toggle(LpSheet *sh)
{
    if (sh->want_shown)
        lp_sheet_hide(sh);
    else
        lp_sheet_show(sh);
}

void lp_sheet_drag(LpSheet *sh, double fraction)
{
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;
    sh->want_shown = fraction > 0.0;
    if (!gtk_widget_get_visible(GTK_WIDGET(sh->win))) {
        gtk_widget_set_opacity(GTK_WIDGET(sh->win),
                               lp_motion_reduced() ? fraction : 1.0);
        gtk_widget_show_all(GTK_WIDGET(sh->win));
    }
    lp_spring_jump(&sh->open, fraction);
    lp_motion_kick(sh->motion);   /* draws once, even with nothing to animate */
}

void lp_sheet_release(LpSheet *sh, double velocity)
{
    double land = lp_spring_project(sh->open.x, velocity);
    double target = land >= 0.5 ? 1.0 : 0.0;
    sh->want_shown = target > 0.5;
    if (target == 0.0)
        sh->pending_closed = TRUE;
    lp_spring_fling(&sh->open, target, velocity);
    lp_motion_kick(sh->motion);
}

void lp_sheet_on_closed(LpSheet *sh, LpSheetClosed cb, gpointer data)
{
    sh->closed_cb = cb;
    sh->closed_data = data;
}

void lp_sheet_free(LpSheet *sh)
{
    if (!sh)
        return;
    lp_motion_free(sh->motion);
    if (sh->win)
        gtk_widget_destroy(GTK_WIDGET(sh->win));
    g_free(sh);
}
