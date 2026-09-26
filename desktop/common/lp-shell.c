/*
 * lp-shell.c - the parts of the desktop shell that every component
 * shares. lp-shell.h says what they are and why they are shared; this
 * file says how, and what went wrong on the way.
 */
#define _GNU_SOURCE 1

#include "lp-shell.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <pwd.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* ── the stylesheet ───────────────────────────────────────────────── */

static GtkCssProvider *css_base;
static GtkCssProvider *css_light;
static GFileMonitor *style_monitor;

/* One step above GTK_STYLE_PROVIDER_PRIORITY_USER (800).
 *
 * ~/.config/gtk-3.0/gtk.css is the LP theme for applications and GTK
 * loads it at USER priority, above anything an application adds. It
 * paints window.background #232323, which is right for a file manager
 * and wrong for a bar: the first run drew the top bar as an opaque grey
 * slab over the wallpaper with none of shell.css's colours on it. The
 * shell's own rules have to outrank the application theme, and this is
 * the only number that does it. */
#define LP_CSS_PRIORITY (GTK_STYLE_PROVIDER_PRIORITY_USER + 100)

const char *lp_share_dir(void)
{
    const char *d = g_getenv("LP_SHARE");
    return (d && *d) ? d : "/usr/local/share/lp";
}

char *lp_share_path(const char *name)
{
    return g_build_filename(lp_share_dir(), name, NULL);
}

char *lp_config_path(const char *name)
{
    return g_build_filename(g_get_user_config_dir(), "lp", name, NULL);
}

gboolean lp_style_light(void)
{
    char *p = lp_config_path("style"), *s = NULL;
    gboolean light = FALSE;
    if (g_file_get_contents(p, &s, NULL, NULL))
        light = g_str_has_prefix(g_strstrip(s), "light");
    g_free(s);
    g_free(p);
    return light;
}

static void load_css(GtkCssProvider **prov, const char *file, gboolean on)
{
    GdkScreen *scr = gdk_screen_get_default();
    if (*prov) {
        gtk_style_context_remove_provider_for_screen(scr,
            GTK_STYLE_PROVIDER(*prov));
        g_clear_object(prov);
    }
    if (!on)
        return;
    char *path = lp_share_path(file);
    GError *err = NULL;
    *prov = gtk_css_provider_new();
    if (!gtk_css_provider_load_from_path(*prov, path, &err)) {
        /* Said once, on stderr, and the component carries on unstyled -
         * a bar in GTK's default grey is ugly, a bar that is missing
         * because its stylesheet had a typo is a broken desktop. */
        g_printerr("lp-shell: %s: %s\n", path, err->message);
        g_clear_error(&err);
    }
    gtk_style_context_add_provider_for_screen(scr, GTK_STYLE_PROVIDER(*prov),
                                              LP_CSS_PRIORITY);
    g_free(path);
}

static void apply_style(void)
{
    load_css(&css_base, "shell.css", TRUE);
    load_css(&css_light, "shell-light.css", lp_style_light());
}

static void on_style_changed(GFileMonitor *m, GFile *f, GFile *o,
                             GFileMonitorEvent ev, gpointer d)
{
    (void)m; (void)f; (void)o; (void)d;
    if (ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
        ev == G_FILE_MONITOR_EVENT_CREATED ||
        ev == G_FILE_MONITOR_EVENT_DELETED)
        load_css(&css_light, "shell-light.css", lp_style_light());
}

void lp_style_set_light(gboolean light)
{
    char *p = lp_config_path("style");
    char *dir = g_path_get_dirname(p);
    g_mkdir_with_parents(dir, 0755);
    g_file_set_contents(p, light ? "light\n" : "dark\n", -1, NULL);
    g_free(dir);
    g_free(p);

    /* The applications. libadwaita and the portal read this key, not
     * GTK's settings.ini - see desktop/theme/settings.ini. It needs the
     * session bus; without one gsettings fails quietly and only the
     * shell changes, which is the honest partial result. */
    const char *argv[] = { "gsettings", "set", "org.gnome.desktop.interface",
                           "color-scheme",
                           light ? "default" : "prefer-dark", NULL };
    if (lp_have("gsettings"))
        lp_spawn(argv);
}

