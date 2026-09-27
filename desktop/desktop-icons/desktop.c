/*
 * desktop.c - lp-desktop, the wallpaper and the icons on it.
 *
 *     ┌────────────────────────────────────────────────┐
 *     │ bar                                            │
 *     ├──┬─────────────────────────────────────────────┤
 *     │  │ [Documents]                                 │
 *     │d │ [Welcome.txt]                               │
 *     │o │ [hello.sh]                                  │
 *     │c │                  (wallpaper)                │
 *     │k │                                             │
 *
 * One surface per output on the background layer, drawing both the
 * wallpaper and ~/Desktop's icons. They are one program and one surface
 * rather than swaybg plus a transparent icon layer because a transparent
 * full-screen surface above the wallpaper is blended by the compositor
 * on every frame anything on the desktop moves, and an opaque one is
 * copied: the wallpaper surface declares itself opaque.
 *
 * ── the wallpaper ──
 *
 * In order: the image named in ~/.config/lp/wallpaper (one line, a path,
 * written by Settings - or by hand); $LP_SHARE/wallpaper.png, which the
 * image installs from desktop/branding/wallpaper.png when the branding
 * track has made one and from desktop/theme/wallpaper.png until then; and
 * if neither loads, the same deep-blue gradient drawn with cairo, so a
 * missing file is never a black screen. The image is scaled to cover the
 * output once and kept at the output's pixel size; a redraw is a copy.
 * The config file is watched (inotify), so a new choice shows at once.
 *
 * ── the icons ──
 *
 * Every entry of ~/Desktop (the XDG desktop directory), with the icon and
 * name GIO gives it; a .desktop file shows as the application it
 * launches. Tap opens it with its default application; a long press (or
 * right click) is a menu - Open, Open With Files for a folder, Move to
 * Trash. A long press (or right click) on the wallpaper itself offers
 * New Folder, Open Desktop in Files, Files, Open Terminal Here, Change
 * Background…, Appearance… and Display Settings…
 *
 * Icons sit on a 112x120 grid starting at the left edge and below the
 * bar, filling the first column downwards, the way the owner's mockup has
 * Documents, Welcome.txt and hello.sh. A finger that presses an icon and
 * moves carries it; on release it snaps to the nearest free cell on the
 * slide spring. Where each icon was put is remembered in
 * ~/.config/lp/desktop-icons (a GKeyFile, name=column,row), written
 * atomically and durably, and restored at the next start. Icons nobody
 * moved flow into the free cells in name order.
 *
 * ~/Desktop is watched, so a file saved there appears without a refresh.
 */
#define _GNU_SOURCE 1

#include <math.h>
#include <string.h>

#include <gio/gdesktopappinfo.h>
#include <glib/gstdio.h>

#include "lp-motion.h"
#include "lp-shell.h"

#define CELL_W 112
#define CELL_H 120
#define ORIGIN_X 24            /* the dock is at the bottom now; icons start at the left edge */
#define ORIGIN_Y (40 + 16)     /* below the bar (panel.c: BAR_HEIGHT) */
#define ICON_PX 56

typedef struct Desk Desk;

typedef struct {
    Desk      *desk;
    GFile     *file;
    char      *name;           /* file name: the key positions are kept under */
    GtkWidget *button;
    int        col, row;       /* its cell */
    gboolean   placed;         /* the cell came from the saved positions */
    /* dragging and settling: pixel position of the button */
    LpSpring   sx, sy;
    LpMotion  *motion;
    gboolean   dragging;
    double     press_x, press_y;
    double     start_x, start_y;
} Icon;

struct Desk {
    GdkMonitor *mon;
    GtkWindow  *win;
    GtkWidget  *layout;        /* GtkLayout: icons at pixel positions */
    GPtrArray  *icons;
    cairo_surface_t *wall;     /* at the output's size and scale */
    int         wall_w, wall_h;
};

static GList *desks;
static GFileMonitor *dir_monitor, *wall_monitor;
static GKeyFile *positions;

