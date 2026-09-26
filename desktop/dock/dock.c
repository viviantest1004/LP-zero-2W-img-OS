/*
 * dock.c - lp-dock, the full-height dock on the left.
 *
 * The owner's mockup, top to bottom: notes, terminal, files, text
 * editor, code editor, browser, mail, calculator, software store,
 * system monitor, settings, clock - colourful app icons with a small
 * dot to the left of each one that is running - then a separator and
 * the 3x3 app-grid button at the bottom.
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
 * dock order. It is written only when the person pins or unpins
 * something, so until then the defaults below are the dock, and a
 * later change to the defaults reaches everyone who never touched it.
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
 */
#define _GNU_SOURCE 1

#include <string.h>

#include "lp-apps.h"
#include "lp-shell.h"
#include "lp-toplevel.h"

#define DOCK_WIDTH 72
#define ICON_PX 48

typedef struct {
    const char *ids[3];     /* .desktop ids that can fill the slot */
    const char *package;    /* the Debian package for ids[0] */
    const char *en, *ko;    /* what the slot is, when nothing fills it */
    const char *icon;       /* its icon, when nothing fills it */
} Slot;

/* The mockup's order. */
static const Slot slots[] = {
    { { "org.gnome.Gnote.desktop", "gnote.desktop" }, "gnote",
      "Notes", "메모", "gnote" },
    { { "foot.desktop", "org.codeberg.dnkl.foot.desktop" }, "foot",
      "Terminal", "터미널", "utilities-terminal" },
    { { "lp-files.desktop" }, NULL, "Files", "파일", "system-file-manager" },
    { { "org.gnome.gedit.desktop", "org.gnome.TextEditor.desktop" }, "gedit",
      "Text Editor", "텍스트 편집기", "accessories-text-editor" },
    { { "geany.desktop", "org.geany.Geany.desktop" }, "geany",
      "Code Editor", "코드 편집기", "geany" },
    { { "firefox-esr.desktop", "firefox.desktop" }, "firefox-esr",
      "Web Browser", "웹 브라우저", "firefox-esr" },
    { { "thunderbird.desktop", "org.mozilla.Thunderbird.desktop" }, "thunderbird",
      "Mail", "메일", "thunderbird" },
    { { "org.gnome.Calculator.desktop" }, "gnome-calculator",
      "Calculator", "계산기", "accessories-calculator" },
    { { "lp-software.desktop" }, NULL,
      "Software", "소프트웨어", "system-software-install" },
    { { "lp-tasks.desktop" }, NULL,
      "Task Manager", "작업 관리자", "utilities-system-monitor" },
    { { "lp-settings.desktop" }, NULL, "Settings", "설정", "preferences-system" },
    { { "org.gnome.clocks.desktop" }, "gnome-clocks",
      "Clocks", "시계", "org.gnome.clocks" },
};
#define NSLOTS (sizeof slots / sizeof slots[0])

typedef struct {
    char *id;               /* desktop id, or the slot's ids[0] if missing */
    GDesktopAppInfo *info;  /* NULL when not installed */
    const Slot *slot;       /* the mockup slot it came from, if any */
    gboolean pinned;
    GtkWidget *button;
    GtkWidget *dot;
} Item;

static GtkWindow *win;
static GtkWidget *list;          /* the box the items live in */
static GtkWidget *grid_button;
static GPtrArray *items;         /* Item* in dock order */
static GFileMonitor *pin_monitor;
static guint rebuild_id;

static void rebuild(void);

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
    char *path = lp_config_path("dock");
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_file_set_contents(path, s->str, -1, NULL);
    g_free(dir);
    g_free(path);
    g_string_free(s, TRUE);
}