void lp_shell_init(int *argc, char ***argv)
{
    /* The accessibility bus is not on this machine. Without this every
     * component prints a paragraph about it at start, which reads like
     * a fault and is not one. */
    g_setenv("NO_AT_BRIDGE", "1", FALSE);
    gtk_init(argc, argv);
    apply_style();

    char *p = lp_config_path("style");
    GFile *f = g_file_new_for_path(p);
    style_monitor = g_file_monitor_file(f, G_FILE_MONITOR_NONE, NULL, NULL);
    if (style_monitor)
        g_signal_connect(style_monitor, "changed",
                         G_CALLBACK(on_style_changed), NULL);
    g_object_unref(f);
    g_free(p);
}

/* ── one process per component ───────────────────────────────────── */

typedef struct {
    LpCommandFn fn;
    gpointer data;
} Instance;

static char *socket_path(const char *name)
{
    const char *rt = g_getenv("XDG_RUNTIME_DIR");
    if (rt && *rt)
        return g_strdup_printf("%s/lp-%s.sock", rt, name);
    return g_strdup_printf("/tmp/lp-%s-%u.sock", name, (unsigned)getuid());
}

static gboolean on_incoming(GSocketService *svc, GSocketConnection *conn,
                            GObject *src, gpointer data)
{
    (void)svc; (void)src;
    Instance *in = data;
    GInputStream *is = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    char buf[4096];
    gsize got = 0;

    /* Blocking, and that is fine: the sender writes its few bytes and
     * closes before we are even called, and the socket lives in a 0700
     * directory that only this person can reach. */
    g_input_stream_read_all(is, buf, sizeof buf - 1, &got, NULL, NULL);
    buf[got] = '\0';

    GPtrArray *args = g_ptr_array_new();
    for (gsize i = 0; i < got; ) {
        g_ptr_array_add(args, buf + i);
        i += strlen(buf + i) + 1;
    }
    g_ptr_array_add(args, NULL);
    in->fn((int)args->len - 1, (char **)args->pdata, in->data);
    g_ptr_array_free(args, TRUE);
    return TRUE;
}

static GSocketConnection *connect_to(GSocketAddress *addr)
{
    GSocketClient *cl = g_socket_client_new();
    GSocketConnection *conn = g_socket_client_connect(cl,
        G_SOCKET_CONNECTABLE(addr), NULL, NULL);
    g_object_unref(cl);
    return conn;
}

/* NUL-separated, argv[0] included so both sides index alike. */
static void send_args(GSocketConnection *conn, int argc, const char *const *argv)
{
    GOutputStream *os = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    for (int i = 0; i < argc; i++)
        g_output_stream_write_all(os, argv[i], strlen(argv[i]) + 1,
                                  NULL, NULL, NULL);
    g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
}

gboolean lp_send(const char *name, const char *const *argv)
{
    char *path = socket_path(name);
    GSocketAddress *addr = g_unix_socket_address_new(path);
    GSocketConnection *conn = connect_to(addr);
    if (conn) {
        int n = 0;
        while (argv[n])
            n++;
        send_args(conn, n, argv);
        g_object_unref(conn);
    }
    g_object_unref(addr);
    g_free(path);
    return conn != NULL;
}

gboolean lp_single_instance(const char *name, int argc, char **argv,
                            LpCommandFn fn, gpointer data)
{
    char *path = socket_path(name);
    GSocketAddress *addr = g_unix_socket_address_new(path);
    GSocketConnection *conn = connect_to(addr);

    if (conn) {
        /* Someone is already running: hand over our arguments. */
        send_args(conn, argc, (const char *const *)argv);
        g_object_unref(conn);
        g_object_unref(addr);
        g_free(path);
        return FALSE;
    }

    /* Nobody answered. A socket file left by a component that crashed
     * refuses connections forever and would refuse our bind too, so it
     * goes. */
    unlink(path);
    Instance *in = g_new0(Instance, 1);
    in->fn = fn;
    in->data = data;
    GSocketService *svc = g_socket_service_new();
    GError *err = NULL;
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(svc), addr,
            G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL,
            &err)) {
        /* Carry on as the only instance; toggling from outside will
         * start a second one, which is worse but not broken. */
        g_printerr("lp-shell: %s: %s\n", path, err->message);
        g_clear_error(&err);
    }
    g_signal_connect(svc, "incoming", G_CALLBACK(on_incoming), in);
    g_socket_service_start(svc);
    g_object_unref(addr);
    g_free(path);
    return TRUE;
}

