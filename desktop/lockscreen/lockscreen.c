/*
 * lockscreen.c - lp-lockscreen, LP's lock screen and its sign-in screen.
 *
 *   lp-lockscreen            the screen locked (Super+L, the lock button)
 *   lp-lockscreen --login    the same screen at the start of the machine,
 *                            worded as signing in (lp-shell-start)
 *
 * It replaces swaylock's ring. The ring showed nothing of what was typed
 * - no box, no dots - and a person looking at a wallpaper with a circle
 * on it did not know whether the machine wanted a password, had taken
 * the keys, or was frozen. Here there is a clock, the account's name and
 * a password box that fills with dots as keys are pressed, a line that
 * says what went wrong, and Restart / Shut down.
 *
 * ── How it keeps the desktop shut ──
 *
 * The same way swaylock does on this compositor (sway 1.7, which has no
 * ext-session-lock): an overlay layer surface over every output, and
 * wlr-input-inhibitor, with which sway sends every key, click and touch
 * to this program's surfaces alone until it lets go. It lets go only by
 * exiting after PAM said yes.
 *
 * What the inhibitor cannot do is outlive this process: if it crashed,
 * the desktop would be open. lp-lock therefore runs it in a loop and
 * starts it again at once on anything but a clean unlock (exit 0), and
 * falls back to swaylock when it cannot lock at all (exit 3: no
 * inhibitor, or another program holds it).
 *
 * ── The password ──
 *
 * PAM, service lp-lockscreen (/etc/pam.d/lp-lockscreen: common-auth),
 * or swaylock's when that file is missing. pam_unix checks the account's
 * own password through unix_chkpwd, so this runs as the person, not as
 * root. The check runs in a thread - pam_unix waits two seconds after a
 * wrong password - and the buffer is wiped after it.
 *
 * ── Another account ──
 *
 * With more than one account on the machine, the sign-in screen (--login:
 * the start of the machine, and after a log out) has the accounts under
 * the box, and a tap on one makes the box that account's. The lock
 * screen over a session that is in use does not. Its password cannot
 * be checked from here: pam_unix lets an ordinary account check only its
 * own (this runs as the account whose session it is). So it goes to
 * lp-privd, root's daemon, as `switch-user <name> <password as hex>`
 * over its socket - never on a command line, where any process could
 * read it - and lp-privd checks it against /etc/shadow, with the same
 * growing wait after a wrong one, and leaves the account for
 * start-desktop. This session then ends (lp-logout --switch) with the
 * screen still locked, and the next one is that account's, unlocked.
 *
 * The PAM declarations are written out below rather than taken from
 * <security/pam_appl.h>: the base has libpam but not its headers, and
 * these few are Linux-PAM's stable ABI.
 */

#include <gtk/gtk.h>
#include <gdk/gdkwayland.h>
#include <gtk-layer-shell.h>
#include <wayland-client.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pwd.h>
#include <math.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "lp-shell.h"
#include "lp-i18n.h"
#include "wlr-input-inhibitor-unstable-v1-client-protocol.h"

/* ── PAM (libpam.so.0) ───────────────────────────────────────────── */

typedef struct pam_handle pam_handle_t;
struct pam_message  { int msg_style; const char *msg; };
struct pam_response { char *resp; int resp_retcode; };
struct pam_conv {
    int (*conv)(int, const struct pam_message **, struct pam_response **, void *);
    void *appdata_ptr;
};
extern int pam_start(const char *service, const char *user,
                     const struct pam_conv *conv, pam_handle_t **pamh);
extern int pam_authenticate(pam_handle_t *pamh, int flags);
extern int pam_setcred(pam_handle_t *pamh, int flags);
extern int pam_end(pam_handle_t *pamh, int status);
#define PAM_SUCCESS          0
#define PAM_PROMPT_ECHO_OFF  1
#define PAM_PROMPT_ECHO_ON   2
#define PAM_REFRESH_CRED     0x0010

#define EXIT_UNLOCKED  0
#define EXIT_CANNOT    3       /* lp-lock: use swaylock instead */

/* ── state ───────────────────────────────────────────────────────── */

typedef struct {
    GdkMonitor *mon;
    GtkWidget  *win;
    GtkWidget  *root;         /* faded out on unlock */
    GtkWidget  *bg;
    GtkWidget  *clock, *date;
    GtkWidget  *card;         /* avatar, name, box: on the primary only */
    GtkWidget  *power;
} Screen;

/* An account one can sign in as: uid 1000 and up, with a login shell
 * and a home. */
typedef struct {
    char       *login;
    char       *display;      /* the full name, or the login */
    GdkPixbuf  *face;         /* ~/.face, when this account may read it */
    GtkWidget  *chip;
} Person;

static struct {
    gboolean    login;
    GPtrArray  *screens;      /* Screen* */
    Screen     *primary;
    GtkWidget  *card;         /* the one card, moved to the primary */
    GtkWidget  *entry, *go, *msg, *caps, *hint;
    GtkWidget  *go_stack, *spin;  /* the go button's arrow, or a spinner while checking */
    GtkWidget  *shake;        /* the box the shake moves */
    GdkPixbuf  *wall;
    char       *user;
    char       *name;
    gboolean    checking;
    int         fails;
    gint64      shake_t0;
    GtkWidget  *armed;        /* a power button waiting for its second tap */
    guint       armed_id;
    struct zwlr_input_inhibit_manager_v1 *inhibit_mgr;
    struct zwlr_input_inhibitor_v1 *inhibitor;
    GPtrArray  *people;       /* Person*; the session's own account first */
    Person     *target;       /* whose password the box is for */
    GtkWidget  *name_lbl, *avatar;
    gboolean    switching;    /* another account is signing in */
} L;

/* ── input inhibitor ─────────────────────────────────────────────── */

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t version)
{
    (void)data; (void)version;
    if (!strcmp(iface, zwlr_input_inhibit_manager_v1_interface.name))
        L.inhibit_mgr = wl_registry_bind(reg, name,
                                         &zwlr_input_inhibit_manager_v1_interface, 1);
}

static void reg_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

