/*
 * lp-apps.c - matching windows to applications. See lp-apps.h.
 *
 * Tried in order, first match wins:
 *   1. <app_id>.desktop, as given and in lower case
 *   2. a .desktop file whose StartupWMClass equals the app_id
 *   3. a .desktop file whose Exec program is named like the app_id,
 *      or like its last dotted component ("org.gnome.Calculator" and
 *      gnome-calculator do not match; "org.lpzero.Files" and lp-files
 *      do not either - that is what rule 2 is for)
 */
#define _GNU_SOURCE 1   /* realpath, in the -std=c11 build too (lp-tasks) */
#include "lp-apps.h"

#include <gdk/gdk.h>
#include <gio/gdesktopappinfo.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static char *exec_base(GAppInfo *info)
{
    const char *ex = g_app_info_get_executable(info);
    return ex ? g_path_get_basename(ex) : NULL;
}

gboolean lp_app_owns(GDesktopAppInfo *info, const char *app_id)
{
    if (!info || !app_id || !*app_id)
        return FALSE;
    const char *id = g_app_info_get_id(G_APP_INFO(info));
    if (id) {
        size_t n = strlen(app_id);
        if (g_ascii_strncasecmp(id, app_id, n) == 0 &&
            strcmp(id + n, ".desktop") == 0)
            return TRUE;
    }
    const char *wm = g_desktop_app_info_get_startup_wm_class(info);
    if (wm && g_ascii_strcasecmp(wm, app_id) == 0)
        return TRUE;
    char *ex = exec_base(G_APP_INFO(info));
    gboolean ok = FALSE;
    if (ex) {
        const char *last = strrchr(app_id, '.');
        ok = g_ascii_strcasecmp(ex, app_id) == 0 ||
             (last && g_ascii_strcasecmp(ex, last + 1) == 0);
        g_free(ex);
    }
    return ok;
}

GDesktopAppInfo *lp_app_for_id(const char *app_id)
{
    if (!app_id || !*app_id)
        return NULL;
    char *name = g_strconcat(app_id, ".desktop", NULL);
    GDesktopAppInfo *d = g_desktop_app_info_new(name);
    g_free(name);
    if (d)
        return d;
    char *low = g_ascii_strdown(app_id, -1);
    name = g_strconcat(low, ".desktop", NULL);
    d = g_desktop_app_info_new(name);
    g_free(name);
    g_free(low);
    if (d)
        return d;

    GList *all = g_app_info_get_all();
    GDesktopAppInfo *found = NULL;
    for (GList *l = all; l && !found; l = l->next)
        if (G_IS_DESKTOP_APP_INFO(l->data) &&
            lp_app_owns(G_DESKTOP_APP_INFO(l->data), app_id))
            found = g_object_ref(l->data);
    g_list_free_full(all, g_object_unref);
    return found;
}

/* Terminal=true programs (htop, and the like) run inside the terminal -
 * Console (kgx), whose window has the same title bar as every other,
 * or foot where Console is missing. GLib looks for a terminal from its
 * own fixed list - gnome-terminal, xterm and so on - which neither is on,
 * and the program then started with nothing to show it in: pressing it
 * did nothing. */
static gboolean launch_in_terminal(GAppInfo *info)
{
    if (!G_IS_DESKTOP_APP_INFO(info) ||
        !g_desktop_app_info_get_boolean(G_DESKTOP_APP_INFO(info), "Terminal"))
        return FALSE;
    const char *cmd = g_app_info_get_commandline(info);
    if (!cmd || !*cmd)
        return FALSE;
    GString *c = g_string_new(NULL);
    for (const char *p = cmd; *p; p++) {
        if (*p == '%' && p[1]) {           /* a field code: %f %U ... */
            p++;
            if (*p == '%')
                g_string_append_c(c, '%');
            continue;
        }
        g_string_append_c(c, *p);
    }
    char *kgx = g_find_program_in_path("kgx");
    char *argv_kgx[] = { (char *)"kgx", (char *)"--", (char *)"sh", (char *)"-c",
                         c->str, NULL };
    char *argv_foot[] = { (char *)"foot", (char *)"-e", (char *)"sh", (char *)"-c",
                          c->str, NULL };
    char **argv = kgx ? argv_kgx : argv_foot;
    g_free(kgx);
    GError *err = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err)) {
        g_printerr("lp-shell: %s in %s: %s\n", g_app_info_get_id(info), argv[0], err->message);
        g_clear_error(&err);
    }
    g_string_free(c, TRUE);
    return TRUE;
}

