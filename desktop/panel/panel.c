/*
 * panel.c - lp-panel, the top bar.
 *
 *   [현재 활동] Files            Sat 23:47            [⌨] [▾ ◖ ▮ ⏻]
 *
 * The owner's mockup, left to right: 현재 활동 opens the app grid, then
 * the name of the application in front; the clock in the middle, which
 * opens a calendar; on the right the on-screen keyboard button and the
 * status area - Wi-Fi, volume, battery, power - which is one button and
 * opens quick settings.
 *
 * ── why a program and not waybar ──
 *
 * The bar used to be waybar with a config. waybar cannot draw this one:
 * it has no module for "the focused application's name" (its taskbar
 * lists every window), no calendar popover, and it draws the status
 * icons as separate modules with separate click targets where the
 * mockup has one area that opens one panel. Getting close meant custom
 * scripts polled every two seconds, which is what the old lp-bar-label
 * was. A GTK 3 layer-shell program is a few hundred lines, draws the
 * mockup exactly, and polls nothing faster than every ten seconds.
 *
 * ── what it asks, and how often ──
 *
 *   the focused window    wlr-foreign-toplevel events, no polling
 *   the clock             woken once a minute, on the minute
 *   Wi-Fi                 `lp-net status --json`, every 10 s
 *   volume                `wpctl get-volume`, every 10 s
 *   battery               `lp-tune status --json`, every 30 s
 *
 * and immediately on `lp-panel refresh`, which the volume keys and the
 * quick settings panel send after they change something, so the icons
 * never wait out a poll to catch up with a key press. A query still
 * running when the next one is due is not started twice.
 */
#define _GNU_SOURCE 1

#include <time.h>

#include "lp-apps.h"
#include "lp-json.h"
#include "lp-shell.h"
#include "lp-toplevel.h"

#define BAR_HEIGHT 40

typedef struct {
    GtkWindow *win;
    GdkMonitor *mon;
    GtkWidget *activities;
    GtkWidget *appname;
    GtkWidget *clock;
    GtkWidget *clock_label;
    GtkWidget *cal_pop;
    GtkWidget *cal_day;
    GtkWidget *cal_date;
    GtkWidget *calendar;
    GtkWidget *status;
    GtkWidget *wifi_icon;
    GtkWidget *vol_icon;
    GtkWidget *bat_icon;
    GtkWidget *bat_label;
} Bar;

static GList *bars;
static GHashTable *app_cache;   /* app_id -> GDesktopAppInfo or NULL */

/* The state every bar shows. */
static char *wifi_icon_name, *vol_icon_name, *bat_icon_name, *bat_text;
static int bat_state;           /* 0 plain, 1 warn, 2 bad */
static gboolean busy_net, busy_vol, busy_bat;

/* ── the focused application ─────────────────────────────────────── */

static const char *name_for(LpToplevel *t)
{
    if (t->app_id && *t->app_id) {
        GDesktopAppInfo *d;
        if (!g_hash_table_lookup_extended(app_cache, t->app_id, NULL,
                                          (gpointer *)&d)) {
            d = lp_app_for_id(t->app_id);
            g_hash_table_insert(app_cache, g_strdup(t->app_id), d);
        }
        if (d)
            return lp_app_name(G_APP_INFO(d));
    }
    return t->title ? t->title : "";
}

static void on_toplevels(gpointer data)
{
    (void)data;
    const char *name = "";
    for (GList *l = lp_toplevels(); l; l = l->next) {
        LpToplevel *t = l->data;
        if (t->done && t->activated && !t->minimized)
            name = name_for(t);
    }
    for (GList *b = bars; b; b = b->next)
        gtk_label_set_text(GTK_LABEL(((Bar *)b->data)->appname), name);
}

/* ── the clock ───────────────────────────────────────────────────── */