/* Before any window: a compositor that cannot give the inhibitor, or
 * one where another program holds it (sway answers that with a protocol
 * error, which ends the connection), is exit 3 and swaylock's turn -
 * not a lock screen that shows but does not lock. */
static void take_input(void)
{
    GdkDisplay *d = gdk_display_get_default();
    if (!GDK_IS_WAYLAND_DISPLAY(d)) {
        g_printerr("lp-lockscreen: not a Wayland session\n");
        exit(EXIT_CANNOT);
    }
    struct wl_display *wd = gdk_wayland_display_get_wl_display(d);
    struct wl_registry *reg = wl_display_get_registry(wd);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    if (wl_display_roundtrip(wd) < 0 || !L.inhibit_mgr) {
        g_printerr("lp-lockscreen: the compositor has no input inhibitor\n");
        _exit(EXIT_CANNOT);
    }
    L.inhibitor = zwlr_input_inhibit_manager_v1_get_inhibitor(L.inhibit_mgr);
    if (wl_display_roundtrip(wd) < 0 || wl_display_get_error(wd)) {
        g_printerr("lp-lockscreen: input is inhibited already (another lock?)\n");
        _exit(EXIT_CANNOT);
    }
}

/* ── PAM, in a thread ────────────────────────────────────────────── */

static int conv(int n, const struct pam_message **msg,
                struct pam_response **resp, void *data)
{
    struct pam_response *r = calloc(n, sizeof *r);
    if (!r)
        return 5;               /* PAM_BUF_ERR */
    for (int i = 0; i < n; i++)
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
            msg[i]->msg_style == PAM_PROMPT_ECHO_ON)
            r[i].resp = strdup((const char *)data);
    *resp = r;
    return PAM_SUCCESS;
}

static void check_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)src; (void)c;
    char *pw = data;
    const char *service = g_file_test("/etc/pam.d/lp-lockscreen", G_FILE_TEST_EXISTS)
                          ? "lp-lockscreen" : "swaylock";
    struct pam_conv pc = { conv, pw };
    pam_handle_t *h = NULL;
    int rc = pam_start(service, L.user, &pc, &h);
    if (rc == PAM_SUCCESS)
        rc = pam_authenticate(h, 0);
    if (rc == PAM_SUCCESS)
        pam_setcred(h, PAM_REFRESH_CRED);
    if (h)
        pam_end(h, rc);
    g_task_return_boolean(task, rc == PAM_SUCCESS);
}

/* switch-user, to lp-privd (see "Another account" at the top). The
 * answer is "ok", "wrong", or "say\t<what to tell the person>". */
static char *privd_switch(const char *login, const char *pw)
{
    size_t n = strlen(pw);
    if (n == 0 || n > 200)
        return g_strdup("wrong");
    size_t reqn = strlen("switch-user\t") + strlen(login) + 1 + 2 * n + 2;
    char *req = g_malloc(reqn);
    int k = snprintf(req, reqn, "switch-user\t%s\t", login);
    for (size_t i = 0; i < n; i++)
        k += snprintf(req + k, reqn - k, "%02x", (unsigned char)pw[i]);
    req[k++] = '\n';

    char *ans = NULL;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strncpy(a.sun_path, "/run/lp-privd.sock", sizeof a.sun_path - 1);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        ans = g_strdup_printf("say\t%s", T("Signing in as another account is not available right now.",
                                            "지금은 다른 계정으로 로그인할 수 없습니다."));
    } else {
        size_t off = 0;
        while (off < (size_t)k) {
            ssize_t w = write(fd, req + off, k - off);
            if (w <= 0)
                break;
            off += (size_t)w;
        }
        GString *got = g_string_new(NULL);
        char buf[512];
        ssize_t r;
        while ((r = read(fd, buf, sizeof buf)) > 0 && got->len < 8192)
            g_string_append_len(got, buf, r);
        /* The last line is the verdict: "done ..." or "fail <why> <text>". */
        char **lines = g_strsplit(got->str, "\n", -1);
        const char *last = NULL;
        for (int i = 0; lines[i]; i++)
            if (g_str_has_prefix(lines[i], "done") || g_str_has_prefix(lines[i], "fail"))
                last = lines[i];
        if (last && g_str_has_prefix(last, "done"))
            ans = g_strdup("ok");
        else if (last && g_str_has_prefix(last, "fail auth wrong"))
            ans = g_strdup("wrong");
        else if (last && g_str_has_prefix(last, "fail auth wait")) {
            int secs = 0;
            sscanf(last, "fail auth wait %d", &secs);
            ans = lp_korean()
                ? g_strdup_printf("say\t암호가 여러 번 틀렸습니다. %d초 뒤에 다시 입력하세요.", secs)
                : g_strdup_printf("say\tToo many wrong passwords. Try again in %d seconds.", secs);
        } else if (last && g_str_has_prefix(last, "fail denied"))
            ans = g_strdup_printf("say\t%s", T("This account has no password, so it cannot sign in here.",
                                                "이 계정은 암호가 없어 여기서 로그인할 수 없습니다."));
        else
            ans = g_strdup_printf("say\t%s", T("Could not sign in to that account.",
                                                "그 계정으로 로그인하지 못했습니다."));
        g_strfreev(lines);
        g_string_free(got, TRUE);
    }
    if (fd >= 0)
        close(fd);
    explicit_bzero(req, reqn);
    g_free(req);
    return ans;
}

static void switch_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)src; (void)c;
    g_task_return_pointer(task, privd_switch(L.target->login, data), g_free);
}

static void wipe(char *s)
{
    if (s) {
        explicit_bzero(s, strlen(s));
        g_free(s);
    }
}

/* ── the shake ───────────────────────────────────────────────────── */

static gboolean shake_tick(GtkWidget *w, GdkFrameClock *fc, gpointer d)
{
    (void)d;
    double t = (gdk_frame_clock_get_frame_time(fc) - L.shake_t0) / 1e6;
    if (t >= 0.45) {
        gtk_widget_set_margin_start(w, 0);
        gtk_widget_set_margin_end(w, 0);
        return G_SOURCE_REMOVE;
    }
    /* A damped wobble: three swings, gone in under half a second. */
    int dx = (int)lround(14 * exp(-7 * t) * sin(t * 2 * G_PI * 7));
    gtk_widget_set_margin_start(w, dx > 0 ? 2 * dx : 0);
    gtk_widget_set_margin_end(w, dx < 0 ? -2 * dx : 0);
    return G_SOURCE_CONTINUE;
}