/* ── Chromium and Electron: Korean from the keys ───────────────────────
 *
 * Chromium, and every Electron program (Claude, VS Code, Discord, Slack,
 * ...), draws on Wayland by itself and types only what the keys print
 * unless it is told to speak the text-input protocol - the one way an
 * input method reaches a Wayland program that does not load GTK's or Qt's
 * input method modules. So they typed English only, whatever the language
 * was. Started from the dock, the app grid or the tray they are given
 * the two switches that turn it on, and then lp-osk types Korean into
 * them as into everything else (desktop/osk/type.c). A program that does
 * not know the switches ignores them; one running under Xwayland instead
 * types through fcitx5 as it did.
 *
 * A Chromium is told by its files: resources.pak or chrome-sandbox next
 * to the program (Chrome, Chromium, any Electron build), or an .asar or
 * such a directory named in the wrapper script that starts it (VS Code's
 * /usr/bin/code, Debian's chromium), or, for a Flatpak, somewhere in its
 * files. */
#define IME_FLAGS "--enable-wayland-ime --wayland-text-input-version=3"

static gboolean chromium_dir(const char *dir)
{
    static const char *const marks[] = { "resources.pak", "chrome-sandbox",
                                         "chrome_100_percent.pak", NULL };
    for (int i = 0; marks[i]; i++) {
        char *p = g_build_filename(dir, marks[i], NULL);
        gboolean yes = g_file_test(p, G_FILE_TEST_EXISTS);
        g_free(p);
        if (yes)
            return TRUE;
    }
    return FALSE;
}

/* A path, or a program found on PATH, that is a Chromium's. */
static gboolean chromium_path(const char *path)
{
    if (g_str_has_suffix(path, ".asar"))
        return g_file_test(path, G_FILE_TEST_EXISTS);
    char *full = g_path_is_absolute(path) ? g_strdup(path) : g_find_program_in_path(path);
    if (!full)
        return FALSE;
    char *real = realpath(full, NULL);
    g_free(full);
    if (!real)
        return FALSE;
    gboolean yes;
    if (g_file_test(real, G_FILE_TEST_IS_DIR)) {
        yes = chromium_dir(real);
    } else {
        char *dir = g_path_get_dirname(real);
        yes = chromium_dir(dir);
        g_free(dir);
    }
    free(real);
    return yes;
}

/* A wrapper script names the real program somewhere in its text: every
 * absolute path in it, and the word electron, is looked at. */
static gboolean chromium_script(const char *program)
{
    char *full = g_path_is_absolute(program) ? g_strdup(program)
                                             : g_find_program_in_path(program);
    if (!full)
        return FALSE;
    char *text = NULL;
    gsize len = 0;
    gboolean yes = FALSE;
    if (g_file_get_contents(full, &text, &len, NULL) && len > 2 && len < 256 * 1024 &&
        text[0] == '#' && text[1] == '!') {
        GRegex *re = g_regex_new("(/[A-Za-z0-9._+@-]+)+|\\belectron[0-9]*\\b", 0, 0, NULL);
        GMatchInfo *m = NULL;
        g_regex_match(re, text, 0, &m);
        while (!yes && g_match_info_matches(m)) {
            char *w = g_match_info_fetch(m, 0);
            /* the interpreter on the #! line is not the program */
            if (!g_str_has_prefix(w, "/bin/") && !g_str_has_prefix(w, "/usr/bin/env") &&
                !g_str_has_suffix(w, "/sh") && !g_str_has_suffix(w, "/bash") &&
                !g_str_has_suffix(w, "/dash") && strcmp(w, "/dev/null") != 0)
                yes = chromium_path(w);
            g_free(w);
            g_match_info_next(m, NULL);
        }
        g_match_info_free(m);
        g_regex_unref(re);
    }
    g_free(text);
    g_free(full);
    return yes;
}

/* A Flatpak's files, a few levels down (Chrome's are in extra/, most
 * Electron programs' in a directory of their own). */
