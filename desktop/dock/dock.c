/*
 * dock.c - lp-dock, the floating dock at the bottom of the screen.
 *
 *            ╭──────────────────────────────────────────╮
 *            │ ▣  ▣  ▣  ▣  ▣  ▣  ▣  ▣  ▣  ▣  │  ⠿ │
 *            ╰───•──────•──────────────────────────────╯
 *
 * A rounded, translucent bar centred above the bottom edge, not touching
 * it: files, browser, terminal, editors, the everyday apps, settings -
 * colourful app icons with a small dot under each one that is running
 * (a wider orange bar under the one in front) - then a separator and the
 * app-grid button. It reserves its height, so a maximised window stops
 * above it instead of sliding under.
 *
 * It used to be a full-height column on the left, which is where
 * Ubuntu keeps its dock; LP's is its own shape.
 *
 * ── the mockup's apps and what is behind them ──
 *
 * Each slot names the .desktop files that can fill it, in order of
 * preference, and the Debian package that supplies the first one.
 * Seven are in the image today. The other four - notes, code editor,
 * mail, and the store, which another track is writing as lp-software -
 * are drawn anyway, dimmed, because the dock is the shape the owner drew
 * and a slot that appears the day its package is installed is a dock
 * that rearranges itself under the person's thumb. Tapping a dim one
 * says which package fills it.
 *
 * ── pinning ──
 *
 * The pinned list is ~/.config/lp/dock, one desktop id per line, in
 * dock order. It is written only when the person pins, unpins or
 * rearranges something, so until then the defaults below are the dock,
 * and a later change to the defaults reaches everyone who never touched
 * it. Rearranging is dragging an icon along the dock; dragging it up off
 * the dock and letting go unpins it (see "the row").
 * The app grid pins and unpins by writing the same file; a file monitor
 * (inotify - no polling) brings the dock along.
 *
 * ── touch ──
 *
 * Tap: not running, launch; running elsewhere, bring its newest window
 * forward; already in front, the next window of the same app, or
 * minimise it if it has only one - the same button that summons a
 * window puts it away. Long press (or right click): a menu with a new
 * window, pin/unpin, and close. Every item is 64x60 logical pixels.
 *
 * A finger swiping up from the bottom edge of the screen, or up off the
 * app-grid button, pulls the app grid open and it follows the finger
 * (drag-px / release-px to lp-appgrid). The bottom-edge catcher is a
 * separate 8 px layer surface along the bottom of the output, owned by
 * this process because the dock is the one shell component that is
 * always running; it takes touches only, so a mouse at the bottom of a
 * window is not pulled into it.
 *
 * ── motion ──
 *
 *   press       the highlight is there on touch-down, in the same frame,
 *               and fades out in 91 ms on release (motion.css; the dock
 *               adds nothing)
 *   launching   the icon breathes - scales to 0.92 and dims - on the
 *               window spring until the application's first window
 *               appears, and gives up after 8 s: a tap has to be seen to
 *               have done something before the app draws, and a slow
 *               app is not a tap that failed
 *   running     the dot beside the icon grows in on the insert spring,
 *               with its 4.5% overshoot, and stretches into the taller
 *               accent bar when the app takes focus
 *   in, out     an icon pinned or opened grows in on the insert spring
 *               and the others slide aside; one unpinned or closed
 *               shrinks away and they close up (see "the row")
 *   dragging    the icon lifts and follows the pointer, a gap opens
 *               where it would land, and it glides into place
 *
 * Each item has one tick callback for its springs, and it is removed
 * when they rest: a still dock costs no wakeups.
 *
 * ── what is remembered ──
 *
 * The pinned list, in ~/.config/lp/dock, one desktop id per line, written
 * atomically and durably (lp_write_atomic: temp file, fsync, rename, fsync
 * of the directory) - a power cut leaves the old list or the new, never
 * an empty dock. The effective list, defaults included, is also published
 * to $XDG_RUNTIME_DIR/lp-dock-pins for the app grid's Pin/Unpin menu.
 */
#define _GNU_SOURCE 1

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "lp-apps.h"
#include "lp-motion.h"
#include "lp-shell.h"
#include "lp-toplevel.h"
#include "lp-wfshell.h"

/* The icons' size: Settings > Appearance > Dock size writes small,
 * medium or large into ~/.config/lp/dock.conf. The dock reads it at start
 * and, when the file changes, exits - lp-shell-start starts it again a
 * second later at the new size, which is simpler and surer than resizing
 * every item and the layer surface in place. */
static int icon_px = 36;
#define ICON_PX icon_px
#define DOCK_MARGIN 8      /* between the dock and the screen's bottom edge */
#define EDGE_PX 8           /* the bottom-edge catcher's height */
#define LAUNCH_GIVE_UP 8    /* seconds */

typedef struct {
    const char *ids[3];     /* .desktop ids that can fill the slot */
    const char *package;    /* the Debian package for ids[0] */
    const char *en, *ko;    /* what the slot is, when nothing fills it */
    const char *icon;       /* its icon, when nothing fills it */
} Slot;

/* The dock's order: where a person goes most, first. */
static const Slot slots[] = {
    { { "lp-files.desktop" }, NULL, "Files", "파일", "system-file-manager" },
    { { "firefox-esr.desktop", "firefox.desktop" }, "firefox-esr",
      "Web Browser", "웹 브라우저", "firefox-esr" },
    { { "org.gnome.Console.desktop", "foot.desktop", "org.codeberg.dnkl.foot.desktop" },
      "gnome-console", "Terminal", "터미널", "utilities-terminal" },
    { { "org.gnome.gedit.desktop", "org.gnome.TextEditor.desktop" }, "gedit",
      "Text Editor", "텍스트 편집기", "accessories-text-editor" },
    { { "geany.desktop", "org.geany.Geany.desktop" }, "geany",
      "Code Editor", "코드 편집기", "geany" },
    { { "libreoffice-writer.desktop" }, "libreoffice-writer",
      "Documents", "문서", "libreoffice-writer" },
    /* LP's own (desktop/calc): GNOME's was a package Software could
     * remove, and the slot then opened nothing. */
    { { "lp-calc.desktop" }, NULL,
      "Calculator", "계산기", "accessories-calculator" },
    { { "lp-software.desktop" }, NULL,
      "Software", "소프트웨어", "system-software-install" },
    { { "lp-tasks.desktop" }, NULL,
      "Task Manager", "작업 관리자", "utilities-system-monitor" },
    { { "lp-settings.desktop" }, NULL, "Settings", "설정", "preferences-system" },
};
#define NSLOTS (sizeof slots / sizeof slots[0])

typedef struct {
    char *id;               /* desktop id, or the slot's ids[0] if missing */
    GDesktopAppInfo *info;  /* NULL when not installed */
    const Slot *slot;       /* the mockup slot it came from, if any */
    gboolean pinned;
    GtkWidget *button;
    GtkWidget *dot;
    GtkWidget *img;
    LpSpring run;           /* 0 no dot .. 1 dot (insert spring) */
    LpSpring focus;         /* 0 round dot .. 1 tall accent bar */
    LpSpring pulse;         /* 0 still .. 1 breathed in, while launching */
    LpMotion *motion;
    gboolean launching;
    guint give_up;
    char *pin_id;           /* the line in ~/.config/lp/dock that pins it */
    LpSpring pres;          /* 0 gone .. 1 in its place (see "the row") */
    LpSpring off;           /* px from its place, springing back to 0 */
    gboolean leaving;       /* on its way out: shrinks, then is destroyed */
    gboolean drag_eaten;    /* a drag ended on it: the release is no tap */
    double lay_x;           /* its place in the row, this frame */
    int put_x;              /* where the GtkFixed has it */
} Item;

static GtkWindow *win;
static GtkWidget *list;          /* the box the items live in */
static GtkWidget *grid_button;
static GPtrArray *items;         /* Item* in dock order */
static GFileMonitor *pin_monitor;
static guint rebuild_id;
static GtkWidget *stage;         /* the window's content: room to lift an icon, then the bar */

static void rebuild(void);

/* ── the row: where every item is, and how it moves ──────────────────
 *
 * The items sit in a fixed container, each at a place worked out every
 * frame from the ones before it: an item's room is its width times its
 * presence (0 gone .. 1 there). An item that comes in - pinned from the
 * menu or the app grid, an app that starts - grows out of nothing while
 * the ones after it slide over to make room, and one that goes out -
 * unpinned, closed - shrinks away while they close up behind it; the
 * bar, centred, narrows and widens from both ends with them. An item
 * that changes place keeps where it was drawn as an offset that springs
 * back to nothing: it slides there instead of jumping.
 *
 * Dragging one (mouse or finger) lifts it out of the row: it follows the
 * pointer, slightly larger, and a gap opens where it would land while
 * the one it left closes. Let go and it glides into the gap - the new
 * order is the pinned list from then on. Lifted well above the dock it
 * shrinks and says "Remove": letting go there unpins it, and it goes in
 * a puff, the others closing up. An app that is running but not pinned
 * is pinned by dropping it among the pinned ones. */
typedef struct { int at; LpSpring w; } Gap;
static GPtrArray *gaps;           /* Gap*: room opened while dragging */
static LpMotion *lay;             /* the row's springs and the dragged icon's */
static double slot_w, slot_h;     /* one item's room */
static int row_w, row_h;          /* the row's size, as the container reports it */
static gboolean built_once, rebuild_after_drag;
static guint reap_id;

static struct {
    Item *it;            /* lifted, or gliding back to its place */
    gboolean active;     /* moved past the threshold: a drag, not a tap */
    gboolean settling;   /* let go: the icon glides to its place (or puffs) */
    gboolean out;        /* lifted off the dock: letting go unpins */
    gboolean removing;   /* let go off the dock: the puff */
    int from;            /* its place in the row when it was lifted */
    double cx0, cy0;     /* the icon's centre at the press, stage coordinates */
    LpSpring gx, gy, gs, ga;
} D;

#define DRAG_START 8     /* px the pointer moves before a press is a drag */

static gboolean item_hidden(const Item *it)
{
    return D.it == it && (D.active || D.settling);
}

static gboolean in_row(const Item *it)
{
    return !(D.it == it && D.active);
}

/* ── the pinned list ─────────────────────────────────────────────── */