static void shake(void)
{
    if (!L.shake)
        return;
    L.shake_t0 = g_get_monotonic_time();
    gtk_widget_add_tick_callback(L.shake, shake_tick, NULL, NULL);
}

/* ── unlocking ───────────────────────────────────────────────────── */

static gboolean fade_tick(GtkWidget *w, GdkFrameClock *fc, gpointer d)
{
    (void)w; (void)d;
    static gint64 t0;
    gint64 now = gdk_frame_clock_get_frame_time(fc);
    if (!t0)
        t0 = now;
    double a = 1.0 - (now - t0) / 180000.0;
    if (a <= 0) {
        exit(EXIT_UNLOCKED);
    }
    for (guint i = 0; i < L.screens->len; i++) {
        Screen *s = g_ptr_array_index(L.screens, i);
        gtk_widget_set_opacity(s->root, a);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean exit_later(gpointer d)
{
    (void)d;
    exit(EXIT_UNLOCKED);
    return G_SOURCE_REMOVE;
}

static void unlocked(void)
{
    /* Fade, then go; the exit is also scheduled on its own, in case the
     * frame clock has stopped (an output switched off). */
    gtk_widget_add_tick_callback(L.primary ? L.primary->root : L.card, fade_tick, NULL, NULL);
    g_timeout_add(400, exit_later, NULL);
}

static void set_msg(const char *text, const char *cls)
{
    GtkStyleContext *sc = gtk_widget_get_style_context(L.msg);
    gtk_style_context_remove_class(sc, "lp-lock-error");
    gtk_style_context_remove_class(sc, "lp-lock-busy");
    if (cls)
        gtk_style_context_add_class(sc, cls);
    gtk_label_set_text(GTK_LABEL(L.msg), text ? text : "");
}

/* The go button turns into a spinner while the password is checked, and
 * stays one through the fade on success: signing in is the last of the
 * boot's waiting, and it should look like it, not like a frozen arrow. */
/* Drawn here rather than GtkSpinner, whose look is the icon theme's: the
 * boot screen's spinner (userland/splash) - a faint ring, and a bright
 * stroke going round it - in the go button's white. */
static guint spin_tick_id;

static gboolean spin_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    double r = MIN(W, H) / 2.0 - 2.0, lw = MAX(2.0, r / 3.5);
    gint64 t = g_get_monotonic_time() / 1000;
    double a0 = (t % 1300) / 1300.0 * 2 * G_PI - G_PI / 2;
    cairo_set_line_width(cr, lw);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.25);
    cairo_arc(cr, W / 2, H / 2, r, 0, 2 * G_PI);
    cairo_stroke(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    cairo_arc(cr, W / 2, H / 2, r, a0, a0 + G_PI * 0.6);
    cairo_stroke(cr);
    return TRUE;
}

static gboolean spin_tick(GtkWidget *w, GdkFrameClock *fc, gpointer d)
{
    (void)fc; (void)d;
    gtk_widget_queue_draw(w);
    return G_SOURCE_CONTINUE;
}

static void go_busy(gboolean busy)
{
    gtk_stack_set_visible_child(GTK_STACK(L.go_stack), busy ? L.spin : gtk_stack_get_child_by_name(GTK_STACK(L.go_stack), "arrow"));
    if (busy && !spin_tick_id)
        spin_tick_id = gtk_widget_add_tick_callback(L.spin, spin_tick, NULL, NULL);
    else if (!busy && spin_tick_id) {
        gtk_widget_remove_tick_callback(L.spin, spin_tick_id);
        spin_tick_id = 0;
    }
}

/* The box ready for another try, with `say` under it (or the usual
 * "wrong password"). */
static void try_again(const char *say)
{
    go_busy(FALSE);
    gtk_widget_set_sensitive(L.entry, TRUE);
    gtk_widget_set_sensitive(L.go, TRUE);
    gtk_entry_set_text(GTK_ENTRY(L.entry), "");
    gtk_widget_grab_focus(L.entry);
    if (!say) {
        L.fails++;
        say = L.fails > 2 ? T("Wrong password. Check the keyboard layout and Caps Lock.",
                              "암호가 틀렸습니다. 키보드 언어와 Caps Lock 을 확인하세요.")
                          : T("Wrong password. Try again.", "암호가 틀렸습니다. 다시 입력하세요.");
    }
    set_msg(say, "lp-lock-error");
    shake();
}

static void checked(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src; (void)data;
    gboolean ok = g_task_propagate_boolean(G_TASK(res), NULL);
    L.checking = FALSE;
    if (ok) {
        set_msg("", NULL);
        unlocked();
        return;
    }
    try_again(NULL);
}

/* lp-privd said yes: this session ends - still locked, the spinner still
 * turning - and the next one is that account's. Nothing here unlocks. */
static void switched(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src; (void)data;
    char *ans = g_task_propagate_pointer(G_TASK(res), NULL);
    L.checking = FALSE;
    if (ans && !strcmp(ans, "ok")) {
        L.switching = TRUE;
        L.checking = TRUE;          /* no second try while the session ends */
        char *m = g_strdup_printf(T("Signing in as %s…", "%s (으)로 로그인하는 중…"),
                                  L.target->display);
        set_msg(m, "lp-lock-busy");
        g_free(m);
        const char *argv[] = { "lp-logout", "--switch", NULL };
        lp_spawn(argv);
    } else if (ans && g_str_has_prefix(ans, "say\t")) {
        try_again(ans + 4);
    } else {
        try_again(NULL);
    }
    g_free(ans);
}

static void submit(void)
{
    if (L.checking)
        return;
    const char *pw = gtk_entry_get_text(GTK_ENTRY(L.entry));
    if (!*pw) {
        shake();
        return;
    }
    L.checking = TRUE;
    gtk_widget_set_sensitive(L.entry, FALSE);
    /* The button stays lit, with the spinner in it; a second press while
     * checking does nothing (L.checking, above). */
    go_busy(TRUE);
    set_msg(L.login ? T("Signing in…", "로그인하는 중…") : T("Checking…", "확인하는 중…"),
            "lp-lock-busy");
    gboolean other = L.target && strcmp(L.target->login, L.user) != 0;
    GTask *t = g_task_new(NULL, NULL, other ? switched : checked, NULL);
    g_task_set_task_data(t, g_strdup(pw), (GDestroyNotify)wipe);
    g_task_run_in_thread(t, other ? switch_thread : check_thread);
    g_object_unref(t);
}

static void on_activate(GtkEntry *e, gpointer d) { (void)e; (void)d; submit(); }
static void on_go(GtkButton *b, gpointer d) { (void)b; (void)d; submit(); }

/* ── Caps Lock ───────────────────────────────────────────────────── */

static void caps_update(GdkKeymap *km, gpointer d)
{
    (void)d;
    gtk_widget_set_visible(L.caps, gdk_keymap_get_caps_lock_state(km));
}

/* ── the clock ───────────────────────────────────────────────────── */

static gboolean tick_clock(gpointer d)
{
    (void)d;
    GDateTime *now = g_date_time_new_now_local();
    char *hm = g_date_time_format(now, "%H:%M");
    static const char *const wk_ko[] = { "", "월요일", "화요일", "수요일", "목요일",
                                         "금요일", "토요일", "일요일" };
    static const char *const wk_en[] = { "", "Monday", "Tuesday", "Wednesday", "Thursday",
                                         "Friday", "Saturday", "Sunday" };
    static const char *const mo_en[] = { "", "January", "February", "March", "April", "May",
                                         "June", "July", "August", "September", "October",
                                         "November", "December" };
    int w = g_date_time_get_day_of_week(now);
    int m = g_date_time_get_month(now), day = g_date_time_get_day_of_month(now);
    char *date = lp_korean()
        ? g_strdup_printf("%d월 %d일 %s", m, day, wk_ko[w])
        : g_strdup_printf("%s, %s %d", wk_en[w], mo_en[m], day);
    for (guint i = 0; i < L.screens->len; i++) {
        Screen *s = g_ptr_array_index(L.screens, i);
        gtk_label_set_text(GTK_LABEL(s->clock), hm);
        gtk_label_set_text(GTK_LABEL(s->date), date);
    }
    g_free(hm);
    g_free(date);
    g_date_time_unref(now);
    return G_SOURCE_CONTINUE;
}

/* ── drawing ─────────────────────────────────────────────────────── */

static gboolean draw_bg(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    if (L.wall) {
        /* Cover, centred. */
        int iw = gdk_pixbuf_get_width(L.wall), ih = gdk_pixbuf_get_height(L.wall);
        double s = MAX((double)W / iw, (double)H / ih);
        cairo_save(cr);
        cairo_translate(cr, (W - iw * s) / 2, (H - ih * s) / 2);
        cairo_scale(cr, s, s);
        gdk_cairo_set_source_pixbuf(cr, L.wall, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        cairo_set_source_rgb(cr, 0x0B / 255.0, 0x17 / 255.0, 0x22 / 255.0);
        cairo_paint(cr);
    }
    /* Dimmed, darker toward the bottom, so white text reads on any
     * wallpaper the person picked. */
    cairo_pattern_t *p = cairo_pattern_create_linear(0, 0, 0, H);
    cairo_pattern_add_color_stop_rgba(p, 0.0, 0.03, 0.07, 0.11, 0.45);
    cairo_pattern_add_color_stop_rgba(p, 1.0, 0.02, 0.05, 0.09, 0.72);
    cairo_set_source(cr, p);
    cairo_paint(cr);
    cairo_pattern_destroy(p);
    return FALSE;
}

/* The person's picture (~/.face) in a circle, or their initial on the
 * theme's teal - the first character of the name, Hangul included. */
static gboolean draw_avatar(GtkWidget *w, cairo_t *cr, gpointer d)
{
    /* The card's own avatar is whoever the box is for; a chip's is its. */
    Person *who = d ? d : L.target;
    GdkPixbuf *face = who ? who->face : NULL;
    const char *nm = who ? who->display : L.name;
    int S = MIN(gtk_widget_get_allocated_width(w), gtk_widget_get_allocated_height(w));
    double r = S / 2.0;
    cairo_arc(cr, r, r, r, 0, 2 * G_PI);
    cairo_clip(cr);
    if (face) {
        int fw = gdk_pixbuf_get_width(face), fh = gdk_pixbuf_get_height(face);
        double s = MAX((double)S / fw, (double)S / fh);
        cairo_translate(cr, (S - fw * s) / 2, (S - fh * s) / 2);
        cairo_scale(cr, s, s);
        gdk_cairo_set_source_pixbuf(cr, face, 0, 0);
        cairo_paint(cr);
        return FALSE;
    }
    cairo_pattern_t *p = cairo_pattern_create_linear(0, 0, S, S);
    cairo_pattern_add_color_stop_rgb(p, 0, 0x2B / 255.0, 0xC6 / 255.0, 0xB5 / 255.0);
    cairo_pattern_add_color_stop_rgb(p, 1, 0x13 / 255.0, 0x7E / 255.0, 0x9B / 255.0);
    cairo_set_source(cr, p);
    cairo_paint(cr);
    cairo_pattern_destroy(p);

    char initial[8] = "?";
    if (nm && *nm) {
        gunichar c = g_unichar_toupper(g_utf8_get_char(nm));
        initial[g_unichar_to_utf8(c, initial)] = '\0';
    }
    PangoLayout *lay = gtk_widget_create_pango_layout(w, initial);
    char *font = g_strdup_printf("Pretendard Variable Semi-Bold %d", (int)(S * 0.40));
    PangoFontDescription *fd = pango_font_description_from_string(font);
    pango_layout_set_font_description(lay, fd);
    int lw, lh;
    pango_layout_get_pixel_size(lay, &lw, &lh);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_move_to(cr, (S - lw) / 2.0, (S - lh) / 2.0);
    pango_cairo_show_layout(cr, lay);
    pango_font_description_free(fd);
    g_free(font);
    g_object_unref(lay);
    return FALSE;
}

/* ── power ───────────────────────────────────────────────────────── */

static gboolean disarm(gpointer d)
{
    (void)d;
    if (L.armed) {
        gtk_style_context_remove_class(gtk_widget_get_style_context(L.armed), "lp-lock-armed");
        GtkWidget *lbl = g_object_get_data(G_OBJECT(L.armed), "label");
        gtk_label_set_text(GTK_LABEL(lbl), g_object_get_data(G_OBJECT(L.armed), "text"));
    }
    L.armed = NULL;
    L.armed_id = 0;
    return G_SOURCE_REMOVE;
}

/* Two taps: the first says what the second will do. A lock screen is
 * where a pocket or an elbow lands on the touch panel. */
static void on_power(GtkButton *b, gpointer d)
{
    const char *what = d;
    if (L.armed != GTK_WIDGET(b)) {
        disarm(NULL);
        L.armed = GTK_WIDGET(b);
        gtk_style_context_add_class(gtk_widget_get_style_context(L.armed), "lp-lock-armed");
        GtkWidget *lbl = g_object_get_data(G_OBJECT(b), "label");
        gtk_label_set_text(GTK_LABEL(lbl), T("Tap again", "한 번 더 누르세요"));
        L.armed_id = g_timeout_add_seconds(4, disarm, NULL);
        return;
    }
    if (L.armed_id)
        g_source_remove(L.armed_id);
    disarm(NULL);
    const char *argv[] = { "lp-power", what, NULL };
    lp_spawn(argv);
}

static GtkWidget *power_button(const char *icon, const char *text, const char *what)
{
    GtkWidget *b = gtk_button_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_LARGE_TOOLBAR);
    GtkWidget *lbl = gtk_label_new(text);
    gtk_container_add(GTK_CONTAINER(box), img);
    gtk_container_add(GTK_CONTAINER(box), lbl);
    gtk_container_add(GTK_CONTAINER(b), box);
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "lp-lock-power");
    g_object_set_data(G_OBJECT(b), "label", lbl);
    g_object_set_data(G_OBJECT(b), "text", (gpointer)text);
    g_signal_connect(b, "clicked", G_CALLBACK(on_power), (gpointer)what);
    return b;
}