/* ── positions ───────────────────────────────────────────────────── */

static void positions_load(void)
{
    positions = g_key_file_new();
    char *p = lp_config_path("desktop-icons");
    g_key_file_load_from_file(positions, p, G_KEY_FILE_NONE, NULL);
    g_free(p);
}

static void positions_save(void)
{
    gsize len = 0;
    char *data = g_key_file_to_data(positions, &len, NULL);
    lp_config_write("desktop-icons", data);
    g_free(data);
}

static char *desktop_dir(void)
{
    const char *d = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    /* GLib answers $HOME when there is no user-dirs.dirs entry, and the
     * home directory is not the desktop. */
    if (!d || g_strcmp0(d, g_get_home_dir()) == 0)
        return g_build_filename(g_get_home_dir(), "Desktop", NULL);
    return g_strdup(d);
}

/* ── the wallpaper ───────────────────────────────────────────────── */

/* The theme's deep blue: teal light top-left, into near-black navy. The
 * same stops desktop/branding/src/wallpaper.py draws the wallpaper with. */
static void draw_gradient(cairo_t *cr, double W, double H)
{
    double cx = 0.18 * W, cy = 0.12 * H;
    double R = hypot(W - cx, H - cy);
    cairo_pattern_t *p = cairo_pattern_create_radial(cx, cy, 0, cx, cy, R);
    cairo_pattern_add_color_stop_rgb(p, 0.00, 0x23 / 255.0, 0xa6 / 255.0, 0xa0 / 255.0);
    cairo_pattern_add_color_stop_rgb(p, 0.20, 0x17 / 255.0, 0x75 / 255.0, 0x85 / 255.0);
    cairo_pattern_add_color_stop_rgb(p, 0.46, 0x12 / 255.0, 0x47 / 255.0, 0x6e / 255.0);
    cairo_pattern_add_color_stop_rgb(p, 0.74, 0x0d / 255.0, 0x28 / 255.0, 0x46 / 255.0);
    cairo_pattern_add_color_stop_rgb(p, 1.00, 0x07 / 255.0, 0x12 / 255.0, 0x1f / 255.0);
    cairo_set_source(cr, p);
    cairo_paint(cr);
    cairo_pattern_destroy(p);
}

static GdkPixbuf *load_wallpaper(void)
{
    char *chosen = lp_config_read("wallpaper");
    GdkPixbuf *pb = NULL;
    if (chosen && *chosen)
        pb = gdk_pixbuf_new_from_file(chosen, NULL);
    if (!pb && chosen && *chosen)
        g_printerr("lp-desktop: %s: cannot load; using the default\n", chosen);
    g_free(chosen);
    if (!pb) {
        char *def = lp_share_path("wallpaper.png");
        pb = gdk_pixbuf_new_from_file(def, NULL);
        g_free(def);
    }
    return pb;
}

/* Scaled to cover the output once, at its pixel size: every redraw after
 * this - an icon pressed, an icon dragged - is a plain copy of the part
 * that changed, never a resample. */
static void make_wall(Desk *d)
{
    int W = gtk_widget_get_allocated_width(GTK_WIDGET(d->win));
    int H = gtk_widget_get_allocated_height(GTK_WIDGET(d->win));
    if (W < 2 || H < 2)
        return;
    int sf = gtk_widget_get_scale_factor(GTK_WIDGET(d->win));
    if (d->wall && d->wall_w == W && d->wall_h == H)
        return;
    if (d->wall)
        cairo_surface_destroy(d->wall);
    d->wall = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W * sf, H * sf);
    cairo_surface_set_device_scale(d->wall, sf, sf);
    d->wall_w = W;
    d->wall_h = H;
    cairo_t *cr = cairo_create(d->wall);
    GdkPixbuf *pb = load_wallpaper();
    if (pb) {
        double iw = gdk_pixbuf_get_width(pb), ih = gdk_pixbuf_get_height(pb);
        double s = MAX(W / iw, H / ih);
        cairo_translate(cr, (W - iw * s) / 2, (H - ih * s) / 2);
        cairo_scale(cr, s, s);
        gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        g_object_unref(pb);
    } else {
        draw_gradient(cr, W, H);
    }
    cairo_destroy(cr);
}