static const Slot *slot_for(const char *id)
{
    for (guint i = 0; i < NSLOTS; i++)
        for (int k = 0; k < 3 && slots[i].ids[k]; k++)
            if (strcmp(slots[i].ids[k], id) == 0)
                return &slots[i];
    return NULL;
}

/* The first installed id of a slot, or NULL. */
static const char *slot_installed(const Slot *s, GDesktopAppInfo **out)
{
    for (int k = 0; k < 3 && s->ids[k]; k++) {
        GDesktopAppInfo *d = g_desktop_app_info_new(s->ids[k]);
        if (d) {
            *out = d;
            return s->ids[k];
        }
    }
    *out = NULL;
    return NULL;
}

/* The pinned ids, from the file or the defaults. */
static GPtrArray *read_pins(void)
{
    GPtrArray *pins = g_ptr_array_new_with_free_func(g_free);
    char *path = lp_config_path("dock"), *text = NULL;
    if (g_file_get_contents(path, &text, NULL, NULL)) {
        char **lines = g_strsplit(text, "\n", -1);
        for (char **l = lines; *l; l++) {
            char *s = g_strstrip(*l);
            /* The calculator's slot is LP's own now; a dock saved when it
             * was GNOME's keeps its place with the new one. */
            if (!strcmp(s, "org.gnome.Calculator.desktop"))
                s = (char *)"lp-calc.desktop";
            if (*s && *s != '#')
                g_ptr_array_add(pins, g_strdup(s));
        }
        g_strfreev(lines);
    } else {
        for (guint i = 0; i < NSLOTS; i++)
            g_ptr_array_add(pins, g_strdup(slots[i].ids[0]));
    }
    g_free(text);
    g_free(path);
    return pins;
}

static void write_pins(GPtrArray *pins)
{
    GString *s = g_string_new("# lp-dock: pinned applications, in dock order.\n");
    for (guint i = 0; i < pins->len; i++)
        g_string_append_printf(s, "%s\n", (char *)g_ptr_array_index(pins, i));
    lp_config_write("dock", s->str);
    g_string_free(s, TRUE);
}

/* The list as the dock is showing it - defaults included, which the file
 * does not have until someone changes something - for the app grid. */
static void publish_pins(void)
{
    GPtrArray *pins = read_pins();
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < pins->len; i++) {
        const char *id = g_ptr_array_index(pins, i);
        const Slot *slot = slot_for(id);
        g_string_append_printf(s, "%s\n", id);
        /* Every alternative of a slot counts as pinned: pinning gedit
         * pins the text-editor slot, whichever editor fills it. */
        for (int k = 0; slot && k < 3 && slot->ids[k]; k++)
            if (strcmp(slot->ids[k], id))
                g_string_append_printf(s, "%s\n", slot->ids[k]);
    }
    char *p = g_build_filename(g_get_user_runtime_dir(), "lp-dock-pins", NULL);
    g_file_set_contents(p, s->str, -1, NULL);   /* runtime: no fsync needed */
    g_free(p);
    g_string_free(s, TRUE);
    g_ptr_array_unref(pins);
}

static void set_pinned(const char *id, gboolean pin)
{
    GPtrArray *pins = read_pins();
    /* A slot is pinned under its first id; unpinning any of its
     * alternatives (the app grid only knows the installed one) means
     * that entry. */
    const Slot *slot = slot_for(id);
    if (slot && !pin)
        id = slot->ids[0];
    for (guint i = 0; i < pins->len; i++)
        if (strcmp(g_ptr_array_index(pins, i), id) == 0 ||
            (slot && slot_for(g_ptr_array_index(pins, i)) == slot)) {
            if (pin)
                goto out;
            g_ptr_array_remove_index(pins, i);
            break;
        }
    if (pin)
        g_ptr_array_add(pins, g_strdup(id));
    write_pins(pins);
out:
    g_ptr_array_unref(pins);
    rebuild();
}

/* ── windows ─────────────────────────────────────────────────────── */

static GList *windows_of(Item *it)
{
    GList *out = NULL;
    for (GList *l = lp_toplevels(); l; l = l->next) {
        LpToplevel *t = l->data;
        if (t->done && it->info && lp_app_owns(it->info, t->app_id))
            out = g_list_append(out, t);
    }
    return out;
}

/* ── launching: the icon breathes until a window appears ───────── */

static void pulse_stop(Item *it)
{
    if (!it->launching)
        return;
    it->launching = FALSE;
    if (it->give_up)
        g_source_remove(it->give_up);
    it->give_up = 0;
    lp_spring_set_target_out(&it->pulse, 0.0);
    lp_motion_kick(it->motion);
}

static gboolean give_up(gpointer d)
{
    Item *it = d;
    it->give_up = 0;
    pulse_stop(it);
    return G_SOURCE_REMOVE;
}

static void launch(Item *it)
{
    lp_app_launch(G_APP_INFO(it->info));
    if (lp_motion_reduced())
        return;          /* reduced motion: the dot appearing says enough */
    it->launching = TRUE;
    if (it->give_up)
        g_source_remove(it->give_up);
    it->give_up = g_timeout_add_seconds(LAUNCH_GIVE_UP, give_up, it);
    lp_spring_set_target(&it->pulse, 1.0);
    lp_motion_kick(it->motion);
}

/* ── tap ─────────────────────────────────────────────────────────── */

static void show_missing(Item *it)
{
    GtkWidget *pop = gtk_popover_new(it->button);
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_TOP);
    gtk_style_context_add_class(gtk_widget_get_style_context(pop), "lp-note");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *t = gtk_label_new(T(it->slot->en, it->slot->ko));
    gtk_style_context_add_class(gtk_widget_get_style_context(t), "lp-note-title");
    gtk_widget_set_halign(t, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
    if (it->slot->package) {
        GtkWidget *m = gtk_label_new(T("Not installed yet. It comes with:",
                                       "아직 설치되지 않았습니다. 이 패키지로 설치합니다:"));
        gtk_widget_set_halign(m, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(box), m, FALSE, FALSE, 0);
        char *cmd = g_strdup_printf("sudo apt install %s", it->slot->package);
        GtkWidget *c = gtk_label_new(cmd);
        gtk_label_set_selectable(GTK_LABEL(c), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(c), "lp-mono");
        gtk_widget_set_halign(c, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(box), c, FALSE, FALSE, 0);
        g_free(cmd);
    } else {
        GtkWidget *m = gtk_label_new(T("This part of LP is not installed yet.",
                                       "LP 의 이 부분은 아직 설치되지 않았습니다."));
        gtk_widget_set_halign(m, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(box), m, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(box);
    gtk_container_add(GTK_CONTAINER(pop), box);
    g_signal_connect(pop, "closed", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_popover_popup(GTK_POPOVER(pop));
}

static void on_item(GtkWidget *b, gpointer d)
{
    Item *it = d;
    if (lp_hold_consumed(b))
        return;
    if (it->drag_eaten) {
        it->drag_eaten = FALSE;
        return;
    }
    if (!it->info) {
        show_missing(it);
        return;
    }
    GList *wins = windows_of(it);
    if (!wins) {
        launch(it);
        return;
    }
    LpToplevel *front = NULL;
    for (GList *l = wins; l; l = l->next)
        if (((LpToplevel *)l->data)->activated)
            front = l->data;
    if (!front) {
        /* The newest is the one the person most likely means. */
        lp_toplevel_activate(g_list_last(wins)->data);
    } else if (g_list_length(wins) > 1) {
        GList *me = g_list_find(wins, front);
        lp_toplevel_activate(me->next ? me->next->data : wins->data);
    } else {
        lp_toplevel_set_minimized(front, TRUE);
    }
    g_list_free(wins);
}

/* ── long press ──────────────────────────────────────────────────── */

static void m_new_window(GtkMenuItem *m, gpointer d)
{
    (void)m;
    Item *it = d;
    if (it->info)
        launch(it);
}

static void m_pin(GtkMenuItem *m, gpointer d)
{
    (void)m;
    Item *it = d;
    set_pinned(it->id, !it->pinned);
}

static void m_close(GtkMenuItem *m, gpointer d)
{
    (void)m;
    GList *wins = windows_of(d);
    for (GList *l = wins; l; l = l->next)
        lp_toplevel_close(l->data);
    g_list_free(wins);
}

static void menu_add(GtkWidget *menu, const char *label, GCallback cb,
                     gpointer d, gboolean danger)
{
    GtkWidget *mi = gtk_menu_item_new_with_label(label);
    if (danger)
        gtk_style_context_add_class(gtk_widget_get_style_context(mi), "lp-danger");
    g_signal_connect(mi, "activate", cb, d);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

/* Menus the dock has open. While one is, the dock stays up: over a
 * full-screen window it slides away 0.7s after the pointer leaves it, and
 * going up into the menu is leaving it (the menu is another surface) -
 * the dock went, and the menu with it, before an item could be reached. */
static int dock_menus;
static void dock_menu_done(GtkMenuShell *menu, gpointer d);

static void on_hold(GtkWidget *w, double x, double y, gpointer d)
{
    Item *it = d;
    GtkWidget *menu = gtk_menu_new();
    const char *name = it->info ? lp_app_name(G_APP_INFO(it->info))
                                : T(it->slot->en, it->slot->ko);
    GtkWidget *head = gtk_menu_item_new_with_label(name);
    gtk_widget_set_sensitive(head, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), head);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GList *wins = windows_of(it);
    if (it->info)
        menu_add(menu, wins ? T("New window", "새 창") : T("Open", "열기"),
                 G_CALLBACK(m_new_window), it, FALSE);
    menu_add(menu, it->pinned ? T("Unpin from dock", "독에서 고정 해제")
                              : T("Pin to dock", "독에 고정"),
             G_CALLBACK(m_pin), it, FALSE);
    if (wins) {
        char *c = g_list_length(wins) > 1
            ? g_strdup_printf(T("Close %u windows", "창 %u개 닫기"),
                              g_list_length(wins))
            : g_strdup(T("Close", "닫기"));
        menu_add(menu, c, G_CALLBACK(m_close), it, TRUE);
        g_free(c);
    }
    g_list_free(wins);
    gtk_widget_show_all(menu);
    dock_menus++;
    g_signal_connect(menu, "deactivate", G_CALLBACK(dock_menu_done), NULL);
    lp_menu_destroy_when_closed(menu);
    lp_menu_popup_at(menu, w, x, y);
}

/* ── building the dock ───────────────────────────────────────────── */

static GtkWidget *app_image(Item *it)
{
    GtkWidget *img;
    GIcon *gi = it->info ? g_app_info_get_icon(G_APP_INFO(it->info)) : NULL;
    if (gi) {
        img = gtk_image_new_from_gicon(gi, GTK_ICON_SIZE_DIALOG);
    } else {
        const char *n = it->slot ? it->slot->icon : "application-x-executable";
        img = gtk_image_new_from_icon_name(n, GTK_ICON_SIZE_DIALOG);
    }
    gtk_image_set_pixel_size(GTK_IMAGE(img), ICON_PX);
    return img;
}

/* The breathing icon. While it breathes the dock draws the icon itself -
 * scaled about its centre and faded - and GtkImage does not draw at all;
 * the rest of the time this returns at once and GtkImage draws as usual.
 *
 * It used to wrap GtkImage's own drawing in cairo_push_group() here and
 * cairo_pop_group_to_source() in an after-handler. GTK 3 brackets every
 * "draw" handler in its own cairo_save()/cairo_restore(), so the restore
 * after the first handler met the pushed group instead: "cairo_restore()
 * without matching cairo_save()", the context went into an error state,
 * and every widget drawn after it that frame failed - the first tap on
 * any app emptied the rest of the dock. One handler that does its own
 * painting and returns TRUE has nothing to leave unbalanced.
 *
 * The icon's surface is loaded once per launch at the output's scale and
 * kept on the widget until the breathing ends. */
static cairo_surface_t *breath_surface(GtkWidget *w)
{
    int sf = gtk_widget_get_scale_factor(w);
    cairo_surface_t *s = g_object_get_data(G_OBJECT(w), "lp-breath");
    if (s && GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "lp-breath-sf")) == sf)
        return s;
    GtkIconTheme *th = gtk_icon_theme_get_for_screen(gtk_widget_get_screen(w));
    GtkIconInfo *ii = NULL;
    GIcon *gi = NULL;
    const char *name = NULL;
    switch (gtk_image_get_storage_type(GTK_IMAGE(w))) {
    case GTK_IMAGE_GICON:
        gtk_image_get_gicon(GTK_IMAGE(w), &gi, NULL);
        if (gi)
            ii = gtk_icon_theme_lookup_by_gicon_for_scale(th, gi, ICON_PX, sf,
                                                          GTK_ICON_LOOKUP_FORCE_SIZE);
        break;
    case GTK_IMAGE_ICON_NAME:
        gtk_image_get_icon_name(GTK_IMAGE(w), &name, NULL);
        if (name)
            ii = gtk_icon_theme_lookup_icon_for_scale(th, name, ICON_PX, sf,
                                                      GTK_ICON_LOOKUP_FORCE_SIZE);
        break;
    default:
        break;
    }
    if (!ii)
        return NULL;
    s = gtk_icon_info_load_surface(ii, gtk_widget_get_window(w), NULL);
    g_object_unref(ii);
    if (!s)
        return NULL;
    g_object_set_data_full(G_OBJECT(w), "lp-breath", s,
                           (GDestroyNotify)cairo_surface_destroy);
    g_object_set_data(G_OBJECT(w), "lp-breath-sf", GINT_TO_POINTER(sf));
    return s;
}

/* How far an item's drawing is from its widget's centre: the middle of
 * the room it has now, which is narrower than the widget while it comes
 * in or goes out (the widget keeps its full size; the room is what the
 * next item is placed after). */
static double room_shift(const Item *it)
{
    double pr = CLAMP(it->pres.x, 0.0, 1.0);
    return -slot_w * (1.0 - pr) / 2.0;
}

static gboolean img_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
    Item *it = d;
    if (item_hidden(it))
        return TRUE;            /* the lifted icon is drawn over the dock */
    double p = CLAMP(it->pulse.x, 0.0, 1.0);
    double pr = CLAMP(it->pres.x, 0.0, 1.08);
    if (p < 0.001 && fabs(pr - 1.0) < 0.002) {
        g_object_set_data(G_OBJECT(w), "lp-breath", NULL);
        return FALSE;
    }
    if (pr < 0.01)
        return TRUE;
    cairo_surface_t *s = breath_surface(w);
    if (!s)
        return FALSE;
    double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    double sc = pr * (1.0 - 0.08 * p);
    cairo_translate(cr, W / 2 + room_shift(it), H / 2);
    cairo_scale(cr, sc, sc);
    cairo_set_source_surface(cr, s, -ICON_PX / 2.0, -ICON_PX / 2.0);
    cairo_paint_with_alpha(cr, MIN(pr, 1.0) * (1.0 - 0.45 * p));
    return TRUE;
}