/* ── building ────────────────────────────────────────────────────── */

static GtkWidget *label(const char *text, const char *cls)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), cls);
    return l;
}

/* ── the accounts ────────────────────────────────────────────────── */

static Person *person_new(const char *login, const char *display, const char *home)
{
    Person *p = g_new0(Person, 1);
    p->login = g_strdup(login);
    p->display = g_strdup(display && *display ? display : login);
    char *fp = g_build_filename(home, ".face", NULL);
    p->face = gdk_pixbuf_new_from_file_at_scale(fp, 256, 256, TRUE, NULL);
    g_free(fp);
    return p;
}

/* Everybody one could sign in as, straight from /etc/passwd: uid 1000
 * up to 59999, a login shell, a home that exists. The session's own
 * account first, under the name the rest of the desktop shows. */
static void load_people(void)
{
    L.people = g_ptr_array_new();
    struct passwd *me = getpwuid(getuid());
    g_ptr_array_add(L.people, person_new(L.user, L.name, me ? me->pw_dir : g_get_home_dir()));
    FILE *f = fopen("/etc/passwd", "re");
    if (!f)
        return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        char **fl = g_strsplit(line, ":", 7);
        if (g_strv_length(fl) == 7) {
            long uid = strtol(fl[2], NULL, 10);
            const char *sh = strrchr(fl[6], '/');
            sh = sh ? sh + 1 : fl[6];
            gboolean login_sh = *fl[6] && strcmp(sh, "nologin") && strcmp(sh, "false");
            if (uid >= 1000 && uid < 60000 && login_sh && strcmp(fl[0], L.user) &&
                g_file_test(fl[5], G_FILE_TEST_IS_DIR)) {
                char *full = g_strdup(fl[4]);
                char *comma = strchr(full, ',');
                if (comma) *comma = '\0';
                g_ptr_array_add(L.people, person_new(fl[0], g_strstrip(full), fl[5]));
                g_free(full);
            }
        }
        g_strfreev(fl);
    }
    fclose(f);
}