static void set_clock(void)
{
    GDateTime *now = g_date_time_new_now_local();
    /* The mockup's "Sat 23:47". %a is the locale's own short weekday,
     * so a Korean session reads 토 23:47 - the same shape, in its own
     * words - without a second format string to keep in step. */
    char *s = g_date_time_format(now, "%a %H:%M");
    for (GList *b = bars; b; b = b->next)
        gtk_label_set_text(GTK_LABEL(((Bar *)b->data)->clock_label), s);
    g_free(s);
    g_date_time_unref(now);
}

static gboolean clock_tick(gpointer d)
{
    (void)d;
    set_clock();
    /* Re-aim at the next minute boundary every time rather than adding
     * 60 s forever: a suspend, or a clock set by NTP, would otherwise
     * leave the bar a minute behind until the next restart. */
    GDateTime *now = g_date_time_new_now_local();
    int wait = 60 - g_date_time_get_second(now);
    g_date_time_unref(now);
    g_timeout_add_seconds(wait > 0 ? wait : 60, clock_tick, NULL);
    return G_SOURCE_REMOVE;
}

static void fill_calendar(Bar *b)
{
    GDateTime *now = g_date_time_new_now_local();
    char *day = g_date_time_format(now, "%A");
    char *date = g_date_time_format(now, T("%B %-d, %Y", "%Y년 %-m월 %-d일"));
    gtk_label_set_text(GTK_LABEL(b->cal_day), day);
    gtk_label_set_text(GTK_LABEL(b->cal_date), date);
    gtk_calendar_select_month(GTK_CALENDAR(b->calendar),
                              g_date_time_get_month(now) - 1,
                              g_date_time_get_year(now));
    gtk_calendar_select_day(GTK_CALENDAR(b->calendar),
                            g_date_time_get_day_of_month(now));
    g_free(day);
    g_free(date);
    g_date_time_unref(now);
}

static void on_clock(GtkButton *btn, gpointer d)
{
    (void)btn;
    Bar *b = d;
    if (gtk_widget_get_visible(b->cal_pop)) {
        gtk_popover_popdown(GTK_POPOVER(b->cal_pop));
        return;
    }
    fill_calendar(b);
    gtk_popover_popup(GTK_POPOVER(b->cal_pop));
}

/* ── status: Wi-Fi, volume, battery ─────────────────────────────── */