static gboolean dot_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
    Item *it = d;
    double r = it->run.x * CLAMP(it->pres.x, 0.0, 1.0);
    if (r < 0.01 || item_hidden(it))
        return TRUE;
    double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    GdkRGBA a = { 0.66, 0.66, 0.66, 1 }, b = { 0.91, 0.33, 0.13, 1 };
    gtk_style_context_lookup_color(sc, "lp_t2", &a);
    gtk_style_context_lookup_color(sc, "lp_accent", &b);
    double f = CLAMP(it->focus.x, 0, 1);
    /* Under the icon: a round dot, stretched sideways into a short bar
     * when the app is the one in front. */
    double dh = 5.0 * r, dw = (5.0 + 11.0 * it->focus.x) * r;
    double x = (W - dw) / 2 + room_shift(it), y = (H - dh) / 2, rad = dh / 2;
    cairo_set_source_rgba(cr, a.red + (b.red - a.red) * f, a.green + (b.green - a.green) * f,
                          a.blue + (b.blue - a.blue) * f, CLAMP(r, 0, 1));
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + rad, y + rad, rad, G_PI, 1.5 * G_PI);
    cairo_arc(cr, x + dw - rad, y + rad, rad, 1.5 * G_PI, 2 * G_PI);
    cairo_arc(cr, x + dw - rad, y + dh - rad, rad, 0, 0.5 * G_PI);
    cairo_arc(cr, x + rad, y + dh - rad, rad, 0.5 * G_PI, G_PI);
    cairo_close_path(cr);
    cairo_fill(cr);
    return TRUE;
}

static void item_frame(GtkWidget *w, gpointer d)
{
    (void)w;
    Item *it = d;
    /* Keep breathing: turn the pulse round a little before it settles,
     * so the spring never comes to rest (and never drops its tick) while
     * the app is still starting. */
    if (it->launching && fabs(it->pulse.x - it->pulse.target) < 0.08) {
        if (it->pulse.target > 0.5)
            lp_spring_set_target_out(&it->pulse, 0.0);
        else
            lp_spring_set_target(&it->pulse, 1.0);
    }
    gtk_widget_queue_draw(it->dot);
    gtk_widget_queue_draw(it->img);
}

static void item_free(gpointer p)
{
    Item *it = p;
    if (it->give_up)
        g_source_remove(it->give_up);
    lp_motion_free(it->motion);
    lp_motion_remove(lay, &it->pres);
    lp_motion_remove(lay, &it->off);
    g_free(it->id);
    g_free(it->pin_id);
    g_clear_object(&it->info);
    g_free(it);
}

static void on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer d);
static void on_drag_update(GtkGestureDrag *g, double dx, double dy, gpointer d);
static void on_drag_end(GtkGestureDrag *g, double dx, double dy, gpointer d);
static void on_drag_cancel(GtkGesture *g, GdkEventSequence *seq, gpointer d);

/* A new item, in the container but not yet in the row (rebuild_now puts
 * it there). */
static Item *item_new(const char *id, GDesktopAppInfo *info, const Slot *slot,
                      gboolean pinned, const char *pin_id)
{
    Item *it = g_new0(Item, 1);
    it->id = g_strdup(id);
    it->info = info;
    it->slot = slot;
    it->pinned = pinned;
    it->pin_id = g_strdup(pin_id ? pin_id : id);
    it->put_x = G_MININT;

    it->button = gtk_button_new();
    GtkStyleContext *sc = gtk_widget_get_style_context(it->button);
    gtk_style_context_add_class(sc, "lp-dock-item");
    if (!info)
        gtk_style_context_add_class(sc, "lp-missing");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *img = app_image(it);
    it->img = img;
    gtk_widget_set_vexpand(img, TRUE);
    gtk_widget_set_margin_top(img, 6);
    g_signal_connect(img, "draw", G_CALLBACK(img_draw), it);
    gtk_box_pack_start(GTK_BOX(row), img, TRUE, TRUE, 0);
    it->dot = gtk_drawing_area_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(it->dot), "lp-dot");
    gtk_widget_set_size_request(it->dot, 20, 9);
    gtk_widget_set_halign(it->dot, GTK_ALIGN_CENTER);
    g_signal_connect(it->dot, "draw", G_CALLBACK(dot_draw), it);
    gtk_box_pack_start(GTK_BOX(row), it->dot, FALSE, FALSE, 0);
    lp_spring_init(&it->run, LP_SPRING_INSERT, 0.0);
    lp_spring_init(&it->focus, LP_SPRING_EXPAND, 0.0);
    lp_spring_init(&it->pulse, LP_SPRING_WINDOW, 0.0);
    it->motion = lp_motion_new(it->button, item_frame, it);
    lp_motion_add(it->motion, &it->run);
    lp_motion_add(it->motion, &it->focus);
    lp_motion_add(it->motion, &it->pulse);
    gtk_container_add(GTK_CONTAINER(it->button), row);
    /* The name for the touchpad user who rests the pointer; a finger
     * user gets it from the long-press menu's heading. */
    /* A mockup slot goes by what it is ("Terminal"), not by what the
     * package happens to call itself ("Foot"). */
    gtk_widget_set_tooltip_text(it->button, slot ? T(slot->en, slot->ko)
                                                 : lp_app_name(G_APP_INFO(info)));
    lp_on_tap(it->button, (LpTapFn)on_item, it);
    lp_on_hold(it->button, on_hold, it);
    /* Bubble phase, after the tap and long-press gestures (capture) have
     * seen the press: it claims the sequence only once the pointer has
     * travelled DRAG_START, which cancels those two - so a drag is never
     * also a tap, a long press never also a drag. */
    GtkGesture *dg = gtk_gesture_drag_new(it->button);
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(dg), GDK_BUTTON_PRIMARY);
    gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(dg), FALSE);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(dg), GTK_PHASE_BUBBLE);
    g_signal_connect(dg, "drag-begin", G_CALLBACK(on_drag_begin), it);
    g_signal_connect(dg, "drag-update", G_CALLBACK(on_drag_update), it);
    g_signal_connect(dg, "drag-end", G_CALLBACK(on_drag_end), it);
    g_signal_connect(dg, "cancel", G_CALLBACK(on_drag_cancel), it);
    g_object_set_data_full(G_OBJECT(it->button), "lp-dock-drag", dg, g_object_unref);

    lp_spring_init(&it->pres, LP_SPRING_INSERT, 0.0);
    lp_spring_init(&it->off, LP_SPRING_SLIDE, 0.0);
    lp_motion_add(lay, &it->pres);
    lp_motion_add(lay, &it->off);
    gtk_fixed_put(GTK_FIXED(list), it->button, 0, 0);
    gtk_widget_show_all(it->button);
    if (slot_w <= 0) {
        int mn, nat;
        gtk_widget_get_preferred_width(it->button, &mn, &nat);
        slot_w = nat;
        gtk_widget_get_preferred_height(it->button, &mn, &nat);
        slot_h = nat;
        row_h = (int)slot_h;
    }
    return it;
}