static void pick(GtkButton *b, gpointer d)
{
    (void)b;
    Person *p = d;
    if (L.checking || p == L.target)
        return;
    L.target = p;
    for (guint i = 0; i < L.people->len; i++) {
        Person *q = g_ptr_array_index(L.people, i);
        GtkStyleContext *sc = gtk_widget_get_style_context(q->chip);
        if (q == p)
            gtk_style_context_add_class(sc, "lp-lock-person-on");
        else
            gtk_style_context_remove_class(sc, "lp-lock-person-on");
    }
    gtk_label_set_text(GTK_LABEL(L.name_lbl), p->display);
    gboolean other = strcmp(p->login, L.user) != 0;
    char *hint = other ? g_strdup_printf(T("Enter the password for %s to sign in.",
                                           "%s 의 암호를 입력해 로그인하세요."), p->display)
                       : g_strdup(L.login ? T("Enter your password to sign in.", "암호를 입력해 로그인하세요.")
                                          : T("Locked. Enter your password to unlock.",
                                              "잠겨 있습니다. 암호를 입력해 잠금을 해제하세요."));
    gtk_label_set_text(GTK_LABEL(L.hint), hint);
    g_free(hint);
    gtk_widget_queue_draw(L.avatar);
    gtk_entry_set_text(GTK_ENTRY(L.entry), "");
    set_msg("", NULL);
    L.fails = 0;
    gtk_widget_grab_focus(L.entry);
}