static gboolean desk_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    Desk *d = data;
    (void)w;
    make_wall(d);
    if (d->wall) {
        cairo_set_source_surface(cr, d->wall, 0, 0);
        cairo_paint(cr);
    }
    return FALSE;      /* and then the icons, drawn by the layout */
}

static void on_wall_changed(GFileMonitor *m, GFile *f, GFile *o,
                            GFileMonitorEvent ev, gpointer u)
{
    (void)m; (void)f; (void)o; (void)u;
    if (ev != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT &&
        ev != G_FILE_MONITOR_EVENT_CREATED && ev != G_FILE_MONITOR_EVENT_DELETED)
        return;
    for (GList *l = desks; l; l = l->next) {
        Desk *d = l->data;
        if (d->wall)
            cairo_surface_destroy(d->wall);
        d->wall = NULL;
        gtk_widget_queue_draw(GTK_WIDGET(d->win));
    }
}

/* ── opening things ──────────────────────────────────────────────── */

static void open_file(GFile *f)
{
    char *path = g_file_get_path(f);
    if (path && g_str_has_suffix(path, ".desktop")) {
        GDesktopAppInfo *a = g_desktop_app_info_new_from_filename(path);
        if (a) {
            GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gdk_display_get_default());
            g_app_info_launch(G_APP_INFO(a), NULL, G_APP_LAUNCH_CONTEXT(ctx), NULL);
            g_object_unref(ctx);
            g_object_unref(a);
            g_free(path);
            return;
        }
    }
    g_free(path);
    char *uri = g_file_get_uri(f);
    GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gdk_display_get_default());
    GError *err = NULL;
    if (!g_app_info_launch_default_for_uri(uri, G_APP_LAUNCH_CONTEXT(ctx), &err)) {
        g_printerr("lp-desktop: %s: %s\n", uri, err->message);
        g_clear_error(&err);
    }
    g_object_unref(ctx);
    g_free(uri);
}

static void open_in_files(GFile *f)
{
    char *path = g_file_get_path(f);
    const char *a[] = { lp_have("lp-files") ? "lp-files" : "xdg-open", path, NULL };
    lp_spawn(a);
    g_free(path);
}

/* ── icon placement ──────────────────────────────────────────────── */

static int rows_on(Desk *d)
{
    int H = gtk_widget_get_allocated_height(GTK_WIDGET(d->win));
    int r = (H - ORIGIN_Y - 8) / CELL_H;
    return r > 0 ? r : 1;
}

static int cols_on(Desk *d)
{
    int W = gtk_widget_get_allocated_width(GTK_WIDGET(d->win));
    int c = (W - ORIGIN_X - 8) / CELL_W;
    return c > 0 ? c : 1;
}

static gboolean cell_taken(Desk *d, int col, int row, Icon *except)
{
    for (guint i = 0; i < d->icons->len; i++) {
        Icon *ic = g_ptr_array_index(d->icons, i);
        if (ic != except && ic->placed && ic->col == col && ic->row == row)
            return TRUE;
    }
    return FALSE;
}

static void cell_xy(int col, int row, double *x, double *y)
{
    *x = ORIGIN_X + col * CELL_W;
    *y = ORIGIN_Y + row * CELL_H;
}

static void icon_move_to(Icon *ic, double x, double y)
{
    gtk_layout_move(GTK_LAYOUT(ic->desk->layout), ic->button, (int)lround(x), (int)lround(y));
}

static void icon_frame(GtkWidget *w, gpointer data)
{
    (void)w;
    Icon *ic = data;
    icon_move_to(ic, ic->sx.x, ic->sy.x);
}

/* Put every icon without a saved cell into the free cells, column by
 * column, top to bottom. */