/* ── the row ─────────────────────────────────────────────────────── */

/* The container: a GtkFixed that is exactly as wide as the row says,
 * not as wide as its children reach - an item going out at the end is
 * still full width in there while its room shrinks, and the bar has to
 * narrow with the room. */
typedef struct { GtkFixed parent; } LpRow;
typedef struct { GtkFixedClass parent; } LpRowClass;
G_DEFINE_TYPE(LpRow, lp_row, GTK_TYPE_FIXED)

static void row_pref_w(GtkWidget *w, int *min, int *nat) { (void)w; *min = *nat = row_w; }
static void row_pref_h(GtkWidget *w, int *min, int *nat) { (void)w; *min = *nat = row_h; }
static void lp_row_class_init(LpRowClass *c)
{
    GTK_WIDGET_CLASS(c)->get_preferred_width = row_pref_w;
    GTK_WIDGET_CLASS(c)->get_preferred_height = row_pref_h;
}
static void lp_row_init(LpRow *r) { (void)r; }

static double gaps_at(int k, gboolean and_after)
{
    double w = 0;
    for (guint g = 0; gaps && g < gaps->len; g++) {
        Gap *gp = g_ptr_array_index(gaps, g);
        if (gp->at == k || (and_after && gp->at > k))
            w += slot_w * MAX(gp->w.x, 0.0);
    }
    return w;
}

/* Every item's place this frame (lay_x); the row's width. */
static double layout_compute(void)
{
    double x = 0;
    int k = 0;
    for (guint i = 0; i < items->len; i++) {
        Item *it = g_ptr_array_index(items, i);
        if (!in_row(it))
            continue;
        x += gaps_at(k, FALSE);
        it->lay_x = x;
        x += slot_w * MAX(it->pres.x, 0.0);
        k++;
    }
    return x + gaps_at(k, TRUE);
}

static void update_input(void);

static gboolean reap(gpointer d)
{
    (void)d;
    reap_id = 0;
    for (guint i = items->len; i-- > 0;) {
        Item *it = g_ptr_array_index(items, i);
        if (it->leaving && it->pres.x < 0.01 && !it->pres.moving && D.it != it) {
            gtk_widget_destroy(it->button);
            g_ptr_array_remove_index(items, i);
        }
    }
    for (guint g = gaps ? gaps->len : 0; g-- > 0;) {
        Gap *gp = g_ptr_array_index(gaps, g);
        if (gp->w.target < 0.5 && gp->w.x < 0.01 && !gp->w.moving) {
            lp_motion_remove(lay, &gp->w);
            g_ptr_array_remove_index(gaps, g);
        }
    }
    return G_SOURCE_REMOVE;
}

/* The centre of an item's icon, in the stage's coordinates. */
static void icon_centre(const Item *it, double *x, double *y)
{
    int lx = 0, ly = 0;
    gtk_widget_translate_coordinates(list, stage, 0, 0, &lx, &ly);
    GtkAllocation ia, ba;
    gtk_widget_get_allocation(it->img, &ia);
    gtk_widget_get_allocation(it->button, &ba);
    *x = lx + it->lay_x + slot_w * CLAMP(it->pres.x, 0.0, 1.0) / 2.0;
    *y = ly + (ia.y - ba.y) + ia.height / 2.0;
}

static void drag_done(void);

/* Every frame anything in the row moves. */
static void layout_apply(void)
{
    if (slot_w <= 0)
        return;
    double w = layout_compute();
    for (guint i = 0; i < items->len; i++) {
        Item *it = g_ptr_array_index(items, i);
        /* The lifted one's widget stays where the press was: the drag's
         * offsets are measured in its coordinates. */
        if (!in_row(it))
            continue;
        int x = (int)lround(it->lay_x + it->off.x);
        if (x != it->put_x) {
            gtk_fixed_move(GTK_FIXED(list), it->button, x, 0);
            it->put_x = x;
        }
    }
    int wi = (int)ceil(w - 0.01);
    if (wi != row_w) {
        row_w = MAX(wi, 0);
        gtk_widget_queue_resize(list);
    }
    if (D.it && D.settling && !D.removing) {
        double tx, ty;
        icon_centre(D.it, &tx, &ty);
        if (fabs(tx - D.gx.target) > 0.5) lp_spring_set_target(&D.gx, tx);
        if (fabs(ty - D.gy.target) > 0.5) lp_spring_set_target(&D.gy, ty);
    }
    if (D.it && D.settling && !D.gx.moving && !D.gy.moving && !D.ga.moving && !D.gs.moving)
        drag_done();
    gtk_widget_queue_draw(stage);
    if (!reap_id)
        reap_id = g_idle_add(reap, NULL);
}

static void layout_frame(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    layout_apply();
}

/* ── dragging ────────────────────────────────────────────────────── */

static int row_index(const Item *it)
{
    int k = 0;
    for (guint i = 0; i < items->len; i++) {
        Item *o = g_ptr_array_index(items, i);
        if (o == it)
            return k;
        if (in_row(o))
            k++;
    }
    return k;
}

static Gap *gap_open(int k, gboolean full)
{
    if (!gaps)
        gaps = g_ptr_array_new_with_free_func(g_free);
    Gap *gp = NULL;
    for (guint g = 0; g < gaps->len; g++)
        if (((Gap *)g_ptr_array_index(gaps, g))->at == k)
            gp = g_ptr_array_index(gaps, g);
    if (!gp) {
        gp = g_new0(Gap, 1);
        gp->at = k;
        lp_spring_init(&gp->w, LP_SPRING_INSERT, full ? 1.0 : 0.0);
        lp_motion_add(lay, &gp->w);
        g_ptr_array_add(gaps, gp);
    }
    if (full)
        lp_spring_jump(&gp->w, 1.0);
    else
        lp_spring_set_target(&gp->w, 1.0);
    return gp;
}

/* The gap to be at k (or nowhere, k < 0); every other one closes. */
static void gap_move_to(int k)
{
    for (guint g = 0; gaps && g < gaps->len; g++) {
        Gap *gp = g_ptr_array_index(gaps, g);
        if (gp->at != k && gp->w.target > 0.5)
            lp_spring_set_target_out(&gp->w, 0.0);
    }
    if (k >= 0)
        gap_open(k, FALSE);
    lp_motion_kick(lay);
}

/* Where the lifted icon would land: counted on the row as it would be
 * without any gap, so the gap opening does not move the answer. Pinned
 * apps first: an icon dropped past the last pinned one lands after it -
 * except a running, unpinned app over the unpinned ones, which stays
 * where it was. */
static int drop_index(double stage_x)
{
    int lx = 0, ly = 0;
    gtk_widget_translate_coordinates(list, stage, 0, 0, &lx, &ly);
    double x = stage_x - lx, cum = 0;
    int k = 0, n = 0, pinned_end = 0;
    for (guint i = 0; i < items->len; i++) {
        Item *it = g_ptr_array_index(items, i);
        if (!in_row(it))
            continue;
        double room = slot_w * CLAMP(it->pres.target, 0.0, 1.0);
        if (cum + room / 2 < x)
            k = n + 1;
        cum += room;
        n++;
        if (it->pinned && !it->leaving)
            pinned_end = n;
    }
    if (D.it->pinned)
        return MIN(k, pinned_end);
    return k > pinned_end ? D.from : k;
}

static void on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer d)
{
    (void)x; (void)y;
    Item *it = d;
    if (D.it || it->leaving || slot_w <= 0) {
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }
    D.it = it;
    D.active = D.settling = D.out = D.removing = FALSE;
    icon_centre(it, &D.cx0, &D.cy0);
}

static void on_drag_update(GtkGestureDrag *g, double dx, double dy, gpointer d)
{
    Item *it = d;
    if (D.it != it || D.settling)
        return;
    if (!D.active) {
        if (hypot(dx, dy) < DRAG_START)
            return;
        /* A drag now: the tap and the long press are off. */
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
        it->drag_eaten = TRUE;
        D.from = row_index(it);
        D.active = TRUE;
        gtk_style_context_add_class(gtk_widget_get_style_context(it->button), "lp-dragging");
        /* Its room stays, as a gap of the same width: nothing moves yet. */
        gap_open(D.from, TRUE);
        lp_spring_jump(&D.gs, 1.0);
        lp_spring_set_target(&D.gs, 1.12);
        lp_spring_jump(&D.ga, 1.0);
    }
    lp_spring_jump(&D.gx, D.cx0 + dx);
    /* No higher than leaves the icon and its "Remove" inside the surface:
     * the room above the bar is all there is to draw in. */
    lp_spring_jump(&D.gy, MAX(D.cy0 + dy, ICON_PX * 1.2 + 34));
    /* Lifted most of an item's height above the dock: off it. Only a
     * pinned app can be taken off; a running one just goes back. */
    gboolean out = it->pinned && dy < -slot_h * 0.9;
    if (out != D.out) {
        D.out = out;
        lp_spring_set_target(&D.gs, out ? 0.8 : 1.12);
        lp_spring_set_target(&D.ga, out ? 0.6 : 1.0);
    }
    gap_move_to(out ? -1 : drop_index(D.cx0 + dx));
    layout_apply();
}