/* ── running things ──────────────────────────────────────────────── */

gboolean lp_have(const char *prog)
{
    char *p = g_find_program_in_path(prog);
    gboolean ok = p != NULL;
    g_free(p);
    return ok;
}

void lp_spawn(const char *const *argv)
{
    GError *err = NULL;
    /* No G_SPAWN_DO_NOT_REAP_CHILD: GLib then forks twice, the child is
     * adopted by init, and nothing here has to wait for it. */
    if (!g_spawn_async(NULL, (char **)argv, NULL,
                       G_SPAWN_SEARCH_PATH | G_SPAWN_STDIN_FROM_DEV_NULL,
                       NULL, NULL, NULL, &err)) {
        g_printerr("lp-shell: %s: %s\n", argv[0], err->message);
        g_clear_error(&err);
    }
}

void lp_spawn_cmdline(const char *cmdline)
{
    char **argv = NULL;
    if (g_shell_parse_argv(cmdline, NULL, &argv, NULL)) {
        lp_spawn((const char *const *)argv);
        g_strfreev(argv);
    }
}

typedef struct {
    LpOutputFn fn;
    gpointer data;
    guint timeout;
    GSubprocess *proc;
} Pending;

static gboolean run_timeout(gpointer d)
{
    Pending *p = d;
    p->timeout = 0;
    /* Ten seconds is forever for a status query. Killing it makes the
     * communicate call below finish with a failure, which is reported
     * as "no answer" rather than leaving the caller waiting for good. */
    g_subprocess_force_exit(p->proc);
    return G_SOURCE_REMOVE;
}

static void run_done(GObject *src, GAsyncResult *res, gpointer d)
{
    Pending *p = d;
    char *out = NULL;
    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res,
                                                       &out, NULL, NULL);
    if (p->timeout)
        g_source_remove(p->timeout);
    if (ok && !g_subprocess_get_successful(G_SUBPROCESS(src)))
        ok = FALSE;
    p->fn(ok ? out : NULL, p->data);
    g_free(out);
    g_object_unref(p->proc);
    g_free(p);
}

void lp_run_async(const char *const *argv, LpOutputFn fn, gpointer data)
{
    GError *err = NULL;
    GSubprocess *proc = g_subprocess_newv(argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        &err);
    if (!proc) {
        g_clear_error(&err);
        fn(NULL, data);
        return;
    }
    Pending *p = g_new0(Pending, 1);
    p->fn = fn;
    p->data = data;
    p->proc = proc;
    p->timeout = g_timeout_add_seconds(10, run_timeout, p);
    g_subprocess_communicate_utf8_async(proc, NULL, NULL, run_done, p);
}

/* ── windows ─────────────────────────────────────────────────────── */

GtkWindow *lp_layer_window(const char *ns, GtkLayerShellLayer layer, int edges)
{
    GtkWindow *w = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    gtk_layer_init_for_window(w);
    gtk_layer_set_namespace(w, ns);
    gtk_layer_set_layer(w, layer);
    gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_TOP, (edges & LP_EDGE_TOP) != 0);
    gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_BOTTOM, (edges & LP_EDGE_BOTTOM) != 0);
    gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_LEFT, (edges & LP_EDGE_LEFT) != 0);
    gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_RIGHT, (edges & LP_EDGE_RIGHT) != 0);
    /* The window itself is transparent; what shows is the styled box
     * inside it. That is what lets a popup carry a real shadow: GTK can
     * only paint inside the surface, so the shadow needs room in it. */
    gtk_widget_set_app_paintable(GTK_WIDGET(w), TRUE);
    GdkScreen *scr = gtk_widget_get_screen(GTK_WIDGET(w));
    GdkVisual *v = gdk_screen_get_rgba_visual(scr);
    if (v)
        gtk_widget_set_visual(GTK_WIDGET(w), v);
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(w)),
                                "lp-layer");
    return w;
}

/* ── touch ───────────────────────────────────────────────────────── */