static void place_all(Desk *d)
{
    int rows = rows_on(d), cols = cols_on(d);
    for (guint i = 0; i < d->icons->len; i++) {
        Icon *ic = g_ptr_array_index(d->icons, i);
        if (ic->placed && (ic->col >= cols || ic->row >= rows))
            ic->placed = FALSE;          /* off a smaller screen: re-flow */
        if (ic->placed)
            continue;
        for (int n = 0; n < rows * cols; n++) {
            int c = n / rows, r = n % rows;
            if (!cell_taken(d, c, r, ic)) {
                ic->col = c;
                ic->row = r;
                ic->placed = TRUE;
                break;
            }
        }
    }
    for (guint i = 0; i < d->icons->len; i++) {
        Icon *ic = g_ptr_array_index(d->icons, i);
        double x, y;
        cell_xy(ic->col, ic->row, &x, &y);
        lp_spring_jump(&ic->sx, x);
        lp_spring_jump(&ic->sy, y);
        icon_move_to(ic, x, y);
    }
}

/* ── touch: tap, hold, carry ─────────────────────────────────────── */

static void on_icon_clicked(GtkButton *b, gpointer data)
{
    Icon *ic = data;
    if (lp_hold_consumed(GTK_WIDGET(b)) || ic->dragging)
        return;
    open_file(ic->file);
}

static void m_open(GtkMenuItem *m, gpointer d) { (void)m; open_file(((Icon *)d)->file); }
static void m_files(GtkMenuItem *m, gpointer d) { (void)m; open_in_files(((Icon *)d)->file); }
static void m_trash(GtkMenuItem *m, gpointer d)
{
    (void)m;
    GError *err = NULL;
    if (!g_file_trash(((Icon *)d)->file, NULL, &err)) {
        g_printerr("lp-desktop: trash: %s\n", err->message);
        g_clear_error(&err);
    }
}

static void menu_item(GtkWidget *menu, const char *label, GCallback cb, gpointer d,
                      gboolean danger)
{
    GtkWidget *mi = gtk_menu_item_new_with_label(label);
    if (danger)
        gtk_style_context_add_class(gtk_widget_get_style_context(mi), "lp-danger");
    g_signal_connect(mi, "activate", cb, d);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

static void on_icon_hold(GtkWidget *w, double x, double y, gpointer data)
{
    Icon *ic = data;
    if (ic->dragging)
        return;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *head = gtk_menu_item_new_with_label(ic->name);
    gtk_widget_set_sensitive(head, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), head);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    menu_item(menu, T("Open", "열기"), G_CALLBACK(m_open), ic, FALSE);
    if (g_file_query_file_type(ic->file, G_FILE_QUERY_INFO_NONE, NULL) == G_FILE_TYPE_DIRECTORY)
        menu_item(menu, T("Open in Files", "파일에서 열기"), G_CALLBACK(m_files), ic, FALSE);
    menu_item(menu, T("Move to Trash", "휴지통으로 이동"), G_CALLBACK(m_trash), ic, TRUE);
    gtk_widget_show_all(menu);
    lp_menu_destroy_when_closed(menu);
    lp_menu_popup_at(menu, w, x, y);
}

/* The carry. A GtkGestureDrag on the icon, in the capture phase: it
 * waits for 12 px of movement before it takes the touch from the button
 * (so a tap still taps and a long press still opens the menu), and from
 * then on the icon sits under the finger. */
static void on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer data)
{
    (void)g;
    Icon *ic = data;
    ic->press_x = x;
    ic->press_y = y;
    ic->start_x = ic->sx.x;
    ic->start_y = ic->sy.x;
}

