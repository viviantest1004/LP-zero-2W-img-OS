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
#include "lp-apps.h"

#include <gdk/gdk.h>
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

void lp_app_launch(GAppInfo *info)
{
    GdkAppLaunchContext *ctx =
        gdk_display_get_app_launch_context(gdk_display_get_default());
    GError *err = NULL;
    if (!g_app_info_launch(info, NULL, G_APP_LAUNCH_CONTEXT(ctx), &err)) {
        g_printerr("lp-shell: %s: %s\n", g_app_info_get_id(info),
                   err->message);
        g_clear_error(&err);
    }
    g_object_unref(ctx);
}

const char *lp_app_name(GAppInfo *info)
{
    const char *n = g_app_info_get_display_name(info);
    return n ? n : g_app_info_get_id(info);
}