/* The pinned list in the dock's order, written down. */
static void save_order(void)
{
    GPtrArray *pins = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < items->len; i++) {
        Item *it = g_ptr_array_index(items, i);
        if (it->pinned && !it->leaving)
            g_ptr_array_add(pins, g_strdup(it->pin_id));
    }
    write_pins(pins);
    g_ptr_array_unref(pins);
}

static void drop(gboolean cancelled)
{
    Item *it = D.it;
    D.active = FALSE;
    D.settling = TRUE;
    if (D.out && !cancelled) {
        /* Off the dock: unpinned, in a puff where it was let go. Its gap
         * is already closing; it comes back (at the end) only if it is
         * running. */
        D.removing = TRUE;
        it->leaving = TRUE;
        it->pinned = FALSE;
        lp_spring_jump(&it->pres, 0.0);
        gap_move_to(-1);
        lp_spring_set_target_out(&D.gs, 0.25);
        lp_spring_set_target_out(&D.ga, 0.0);
        save_order();
        lp_motion_kick(lay);
        return;
    }
    /* Into the gap: the item takes its place in the row, starting at the
     * gap's width so nothing beside it jumps. */
    Gap *gp = NULL;
    for (guint g = 0; gaps && g < gaps->len; g++) {
        Gap *o = g_ptr_array_index(gaps, g);
        if (o->w.target > 0.5)
            gp = o;
    }
    int k = gp ? gp->at : D.from;
    double w0 = gp ? gp->w.x : 0.0;
    if (gp) {
        lp_motion_remove(lay, &gp->w);
        g_ptr_array_remove(gaps, gp);
    }
    for (guint g = 0; gaps && g < gaps->len; g++) {
        Gap *o = g_ptr_array_index(gaps, g);
        if (o->at > k)
            o->at++;
    }
    /* Out of the array, and back in as the k-th item of the row. */
    g_ptr_array_set_free_func(items, NULL);
    g_ptr_array_remove(items, it);
    g_ptr_array_set_free_func(items, item_free);
    guint pos = items->len;
    int n = 0;
    for (guint i = 0; i < items->len; i++) {
        if (n == k) {
            pos = i;
            break;
        }
        if (in_row(g_ptr_array_index(items, i)))
            n++;
    }
    g_ptr_array_insert(items, pos, it);
    lp_spring_jump(&it->pres, w0);
    lp_spring_set_target(&it->pres, 1.0);
    lp_spring_jump(&it->off, 0.0);
    it->put_x = G_MININT;
    lp_spring_set_target(&D.gs, 1.0);
    lp_spring_set_target(&D.ga, 1.0);
    layout_compute();
    double tx, ty;
    icon_centre(it, &tx, &ty);
    lp_spring_set_target(&D.gx, tx);
    lp_spring_set_target(&D.gy, ty);
    gboolean moved = k != D.from;
    if (!cancelled && moved) {
        if (!it->pinned) {
            /* A running app dropped among the pinned ones: pinned there. */
            it->pinned = TRUE;
            g_free(it->pin_id);
            it->pin_id = g_strdup(it->id);
        }
        save_order();
    }
    lp_motion_kick(lay);
    layout_apply();
}

static void drag_done(void)
{
    Item *it = D.it;
    D.it = NULL;
    D.settling = D.removing = D.out = FALSE;
    if (it)
        gtk_style_context_remove_class(gtk_widget_get_style_context(it->button), "lp-dragging");
    gtk_widget_queue_draw(stage);
    if (rebuild_after_drag) {
        rebuild_after_drag = FALSE;
        rebuild();
    }
}

static void on_drag_end(GtkGestureDrag *g, double dx, double dy, gpointer d)
{
    (void)g; (void)dx; (void)dy;
    Item *it = d;
    if (D.it != it || D.settling)
        return;
    if (!D.active) {
        D.it = NULL;            /* a tap: the tap gesture has it */
        return;
    }
    drop(FALSE);
}

static void on_drag_cancel(GtkGesture *g, GdkEventSequence *seq, gpointer d)
{
    (void)g; (void)seq;
    Item *it = d;
    if (D.it != it || D.settling)
        return;
    if (!D.active) {
        D.it = NULL;
        return;
    }
    drop(TRUE);
}

/* The lifted icon, over everything in the dock's surface; and "Remove"
 * over it while it is off the dock. */
static gboolean ghost_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    if (!D.it || !(D.active || D.settling))
        return FALSE;
    cairo_surface_t *s = breath_surface(D.it->img);
    if (!s)
        return FALSE;
    double a = CLAMP(D.ga.x, 0.0, 1.0), sc = MAX(D.gs.x, 0.0);
    cairo_save(cr);
    cairo_translate(cr, D.gx.x, D.gy.x);
    cairo_scale(cr, sc, sc);
    cairo_set_source_surface(cr, s, -ICON_PX / 2.0, -ICON_PX / 2.0);
    cairo_paint_with_alpha(cr, a);
    cairo_restore(cr);
    if (D.out && D.active) {
        PangoLayout *pl = gtk_widget_create_pango_layout(w, T("Remove", "고정 해제"));
        PangoFontDescription *fd = pango_font_description_from_string("Pretendard Variable Semi-Bold 10");
        pango_layout_set_font_description(pl, fd);
        int tw, th;
        pango_layout_get_pixel_size(pl, &tw, &th);
        double bx = D.gx.x - tw / 2.0 - 10, by = D.gy.x - ICON_PX * sc / 2 - th - 14;
        double bw = tw + 20, bh = th + 8, r = bh / 2;
        cairo_new_sub_path(cr);
        cairo_arc(cr, bx + r, by + r, r, G_PI / 2, 3 * G_PI / 2);
        cairo_arc(cr, bx + bw - r, by + r, r, -G_PI / 2, G_PI / 2);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, 0.06, 0.13, 0.20, 0.92);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.92, 0.95, 0.97, 1.0);
        cairo_move_to(cr, bx + 10, by + 4);
        pango_cairo_show_layout(cr, pl);
        pango_font_description_free(fd);
        g_object_unref(pl);
    }
    return FALSE;
}

static void paint_dots(gboolean animate)
{
    for (guint i = 0; i < items->len; i++) {
        Item *it = g_ptr_array_index(items, i);
        GList *wins = windows_of(it);
        gboolean focused = FALSE;
        for (GList *l = wins; l; l = l->next)
            if (((LpToplevel *)l->data)->activated)
                focused = TRUE;
        double run = wins ? 1.0 : 0.0, foc = focused ? 1.0 : 0.0;
        if (wins)
            pulse_stop(it);        /* it has a window: it has started */
        g_list_free(wins);
        if (!animate) {
            lp_spring_jump(&it->run, run);
            lp_spring_jump(&it->focus, foc);
        } else {
            if (run != it->run.target) {
                if (run > 0.5) lp_spring_set_target(&it->run, run);
                else lp_spring_set_target_out(&it->run, run);
            }
            if (foc != it->focus.target) {
                if (foc > 0.5) lp_spring_set_target(&it->focus, foc);
                else lp_spring_set_target_out(&it->focus, foc);
            }
        }
        lp_motion_kick(it->motion);
    }
}

static gboolean owned_by_items(const char *app_id)
{
    for (guint i = 0; i < items->len; i++) {
        Item *it = g_ptr_array_index(items, i);
        if (!it->leaving && it->info && lp_app_owns(it->info, app_id))
            return TRUE;
    }
    return FALSE;
}

/* What the dock should show, in order: the pinned list, then the running
 * apps that are not pinned. */
typedef struct {
    char *use;               /* the desktop id the item is for */
    GDesktopAppInfo *info;   /* owned; NULL for a slot nothing fills */
    const Slot *slot;
    gboolean pinned;
    char *pin_id;
} Want;

static void want_free(gpointer p)
{
    Want *w = p;
    g_free(w->use);
    g_free(w->pin_id);
    g_clear_object(&w->info);
    g_free(w);
}

static gboolean wanted_owns(GPtrArray *want, const char *app_id)
{
    for (guint i = 0; i < want->len; i++) {
        Want *w = g_ptr_array_index(want, i);
        if (w->info && lp_app_owns(w->info, app_id))
            return TRUE;
    }
    return FALSE;
}

/* The dock brought up to date without starting over: items that stay are
 * the same widgets (their dots and springs carry on), new ones grow in,
 * gone ones shrink out where they are, and any that changed place slide
 * there (see "the row"). */