static void on_drag_update(GtkGestureDrag *g, double dx, double dy, gpointer data)
{
    Icon *ic = data;
    if (!ic->dragging) {
        if (hypot(dx, dy) < 12)
            return;
        ic->dragging = TRUE;
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    }
    /* dx/dy are relative to the button, which is itself moving: track
     * against where the icon started instead. */
    GdkEventSequence *seq = gtk_gesture_single_get_current_sequence(GTK_GESTURE_SINGLE(g));
    double wx, wy;
    if (gtk_gesture_get_point(GTK_GESTURE(g), seq, &wx, &wy)) {
        int ox = 0, oy = 0;
        gtk_widget_translate_coordinates(ic->button, ic->desk->layout, 0, 0, &ox, &oy);
        double fx = ox + wx, fy = oy + wy;           /* the finger, on the desktop */
        lp_spring_jump(&ic->sx, fx - ic->press_x);
        lp_spring_jump(&ic->sy, fy - ic->press_y);
        icon_move_to(ic, ic->sx.x, ic->sy.x);
    }
}

static void on_drag_end(GtkGestureDrag *g, double dx, double dy, gpointer data)
{
    (void)g; (void)dx; (void)dy;
    Icon *ic = data;
    if (!ic->dragging)
        return;
    Desk *d = ic->desk;
    int col = (int)lround((ic->sx.x - ORIGIN_X) / CELL_W);
    int row = (int)lround((ic->sy.x - ORIGIN_Y) / CELL_H);
    col = CLAMP(col, 0, cols_on(d) - 1);
    row = CLAMP(row, 0, rows_on(d) - 1);
    if (cell_taken(d, col, row, ic)) {
        /* Taken: the nearest free cell, searching outwards. */
        int best = -1, bc = ic->col, br = ic->row;
        for (int c = 0; c < cols_on(d); c++)
            for (int r = 0; r < rows_on(d); r++)
                if (!cell_taken(d, c, r, ic)) {
                    int dist = (c - col) * (c - col) + (r - row) * (r - row);
                    if (best < 0 || dist < best) { best = dist; bc = c; br = r; }
                }
        col = bc;
        row = br;
    }
    ic->col = col;
    ic->row = row;
    ic->placed = TRUE;
    double x, y;
    cell_xy(col, row, &x, &y);
    lp_spring_set_target(&ic->sx, x);
    lp_spring_set_target(&ic->sy, y);
    lp_motion_kick(ic->motion);
    char v[32];
    g_snprintf(v, sizeof v, "%d,%d", col, row);
    g_key_file_set_string(positions, "positions", ic->name, v);
    positions_save();
    ic->dragging = FALSE;
    /* Belt and braces: claiming the touch already cancelled the button's
     * click, and a carry that ends over the icon must never open it. */
    g_object_set_data(G_OBJECT(ic->button), "lp-hold-consumed", GINT_TO_POINTER(1));
}

/* ── building ────────────────────────────────────────────────────── */

static void icon_free(gpointer p)
{
    Icon *ic = p;
    lp_motion_free(ic->motion);
    g_object_unref(ic->file);
    g_free(ic->name);
    g_free(ic);
}