static void set_pinned(const char *id, gboolean pin)
{
    GPtrArray *pins = read_pins();
    for (guint i = 0; i < pins->len; i++)
        if (strcmp(g_ptr_array_index(pins, i), id) == 0) {
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

/* ── tap ─────────────────────────────────────────────────────────── */

static void show_missing(Item *it)
{
    GtkWidget *pop = gtk_popover_new(it->button);
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_RIGHT);
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

static void on_item(GtkButton *b, gpointer d)
{
    Item *it = d;
    if (lp_hold_consumed(GTK_WIDGET(b)))
        return;
    if (!it->info) {
        show_missing(it);
        return;
    }
    GList *wins = windows_of(it);
    if (!wins) {
        lp_app_launch(G_APP_INFO(it->info));
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
        lp_app_launch(G_APP_INFO(it->info));
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
    g_signal_connect(menu, "deactivate", G_CALLBACK(gtk_widget_destroy), NULL);
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

static void item_free(gpointer p)
{
    Item *it = p;
    g_free(it->id);
    g_clear_object(&it->info);
    g_free(it);
}

static Item *add_item(const char *id, GDesktopAppInfo *info, const Slot *slot,
                      gboolean pinned)
{
    Item *it = g_new0(Item, 1);
    it->id = g_strdup(id);
    it->info = info;
    it->slot = slot;
    it->pinned = pinned;

    it->button = gtk_button_new();
    GtkStyleContext *sc = gtk_widget_get_style_context(it->button);
    gtk_style_context_add_class(sc, "lp-dock-item");
    if (!info)
        gtk_style_context_add_class(sc, "lp-missing");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    it->dot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(it->dot), "lp-dot");
    gtk_widget_set_valign(it->dot, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(row), it->dot, FALSE, FALSE, 0);
    GtkWidget *img = app_image(it);
    gtk_widget_set_hexpand(img, TRUE);
    gtk_widget_set_margin_end(img, 8);
    gtk_box_pack_start(GTK_BOX(row), img, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(it->button), row);
    /* The name for the touchpad user who rests the pointer; a finger
     * user gets it from the long-press menu's heading. */
    gtk_widget_set_tooltip_text(it->button, info ? lp_app_name(G_APP_INFO(info))
                                                 : T(slot->en, slot->ko));
    g_signal_connect(it->button, "clicked", G_CALLBACK(on_item), it);
    lp_on_hold(it->button, on_hold, it);
    gtk_box_pack_start(GTK_BOX(list), it->button, FALSE, FALSE, 0);
    g_ptr_array_add(items, it);
    return it;
}

static void paint_dots(void)
{
    for (guint i = 0; i < items->len; i++) {
        Item *it = g_ptr_array_index(items, i);
        GList *wins = windows_of(it);
        gboolean focused = FALSE;
        for (GList *l = wins; l; l = l->next)
            if (((LpToplevel *)l->data)->activated)
                focused = TRUE;
        GtkStyleContext *sc = gtk_widget_get_style_context(it->dot);
        gtk_style_context_remove_class(sc, "lp-running");
        gtk_style_context_remove_class(sc, "lp-focused");
        if (wins)
            gtk_style_context_add_class(sc, focused ? "lp-focused" : "lp-running");
        g_list_free(wins);
    }
}

static gboolean owned_by_items(const char *app_id)
{
    for (guint i = 0; i < items->len; i++) {
        Item *it = g_ptr_array_index(items, i);
        if (it->info && lp_app_owns(it->info, app_id))
            return TRUE;
    }
    return FALSE;
}

static void rebuild_now(void)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(kids);
    g_ptr_array_set_size(items, 0);

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
        add_item(use, info, slot, TRUE);
    }
    g_ptr_array_unref(pins);

    /* Running applications that are not pinned, after the pinned ones,
     * in the order they were opened. */
    for (GList *l = lp_toplevels(); l; l = l->next) {
        LpToplevel *t = l->data;
        if (!t->done || !t->app_id || owned_by_items(t->app_id))
            continue;
        GDesktopAppInfo *info = lp_app_for_id(t->app_id);
        if (!info)
            continue;
        add_item(g_app_info_get_id(G_APP_INFO(info)), info, NULL, FALSE);
    }
    gtk_widget_show_all(list);
    paint_dots();
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

static void on_toplevels(gpointer d)
{
    (void)d;
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
        GList *w = it->pinned ? NULL : windows_of(it);
        gboolean gone = !it->pinned && !w;
        g_list_free(w);
        if (gone) {
            rebuild();
            return;
        }
    }
    paint_dots();
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

static void on_grid(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    const char *a[] = { "lp-appgrid", "toggle", NULL };
    lp_spawn(a);
}

static void on_command(int argc, char **argv, gpointer d)
{
    (void)d;
    if (argc >= 2 && strcmp(argv[1], "refresh") == 0)
        rebuild();
    else if (argc >= 4 && strcmp(argv[1], "open") == 0 &&
             strcmp(argv[2], "grid") == 0) {
        GtkStyleContext *sc = gtk_widget_get_style_context(grid_button);
        if (strcmp(argv[3], "1") == 0)
            gtk_style_context_add_class(sc, "lp-open");
        else
            gtk_style_context_remove_class(sc, "lp-open");
    }
}

int main(int argc, char **argv)
{
    if (!lp_single_instance("dock", argc, argv, on_command, NULL))
        return 0;
    lp_shell_init(&argc, &argv);
    items = g_ptr_array_new_with_free_func(item_free);

    win = lp_layer_window("lp-dock", GTK_LAYER_SHELL_LAYER_TOP,
                          LP_EDGE_TOP | LP_EDGE_BOTTOM | LP_EDGE_LEFT);
    gtk_layer_auto_exclusive_zone_enable(win);
    gtk_widget_set_size_request(GTK_WIDGET(win), DOCK_WIDTH, -1);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(outer), "lp-dock");
    gtk_container_add(GTK_CONTAINER(win), outer);

    /* The items scroll when there are more than fit - a dock that grows
     * past the screen edge hides its last items where no finger can
     * reach them. GtkScrolledWindow scrolls kinetically under a finger
     * by default. */
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(sw), TRUE);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw), TRUE);
    gtk_widget_set_vexpand(sw, TRUE);
    list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(list, GTK_ALIGN_START);
    gtk_container_add(GTK_CONTAINER(sw), list);
    gtk_box_pack_start(GTK_BOX(outer), sw, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(outer),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);
    grid_button = gtk_button_new();
    GtkStyleContext *gsc = gtk_widget_get_style_context(grid_button);
    gtk_style_context_add_class(gsc, "lp-dock-item");
    gtk_style_context_add_class(gsc, "lp-grid-button");
    gtk_container_add(GTK_CONTAINER(grid_button),
                      lp_icon("view-app-grid-symbolic", 28));
    gtk_widget_set_tooltip_text(grid_button, T("Show applications", "앱 보기"));
    g_signal_connect(grid_button, "clicked", G_CALLBACK(on_grid), NULL);
    gtk_box_pack_start(GTK_BOX(outer), grid_button, FALSE, FALSE, 0);

    if (lp_toplevels_init())
        lp_toplevels_watch(on_toplevels, NULL);

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
    gtk_main();
    return 0;
}