/* One tap target per account: its picture or initial, and its name. */
static GtkWidget *people_row(void)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "lp-lock-people");
    for (guint i = 0; i < L.people->len; i++) {
        Person *p = g_ptr_array_index(L.people, i);
        GtkWidget *b = gtk_button_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *av = gtk_drawing_area_new();
        gtk_widget_set_size_request(av, 36, 36);
        gtk_widget_set_valign(av, GTK_ALIGN_CENTER);
        g_signal_connect(av, "draw", G_CALLBACK(draw_avatar), p);
        gtk_container_add(GTK_CONTAINER(box), av);
        GtkWidget *l = gtk_label_new(p->display);
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(l), 14);
        gtk_container_add(GTK_CONTAINER(box), l);
        gtk_container_add(GTK_CONTAINER(b), box);
        gtk_style_context_add_class(gtk_widget_get_style_context(b), "lp-lock-person");
        if (p == L.target)
            gtk_style_context_add_class(gtk_widget_get_style_context(b), "lp-lock-person-on");
        g_signal_connect(b, "clicked", G_CALLBACK(pick), p);
        p->chip = b;
        gtk_container_add(GTK_CONTAINER(row), b);
    }
    return row;
}

static GtkWidget *build_card(void)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(card, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "lp-lock-card");

    GtkWidget *av = gtk_drawing_area_new();
    gtk_widget_set_size_request(av, 104, 104);
    gtk_widget_set_halign(av, GTK_ALIGN_CENTER);
    g_signal_connect(av, "draw", G_CALLBACK(draw_avatar), NULL);
    gtk_container_add(GTK_CONTAINER(card), av);
    L.avatar = av;

    L.name_lbl = label(L.target ? L.target->display : L.name, "lp-lock-name");
    gtk_container_add(GTK_CONTAINER(card), L.name_lbl);
    L.hint = label(L.login ? T("Enter your password to sign in.", "암호를 입력해 로그인하세요.")
                           : T("Locked. Enter your password to unlock.",
                               "잠겨 있습니다. 암호를 입력해 잠금을 해제하세요."),
                   "lp-lock-hint");
    gtk_container_add(GTK_CONTAINER(card), L.hint);

    /* The box and its button, one pill; this is what shakes. */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "lp-lock-field");
    L.entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(L.entry), FALSE);
    gtk_entry_set_invisible_char(GTK_ENTRY(L.entry), 0x25CF);   /* ● */
    gtk_entry_set_placeholder_text(GTK_ENTRY(L.entry), T("Password", "암호"));
    gtk_entry_set_input_purpose(GTK_ENTRY(L.entry), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_width_chars(GTK_ENTRY(L.entry), 22);
    gtk_widget_set_hexpand(L.entry, TRUE);
    g_signal_connect(L.entry, "activate", G_CALLBACK(on_activate), NULL);
    gtk_container_add(GTK_CONTAINER(row), L.entry);
    L.go = gtk_button_new();
    L.go_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(L.go_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(L.go_stack), 150);
    gtk_stack_add_named(GTK_STACK(L.go_stack),
                        gtk_image_new_from_icon_name("go-next-symbolic", GTK_ICON_SIZE_BUTTON), "arrow");
    L.spin = gtk_drawing_area_new();
    gtk_widget_set_size_request(L.spin, 18, 18);
    gtk_widget_set_halign(L.spin, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(L.spin, GTK_ALIGN_CENTER);
    g_signal_connect(L.spin, "draw", G_CALLBACK(spin_draw), NULL);
    gtk_stack_add_named(GTK_STACK(L.go_stack), L.spin, "spin");
    gtk_container_add(GTK_CONTAINER(L.go), L.go_stack);
    gtk_widget_set_tooltip_text(L.go, L.login ? T("Sign in", "로그인") : T("Unlock", "잠금 해제"));
    gtk_style_context_add_class(gtk_widget_get_style_context(L.go), "lp-lock-go");
    g_signal_connect(L.go, "clicked", G_CALLBACK(on_go), NULL);
    gtk_container_add(GTK_CONTAINER(row), L.go);
    L.shake = row;
    gtk_container_add(GTK_CONTAINER(card), row);

    L.caps = label(T("Caps Lock is on", "Caps Lock 이 켜져 있습니다"), "lp-lock-caps");
    gtk_widget_set_no_show_all(L.caps, TRUE);
    gtk_container_add(GTK_CONTAINER(card), L.caps);
    L.msg = label("", "lp-lock-msg");
    gtk_label_set_line_wrap(GTK_LABEL(L.msg), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(L.msg), 40);
    gtk_label_set_justify(GTK_LABEL(L.msg), GTK_JUSTIFY_CENTER);
    gtk_container_add(GTK_CONTAINER(card), L.msg);
    /* At sign-in only (the start of the machine, after a log out), not
     * on a screen locked over somebody's open work: signing in another
     * account ends this session, and knowing one's own password must not
     * be enough to close everybody else's windows. */
    if (L.login && L.people && L.people->len > 1)
        gtk_container_add(GTK_CONTAINER(card), people_row());
    return card;
}

/* A key typed while the box does not have the focus (after a click on
 * the wallpaper) still goes into it. */
static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w; (void)d;
    if (!L.entry || L.checking)
        return FALSE;
    if (ev->keyval == GDK_KEY_Escape) {
        gtk_entry_set_text(GTK_ENTRY(L.entry), "");
        set_msg("", NULL);
        return TRUE;
    }
    if (!gtk_widget_has_focus(L.entry)) {
        gtk_widget_grab_focus(L.entry);
        return gtk_widget_event(L.entry, (GdkEvent *)ev);
    }
    return FALSE;
}