static Icon *icon_new(Desk *d, GFile *f, GFileInfo *info)
{
    Icon *ic = g_new0(Icon, 1);
    ic->desk = d;
    ic->file = g_object_ref(f);
    ic->name = g_strdup(g_file_info_get_name(info));

    const char *label = g_file_info_get_display_name(info);
    GIcon *gi = g_file_info_get_icon(info);
    GDesktopAppInfo *app = NULL;
    if (g_str_has_suffix(ic->name, ".desktop")) {
        char *path = g_file_get_path(f);
        app = g_desktop_app_info_new_from_filename(path);
        g_free(path);
        if (app) {
            label = g_app_info_get_display_name(G_APP_INFO(app));
            if (g_app_info_get_icon(G_APP_INFO(app)))
                gi = g_app_info_get_icon(G_APP_INFO(app));
        }
    }
    ic->button = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(ic->button), "lp-desk-item");
    gtk_widget_set_size_request(ic->button, CELL_W - 8, CELL_H - 8);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *img = gi ? gtk_image_new_from_gicon(gi, GTK_ICON_SIZE_DIALOG)
                        : gtk_image_new_from_icon_name("text-x-generic", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(img), ICON_PX);
    gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines(GTK_LABEL(l), 2);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 12);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(ic->button), box);
    if (app)
        g_object_unref(app);

    g_signal_connect(ic->button, "clicked", G_CALLBACK(on_icon_clicked), ic);
    lp_on_hold(ic->button, on_icon_hold, ic);
    GtkGesture *drag = gtk_gesture_drag_new(ic->button);
    gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(drag), FALSE);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(drag), GTK_PHASE_CAPTURE);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), ic);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), ic);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), ic);
    g_object_set_data_full(G_OBJECT(ic->button), "lp-drag", drag, g_object_unref);

    char *v = g_key_file_get_string(positions, "positions", ic->name, NULL);
    int c, r;
    if (v && sscanf(v, "%d,%d", &c, &r) == 2 && c >= 0 && r >= 0) {
        ic->col = c;
        ic->row = r;
        ic->placed = TRUE;
    }
    g_free(v);
    lp_spring_init(&ic->sx, LP_SPRING_SLIDE, 0);
    lp_spring_init(&ic->sy, LP_SPRING_SLIDE, 0);
    ic->motion = lp_motion_new(ic->button, icon_frame, ic);
    lp_motion_add(ic->motion, &ic->sx);
    lp_motion_add(ic->motion, &ic->sy);
    gtk_layout_put(GTK_LAYOUT(d->layout), ic->button, 0, 0);
    return ic;
}

static gint by_name(gconstpointer a, gconstpointer b)
{
    GFileInfo *x = *(GFileInfo **)a, *y = *(GFileInfo **)b;
    return g_utf8_collate(g_file_info_get_display_name(x), g_file_info_get_display_name(y));
}

static void fill(Desk *d)
{
    g_ptr_array_set_size(d->icons, 0);
    GList *kids = gtk_container_get_children(GTK_CONTAINER(d->layout));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(kids);

    char *dir = desktop_dir();
    GFile *df = g_file_new_for_path(dir);
    GFileEnumerator *en = g_file_enumerate_children(df,
        G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","
        G_FILE_ATTRIBUTE_STANDARD_ICON "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN ","
        G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NONE, NULL, NULL);
    GPtrArray *infos = g_ptr_array_new_with_free_func(g_object_unref);
    if (en) {
        GFileInfo *info;
        while ((info = g_file_enumerator_next_file(en, NULL, NULL))) {
            if (g_file_info_get_is_hidden(info))
                g_object_unref(info);
            else
                g_ptr_array_add(infos, info);
        }
        g_object_unref(en);
    }
    g_ptr_array_sort(infos, by_name);
    for (guint i = 0; i < infos->len; i++) {
        GFileInfo *info = g_ptr_array_index(infos, i);
        GFile *f = g_file_get_child(df, g_file_info_get_name(info));
        g_ptr_array_add(d->icons, icon_new(d, f, info));
        g_object_unref(f);
    }
    g_ptr_array_unref(infos);
    g_object_unref(df);
    g_free(dir);
    gtk_widget_show_all(d->layout);
    place_all(d);
}

static void on_dir_changed(GFileMonitor *m, GFile *f, GFile *o,
                           GFileMonitorEvent ev, gpointer u)
{
    (void)m; (void)f; (void)o; (void)u;
    if (ev == G_FILE_MONITOR_EVENT_CREATED || ev == G_FILE_MONITOR_EVENT_DELETED ||
        ev == G_FILE_MONITOR_EVENT_RENAMED || ev == G_FILE_MONITOR_EVENT_MOVED_IN ||
        ev == G_FILE_MONITOR_EVENT_MOVED_OUT)
        for (GList *l = desks; l; l = l->next)
            fill(l->data);
}

/* ── the wallpaper's own menu ────────────────────────────────────── */