static void rebuild_now(void)
{
    if (D.it) {
        rebuild_after_drag = TRUE;     /* after the dragged icon has landed */
        return;
    }
    GPtrArray *want = g_ptr_array_new_with_free_func(want_free);
    GPtrArray *pins = read_pins();
    for (guint i = 0; i < pins->len; i++) {
        const char *id = g_ptr_array_index(pins, i);
        const Slot *slot = slot_for(id);
        GDesktopAppInfo *info = NULL;
        const char *use = id;
        if (slot) {
            const char *got = slot_installed(slot, &info);
            if (got)
                use = got;
        } else {
            info = g_desktop_app_info_new(id);
            if (!info)
                continue;       /* uninstalled and unknown: nothing to draw */
        }
        Want *w = g_new0(Want, 1);
        w->use = g_strdup(use);
        w->info = info;
        w->slot = slot;
        w->pinned = TRUE;
        w->pin_id = g_strdup(id);
        g_ptr_array_add(want, w);
    }
    g_ptr_array_unref(pins);
    /* Running applications that are not pinned, after the pinned ones,
     * in the order they were opened. */
    for (GList *l = lp_toplevels(); l; l = l->next) {
        LpToplevel *t = l->data;
        if (!t->done || !t->app_id || wanted_owns(want, t->app_id))
            continue;
        GDesktopAppInfo *info = lp_app_for_id(t->app_id);
        if (!info)
            continue;
        Want *w = g_new0(Want, 1);
        w->use = g_strdup(g_app_info_get_id(G_APP_INFO(info)));
        w->info = info;
        w->pin_id = g_strdup(w->use);
        g_ptr_array_add(want, w);
    }

    /* Where everything is drawn now, to slide what changes place. */
    layout_compute();
    for (guint i = 0; i < items->len; i++) {
        Item *it = g_ptr_array_index(items, i);
        it->lay_x += it->off.x;          /* drawn position, kept below */
    }

    GPtrArray *next = g_ptr_array_new();
    GHashTable *fresh = g_hash_table_new(NULL, NULL);
    for (guint i = 0; i < want->len; i++) {
        Want *w = g_ptr_array_index(want, i);
        Item *it = NULL;
        for (guint j = 0; j < items->len && !it; j++) {
            Item *o = g_ptr_array_index(items, j);
            /* Same app, and still the same kind of thing: a dim slot that
             * got its app installed is a new icon, not the old one lit. */
            if (!o->leaving && strcmp(o->id, w->use) == 0 && !o->info == !w->info &&
                !g_ptr_array_find(next, o, NULL))
                it = o;
        }
        if (it) {
            it->pinned = w->pinned;
            g_free(it->pin_id);
            it->pin_id = g_strdup(w->pin_id);
            g_clear_object(&w->info);
        } else {
            it = item_new(w->use, w->info, w->slot, w->pinned, w->pin_id);
            w->info = NULL;              /* the item has it */
            g_hash_table_add(fresh, it);
        }
        g_ptr_array_add(next, it);
    }
    /* What is not wanted any more goes out from where it is. */
    int last = -1;
    for (guint i = 0; i < items->len; i++) {
        Item *o = g_ptr_array_index(items, i);
        guint pos;
        if (g_ptr_array_find(next, o, &pos)) {
            last = (int)pos;
            continue;
        }
        if (!o->leaving) {
            o->leaving = TRUE;
            o->pinned = FALSE;
            gtk_style_context_add_class(gtk_widget_get_style_context(o->button), "lp-leaving");
            if (built_once && !lp_motion_reduced())
                lp_spring_set_target_out(&o->pres, 0.0);
            else
                lp_spring_jump(&o->pres, 0.0);
        }
        g_ptr_array_insert(next, last + 1, o);
        last++;
    }
    g_ptr_array_set_free_func(items, NULL);
    g_ptr_array_set_size(items, 0);
    for (guint i = 0; i < next->len; i++)
        g_ptr_array_add(items, g_ptr_array_index(next, i));
    g_ptr_array_set_free_func(items, item_free);
    g_ptr_array_unref(next);
    g_ptr_array_unref(want);

    /* New ones grow in - all at once, without growing, at the start. */
    gboolean animate = built_once && !lp_motion_reduced();
    for (guint i = 0; i < items->len; i++) {
        Item *it = g_ptr_array_index(items, i);
        if (!g_hash_table_contains(fresh, it))
            continue;
        if (animate)
            lp_spring_set_target(&it->pres, 1.0);
        else
            lp_spring_jump(&it->pres, 1.0);
    }
    /* The ones that stay keep their drawn position and slide from it. */
    GArray *was = g_array_new(FALSE, FALSE, sizeof(double));
    for (guint i = 0; i < items->len; i++) {
        Item *it = g_ptr_array_index(items, i);
        double x = it->lay_x;
        g_array_append_val(was, x);
    }
    layout_compute();
    for (guint i = 0; i < items->len; i++) {
        Item *it = g_ptr_array_index(items, i);
        if (g_hash_table_contains(fresh, it))
            continue;
        double d = g_array_index(was, double, i) - it->lay_x;
        if (!animate) {
            lp_spring_jump(&it->off, 0.0);
        } else if (fabs(d - it->off.x) > 0.5) {
            it->off.x = d;
            lp_spring_set_target(&it->off, 0.0);
        }
    }
    g_array_unref(was);
    g_hash_table_unref(fresh);

    built_once = TRUE;
    paint_dots(TRUE);
    publish_pins();
    layout_apply();
    lp_motion_kick(lay);
}

static gboolean rebuild_idle(gpointer d)
{
    (void)d;
    rebuild_id = 0;
    rebuild_now();
    return G_SOURCE_REMOVE;
}

static void rebuild(void)
{
    if (!rebuild_id)
        rebuild_id = g_idle_add(rebuild_idle, NULL);
}

static void update_away(void);

static void on_toplevels(gpointer d)
{
    (void)d;
    update_away();
    /* A window of an app not yet in the dock needs a new item; anything
     * else is only a dot. Rebuilding for every title change would redraw
     * the dock every time a terminal prints its working directory. */
    for (GList *l = lp_toplevels(); l; l = l->next) {
        LpToplevel *t = l->data;
        if (t->done && t->app_id && !owned_by_items(t->app_id)) {
            GDesktopAppInfo *d2 = lp_app_for_id(t->app_id);
            if (d2) {
                g_object_unref(d2);
                rebuild();
                return;
            }
        }
    }
    /* And an unpinned item whose last window closed has to go. */
    for (guint i = 0; i < items->len; i++) {
        Item *it = g_ptr_array_index(items, i);
        if (it->leaving)
            continue;
        GList *w = it->pinned ? NULL : windows_of(it);
        gboolean gone = !it->pinned && !w;
        g_list_free(w);
        if (gone) {
            rebuild();
            return;
        }
    }
    paint_dots(TRUE);
}

static void on_pins_changed(GFileMonitor *m, GFile *f, GFile *o,
                            GFileMonitorEvent ev, gpointer d)
{
    (void)m; (void)f; (void)o; (void)d;
    if (ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
        ev == G_FILE_MONITOR_EVENT_DELETED ||
        ev == G_FILE_MONITOR_EVENT_CREATED)
        rebuild();
}

static void on_apps_changed(GAppInfoMonitor *m, gpointer d)
{
    (void)m; (void)d;
    rebuild();
}

static void on_grid(GtkWidget *b, gpointer d)
{
    (void)b; (void)d;
    const char *a[] = { "lp-appgrid", "toggle", NULL };
    if (!lp_send("appgrid", a))
        lp_spawn(a);
}

/* ── pulling the app grid up ─────────────────────────────────────── */

typedef struct {
    LpVelocity v;
    gboolean   pulling;
} Pull;

static void grid_send(const char *verb, double v)
{
    char num[32];
    g_ascii_formatd(num, sizeof num, "%.1f", v);
    const char *a[] = { "lp-appgrid", verb, num, NULL };
    if (!lp_send("appgrid", a) && !strcmp(verb, "release-px") && v > 0) {
        const char *s2[] = { "lp-appgrid", "show", NULL };
        lp_spawn(s2);
    }
}

static void on_pull_update(GtkGestureDrag *g, double dx, double dy, gpointer d)
{
    Pull *p = d;
    double up = -dy;
    if (!p->pulling) {
        if (up < 10 || up < 1.5 * ABS(dx))
            return;
        p->pulling = TRUE;
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
        lp_velocity_reset(&p->v);
    }
    double px = MAX(0.0, up - 10);
    lp_velocity_add(&p->v, g_get_monotonic_time(), px);
    grid_send("drag-px", px);
}

static void on_pull_end(GtkGestureDrag *g, double dx, double dy, gpointer d)
{
    (void)g; (void)dx;
    Pull *p = d;
    if (!p->pulling)
        return;
    p->pulling = FALSE;
    lp_velocity_add(&p->v, g_get_monotonic_time(), MAX(0.0, -dy - 10));
    grid_send("release-px", lp_velocity_get(&p->v));
}

static void on_pull_cancel(GtkGesture *g, GdkEventSequence *seq, gpointer d)
{
    (void)g; (void)seq;
    Pull *p = d;
    if (p->pulling) {
        p->pulling = FALSE;
        grid_send("release-px", 0);
    }
}

static void add_pull(GtkWidget *w, gboolean touch_only)
{
    Pull *p = g_new0(Pull, 1);
    GtkGesture *g = gtk_gesture_drag_new(w);
    gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(g), touch_only);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(g),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(g, "drag-update", G_CALLBACK(on_pull_update), p);
    g_signal_connect(g, "drag-end", G_CALLBACK(on_pull_end), p);
    g_signal_connect(g, "cancel", G_CALLBACK(on_pull_cancel), p);
    g_object_set_data_full(G_OBJECT(w), "lp-pull", g, g_object_unref);
    g_object_set_data_full(G_OBJECT(w), "lp-pull-state", p, g_free);
}

static gboolean on_edge_enter(GtkWidget *w, GdkEventCrossing *e, gpointer d);
static gboolean on_edge_leave(GtkWidget *w, GdkEventCrossing *e, gpointer d);

/* The bottom edge of every output: an invisible strip that only a finger
 * can use. */
static GPtrArray *edges;

static void edge_for(GdkMonitor *mon)
{
    GtkWindow *e = lp_layer_window("lp-edge", GTK_LAYER_SHELL_LAYER_TOP,
                                   LP_EDGE_BOTTOM | LP_EDGE_LEFT | LP_EDGE_RIGHT);
    if (!edges)
        edges = g_ptr_array_new();
    g_ptr_array_add(edges, e);
    gtk_layer_set_monitor(e, mon);
    gtk_layer_set_exclusive_zone(e, -1);
    gtk_widget_set_size_request(GTK_WIDGET(e), -1, EDGE_PX);
    GtkWidget *area = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(area), FALSE);
    gtk_container_add(GTK_CONTAINER(e), area);
    add_pull(area, TRUE);
    /* ... and the pointer, which brings the dock up over a full-screen
     * window (see update_away). */
    gtk_widget_add_events(area, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(area, "enter-notify-event", G_CALLBACK(on_edge_enter), NULL);
    g_signal_connect(area, "leave-notify-event", G_CALLBACK(on_edge_leave), NULL);
    gtk_widget_show_all(GTK_WIDGET(e));
}

/* The room the dock takes. The dock is anchored to a corner (see main),
 * and the layer-shell rule is that a corner-anchored surface reserves
 * nothing - so a maximised window went on under the dock and its bottom
 * was hidden. This surface is anchored along the whole bottom edge, one
 * pixel high, invisible and never clicked, and reserves the dock's
 * height and the gap under it: windows stop above the dock. */
static GtkWindow *spacer;
static gboolean dock_away;      /* slid away: the focused window is full screen */
static GtkWidget *dock_bar;     /* the visible bar, inside its shadow's room */