static void make_primary(Screen *s)
{
    if (L.primary == s)
        return;
    if (L.primary)
        gtk_layer_set_keyboard_mode(GTK_WINDOW(L.primary->win),
                                    GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    GtkWidget *parent = gtk_widget_get_parent(L.card);
    g_object_ref(L.card);
    if (parent)
        gtk_container_remove(GTK_CONTAINER(parent), L.card);
    gtk_overlay_add_overlay(GTK_OVERLAY(s->root), L.card);
    g_object_unref(L.card);
    L.primary = s;
    gtk_layer_set_keyboard_mode(GTK_WINDOW(s->win), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    gtk_widget_show_all(L.card);
    caps_update(gdk_keymap_get_for_display(gdk_display_get_default()), NULL);
    gtk_widget_grab_focus(L.entry);
}

static Screen *add_screen(GdkMonitor *mon)
{
    Screen *s = g_new0(Screen, 1);
    s->mon = mon;
    s->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWindow *win = GTK_WINDOW(s->win);
    gtk_layer_init_for_window(win);
    gtk_layer_set_namespace(win, "lp-lockscreen");
    gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_monitor(win, mon);
    for (int e = 0; e < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER; e++)
        gtk_layer_set_anchor(win, e, TRUE);
    gtk_layer_set_exclusive_zone(win, -1);
    gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    GdkScreen *scr = gtk_widget_get_screen(s->win);
    GdkVisual *v = gdk_screen_get_rgba_visual(scr);
    if (v)
        gtk_widget_set_visual(s->win, v);
    gtk_style_context_add_class(gtk_widget_get_style_context(s->win), "lp-lock");
    g_signal_connect(s->win, "key-press-event", G_CALLBACK(on_key), NULL);

    s->root = gtk_overlay_new();
    s->bg = gtk_drawing_area_new();
    g_signal_connect(s->bg, "draw", G_CALLBACK(draw_bg), NULL);
    gtk_container_add(GTK_CONTAINER(s->root), s->bg);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(top, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(top, GTK_ALIGN_START);
    /* A seventh of the way down (GTK 3's CSS has no percentages). */
    GdkRectangle g;
    gdk_monitor_get_geometry(mon, &g);
    gtk_widget_set_margin_top(top, g.height / 7);
    gtk_style_context_add_class(gtk_widget_get_style_context(top), "lp-lock-top");
    s->clock = label("", "lp-lock-clock");
    s->date = label("", "lp-lock-date");
    gtk_container_add(GTK_CONTAINER(top), s->clock);
    gtk_container_add(GTK_CONTAINER(top), s->date);
    gtk_overlay_add_overlay(GTK_OVERLAY(s->root), top);

    s->power = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_set_halign(s->power, GTK_ALIGN_END);
    gtk_widget_set_valign(s->power, GTK_ALIGN_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(s->power), "lp-lock-powerbox");
    gtk_container_add(GTK_CONTAINER(s->power),
                      power_button("system-reboot-symbolic", T("Restart", "다시 시작"), "restart"));
    gtk_container_add(GTK_CONTAINER(s->power),
                      power_button("system-shutdown-symbolic", T("Shut down", "전원 끄기"), "off"));
    gtk_overlay_add_overlay(GTK_OVERLAY(s->root), s->power);

    gtk_container_add(GTK_CONTAINER(s->win), s->root);
    g_ptr_array_add(L.screens, s);
    gtk_widget_show_all(s->win);
    return s;
}

static void on_monitor_added(GdkDisplay *d, GdkMonitor *m, gpointer data)
{
    (void)d; (void)data;
    add_screen(m);
    tick_clock(NULL);
    if (!L.primary)
        make_primary(g_ptr_array_index(L.screens, 0));
}

static void on_monitor_removed(GdkDisplay *d, GdkMonitor *m, gpointer data)
{
    (void)d; (void)data;
    for (guint i = 0; i < L.screens->len; i++) {
        Screen *s = g_ptr_array_index(L.screens, i);
        if (s->mon != m)
            continue;
        gboolean was_primary = s == L.primary;
        if (was_primary) {
            g_object_ref(L.card);
            gtk_container_remove(GTK_CONTAINER(s->root), L.card);
            L.primary = NULL;
        }
        gtk_widget_destroy(s->win);
        g_ptr_array_remove_index(L.screens, i);
        g_free(s);
        if (was_primary) {
            if (L.screens->len)
                make_primary(g_ptr_array_index(L.screens, 0));
            g_object_unref(L.card);
        }
        return;
    }
}

static const char *CSS =
    ".lp-lock { background-color: transparent; }"
    ".lp-lock label { color: #EAF2F8; font-family: 'Pretendard Variable', 'Noto Sans CJK KR', sans-serif; }"
    ".lp-lock-clock { font-size: 96px; font-weight: 300; letter-spacing: 1px;"
    "  text-shadow: 0 2px 12px rgba(0,0,0,0.35); }"
    ".lp-lock-date { font-size: 21px; font-weight: 500; color: rgba(234,242,248,0.86); margin-top: 2px; }"
    ".lp-lock-card { margin-top: 90px; }"
    ".lp-lock-name { font-size: 26px; font-weight: 600; margin-top: 16px; }"
    ".lp-lock-hint { font-size: 15px; color: rgba(234,242,248,0.72); margin-top: 6px; margin-bottom: 20px; }"
    ".lp-lock-field { background-color: rgba(255,255,255,0.12); border: 1px solid rgba(255,255,255,0.22);"
    "  border-radius: 16px; padding: 5px; box-shadow: 0 6px 24px rgba(0,0,0,0.25); }"
    ".lp-lock-field entry, .lp-lock-field entry:focus { background-image: none;"
    "  background-color: transparent; border: none; border-radius: 12px; box-shadow: none;"
    "  outline: none; color: #FFFFFF; font-size: 20px; min-height: 44px; padding: 0 12px;"
    "  caret-color: #1FB5A8; }"
    ".lp-lock-field entry:disabled { color: rgba(255,255,255,0.55); }"
    ".lp-lock-go { background-image: none; background-color: #F28C28; border: none; border-radius: 12px;"
    "  min-width: 46px; min-height: 44px; color: #FFFFFF; box-shadow: none; }"
    ".lp-lock-go:hover { background-color: #F59B42; }"
    ".lp-lock-go:disabled { background-color: rgba(242,140,40,0.45); }"
    ".lp-lock-go image { color: #FFFFFF; }"
    ".lp-lock-caps { font-size: 14px; color: #FFD27A; margin-top: 12px; }"
    ".lp-lock-msg { font-size: 15px; margin-top: 12px; min-height: 20px; }"
    ".lp-lock label.lp-lock-msg.lp-lock-error { color: #FF9A7A; font-weight: 600; }"
    ".lp-lock label.lp-lock-msg.lp-lock-busy { color: rgba(234,242,248,0.75); }"
    ".lp-lock label.lp-lock-caps { color: #FFD27A; }"
    ".lp-lock-people { margin-top: 26px; }"
    ".lp-lock-person { background-image: none; background-color: rgba(255,255,255,0.08);"
    "  border: 1px solid rgba(255,255,255,0.14); border-radius: 26px; padding: 6px 16px 6px 6px;"
    "  min-height: 36px; box-shadow: none; }"
    ".lp-lock-person:hover { background-color: rgba(255,255,255,0.16); }"
    ".lp-lock-person label { font-size: 15px; font-weight: 500; }"
    ".lp-lock-person.lp-lock-person-on { background-color: rgba(31,181,168,0.30); border-color: #1FB5A8; }"
    ".lp-lock-powerbox { margin: 0 40px 36px 0; }"
    ".lp-lock-power { background-image: none; background-color: rgba(255,255,255,0.10);"
    "  border: 1px solid rgba(255,255,255,0.16); border-radius: 16px; padding: 12px 16px;"
    "  min-width: 84px; box-shadow: none; color: #EAF2F8; }"
    ".lp-lock-power:hover { background-color: rgba(255,255,255,0.18); }"
    ".lp-lock-power image { color: #EAF2F8; }"
    ".lp-lock-power label { font-size: 13px; }"
    ".lp-lock-power.lp-lock-armed { background-color: rgba(232,102,61,0.85); border-color: #E8663D; }";

static GdkPixbuf *load_wallpaper(void)
{
    char *chosen = lp_config_read("wallpaper");
    GdkPixbuf *pb = NULL;
    if (chosen && *chosen)
        pb = gdk_pixbuf_new_from_file(chosen, NULL);
    g_free(chosen);
    if (!pb) {
        char *def = lp_share_path("wallpaper.png");
        pb = gdk_pixbuf_new_from_file(def, NULL);
        g_free(def);
    }
    if (!pb)
        pb = gdk_pixbuf_new_from_file("/usr/share/backgrounds/lp/lp-wallpaper-dark-3840x2160.png",
                                      NULL);
    return pb;
}

/* ── arriving ─────────────────────────────────────────────────────
 * Signing in at boot, the screen under this one is the boot's loading
 * cover (lp-splash-fade: the maker's logo, ours, the spinner), and the
 * sign-in screen fades in over it - then the cover is told it can go,
 * underneath, unseen. A lock of a running session comes up at once. */
static void cover_done(void)
{
    const char *argv[] = { "lp-splash-fade", "done", NULL };
    g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH |
                  G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
}

static gboolean arrive_tick(GtkWidget *w, GdkFrameClock *fc, gpointer d)
{
    (void)w; (void)d;
    static gint64 t0;
    gint64 now = gdk_frame_clock_get_frame_time(fc);
    if (!t0)
        t0 = now;
    double t = (now - t0) / 260000.0;
    double a = t >= 1.0 ? 1.0 : 1.0 - (1.0 - t) * (1.0 - t);
    for (guint i = 0; i < L.screens->len; i++) {
        Screen *s = g_ptr_array_index(L.screens, i);
        gtk_widget_set_opacity(s->root, a);
    }
    if (t >= 1.0) {
        cover_done();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean arrive_anyway(gpointer d)
{
    /* the frame clock never ran (an output off): show it as it is */
    (void)d;
    for (guint i = 0; i < L.screens->len; i++) {
        Screen *s = g_ptr_array_index(L.screens, i);
        if (gtk_widget_get_opacity(s->root) < 1.0)
            gtk_widget_set_opacity(s->root, 1.0);
    }
    cover_done();
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--login"))
            L.login = TRUE;

    lp_shell_init(&argc, &argv);
    take_input();

    struct passwd *pw = getpwuid(getuid());
    L.user = g_strdup(pw ? pw->pw_name : g_get_user_name());
    L.name = lp_user_display_name();
    load_people();
    L.target = g_ptr_array_index(L.people, 0);
    L.wall = load_wallpaper();
    L.screens = g_ptr_array_new();

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_USER + 101);

    L.card = build_card();
    g_object_ref_sink(L.card);

    GdkDisplay *d = gdk_display_get_default();
    int n = gdk_display_get_n_monitors(d);
    for (int i = 0; i < n; i++)
        add_screen(gdk_display_get_monitor(d, i));
    if (L.screens->len == 0) {
        g_printerr("lp-lockscreen: no monitor\n");
        return EXIT_CANNOT;
    }
    /* The built-in panel, or else the first: where the person is. */
    Screen *first = g_ptr_array_index(L.screens, 0);
    GdkMonitor *pm = gdk_display_get_primary_monitor(d);
    for (guint i = 0; i < L.screens->len; i++) {
        Screen *s = g_ptr_array_index(L.screens, i);
        if (s->mon == pm)
            first = s;
    }
    make_primary(first);
    g_object_unref(L.card);
    g_signal_connect(d, "monitor-added", G_CALLBACK(on_monitor_added), NULL);
    g_signal_connect(d, "monitor-removed", G_CALLBACK(on_monitor_removed), NULL);

    GdkKeymap *km = gdk_keymap_get_for_display(d);
    g_signal_connect(km, "state-changed", G_CALLBACK(caps_update), NULL);
    caps_update(km, NULL);

    tick_clock(NULL);
    g_timeout_add_seconds(1, tick_clock, NULL);
    if (L.login) {
        for (guint i = 0; i < L.screens->len; i++) {
            Screen *s = g_ptr_array_index(L.screens, i);
            gtk_widget_set_opacity(s->root, 0.0);
        }
        gtk_widget_add_tick_callback(L.primary->root, arrive_tick, NULL, NULL);
        g_timeout_add(1500, arrive_anyway, NULL);
    } else
        cover_done();
    gtk_main();
    /* gtk_main returns only if every window went away, which is not an
     * unlock: lp-lock starts this again. */
    return 1;
}