static void m_new_folder(GtkMenuItem *m, gpointer d)
{
    (void)m; (void)d;
    char *dir = desktop_dir();
    for (int n = 1; n < 100; n++) {
        char *name = n == 1 ? g_strdup(T("New Folder", "새 폴더"))
                            : g_strdup_printf("%s %d", T("New Folder", "새 폴더"), n);
        char *p = g_build_filename(dir, name, NULL);
        gboolean ok = g_mkdir(p, 0755) == 0;
        g_free(p);
        g_free(name);
        if (ok)
            break;
    }
    g_free(dir);
}

static void m_open_desktop(GtkMenuItem *m, gpointer d)
{
    (void)m; (void)d;
    char *dir = desktop_dir();
    GFile *f = g_file_new_for_path(dir);
    open_in_files(f);
    g_object_unref(f);
    g_free(dir);
}

static void m_background(GtkMenuItem *m, gpointer d)
{
    (void)m; (void)d;
    const char *a[] = { "lp-settings", "appearance", NULL };
    lp_spawn(a);
}

static void m_files_home(GtkMenuItem *m, gpointer d)
{
    (void)m; (void)d;
    const char *a[] = { "lp-files", NULL };
    lp_spawn(a);
}

/* A terminal that opens in ~/Desktop, where the finger was. */
static void m_terminal(GtkMenuItem *m, gpointer d)
{
    (void)m; (void)d;
    char *dir = desktop_dir();
    char *kgx = g_find_program_in_path("kgx");
    char *wd = g_strconcat("--working-directory=", dir, NULL);
    const char *a_kgx[] = { "kgx", wd, NULL };
    const char *a_foot[] = { "foot", "--working-directory", dir, NULL };
    lp_spawn(kgx ? a_kgx : a_foot);
    g_free(kgx);
    g_free(wd);
    g_free(dir);
}

static void m_display(GtkMenuItem *m, gpointer d)
{
    (void)m; (void)d;
    const char *a[] = { "lp-settings", "display", NULL };
    lp_spawn(a);
}