/* The app grid covers the whole screen and keeps its tiles out of this
 * much at the bottom, where the dock floats over it. */
static void publish_zone(int zone)
{
    char *p = g_build_filename(g_get_user_runtime_dir(), "lp-dock-zone", NULL);
    char n[16];
    g_snprintf(n, sizeof n, "%d\n", zone);
    g_file_set_contents(p, n, -1, NULL);
    g_free(p);
}

static void reserve_room(void)
{
    if (!spacer)
        return;
    int h = dock_bar ? gtk_widget_get_allocated_height(dock_bar) : 0;
    if (h <= 1)
        return;
    /* The bar's own height and the gap under it, not the surface's: the
     * surface is taller by the room above the bar for its shadow (and for
     * an icon being dragged), and a maximised window stopped that far
     * above the dock - a strip of wallpaper between the two. It meets
     * the bar's top edge now. */
    if (dock_bar) {
        GtkBorder m = { 0 };
        GtkStyleContext *sc = gtk_widget_get_style_context(dock_bar);
        gtk_style_context_get_margin(sc, gtk_style_context_get_state(sc), &m);
        if (m.top > 0 && m.top < h)
            h -= m.top;
    }
    /* Kept while a window is full screen too: a full-screen window
     * covers the reserved room anyway, and giving it back made wayfire
     * re-tile that window - out of full screen again. */
    int zone = h + DOCK_MARGIN;
    if (gtk_layer_get_exclusive_zone(spacer) != zone) {
        gtk_layer_set_exclusive_zone(spacer, zone);
        publish_zone(zone);
    }
}

static void spacer_realized(GtkWidget *w, gpointer d)
{
    (void)d;
    cairo_region_t *none = cairo_region_create();
    gtk_widget_input_shape_combine_region(w, none);
    cairo_region_destroy(none);
}

/* ── out of the way of a full-screen window ──────────────────────────
 *
 * Windows open and stop above the dock: the spacer keeps that room. A
 * maximised window does too - it fills the screen down to the dock, and
 * the dock stays where it is, always there to be clicked. (For a while a
 * maximised window sent the dock away as well; the dock then seemed to
 * vanish whenever a window was made big, and the owner wants it to stay.)
 *
 * Only a full-screen window - F11, or Super+F - has the whole screen:
 * the dock slides off the bottom edge and gives its room back. Pushing
 * the pointer against the bottom edge brings it up over the window (the
 * edge strip below catches it); it goes again a moment after the pointer
 * leaves it. When the window leaves full screen, is minimised, closed or
 * loses focus to one that is not full screen, the dock comes back. */
#define SLIDE_MS 200
static gboolean dock_peek;      /* up over a full-screen window, for now */
static double dock_pos;         /* 0 = in place, 1 = below the edge */
static double slide_from, slide_to;
static gint64 slide_start;
static guint slide_id, leave_id;

/* wayfire says it outright (lp-wfshell.h: its foreign-toplevel state
 * never carries F11); sway's foreign-toplevel state is right. */
static gboolean wf_shell;       /* wayfire-shell is there */
static gboolean wf_fullscreen;  /* a full-screen window covers the output */

static gboolean window_wants_screen(void)
{
    if (wf_fullscreen)
        return TRUE;
    for (GList *l = lp_toplevels(); l; l = l->next) {
        LpToplevel *t = l->data;
        if (t->done && t->activated && !t->minimized && t->fullscreen)
            return TRUE;
    }
    return FALSE;
}

/* Hidden, the dock still has its top pixel row on the screen - a row of
 * its transparent margin, which shows nothing. Fully off the screen it
 * got no more frame callbacks from the compositor, GTK waited for one
 * before committing the next margin, and the dock never came back. */
static void place_dock(void)
{
    int h = gtk_widget_get_allocated_height(GTK_WIDGET(win));
    int down = (int)(dock_pos * (h + DOCK_MARGIN - 1) + 0.5);
    int m = DOCK_MARGIN - down;
    if (gtk_layer_get_margin(win, GTK_LAYER_SHELL_EDGE_BOTTOM) != m)
        gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_BOTTOM, m);
}

static gboolean slide_step(gpointer d)
{
    (void)d;
    double t = (g_get_monotonic_time() - slide_start) / 1000.0 / SLIDE_MS;
    if (t > 1)
        t = 1;
    double u = 1 - t;
    dock_pos = slide_from + (slide_to - slide_from) * (1 - u * u * u);
    place_dock();
    if (t < 1)
        return G_SOURCE_CONTINUE;
    slide_id = 0;
    return G_SOURCE_REMOVE;
}

static void slide(double to)
{
    if (slide_id) {
        g_source_remove(slide_id);
        slide_id = 0;
    }
    if (lp_motion_reduced() || !gtk_widget_get_mapped(GTK_WIDGET(win))) {
        dock_pos = to;
        place_dock();
        return;
    }
    slide_from = dock_pos;
    slide_to = to;
    slide_start = g_get_monotonic_time();
    slide_id = g_timeout_add(16, slide_step, NULL);
}

/* Both compositors draw a full-screen window over the top layer, where
 * the dock and the edge strip live: the dock could not be brought up
 * over that window, and the strip under it never saw the pointer. While
 * a window is full screen the two move up to the overlay layer, and back
 * down after - the rest of the time the dock has no business over the
 * lock screen or a notification. */
/* Unmapped and mapped again around the change: gtk-layer-shell sends
 * set_layer to a mapped surface, and wayfire 0.7 reads a surface's layer
 * only when it is mapped - the dock and the strip stayed under the
 * full-screen window, and the pointer at the bottom edge reached the
 * window instead. */
static void move_to_layer(GtkWindow *w, GtkLayerShellLayer l)
{
    if (gtk_layer_get_layer(w) == l)
        return;
    gboolean shown = gtk_widget_get_visible(GTK_WIDGET(w));
    if (shown)
        gtk_widget_hide(GTK_WIDGET(w));
    gtk_layer_set_layer(w, l);
    if (shown)
        gtk_widget_show(GTK_WIDGET(w));
}

static void set_layers(gboolean over)
{
    GtkLayerShellLayer l = over ? GTK_LAYER_SHELL_LAYER_OVERLAY
                                : GTK_LAYER_SHELL_LAYER_TOP;
    move_to_layer(win, l);
    /* Under wayfire the bottom edge is its hotspot (below), which is
     * reported over a full-screen window; the strips stay where they are. */
    if (wf_shell)
        return;
    for (guint i = 0; edges && i < edges->len; i++)
        move_to_layer(g_ptr_array_index(edges, i), l);
}

/* The app grid is open: it covers the whole screen, as Launchpad does,
 * and the dock stays over it - on the overlay layer, as over a
 * full-screen window, since the grid is on the top layer too and was
 * put above the dock each time it opened. */
static gboolean grid_open;

static void update_away(void)
{
    gboolean away = window_wants_screen();
    if (away == dock_away)
        return;
    dock_away = away;
    dock_peek = FALSE;
    if (leave_id) {
        g_source_remove(leave_id);
        leave_id = 0;
    }
    reserve_room();
    set_layers(away || grid_open);
    slide(away ? 1.0 : 0.0);
}

static gboolean go_again(gpointer d)
{
    (void)d;
    leave_id = 0;
    if (dock_menus > 0)
        return G_SOURCE_REMOVE;          /* dock_menu_done asks again */
    if (dock_away && dock_peek) {
        dock_peek = FALSE;
        slide(1.0);
    }
    return G_SOURCE_REMOVE;
}

/* A menu closed: the dock goes as if the pointer had just left it. If
 * the pointer is on the dock, its enter (the menu surface going) takes
 * that back. */
static void dock_menu_done(GtkMenuShell *menu, gpointer d)
{
    (void)menu; (void)d;
    if (dock_menus > 0)
        dock_menus--;
    if (!dock_menus && dock_away && dock_peek && !leave_id)
        leave_id = g_timeout_add(700, go_again, NULL);
}

static gboolean on_edge_enter(GtkWidget *w, GdkEventCrossing *e, gpointer d)
{
    (void)w; (void)e; (void)d;
    if (dock_away && !dock_peek) {
        dock_peek = TRUE;
        slide(0.0);
    }
    return FALSE;
}

/* Off the edge strip and not onto the dock: gone again, as off the dock. */
static gboolean on_edge_leave(GtkWidget *w, GdkEventCrossing *e, gpointer d)
{
    (void)w; (void)d;
    if (e->mode != GDK_CROSSING_NORMAL)
        return FALSE;
    if (dock_away && dock_peek && !leave_id)
        leave_id = g_timeout_add(700, go_again, NULL);
    return FALSE;
}

static void on_wf_fullscreen(GdkMonitor *mon, gboolean fs, gpointer d)
{
    (void)mon; (void)d;
    wf_fullscreen = fs;
    update_away();
}

/* The pointer (or a finger) resting at the bottom edge: up over the
 * full-screen window, and away again a moment after it has left both the
 * edge and the dock. */
static void on_wf_hotspot(GdkMonitor *mon, gboolean inside, gpointer d)
{
    (void)mon; (void)d;
    if (inside) {
        if (leave_id) {
            g_source_remove(leave_id);
            leave_id = 0;
        }
        if (dock_away && !dock_peek) {
            dock_peek = TRUE;
            slide(0.0);
        }
    } else if (dock_away && dock_peek && !leave_id) {
        leave_id = g_timeout_add(700, go_again, NULL);
    }
}

static gboolean on_dock_enter(GtkWidget *w, GdkEventCrossing *e, gpointer d)
{
    (void)w; (void)e; (void)d;
    if (leave_id) {
        g_source_remove(leave_id);
        leave_id = 0;
    }
    /* Its one row left on the screen is reached before the edge strip
     * is, where the two overlap. */
    if (dock_away && !dock_peek) {
        dock_peek = TRUE;
        slide(0.0);
    }
    return FALSE;
}

static gboolean on_dock_leave(GtkWidget *w, GdkEventCrossing *e, gpointer d)
{
    (void)w; (void)d;
    /* Into one of its own items, or a menu it opened: still here. */
    if (e->detail == GDK_NOTIFY_INFERIOR || e->mode != GDK_CROSSING_NORMAL)
        return FALSE;
    if (dock_away && dock_peek && !leave_id)
        leave_id = g_timeout_add(700, go_again, NULL);
    return FALSE;
}