static void paint_status(void)
{
    for (GList *l = bars; l; l = l->next) {
        Bar *b = l->data;
        gtk_image_set_from_icon_name(GTK_IMAGE(b->wifi_icon),
            wifi_icon_name ? wifi_icon_name : "network-wireless-offline-symbolic",
            GTK_ICON_SIZE_BUTTON);
        gtk_image_set_from_icon_name(GTK_IMAGE(b->vol_icon),
            vol_icon_name ? vol_icon_name : "audio-volume-muted-symbolic",
            GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_visible(b->bat_icon, bat_icon_name != NULL);
        gtk_widget_set_visible(b->bat_label, bat_text != NULL);
        if (bat_icon_name)
            gtk_image_set_from_icon_name(GTK_IMAGE(b->bat_icon), bat_icon_name,
                                         GTK_ICON_SIZE_BUTTON);
        if (bat_text)
            gtk_label_set_text(GTK_LABEL(b->bat_label), bat_text);
        GtkStyleContext *sc = gtk_widget_get_style_context(b->bat_label);
        gtk_style_context_remove_class(sc, "lp-warn");
        gtk_style_context_remove_class(sc, "lp-bad");
        if (bat_state)
            gtk_style_context_add_class(sc, bat_state == 2 ? "lp-bad" : "lp-warn");
    }
}

static void set_str(char **slot, const char *v)
{
    g_free(*slot);
    *slot = v ? g_strdup(v) : NULL;
}

static void net_done(const char *out, gpointer d)
{
    (void)d;
    busy_net = FALSE;
    LpJson *j = lp_json_parse(out);
    const char *icon = "network-wireless-offline-symbolic";
    if (j) {
        const char *st = lp_json_str(j, "wifi.state", "");
        int q = (int)lp_json_num(j, "wifi.quality", 0);
        gboolean wired = FALSE;
        LpJson *w = lp_json_get(j, "wired");
        for (int i = 0; i < lp_json_len(w); i++)
            if (lp_json_bool(lp_json_at(w, i), "carrier", 0))
                wired = TRUE;
        if (g_strcmp0(lp_json_str(j, "radio", "on"), "off") == 0)
            icon = wired ? "network-wired-symbolic"
                         : "network-wireless-disabled-symbolic";
        else if (g_strcmp0(st, "connected") == 0)
            icon = q >= 75 ? "network-wireless-signal-excellent-symbolic"
                 : q >= 50 ? "network-wireless-signal-good-symbolic"
                 : q >= 25 ? "network-wireless-signal-ok-symbolic"
                           : "network-wireless-signal-weak-symbolic";
        else if (wired)
            icon = "network-wired-symbolic";
        else if (g_strcmp0(st, "disconnected") && g_strcmp0(st, "failed"))
            icon = "network-wireless-acquiring-symbolic";
        lp_json_free(j);
    }
    set_str(&wifi_icon_name, icon);
    paint_status();
}

static void vol_done(const char *out, gpointer d)
{
    (void)d;
    busy_vol = FALSE;
    const char *icon = "audio-volume-muted-symbolic";
    double v;
    /* "Volume: 0.40" or "Volume: 0.40 [MUTED]" */
    if (out && sscanf(out, "Volume: %lf", &v) == 1 && !strstr(out, "MUTED"))
        icon = v <= 0.001 ? "audio-volume-muted-symbolic"
             : v < 0.34  ? "audio-volume-low-symbolic"
             : v < 0.67  ? "audio-volume-medium-symbolic"
                         : "audio-volume-high-symbolic";
    set_str(&vol_icon_name, icon);
    paint_status();
}

static void bat_done(const char *out, gpointer d)
{
    (void)d;
    busy_bat = FALSE;
    LpJson *j = lp_json_parse(out);
    if (!j || !lp_json_bool(j, "battery.present", 0)) {
        /* No battery - a desktop, or a VM - so no battery icon at all.
         * An icon that always says "unknown" is noise in the one place
         * the eye goes to for how long the machine has left. */
        set_str(&bat_icon_name, NULL);
        set_str(&bat_text, NULL);
        lp_json_free(j);
        paint_status();
        return;
    }
    int pct = (int)lp_json_num(j, "battery.percent", 0);
    const char *st = lp_json_str(j, "battery.state", "unknown");
    gboolean charging = g_strcmp0(st, "charging") == 0 ||
                        g_strcmp0(st, "full") == 0;
    int lvl = ((pct + 5) / 10) * 10;
    if (lvl > 100) lvl = 100;
    char *icon = g_strdup_printf("battery-level-%d%s-symbolic", lvl,
        lvl == 100 && charging ? "-charged" : charging ? "-charging" : "");
    char *txt = g_strdup_printf("%d%%", pct);
    set_str(&bat_icon_name, icon);
    set_str(&bat_text, txt);
    /* Amber at 20, red at 10: spending red at 20 leaves nothing to say
     * at the moment it really matters (§1-3). */
    bat_state = charging ? 0 : pct <= 10 ? 2 : pct <= 20 ? 1 : 0;
    g_free(icon);
    g_free(txt);
    lp_json_free(j);
    paint_status();
}

static gboolean poll_net(gpointer d)
{
    (void)d;
    if (!busy_net) {
        busy_net = TRUE;
        const char *a[] = { "lp-net", "status", "--json", NULL };
        lp_run_async(a, net_done, NULL);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean poll_vol(gpointer d)
{
    (void)d;
    if (!busy_vol) {
        busy_vol = TRUE;
        const char *a[] = { "wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL };
        lp_run_async(a, vol_done, NULL);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean poll_bat(gpointer d)
{
    (void)d;
    if (!busy_bat) {
        busy_bat = TRUE;
        const char *a[] = { "lp-tune", "status", "--json", NULL };
        lp_run_async(a, bat_done, NULL);
    }
    return G_SOURCE_CONTINUE;
}

/* ── buttons ─────────────────────────────────────────────────────── */

static void on_activities(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    const char *a[] = { "lp-appgrid", "toggle", NULL };
    lp_spawn(a);
}

static void on_keyboard(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    const char *a[] = { "lp-osk", "toggle", NULL };
    lp_spawn(a);
}

static void on_status(GtkButton *b, gpointer d)
{
    (void)b;
    Bar *bar = d;
    /* Tell quick settings which monitor to open on: the one whose bar
     * was touched, not whichever the compositor happens to prefer. */
    int n = gdk_display_get_n_monitors(gdk_display_get_default());
    int idx = 0;
    for (int i = 0; i < n; i++)
        if (gdk_display_get_monitor(gdk_display_get_default(), i) == bar->mon)
            idx = i;
    char mon[16];
    g_snprintf(mon, sizeof mon, "%d", idx);
    const char *a[] = { "lp-quick", "toggle", mon, NULL };
    lp_spawn(a);
}

/* ── commands from outside ───────────────────────────────────────── */

static void set_open(const char *what, gboolean open)
{
    for (GList *l = bars; l; l = l->next) {
        Bar *b = l->data;
        GtkWidget *w = g_strcmp0(what, "grid") == 0 ? b->activities
                     : g_strcmp0(what, "quick") == 0 ? b->status : NULL;
        if (!w)
            continue;
        GtkStyleContext *sc = gtk_widget_get_style_context(w);
        if (open)
            gtk_style_context_add_class(sc, "lp-open");
        else
            gtk_style_context_remove_class(sc, "lp-open");
    }
}

static void on_command(int argc, char **argv, gpointer d)
{
    (void)d;
    if (argc >= 2 && g_strcmp0(argv[1], "refresh") == 0) {
        poll_vol(NULL);
        poll_net(NULL);
        poll_bat(NULL);
    } else if (argc >= 4 && g_strcmp0(argv[1], "open") == 0) {
        set_open(argv[2], g_strcmp0(argv[3], "1") == 0);
    }
}

/* ── one bar per monitor ─────────────────────────────────────────── */

static GtkWidget *icon_img(const char *name)
{
    return lp_icon(name, 18);
}

static Bar *bar_new(GdkMonitor *mon)
{
    Bar *b = g_new0(Bar, 1);
    b->mon = mon;
    b->win = lp_layer_window("lp-panel", GTK_LAYER_SHELL_LAYER_TOP,
                             LP_EDGE_TOP | LP_EDGE_LEFT | LP_EDGE_RIGHT);
    gtk_layer_set_monitor(b->win, mon);
    gtk_layer_auto_exclusive_zone_enable(b->win);
    gtk_widget_set_size_request(GTK_WIDGET(b->win), -1, BAR_HEIGHT);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "lp-bar");
    gtk_container_add(GTK_CONTAINER(b->win), box);

    /* left */
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    b->activities = gtk_button_new_with_label(T("Activities", "현재 활동"));
    gtk_style_context_add_class(gtk_widget_get_style_context(b->activities),
                                "lp-activities");
    g_signal_connect(b->activities, "clicked", G_CALLBACK(on_activities), b);
    gtk_box_pack_start(GTK_BOX(left), b->activities, FALSE, FALSE, 0);

    b->appname = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(b->appname), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(b->appname), 32);
    gtk_style_context_add_class(gtk_widget_get_style_context(b->appname),
                                "lp-appname");
    gtk_box_pack_start(GTK_BOX(left), b->appname, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), left, FALSE, FALSE, 0);

    /* centre */
    b->clock = gtk_button_new();
    b->clock_label = gtk_label_new("");
    gtk_container_add(GTK_CONTAINER(b->clock), b->clock_label);
    gtk_style_context_add_class(gtk_widget_get_style_context(b->clock),
                                "lp-clock");
    g_signal_connect(b->clock, "clicked", G_CALLBACK(on_clock), b);
    gtk_box_set_center_widget(GTK_BOX(box), b->clock);

    b->cal_pop = gtk_popover_new(b->clock);
    gtk_popover_set_position(GTK_POPOVER(b->cal_pop), GTK_POS_BOTTOM);
    gtk_style_context_add_class(gtk_widget_get_style_context(b->cal_pop), "lp-cal");
    GtkWidget *cbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    b->cal_day = gtk_label_new("");
    b->cal_date = gtk_label_new("");
    gtk_widget_set_halign(b->cal_day, GTK_ALIGN_START);
    gtk_widget_set_halign(b->cal_date, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(b->cal_day), "lp-cal-day");
    gtk_style_context_add_class(gtk_widget_get_style_context(b->cal_date), "lp-cal-date");
    b->calendar = gtk_calendar_new();
    gtk_box_pack_start(GTK_BOX(cbox), b->cal_day, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cbox), b->cal_date, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cbox), b->calendar, FALSE, FALSE, 0);
    gtk_widget_show_all(cbox);
    gtk_container_add(GTK_CONTAINER(b->cal_pop), cbox);

    /* right */
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *kbd = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(kbd), icon_img("input-keyboard-symbolic"));
    gtk_style_context_add_class(gtk_widget_get_style_context(kbd), "lp-kbd");
    g_signal_connect(kbd, "clicked", G_CALLBACK(on_keyboard), b);
    gtk_box_pack_start(GTK_BOX(right), kbd, FALSE, FALSE, 0);

    b->status = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(b->status), "lp-status");
    GtkWidget *st = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    b->wifi_icon = icon_img("network-wireless-offline-symbolic");
    b->vol_icon = icon_img("audio-volume-muted-symbolic");
    b->bat_icon = icon_img("battery-missing-symbolic");
    b->bat_label = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(st), b->wifi_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(st), b->vol_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(st), b->bat_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(st), b->bat_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(st), icon_img("system-shutdown-symbolic"),
                       FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(b->status), st);
    g_signal_connect(b->status, "clicked", G_CALLBACK(on_status), b);
    gtk_box_pack_start(GTK_BOX(right), b->status, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(box), right, FALSE, FALSE, 0);

    gtk_widget_show_all(GTK_WIDGET(b->win));
    bars = g_list_append(bars, b);
    return b;
}