typedef struct {
    LpHoldFn fn;
    gpointer data;
} Hold;

static void hold_fire(GtkWidget *w, double x, double y, Hold *h)
{
    g_object_set_data(G_OBJECT(w), "lp-hold-consumed", GINT_TO_POINTER(1));
    h->fn(w, x, y, h->data);
}

static void on_long_press(GtkGestureLongPress *g, double x, double y,
                          gpointer d)
{
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    /* Claiming the sequence takes it away from the button's own click
     * gesture, so lifting the finger does not also activate the item. */
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    hold_fire(w, x, y, d);
}

static void on_right_press(GtkGestureMultiPress *g, int n, double x,
                           double y, gpointer d)
{
    (void)n;
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    hold_fire(w, x, y, d);
}

static void on_any_press(GtkGestureMultiPress *g, int n, double x, double y,
                         gpointer d)
{
    (void)n; (void)x; (void)y; (void)d;
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    g_object_set_data(G_OBJECT(w), "lp-hold-consumed", NULL);
}

void lp_on_hold(GtkWidget *w, LpHoldFn fn, gpointer data)
{
    Hold *h = g_new0(Hold, 1);
    h->fn = fn;
    h->data = data;

    /* Capture phase, so the press is seen before the button's own
     * gesture takes it. The gestures are owned by the widget: GTK 3
     * does not do that on its own, so they are tied to it with
     * set_data_full and die with it. */
    GtkGesture *lp = gtk_gesture_long_press_new(w);
    gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(lp), FALSE);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(lp),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(lp, "pressed", G_CALLBACK(on_long_press), h);
    g_object_set_data_full(G_OBJECT(w), "lp-hold-lp", lp, g_object_unref);

    GtkGesture *rc = gtk_gesture_multi_press_new(w);
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rc), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(rc),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(rc, "pressed", G_CALLBACK(on_right_press), h);
    g_object_set_data_full(G_OBJECT(w), "lp-hold-rc", rc, g_object_unref);

    GtkGesture *any = gtk_gesture_multi_press_new(w);
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(any), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(any),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(any, "pressed", G_CALLBACK(on_any_press), NULL);
    g_object_set_data_full(G_OBJECT(w), "lp-hold-any", any, g_object_unref);

    g_object_set_data_full(G_OBJECT(w), "lp-hold-fn", h, g_free);
}

gboolean lp_hold_consumed(GtkWidget *w)
{
    gboolean c = g_object_get_data(G_OBJECT(w), "lp-hold-consumed") != NULL;
    g_object_set_data(G_OBJECT(w), "lp-hold-consumed", NULL);
    return c;
}

void lp_menu_popup_at(GtkWidget *menu, GtkWidget *w, double x, double y)
{
    GdkRectangle r = { (int)x, (int)y, 1, 1 };
    if (!gtk_widget_get_has_window(w)) {
        GtkAllocation a;
        gtk_widget_get_allocation(w, &a);
        r.x += a.x;
        r.y += a.y;
    }
    gtk_style_context_add_class(gtk_widget_get_style_context(menu), "lp-menu");
    gtk_menu_popup_at_rect(GTK_MENU(menu), gtk_widget_get_window(w), &r,
                           GDK_GRAVITY_NORTH_WEST, GDK_GRAVITY_NORTH_WEST,
                           NULL);
}

/* ── small things ────────────────────────────────────────────────── */

GtkWidget *lp_icon(const char *name, int px)
{
    GtkWidget *img = gtk_image_new_from_icon_name(name, GTK_ICON_SIZE_BUTTON);
    gtk_image_set_pixel_size(GTK_IMAGE(img), px);
    return img;
}

char *lp_user_display_name(void)
{
    struct passwd *pw = getpwuid(getuid());
    if (!pw)
        return g_strdup(g_get_user_name());
    /* GECOS is "Full Name,room,phone,..." and only the first field is a
     * name. An account made with no name has an empty one. */
    if (pw->pw_gecos && *pw->pw_gecos && *pw->pw_gecos != ',') {
        char *n = g_strdup(pw->pw_gecos);
        char *c = strchr(n, ',');
        if (c)
            *c = '\0';
        return n;
    }
    return g_strdup(pw->pw_name);
}