static gboolean recentre(gpointer d)
{
    (void)d;
    reserve_room();
    /* Its height is known only now: a dock that had to be away from the
     * start (a full-screen window already there) is put away here. */
    place_dock();
    update_input();
    return G_SOURCE_REMOVE;
}

/* The surface is as wide as the screen, and transparent but for the bar:
 * only the bar takes the pointer and the fingers, and everything else
 * goes through to the windows under it. (It used to be as wide as the
 * bar and centred by a margin; a bar that narrows and widens every frame
 * then meant a new surface size and a new margin every frame, and the
 * two never land in the same one - the bar shook.) */
static void update_input(void)
{
    static GdkRectangle last = { -1, -1, -1, -1 };
    if (!dock_bar || !gtk_widget_get_realized(GTK_WIDGET(win)))
        return;
    GtkAllocation a;
    gtk_widget_get_allocation(dock_bar, &a);
    GtkBorder m = { 0 };
    GtkStyleContext *sc = gtk_widget_get_style_context(dock_bar);
    gtk_style_context_get_margin(sc, gtk_style_context_get_state(sc), &m);
    GdkRectangle r = { a.x + m.left, a.y + m.top, a.width - m.left - m.right,
                       a.height - m.top - m.bottom };
    if (r.width <= 0 || r.height <= 0)
        return;
    if (r.x == last.x && r.y == last.y && r.width == last.width && r.height == last.height)
        return;
    last = r;
    cairo_region_t *reg = cairo_region_create_rectangle((cairo_rectangle_int_t *)&r);
    gtk_widget_input_shape_combine_region(GTK_WIDGET(win), reg);
    cairo_region_destroy(reg);
}

static void on_dock_allocate(GtkWidget *w, GdkRectangle *a, gpointer d)
{
    (void)w; (void)a; (void)d;
    /* Not from inside the allocation itself: a zone change asks the
     * compositor for a new configure. */
    g_idle_add(recentre, NULL);
}

static void on_conf_changed(GFileMonitor *m, GFile *f, GFile *o,
                            GFileMonitorEvent ev, gpointer d)
{
    (void)m; (void)f; (void)o; (void)d;
    if (ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
        ev == G_FILE_MONITOR_EVENT_CREATED ||
        ev == G_FILE_MONITOR_EVENT_MOVED_IN)
        exit(0);                 /* lp-shell-start brings it back resized */
}

static void on_command(int argc, char **argv, gpointer d)
{
    (void)d;
    if (argc >= 2 && strcmp(argv[1], "refresh") == 0)
        rebuild();
    else if (argc >= 3 && strcmp(argv[1], "pin") == 0)
        set_pinned(argv[2], TRUE);
    else if (argc >= 3 && strcmp(argv[1], "unpin") == 0)
        set_pinned(argv[2], FALSE);
    else if (argc >= 4 && strcmp(argv[1], "open") == 0 &&
             strcmp(argv[2], "grid") == 0) {
        GtkStyleContext *sc = gtk_widget_get_style_context(grid_button);
        grid_open = strcmp(argv[3], "1") == 0;
        if (grid_open)
            gtk_style_context_add_class(sc, "lp-open");
        else
            gtk_style_context_remove_class(sc, "lp-open");
        set_layers(dock_away || grid_open);
    }
}

int main(int argc, char **argv)
{
    if (!lp_single_instance("dock", argc, argv, on_command, NULL))
        return 0;
    lp_shell_init(&argc, &argv);
    items = g_ptr_array_new_with_free_func(item_free);

    /* The size, and a watch on it (see icon_px): before anything is
     * built, since the room above the bar is measured in icons. */
    char *conf = lp_config_path("dock.conf");
    char *cs = NULL;
    if (g_file_get_contents(conf, &cs, NULL, NULL)) {
        if (strstr(cs, "size=small")) icon_px = 30;
        else if (strstr(cs, "size=large")) icon_px = 48;
        g_free(cs);
    }
    GFile *cf = g_file_new_for_path(conf);
    GFileMonitor *conf_monitor = g_file_monitor_file(cf, G_FILE_MONITOR_NONE, NULL, NULL);
    if (conf_monitor)
        g_signal_connect(conf_monitor, "changed", G_CALLBACK(on_conf_changed), NULL);
    g_object_unref(cf);
    g_free(conf);

    /* Along the whole bottom edge, transparent, with the bar centred in
     * it and the input region the bar's (update_input says why). The
     * exclusive zone is the spacer's. */
    win = lp_layer_window("lp-dock", GTK_LAYER_SHELL_LAYER_TOP,
                          LP_EDGE_BOTTOM | LP_EDGE_LEFT | LP_EDGE_RIGHT);
    gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_BOTTOM, DOCK_MARGIN);
    /* Its own zone would be ignored (a corner); the spacer reserves it.
     * -1, not 0: a surface with zone 0 is laid out inside the room the
     * others reserve - the spacer's included - so with 0 the dock was
     * lifted by its own height and sat ~110px above the screen's bottom
     * edge instead of DOCK_MARGIN. -1 puts it against the edge. */
    gtk_layer_set_exclusive_zone(win, -1);
    spacer = lp_layer_window("lp-dock-space", GTK_LAYER_SHELL_LAYER_BOTTOM,
                             LP_EDGE_BOTTOM | LP_EDGE_LEFT | LP_EDGE_RIGHT);
    gtk_widget_set_size_request(GTK_WIDGET(spacer), -1, 1);
    g_signal_connect(spacer, "realize", G_CALLBACK(spacer_realized), NULL);
    gtk_widget_show(GTK_WIDGET(spacer));
    g_signal_connect(win, "size-allocate", G_CALLBACK(on_dock_allocate), NULL);
    gtk_widget_add_events(GTK_WIDGET(win), GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(win, "enter-notify-event", G_CALLBACK(on_dock_enter), NULL);
    g_signal_connect(win, "leave-notify-event", G_CALLBACK(on_dock_leave), NULL);
    /* A new resolution or scale moves the middle of the screen. */
    g_signal_connect_swapped(gdk_screen_get_default(), "monitors-changed",
                             G_CALLBACK(recentre), NULL);
    g_signal_connect_swapped(gdk_screen_get_default(), "size-changed",
                             G_CALLBACK(recentre), NULL);

    /* Room above the bar for an icon lifted off it (the drag), then the
     * bar. The lifted icon is drawn over all of it (ghost_draw). */
    stage = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *above = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(above, -1, ICON_PX * 3);
    gtk_box_pack_start(GTK_BOX(stage), above, FALSE, FALSE, 0);
    g_signal_connect_after(stage, "draw", G_CALLBACK(ghost_draw), NULL);
    gtk_container_add(GTK_CONTAINER(win), stage);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(outer), "lp-dock");
    gtk_widget_set_halign(outer, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(stage), outer, FALSE, FALSE, 0);
    dock_bar = outer;
    g_signal_connect(outer, "size-allocate", G_CALLBACK(on_dock_allocate), NULL);

    /* The items sit straight in the bar, which is as wide as they are.
     * (A GtkScrolledWindow around them, for a dock wider than the
     * screen, made the layer surface take the scroller's minimum width,
     * which is none: the dock came up as the grid button alone.) Ten
     * slots at 62 px is 620 px, well inside the narrowest screen this
     * runs on at its scale. */
    list = g_object_new(lp_row_get_type(), NULL);
    gtk_widget_set_valign(list, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(outer), list, FALSE, FALSE, 0);
    lay = lp_motion_new(list, layout_frame, NULL);
    lp_spring_init(&D.gx, LP_SPRING_SHEET, 0.0);
    lp_spring_init(&D.gy, LP_SPRING_SHEET, 0.0);
    lp_spring_init(&D.gs, LP_SPRING_MENU, 1.0);
    lp_spring_init(&D.ga, LP_SPRING_MENU, 1.0);
    lp_motion_add(lay, &D.gx);
    lp_motion_add(lay, &D.gy);
    lp_motion_add(lay, &D.gs);
    lp_motion_add(lay, &D.ga);

    gtk_box_pack_start(GTK_BOX(outer),
                       gtk_separator_new(GTK_ORIENTATION_VERTICAL),
                       FALSE, FALSE, 0);
    grid_button = gtk_button_new();
    GtkStyleContext *gsc = gtk_widget_get_style_context(grid_button);
    gtk_style_context_add_class(gsc, "lp-dock-item");
    gtk_style_context_add_class(gsc, "lp-grid-button");
    gtk_container_add(GTK_CONTAINER(grid_button),
                      lp_icon("view-app-grid-symbolic", 22));
    gtk_widget_set_tooltip_text(grid_button, T("Show applications", "앱 보기"));
    lp_on_tap(grid_button, (LpTapFn)on_grid, NULL);
    add_pull(grid_button, FALSE);
    gtk_box_pack_start(GTK_BOX(outer), grid_button, FALSE, FALSE, 0);

    if (lp_toplevels_init())
        lp_toplevels_watch(on_toplevels, NULL);
    wf_shell = lp_wfshell_init();
    if (wf_shell) {
        GdkDisplay *d = gdk_display_get_default();
        for (int i = 0; i < gdk_display_get_n_monitors(d); i++) {
            GdkMonitor *m = gdk_display_get_monitor(d, i);
            lp_wfshell_watch_fullscreen(m, on_wf_fullscreen, NULL);
            lp_wfshell_hotspot(m, LP_WF_EDGE_BOTTOM, 2, 150, on_wf_hotspot, NULL);
        }
    }

    char *path = lp_config_path("dock");
    GFile *f = g_file_new_for_path(path);
    pin_monitor = g_file_monitor_file(f, G_FILE_MONITOR_NONE, NULL, NULL);
    if (pin_monitor)
        g_signal_connect(pin_monitor, "changed", G_CALLBACK(on_pins_changed), NULL);
    g_object_unref(f);
    g_free(path);
    g_signal_connect(g_app_info_monitor_get(), "changed",
                     G_CALLBACK(on_apps_changed), NULL);

    rebuild_now();
    gtk_widget_show_all(GTK_WIDGET(win));
    GdkDisplay *dpy = gdk_display_get_default();
    for (int i = 0; i < gdk_display_get_n_monitors(dpy); i++)
        edge_for(gdk_display_get_monitor(dpy, i));
    gtk_main();
    return 0;
}