static void bar_free(Bar *b)
{
    bars = g_list_remove(bars, b);
    gtk_widget_destroy(GTK_WIDGET(b->win));
    g_free(b);
}

static void on_monitor_added(GdkDisplay *d, GdkMonitor *m, gpointer u)
{
    (void)d; (void)u;
    bar_new(m);
    set_clock();
    paint_status();
    on_toplevels(NULL);
}

static void on_monitor_removed(GdkDisplay *d, GdkMonitor *m, gpointer u)
{
    (void)d; (void)u;
    for (GList *l = bars; l; l = l->next)
        if (((Bar *)l->data)->mon == m) {
            bar_free(l->data);
            break;
        }
}

int main(int argc, char **argv)
{
    if (!lp_single_instance("panel", argc, argv, on_command, NULL))
        return 0;
    lp_shell_init(&argc, &argv);

    /* Values are never freed: the cache lives as long as the bar, and a
     * missing app is cached as NULL, which g_object_unref would not take. */
    app_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    GdkDisplay *dpy = gdk_display_get_default();
    for (int i = 0; i < gdk_display_get_n_monitors(dpy); i++)
        bar_new(gdk_display_get_monitor(dpy, i));
    g_signal_connect(dpy, "monitor-added", G_CALLBACK(on_monitor_added), NULL);
    g_signal_connect(dpy, "monitor-removed", G_CALLBACK(on_monitor_removed), NULL);

    if (lp_toplevels_init())
        lp_toplevels_watch(on_toplevels, NULL);
    on_toplevels(NULL);

    clock_tick(NULL);
    paint_status();
    poll_net(NULL);
    poll_vol(NULL);
    poll_bat(NULL);
    g_timeout_add_seconds(10, poll_net, NULL);
    g_timeout_add_seconds(10, poll_vol, NULL);
    g_timeout_add_seconds(30, poll_bat, NULL);

    gtk_main();
    return 0;
}