static void on_desk_hold(GtkWidget *w, double x, double y, gpointer data)
{
    (void)data;
    GtkWidget *menu = gtk_menu_new();
    menu_item(menu, T("New Folder", "새 폴더"), G_CALLBACK(m_new_folder), NULL, FALSE);
    menu_item(menu, T("Open Desktop in Files", "바탕 화면을 파일에서 열기"),
              G_CALLBACK(m_open_desktop), NULL, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    menu_item(menu, T("Files", "파일"), G_CALLBACK(m_files_home), NULL, FALSE);
    menu_item(menu, T("Open Terminal Here", "여기서 터미널 열기"), G_CALLBACK(m_terminal), NULL, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    menu_item(menu, T("Change Background…", "배경 바꾸기…"), G_CALLBACK(m_background), NULL, FALSE);
    menu_item(menu, T("Appearance…", "모양…"), G_CALLBACK(m_background), NULL, FALSE);
    menu_item(menu, T("Display Settings…", "디스플레이 설정…"), G_CALLBACK(m_display), NULL, FALSE);
    gtk_widget_show_all(menu);
    lp_menu_destroy_when_closed(menu);
    lp_menu_popup_at(menu, w, x, y);
}

/* ── one per output ──────────────────────────────────────────────── */

static void on_realize(GtkWidget *w, gpointer d)
{
    (void)d;
    /* Opaque: the compositor copies it instead of blending it with
     * nothing, every frame. */
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    cairo_rectangle_int_t r = { 0, 0, 16384, 16384 };
    cairo_region_t *reg = cairo_region_create_rectangle(&r);
    gdk_window_set_opaque_region(gtk_widget_get_window(w), reg);
    cairo_region_destroy(reg);
}

static void on_size(GtkWidget *w, GdkRectangle *a, gpointer data)
{
    (void)w; (void)a;
    place_all(data);
}

static Desk *desk_new(GdkMonitor *mon)
{
    Desk *d = g_new0(Desk, 1);
    d->mon = mon;
    d->icons = g_ptr_array_new_with_free_func(icon_free);
    d->win = lp_layer_window("lp-desktop", GTK_LAYER_SHELL_LAYER_BACKGROUND,
                             LP_EDGE_TOP | LP_EDGE_BOTTOM | LP_EDGE_LEFT | LP_EDGE_RIGHT);
    gtk_layer_set_monitor(d->win, mon);
    /* -1: under the bar and the dock too; they are translucent. */
    gtk_layer_set_exclusive_zone(d->win, -1);
    gtk_widget_set_visual(GTK_WIDGET(d->win),
                          gdk_screen_get_system_visual(gdk_screen_get_default()));
    g_signal_connect(d->win, "realize", G_CALLBACK(on_realize), NULL);

    d->layout = gtk_layout_new(NULL, NULL);
    gtk_widget_set_app_paintable(d->layout, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(d->layout), "lp-desktop");
    g_signal_connect(d->layout, "draw", G_CALLBACK(desk_draw), d);
    g_signal_connect(d->layout, "size-allocate", G_CALLBACK(on_size), d);
    gtk_container_add(GTK_CONTAINER(d->win), d->layout);
    lp_on_hold(d->layout, on_desk_hold, d);

    fill(d);
    gtk_widget_show_all(GTK_WIDGET(d->win));
    desks = g_list_append(desks, d);
    return d;
}

static void desk_free(Desk *d)
{
    desks = g_list_remove(desks, d);
    g_ptr_array_unref(d->icons);
    if (d->wall)
        cairo_surface_destroy(d->wall);
    gtk_widget_destroy(GTK_WIDGET(d->win));
    g_free(d);
}

static void on_monitor_added(GdkDisplay *dpy, GdkMonitor *m, gpointer u)
{
    (void)dpy; (void)u;
    desk_new(m);
}

static void on_monitor_removed(GdkDisplay *dpy, GdkMonitor *m, gpointer u)
{
    (void)dpy; (void)u;
    for (GList *l = desks; l; l = l->next)
        if (((Desk *)l->data)->mon == m) {
            desk_free(l->data);
            break;
        }
}

static void on_command(int argc, char **argv, gpointer d)
{
    (void)d;
    if (argc >= 2 && !strcmp(argv[1], "refresh"))
        for (GList *l = desks; l; l = l->next) {
            Desk *k = l->data;
            if (k->wall)
                cairo_surface_destroy(k->wall);
            k->wall = NULL;
            fill(k);
            gtk_widget_queue_draw(GTK_WIDGET(k->win));
        }
}

int main(int argc, char **argv)
{
    if (!lp_single_instance("desktop", argc, argv, on_command, NULL))
        return 0;
    lp_shell_init(&argc, &argv);
    positions_load();

    char *dir = desktop_dir();
    g_mkdir_with_parents(dir, 0755);
    GFile *df = g_file_new_for_path(dir);
    dir_monitor = g_file_monitor_directory(df, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (dir_monitor)
        g_signal_connect(dir_monitor, "changed", G_CALLBACK(on_dir_changed), NULL);
    g_object_unref(df);
    g_free(dir);

    char *wp = lp_config_path("wallpaper");
    GFile *wf = g_file_new_for_path(wp);
    wall_monitor = g_file_monitor_file(wf, G_FILE_MONITOR_NONE, NULL, NULL);
    if (wall_monitor)
        g_signal_connect(wall_monitor, "changed", G_CALLBACK(on_wall_changed), NULL);
    g_object_unref(wf);
    g_free(wp);

    GdkDisplay *dpy = gdk_display_get_default();
    for (int i = 0; i < gdk_display_get_n_monitors(dpy); i++)
        desk_new(gdk_display_get_monitor(dpy, i));
    g_signal_connect(dpy, "monitor-added", G_CALLBACK(on_monitor_added), NULL);
    g_signal_connect(dpy, "monitor-removed", G_CALLBACK(on_monitor_removed), NULL);
    gtk_main();
    return 0;
}