static gboolean chromium_tree(const char *dir, int depth, int *budget)
{
    if (chromium_dir(dir))
        return TRUE;
    if (depth == 0)
        return FALSE;
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d)
        return FALSE;
    gboolean yes = FALSE;
    const char *name;
    while (!yes && (name = g_dir_read_name(d)) && --*budget > 0) {
        if (!strcmp(name, "share") || !strcmp(name, "include") || !strcmp(name, "locales"))
            continue;
        char *p = g_build_filename(dir, name, NULL);
        if (g_file_test(p, G_FILE_TEST_IS_DIR) && !g_file_test(p, G_FILE_TEST_IS_SYMLINK))
            yes = chromium_tree(p, depth - 1, budget);
        g_free(p);
    }
    g_dir_close(d);
    return yes;
}

static gboolean chromium_flatpak(const char *app)
{
    char *where[] = {
        g_build_filename(g_get_user_data_dir(), "flatpak", "app", app,
                         "current", "active", "files", NULL),
        g_build_filename("/var/lib/flatpak", "app", app, "current", "active", "files", NULL),
    };
    gboolean yes = FALSE;
    for (unsigned i = 0; i < G_N_ELEMENTS(where); i++) {
        int budget = 4000;
        if (!yes && g_file_test(where[i], G_FILE_TEST_IS_DIR))
            yes = chromium_tree(where[i], 4, &budget);
        g_free(where[i]);
    }
    return yes;
}

static gboolean wants_ime_flags(GDesktopAppInfo *info)
{
    const char *cmd = g_app_info_get_commandline(G_APP_INFO(info));
    char **argv = NULL;
    if (!cmd || strstr(cmd, "--enable-wayland-ime") ||
        !g_shell_parse_argv(cmd, NULL, &argv, NULL))
        return FALSE;
    int i = 0;
    /* env NAME=value ... program */
    if (argv[i] && !strcmp(argv[i], "env") ) {
        for (i++; argv[i] && (strchr(argv[i], '=') || argv[i][0] == '-'); i++)
            ;
    }
    gboolean yes = FALSE;
    if (argv[i]) {
        char *base = g_path_get_basename(argv[i]);
        if (!strcmp(base, "flatpak")) {
            int j = i + 1;
            while (argv[j] && strcmp(argv[j], "run"))
                j++;
            if (argv[j])
                for (j++; argv[j]; j++)
                    if (argv[j][0] != '-') {
                        yes = chromium_flatpak(argv[j]);
                        break;
                    }
        } else {
            yes = chromium_path(argv[i]) || chromium_script(argv[i]);
        }
        g_free(base);
    }
    g_strfreev(argv);
    return yes;
}

/* The same .desktop file with the switches after its command. */
static GAppInfo *with_ime_flags(GDesktopAppInfo *info)
{
    const char *file = g_desktop_app_info_get_filename(info);
    GKeyFile *kf = g_key_file_new();
    GAppInfo *out = NULL;
    if (file && g_key_file_load_from_file(kf, file, G_KEY_FILE_KEEP_TRANSLATIONS, NULL)) {
        char *exec = g_key_file_get_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                                           G_KEY_FILE_DESKTOP_KEY_EXEC, NULL);
        if (exec) {
            char *more = g_strconcat(exec, " " IME_FLAGS, NULL);
            g_key_file_set_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                                  G_KEY_FILE_DESKTOP_KEY_EXEC, more);
            g_free(more);
            g_free(exec);
            out = G_APP_INFO(g_desktop_app_info_new_from_keyfile(kf));
        }
    }
    g_key_file_unref(kf);
    return out;
}

void lp_app_launch(GAppInfo *info)
{
    if (launch_in_terminal(info))
        return;
    GAppInfo *run = NULL;
    if (G_IS_DESKTOP_APP_INFO(info) && wants_ime_flags(G_DESKTOP_APP_INFO(info)))
        run = with_ime_flags(G_DESKTOP_APP_INFO(info));
    GdkAppLaunchContext *ctx =
        gdk_display_get_app_launch_context(gdk_display_get_default());
    GError *err = NULL;
    if (!g_app_info_launch(run ? run : info, NULL, G_APP_LAUNCH_CONTEXT(ctx), &err)) {
        g_printerr("lp-shell: %s: %s\n", g_app_info_get_id(info),
                   err->message);
        g_clear_error(&err);
    }
    g_clear_object(&run);
    g_object_unref(ctx);
}

const char *lp_app_name(GAppInfo *info)
{
    const char *n = g_app_info_get_display_name(info);
    return n ? n : g_app_info_get_id(info);
}
