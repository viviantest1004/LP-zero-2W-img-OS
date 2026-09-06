/* files - the file manager.
 *
 * One directory at a time: a sidebar of places, a breadcrumb of the way
 * here, and a list of what is in front of you.
 *
 * Everything visible is a CSS class. The colours, the hairlines and the
 * shadow live in desktop/theme/gtk-4.0/gtk.css, so changing the look is
 * changing one stylesheet rather than rebuilding this program. Only the
 * numbers CSS cannot carry are set from here - the sidebar width, the
 * two fixed column widths, and the size the window opens at - and they
 * are named as constants so they are easy to find.
 *
 * Reading a directory is where this program gets lied to, and the three
 * lies are answered where they happen: a name that is not valid UTF-8, a
 * symlink whose target is gone, and a directory that changes underneath
 * the read. Each has a comment at the place the decision was made.
 *
 * GTK 4.8 is the floor. gtk_show_uri_full() and GtkMessageDialog are
 * current API there; both are deprecated from 4.10 onward, which is why
 * the Makefile does not turn warnings into errors.
 */
#include <gtk/gtk.h>
#include "lp-i18n.h"
#include <sys/statvfs.h>
#include <string.h>

#define APP_ID "org.lpzero.Files"

/* The mockup's numbers that CSS has no way to express. */
#define SIDEBAR_WIDTH   96
#define SIZE_COL_WIDTH  46
#define DATE_COL_WIDTH  52

/* Not from the mockup - it draws one window, not the size it opens at. */
#define WINDOW_WIDTH   900
#define WINDOW_HEIGHT  600

/* Below this much room left, the capacity bar turns amber. §1-3 gives
 * amber to limits and constraints, and a disk this full is one. */
#define LOW_SPACE_FRACTION 0.15

/* A directory that changed while we were looking at it is usually still
 * changing. Coalescing the monitor's events over this long turns a file
 * copy into one reload instead of one per block written. */
#define REFRESH_DEBOUNCE_MS 300

#define QUERY_ATTRS \
    G_FILE_ATTRIBUTE_STANDARD_NAME          "," \
    G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME  "," \
    G_FILE_ATTRIBUTE_STANDARD_TYPE          "," \
    G_FILE_ATTRIBUTE_STANDARD_SIZE          "," \
    G_FILE_ATTRIBUTE_ACCESS_CAN_READ        "," \
    G_FILE_ATTRIBUTE_TIME_MODIFIED

/* ------------------------------------------------------------------ */
/* One row                                                             */
/* ------------------------------------------------------------------ */

#define LPF_TYPE_ITEM (lpf_item_get_type())
G_DECLARE_FINAL_TYPE(LpfItem, lpf_item, LPF, ITEM, GObject)

struct _LpfItem {
    GObject   parent_instance;

    /* `name` is the bytes on disk and `display` is the text on screen,
     * and they are not always the same string. A filename is a byte
     * sequence, not text - it may be latin-1, or a fragment of a UTF-8
     * sequence that was cut in half by a broken copy. GIO's display-name
     * substitutes for whatever it cannot decode so the row is readable,
     * which means two different files can display identically. Anything
     * that touches the disk therefore uses `name`, never `display`. */
    char     *name;
    char     *display;

    char     *size_text;
    char     *date_text;
    char     *icon_name;
    char     *sort_key;
    goffset   size;
    gboolean  is_dir;
    gboolean  readable;
    gboolean  dangling;   /* a symlink whose target is not there */
};

G_DEFINE_TYPE(LpfItem, lpf_item, G_TYPE_OBJECT)

static void lpf_item_finalize(GObject *object)
{
    LpfItem *self = LPF_ITEM(object);

    g_free(self->name);
    g_free(self->display);
    g_free(self->size_text);
    g_free(self->date_text);
    g_free(self->icon_name);
    g_free(self->sort_key);

    G_OBJECT_CLASS(lpf_item_parent_class)->finalize(object);
}

static void lpf_item_class_init(LpfItemClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = lpf_item_finalize;
}

static void lpf_item_init(LpfItem *self)
{
    (void)self;
}

/* ------------------------------------------------------------------ */
/* The window                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    GtkWidget *row;    /* the button, so its selected class can be set */
    char      *path;
} Place;

typedef struct {
    GtkWindow          *window;
    GtkWidget          *title;
    GtkWidget          *back;
    GtkWidget          *forward;
    GtkWidget          *crumbs;
    GtkWidget          *stack;
    GtkWidget          *search_bar;
    GtkWidget          *search_entry;
    GtkWidget          *status_left;
    GtkWidget          *status_right;
    GtkWidget          *capacity;
    GtkWidget          *capacity_label;

    GListStore         *store;
    GtkCustomFilter    *filter;
    GtkSelectionModel  *selection;

    /* Where we have been. `pos` is the current entry, so back and
     * forward are nothing more than moving it. */
    GPtrArray          *history;
    guint               pos;

    char               *path;
    GFileMonitor       *monitor;
    guint               refresh_id;
    gboolean            show_hidden;

    GPtrArray          *places;   /* Place*, for the selected row */
} App;

static void navigate(App *app, const char *path, gboolean push);
static void reload(App *app);
static void update_status(App *app);

/* ------------------------------------------------------------------ */
/* How the mockup writes numbers and dates                             */
/* ------------------------------------------------------------------ */

/* 4.2KB, 1.8MB, 35MB - one decimal below ten and none from ten up. Three
 * significant figures would give 4.19KB and 1.80MB; the mockup's rule is
 * shorter and it keeps the column from jittering as a copy grows. */
static char *format_size(goffset bytes)
{
    static const char *unit[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    double v = (double)bytes;
    int    u = 0;

    while (v >= 1024.0 && u < (int)G_N_ELEMENTS(unit) - 1) {
        v /= 1024.0;
        u++;
    }

    if (u == 0)
        return g_strdup_printf("%.0fB", v);
    if (v < 10.0)
        return g_strdup_printf("%.1f%s", v, unit[u]);
    return g_strdup_printf("%.0f%s", v, unit[u]);
}

/* "31GB 남음" for the capacity bar and the right of the status bar. */
static char *format_free(const char *path)
{
    struct statvfs vfs;

    if (statvfs(path, &vfs) != 0)
        return NULL;

    /* f_bavail, not f_bfree. ext4 holds a few percent back for root, and
     * the honest number is what an ordinary process may still use - the
     * same distinction df makes. */
    guint64 bytes = (guint64)vfs.f_bavail * (guint64)vfs.f_frsize;
    char   *size  = format_size((goffset)bytes);
    char   *text  = g_strdup_printf(T("%s free", "%s 남음"), size);

    g_free(size);
    return text;
}

/* How full the filesystem is, 0.0 to 1.0, for the capacity bar. */
static gboolean disk_usage(const char *path, double *used, double *free_part)
{
    struct statvfs vfs;

    if (statvfs(path, &vfs) != 0 || vfs.f_blocks == 0)
        return FALSE;

    double avail = (double)vfs.f_bavail / (double)vfs.f_blocks;

    /* The bar fills to what is gone from an ordinary process's point of
     * view, so the reserve reads as used. It is not available, and a bar
     * that says otherwise promises room that is not there. */
    *free_part = avail;
    *used      = 1.0 - avail;
    return TRUE;
}

/* Whole days between two moments, counted by calendar day and not by
 * dividing seconds - "어제" has to mean yesterday's date, not twenty-four
 * hours ago, or a file saved last night reads as today until noon. */
static int days_between(GDateTime *then, GDateTime *now)
{
    GDate a, b;

    g_date_set_dmy(&a, g_date_time_get_day_of_month(then),
                   g_date_time_get_month(then), g_date_time_get_year(then));
    g_date_set_dmy(&b, g_date_time_get_day_of_month(now),
                   g_date_time_get_month(now), g_date_time_get_year(now));

    return (int)g_date_days_between(&a, &b);
}

/* Today is a time, yesterday is 어제, this year is 9월 4일, and anything
 * older carries its year. The year form is written 2024. 9. 4. rather
 * than 2024년 9월 4일 because the column is 52px wide and the long form
 * does not fit in it. */
static char *format_date(guint64 seconds)
{
    GDateTime *when = g_date_time_new_from_unix_local((gint64)seconds);
    if (!when)
        return g_strdup("");

    GDateTime *now  = g_date_time_new_now_local();
    int        days = days_between(when, now);
    char      *text;

    /* days is negative for a file stamped in the future, which happens
     * with a wrong clock or a copy off a FAT drive in another timezone.
     * Such a file reads as today rather than as a date that has not
     * happened yet. */
    if (days <= 0)
        text = g_date_time_format(when, "%H:%M");
    else if (days == 1)
        text = g_strdup(T("yesterday", "어제"));
    else if (g_date_time_get_year(when) == g_date_time_get_year(now))
        text = g_strdup_printf(T("%d %b", "%d월 %d일"),
                               g_date_time_get_month(when),
                               g_date_time_get_day_of_month(when));
    else
        text = g_strdup_printf("%d. %d. %d.",
                               g_date_time_get_year(when),
                               g_date_time_get_month(when),
                               g_date_time_get_day_of_month(when));

    g_date_time_unref(now);
    g_date_time_unref(when);
    return text;
}

/* ------------------------------------------------------------------ */
/* Saying so                                                           */
/* ------------------------------------------------------------------ */

/* The name goes on its own line rather than into the sentence. Korean
 * would want 을 or 를 after it, and which one is right depends on the
 * last letter of the filename - which can be a digit, a bracket, or
 * Cyrillic. A sentence that cannot be built correctly is not built. */
static void say(App *app, const char *what, const char *name, const char *why)
{
    GtkWidget *dialog = gtk_message_dialog_new(app->window,
                                               GTK_DIALOG_MODAL |
                                               GTK_DIALOG_DESTROY_WITH_PARENT,
                                               GTK_MESSAGE_OTHER,
                                               GTK_BUTTONS_NONE,
                                               "%s", what);

    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
                                             "%s\n\n%s", name, why);
    gtk_dialog_add_button(GTK_DIALOG(dialog), T("OK", "확인"), GTK_RESPONSE_CLOSE);

    g_signal_connect(dialog, "response",
                     G_CALLBACK(gtk_window_destroy), NULL);
    gtk_window_present(GTK_WINDOW(dialog));
}

/* ------------------------------------------------------------------ */
/* Reading a directory                                                 */
/* ------------------------------------------------------------------ */

/* 항목 24. The number has to agree with what you see after entering, so
 * it uses the same plain dotfile rule the listing uses rather than GIO's
 * is-hidden, which also honours .hidden files and backup names and would
 * quietly disagree with the rows.
 *
 * This costs one opendir per folder in the listing. It is readdir with
 * no stat per entry, so a folder of a thousand names is one page of I/O;
 * a directory holding hundreds of subdirectories is where it would show,
 * and that is the trade the mockup asked for by writing 항목 24. */
static gboolean count_entries(const char *path, gboolean show_hidden, guint *out)
{
    GDir *dir = g_dir_open(path, 0, NULL);
    if (!dir)
        return FALSE;   /* no permission: say nothing, rather than 0 */

    guint       n = 0;
    const char *name;

    while ((name = g_dir_read_name(dir)) != NULL)
        if (show_hidden || name[0] != '.')
            n++;

    g_dir_close(dir);
    *out = n;
    return TRUE;
}

/* Folders first, then by name. g_utf8_collate_key_for_filename is what
 * puts 2 before 10, sorts 사진 next to 사전 the way Korean expects, and
 * does not shove the dotfiles to the front when they are shown. */
static int compare_items(gconstpointer a, gconstpointer b)
{
    LpfItem *x = *(LpfItem *const *)a;
    LpfItem *y = *(LpfItem *const *)b;

    if (x->is_dir != y->is_dir)
        return x->is_dir ? -1 : 1;
    return strcmp(x->sort_key, y->sort_key);
}

static LpfItem *item_from_info(App *app, const char *dir_path, GFileInfo *info)
{
    LpfItem   *item = g_object_new(LPF_TYPE_ITEM, NULL);
    GFileType  type = g_file_info_get_file_type(info);

    item->name     = g_strdup(g_file_info_get_name(info));
    item->display  = g_strdup(g_file_info_get_display_name(info));
    item->sort_key = g_utf8_collate_key_for_filename(item->display, -1);
    item->readable = g_file_info_get_attribute_boolean(
                         info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ);

    /* We enumerate following symlinks, so a good link reports the type of
     * whatever it points at. A link still calling itself a link means the
     * stat of the target failed - the target is gone, or a loop, or on a
     * filesystem that is no longer mounted. It gets a row, because a
     * dangling link is a thing you want to see in order to delete it. */
    item->dangling = (type == G_FILE_TYPE_SYMBOLIC_LINK);
    item->is_dir   = (type == G_FILE_TYPE_DIRECTORY);

    if (item->is_dir) {
        char *child = g_build_filename(dir_path, item->name, NULL);
        guint n;

        item->icon_name = g_strdup("folder-symbolic");
        if (count_entries(child, app->show_hidden, &n))
            item->size_text = g_strdup_printf(T("%u items", "항목 %u"), n);
        else
            item->size_text = g_strdup("");
        g_free(child);
    } else if (item->dangling) {
        item->icon_name = g_strdup("dialog-question-symbolic");
        item->size_text = g_strdup("");
    } else {
        item->size      = g_file_info_get_size(info);
        item->icon_name = g_strdup("text-x-generic-symbolic");
        item->size_text = format_size(item->size);
    }

    /* A filesystem that carries no mtime, or an entry whose time could
     * not be read, would otherwise print 1970 with a straight face. */
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED))
        item->date_text = format_date(g_file_info_get_attribute_uint64(
                              info, G_FILE_ATTRIBUTE_TIME_MODIFIED));
    else
        item->date_text = g_strdup("");

    return item;
}

/* The names that were selected, so a reload does not lose the selection.
 * Keyed by the on-disk name: the row is a different object after the
 * reload, but it is the same file. */
static GHashTable *selected_names(App *app)
{
    GHashTable *names = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, NULL);
    GtkBitset  *bits  = gtk_selection_model_get_selection(app->selection);
    GtkBitsetIter iter;
    guint         pos;

    if (gtk_bitset_iter_init_first(&iter, bits, &pos)) {
        do {
            LpfItem *item = g_list_model_get_item(
                                G_LIST_MODEL(app->selection), pos);
            if (item) {
                g_hash_table_add(names, g_strdup(item->name));
                g_object_unref(item);
            }
        } while (gtk_bitset_iter_next(&iter, &pos));
    }

    gtk_bitset_unref(bits);
    return names;
}

static void restore_selection(App *app, GHashTable *names)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(app->selection));

    for (guint i = 0; i < n; i++) {
        LpfItem *item = g_list_model_get_item(G_LIST_MODEL(app->selection), i);

        if (item && g_hash_table_contains(names, item->name))
            gtk_selection_model_select_item(app->selection, i, FALSE);
        g_clear_object(&item);
    }
}

static gboolean refresh_now(gpointer data)
{
    App *app = data;

    app->refresh_id = 0;
    reload(app);
    return G_SOURCE_REMOVE;
}

static void schedule_refresh(App *app)
{
    if (app->refresh_id != 0)
        g_source_remove(app->refresh_id);
    app->refresh_id = g_timeout_add(REFRESH_DEBOUNCE_MS, refresh_now, app);
}

static void on_directory_changed(GFileMonitor *monitor, GFile *file,
                                 GFile *other, GFileMonitorEvent event,
                                 gpointer data)
{
    (void)monitor; (void)file; (void)other; (void)event;
    schedule_refresh(data);
}

/* Fills the list from `path`. Returns FALSE and sets `error` if the
 * directory could not be opened at all, in which case nothing on screen
 * has been touched yet and the caller can stay where it is. */
static gboolean fill_list(App *app, const char *path, GError **error)
{
    GFile           *dir = g_file_new_for_path(path);
    GFileEnumerator *e   = g_file_enumerate_children(dir, QUERY_ATTRS,
                                                     G_FILE_QUERY_INFO_NONE,
                                                     NULL, error);
    if (!e) {
        g_object_unref(dir);
        return FALSE;
    }

    GPtrArray *rows = g_ptr_array_new();
    gboolean   truncated = FALSE;

    for (;;) {
        GError    *one  = NULL;
        GFileInfo *info = g_file_enumerator_next_file(e, NULL, &one);

        if (!info) {
            /* A directory is not a snapshot. Between the enumerator
             * opening and the last entry arriving, names appear and
             * vanish, and one that vanished in that window comes back
             * here as an error instead of as a row. GIO does not promise
             * the enumeration can continue past it, so we keep what we
             * have and ask for another pass - the directory has just
             * proved it is changing, and one more read is cheaper than
             * showing half of it as if it were all of it. */
            if (one) {
                g_error_free(one);
                truncated = TRUE;
            }
            break;
        }

        const char *name = g_file_info_get_name(info);
        if (app->show_hidden || (name && name[0] != '.'))
            g_ptr_array_add(rows, item_from_info(app, path, info));

        g_object_unref(info);
    }

    g_object_unref(e);

    g_ptr_array_sort(rows, compare_items);

    g_list_store_remove_all(app->store);
    if (rows->len > 0)
        g_list_store_splice(app->store, 0, 0, rows->pdata, rows->len);

    /* The store took a reference of its own; ours goes back. */
    for (guint i = 0; i < rows->len; i++)
        g_object_unref(rows->pdata[i]);
    g_ptr_array_free(rows, TRUE);

    /* Watch this directory so someone else's copy, download or delete
     * shows up here without the user asking. */
    g_clear_object(&app->monitor);
    app->monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_NONE,
                                            NULL, NULL);
    if (app->monitor)
        g_signal_connect(app->monitor, "changed",
                         G_CALLBACK(on_directory_changed), app);

    g_object_unref(dir);

    if (truncated)
        schedule_refresh(app);
    return TRUE;
}

static void reload(App *app)
{
    if (!app->path)
        return;

    GHashTable *keep = selected_names(app);
    GError     *error = NULL;

    if (fill_list(app, app->path, &error)) {
        restore_selection(app, keep);
    } else {
        /* The directory we are standing in went away or closed to us
         * while we were in it. Emptying the list is the truth. */
        g_list_store_remove_all(app->store);
        g_clear_error(&error);
    }

    g_hash_table_unref(keep);
    update_status(app);
}

/* ------------------------------------------------------------------ */
/* The breadcrumb                                                      */
/* ------------------------------------------------------------------ */

static void on_crumb_clicked(GtkButton *button, gpointer data)
{
    App        *app  = data;
    const char *path = g_object_get_data(G_OBJECT(button), "path");

    if (g_strcmp0(path, app->path) != 0)
        navigate(app, path, TRUE);
}

static void crumb_append(App *app, const char *label, const char *path,
                         gboolean current)
{
    GtkWidget *button = gtk_button_new_with_label(label);

    gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
    gtk_widget_add_css_class(button, current ? "lp-crumb-current" : "lp-crumb");
    g_object_set_data_full(G_OBJECT(button), "path", g_strdup(path), g_free);
    g_signal_connect(button, "clicked", G_CALLBACK(on_crumb_clicked), app);
    gtk_box_append(GTK_BOX(app->crumbs), button);
}

static void crumb_chevron(App *app)
{
    GtkWidget *chevron = gtk_image_new_from_icon_name("go-next-symbolic");

    gtk_widget_add_css_class(chevron, "lp-crumb-sep");
    gtk_box_append(GTK_BOX(app->crumbs), chevron);
}

/* The trail is built from the path itself, so it is always the way we
 * actually got here and not a story about it. Below the home directory
 * the first crumb is 홈 rather than /home/이름 - the sidebar calls it
 * 홈, and two names for one place is one too many. */
static void build_crumbs(App *app)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(app->crumbs)) != NULL)
        gtk_box_remove(GTK_BOX(app->crumbs), child);

    const char *home = g_get_home_dir();
    const char *rest = app->path;
    char       *base = NULL;

    if (home && g_str_has_prefix(app->path, home) &&
        (app->path[strlen(home)] == '\0' || app->path[strlen(home)] == '/')) {
        base = g_strdup(home);
        rest = app->path + strlen(home);
        crumb_append(app, T("Home", "홈"), home, rest[0] == '\0');
    } else {
        base = g_strdup("/");
        crumb_append(app, "/", "/", app->path[1] == '\0');
        rest = app->path;
    }

    char **parts = g_strsplit(rest, "/", -1);

    for (int i = 0; parts[i] != NULL; i++) {
        if (parts[i][0] == '\0')
            continue;

        char *here = g_build_filename(base, parts[i], NULL);
        crumb_chevron(app);
        crumb_append(app, parts[i], here, g_strcmp0(here, app->path) == 0);
        g_free(base);
        base = here;
    }

    g_strfreev(parts);
    g_free(base);
}

/* ------------------------------------------------------------------ */
/* Where we are                                                        */
/* ------------------------------------------------------------------ */

static void update_places(App *app)
{
    for (guint i = 0; i < app->places->len; i++) {
        Place *place = g_ptr_array_index(app->places, i);

        if (g_strcmp0(place->path, app->path) == 0)
            gtk_widget_add_css_class(place->row, "selected");
        else
            gtk_widget_remove_css_class(place->row, "selected");
    }
}

static void update_capacity(App *app)
{
    double used, free_part;

    /* The bar under 시스템 is the root filesystem, whatever directory the
     * list happens to be showing. That is what makes it a drive and not
     * a property of the folder. */
    if (!disk_usage("/", &used, &free_part)) {
        gtk_widget_set_visible(app->capacity, FALSE);
        gtk_widget_set_visible(app->capacity_label, FALSE);
        return;
    }

    gtk_widget_set_visible(app->capacity, TRUE);
    gtk_widget_set_visible(app->capacity_label, TRUE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->capacity), used);

    if (free_part < LOW_SPACE_FRACTION)
        gtk_widget_add_css_class(app->capacity, "warning");
    else
        gtk_widget_remove_css_class(app->capacity, "warning");

    char *text = format_free("/");
    gtk_label_set_text(GTK_LABEL(app->capacity_label), text ? text : "");
    g_free(text);
}

static void update_status(App *app)
{
    guint      total    = g_list_model_get_n_items(G_LIST_MODEL(app->selection));
    GtkBitset *bits     = gtk_selection_model_get_selection(app->selection);
    guint      chosen   = 0;
    goffset    bytes    = 0;
    GtkBitsetIter iter;
    guint         pos;

    if (gtk_bitset_iter_init_first(&iter, bits, &pos)) {
        do {
            LpfItem *item = g_list_model_get_item(
                                G_LIST_MODEL(app->selection), pos);
            if (item) {
                chosen++;
                /* A folder contributes nothing. Adding up what is inside
                 * it would mean walking the tree, and the number would
                 * change while it was being read. */
                if (!item->is_dir)
                    bytes += item->size;
                g_object_unref(item);
            }
        } while (gtk_bitset_iter_next(&iter, &pos));
    }
    gtk_bitset_unref(bits);

    /* With nothing selected the size is everything in the folder, which
     * is the question you are asking when you have not picked anything
     * yet. Selecting narrows both numbers to what you picked. */
    if (chosen == 0) {
        for (guint i = 0; i < total; i++) {
            LpfItem *item = g_list_model_get_item(
                                G_LIST_MODEL(app->selection), i);
            if (item) {
                if (!item->is_dir)
                    bytes += item->size;
                g_object_unref(item);
            }
        }
    }

    char *size = format_size(bytes);
    char *left;

    if (chosen > 0)
        left = g_strdup_printf(T("%u items · %u selected · %s", "항목 %u개 · %u개 선택 · %s"),
                               total, chosen, size);
    else
        left = g_strdup_printf(T("%u items · %s", "항목 %u개 · %s"), total, size);

    gtk_label_set_text(GTK_LABEL(app->status_left), left);
    g_free(left);
    g_free(size);

    char *right = app->path ? format_free(app->path) : NULL;
    gtk_label_set_text(GTK_LABEL(app->status_right), right ? right : "");
    g_free(right);
}

static void update_history_buttons(App *app)
{
    gtk_widget_set_sensitive(app->back, app->pos > 0);
    gtk_widget_set_sensitive(app->forward,
                             app->pos + 1 < app->history->len);
}

/* Moves to `path`. `push` is FALSE when back and forward are walking the
 * history, which is the whole difference between the two: they move the
 * cursor, everything else cuts the future off and writes a new entry. */
static void navigate(App *app, const char *path, gboolean push)
{
    char   *full  = g_canonicalize_filename(path, NULL);
    GError *error = NULL;

    /* Load before committing. If the directory will not open we have not
     * yet changed the title, the crumbs or the history, so refusing is
     * simply staying where we were. */
    char *previous = app->path;
    app->path = full;

    if (!fill_list(app, full, &error)) {
        char *name = g_path_get_basename(full);
        const char *why =
            g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)
                ? T("No permission to read it.", "읽기 권한이 없습니다.")
                : T("The folder could not be read.", "폴더를 읽지 못했습니다.");

        say(app, T("cannot be opened", "열 수 없습니다"), name, why);

        g_free(name);
        g_clear_error(&error);
        app->path = previous;
        g_free(full);
        return;
    }

    g_free(previous);

    if (push) {
        const char *here = app->history->len > 0
                               ? g_ptr_array_index(app->history, app->pos)
                               : NULL;

        /* Arriving where we already are changes nothing, and it must not
         * look like it does: clicking the sidebar row for the folder you
         * are standing in would otherwise throw the forward entries away
         * and grey out a chevron that was working a moment ago. */
        if (g_strcmp0(here, full) != 0) {
            /* Somewhere new from the middle of the history cuts the
             * future off, the way a browser does. */
            while (app->history->len > app->pos + 1)
                g_ptr_array_remove_index(app->history, app->history->len - 1);

            g_ptr_array_add(app->history, g_strdup(full));
            app->pos = app->history->len - 1;
        }
    }

    char *name = g_path_get_basename(full);
    gtk_label_set_text(GTK_LABEL(app->title), name);
    gtk_window_set_title(app->window, name);
    g_free(name);

    build_crumbs(app);
    update_places(app);
    update_capacity(app);
    update_history_buttons(app);
    update_status(app);
}

/* ------------------------------------------------------------------ */
/* Opening what is under the cursor                                    */
/* ------------------------------------------------------------------ */

/* Which file a launch was for. Two double-clicks in a row have two of
 * these in flight at once, so the name cannot live anywhere shared or
 * the second answer names the first file. */
typedef struct {
    App  *app;
    char *name;
} Launch;

static void on_launched(GObject *source, GAsyncResult *result, gpointer data)
{
    Launch *launch = data;
    GError *error  = NULL;

    (void)source;

    /* Without this the failure is a line in the journal and nothing on
     * screen: you double-click, and the machine does nothing at all. */
    if (!gtk_show_uri_full_finish(launch->app->window, result, &error)) {
        say(launch->app, T("cannot be opened", "열 수 없습니다"), launch->name,
            T("Nothing installed can open this file.", "이 파일을 열 수 있는 프로그램이 없습니다."));
        g_clear_error(&error);
    }

    g_free(launch->name);
    g_free(launch);
}

static void open_item(App *app, LpfItem *item)
{
    char *path = g_build_filename(app->path, item->name, NULL);

    if (item->dangling) {
        say(app, T("cannot be opened", "열 수 없습니다"), item->display,
            T("The link points at nothing.", "링크가 가리키는 위치가 없습니다."));
    } else if (!item->readable) {
        /* Shown, not hidden. A file you may not read is still a fact
         * about the folder, and hiding it turns "no permission" into
         * "not there" - the second is a lie and it is the harder one to
         * debug. */
        say(app, T("cannot be opened", "열 수 없습니다"), item->display, T("No permission to read it.", "읽기 권한이 없습니다."));
    } else if (item->is_dir) {
        navigate(app, path, TRUE);
    } else {
        GFile  *file   = g_file_new_for_path(path);
        char   *uri    = g_file_get_uri(file);
        Launch *launch = g_new0(Launch, 1);

        launch->app  = app;
        launch->name = g_strdup(item->display);

        gtk_show_uri_full(app->window, uri, GDK_CURRENT_TIME, NULL,
                          on_launched, launch);
        g_free(uri);
        g_object_unref(file);
    }

    g_free(path);
}

static void on_row_activated(GtkWidget *view, guint position, gpointer data)
{
    App     *app  = data;
    LpfItem *item = g_list_model_get_item(G_LIST_MODEL(app->selection),
                                          position);
    (void)view;

    if (item) {
        open_item(app, item);
        g_object_unref(item);
    }
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */

static void act_back(GSimpleAction *a, GVariant *p, gpointer data)
{
    App *app = data;
    (void)a; (void)p;

    if (app->pos > 0) {
        app->pos--;
        navigate(app, g_ptr_array_index(app->history, app->pos), FALSE);
    }
}

static void act_forward(GSimpleAction *a, GVariant *p, gpointer data)
{
    App *app = data;
    (void)a; (void)p;

    if (app->pos + 1 < app->history->len) {
        app->pos++;
        navigate(app, g_ptr_array_index(app->history, app->pos), FALSE);
    }
}

static void act_up(GSimpleAction *a, GVariant *p, gpointer data)
{
    App *app = data;
    (void)a; (void)p;

    if (g_strcmp0(app->path, "/") == 0)
        return;

    char *parent = g_path_get_dirname(app->path);
    navigate(app, parent, TRUE);
    g_free(parent);
}

static void act_home(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    navigate(data, g_get_home_dir(), TRUE);
}

static void act_refresh(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    reload(data);
}

static void act_show_hidden(GSimpleAction *action, GVariant *p, gpointer data)
{
    App     *app = data;
    GVariant *state = g_action_get_state(G_ACTION(action));
    gboolean  on = !g_variant_get_boolean(state);

    (void)p;
    g_variant_unref(state);

    g_simple_action_set_state(action, g_variant_new_boolean(on));
    app->show_hidden = on;
    reload(app);
}

/* ------------------------------------------------------------------ */
/* Search                                                              */
/* ------------------------------------------------------------------ */

static gboolean filter_match(gpointer object, gpointer data)
{
    LpfItem    *item  = object;
    App        *app   = data;
    const char *query = gtk_editable_get_text(GTK_EDITABLE(app->search_entry));

    if (!query || query[0] == '\0')
        return TRUE;

    /* Casefold both sides so English matches regardless of case. Korean
     * has no case, so for 다운로드 this is a plain substring test. */
    char    *haystack = g_utf8_casefold(item->display, -1);
    char    *needle   = g_utf8_casefold(query, -1);
    gboolean hit      = (strstr(haystack, needle) != NULL);

    g_free(haystack);
    g_free(needle);
    return hit;
}

static void on_search_changed(GtkEditable *entry, gpointer data)
{
    App *app = data;
    (void)entry;

    gtk_filter_changed(GTK_FILTER(app->filter), GTK_FILTER_CHANGE_DIFFERENT);
    update_status(app);
}

/* ------------------------------------------------------------------ */
/* Cells                                                               */
/* ------------------------------------------------------------------ */

static void name_setup(GtkSignalListItemFactory *f, GtkListItem *cell,
                       gpointer data)
{
    GtkWidget *box   = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *icon  = gtk_image_new();
    GtkWidget *label = gtk_label_new(NULL);

    (void)f; (void)data;

    /* Spacing is left at 0 on purpose: GtkBox spacing is a property and
     * the theme cannot reach it, so the 9px gap is a margin in the
     * stylesheet instead, where the rest of the numbers are. */
    gtk_widget_add_css_class(box, "lp-cell");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    /* Middle, not end: two files often differ only in their extension or
     * their last few characters, and cutting the tail off hides exactly
     * the part that tells them apart. */
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(label, TRUE);

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_list_item_set_child(cell, box);
}

static void name_bind(GtkSignalListItemFactory *f, GtkListItem *cell,
                      gpointer data)
{
    LpfItem   *item  = gtk_list_item_get_item(cell);
    GtkWidget *box   = gtk_list_item_get_child(cell);
    GtkWidget *icon  = gtk_widget_get_first_child(box);
    GtkWidget *label = gtk_widget_get_last_child(box);

    (void)f; (void)data;

    gtk_image_set_from_icon_name(GTK_IMAGE(icon), item->icon_name);
    gtk_label_set_text(GTK_LABEL(label), item->display);

    /* Row widgets are recycled as the list scrolls, so a class left by
     * whichever row this widget showed last is still on it. Take both
     * off before putting one on. */
    gtk_widget_remove_css_class(icon, "lp-icon-folder");
    gtk_widget_remove_css_class(icon, "lp-icon-file");
    gtk_widget_remove_css_class(label, "lp-name");
    gtk_widget_remove_css_class(label, "lp-name-file");

    gtk_widget_add_css_class(icon, item->is_dir ? "lp-icon-folder"
                                                : "lp-icon-file");
    gtk_widget_add_css_class(label, item->is_dir ? "lp-name" : "lp-name-file");
}

static void text_setup(GtkSignalListItemFactory *f, GtkListItem *cell,
                       gpointer data)
{
    GtkWidget *label = gtk_label_new(NULL);

    (void)f;

    gtk_label_set_xalign(GTK_LABEL(label), 1.0);
    gtk_widget_add_css_class(label, (const char *)data);
    gtk_list_item_set_child(cell, label);
}

static void size_bind(GtkSignalListItemFactory *f, GtkListItem *cell,
                      gpointer data)
{
    (void)f; (void)data;
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(cell)),
                       LPF_ITEM(gtk_list_item_get_item(cell))->size_text);
}

static void date_bind(GtkSignalListItemFactory *f, GtkListItem *cell,
                      gpointer data)
{
    (void)f; (void)data;
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(cell)),
                       LPF_ITEM(gtk_list_item_get_item(cell))->date_text);
}

static void tile_setup(GtkSignalListItemFactory *f, GtkListItem *cell,
                       gpointer data)
{
    GtkWidget *box   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *icon  = gtk_image_new();
    GtkWidget *label = gtk_label_new(NULL);

    (void)f; (void)data;

    gtk_widget_add_css_class(box, "lp-tile");
    gtk_widget_add_css_class(icon, "lp-tile-icon");
    gtk_widget_add_css_class(label, "lp-tile-name");
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_list_item_set_child(cell, box);
}

static void tile_bind(GtkSignalListItemFactory *f, GtkListItem *cell,
                      gpointer data)
{
    LpfItem   *item  = gtk_list_item_get_item(cell);
    GtkWidget *box   = gtk_list_item_get_child(cell);
    GtkWidget *icon  = gtk_widget_get_first_child(box);
    GtkWidget *label = gtk_widget_get_last_child(box);

    (void)f; (void)data;

    gtk_image_set_from_icon_name(GTK_IMAGE(icon), item->icon_name);
    gtk_label_set_text(GTK_LABEL(label), item->display);
}

/* ------------------------------------------------------------------ */
/* The sidebar                                                         */
/* ------------------------------------------------------------------ */

static void on_place_clicked(GtkButton *button, gpointer data)
{
    navigate(data, g_object_get_data(G_OBJECT(button), "path"), TRUE);
}

static GtkWidget *sidebar_heading(const char *text)
{
    GtkWidget *label = gtk_label_new(text);

    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_add_css_class(label, "lp-group");
    return label;
}

/* A row that leads somewhere. `icon_name` may be NULL, in which case the
 * row gets the tag dot instead - same row, different mark. */
static GtkWidget *sidebar_row(App *app, const char *label_text,
                              const char *icon_name, const char *path)
{
    GtkWidget *button = gtk_button_new();
    GtkWidget *box    = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *label  = gtk_label_new(label_text);
    GtkWidget *mark;

    if (icon_name) {
        mark = gtk_image_new_from_icon_name(icon_name);
        gtk_widget_add_css_class(mark, "lp-place-icon");
    } else {
        /* An empty box is the dot: the theme gives it 7px of min width
         * and height, a radius and a colour. Nothing to draw from here. */
        mark = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(mark, "lp-tag-dot");
        gtk_widget_set_valign(mark, GTK_ALIGN_CENTER);
    }

    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_add_css_class(label, "lp-place-label");

    gtk_box_append(GTK_BOX(box), mark);
    gtk_box_append(GTK_BOX(box), label);

    gtk_button_set_child(GTK_BUTTON(button), box);
    gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
    gtk_widget_add_css_class(button, "lp-place");
    g_object_set_data_full(G_OBJECT(button), "path", g_strdup(path), g_free);
    g_signal_connect(button, "clicked", G_CALLBACK(on_place_clicked), app);

    Place *place = g_new0(Place, 1);
    place->row  = button;
    place->path = g_strdup(path);
    g_ptr_array_add(app->places, place);

    return button;
}

/* The place the sidebar names has to exist, or the row is a promise the
 * machine does not keep. XDG's own directory is used when the user has
 * one; otherwise the folder is created under the Korean name the sidebar
 * shows, so the label and the path say the same thing. */
/* Where one of the standard folders actually is.
 *
 * The name on disk and the name on the screen are two different things
 * and this used to conflate them: the label was passed in and used as
 * the directory name, so the sidebar's "다운로드" was also the folder's
 * name. That worked while the machine spoke one language. It stops
 * working the moment it speaks two - the English session would make and
 * use ~/Downloads while the Korean one made and used ~/다운로드, and the
 * same person's files would be in whichever folder matched the language
 * they were in when they saved.
 *
 * So the folder is named in English on disk, always, and what the
 * sidebar prints is the caller's business. `fallback` is that on-disk
 * name and is deliberately not translated.
 *
 * g_get_user_special_dir reads ~/.config/user-dirs.dirs, which the image
 * writes, so this normally answers from there and the fallback is only
 * for a home directory nobody set up. */
static char *place_dir(GUserDirectory which, const char *fallback)
{
    const char *xdg = g_get_user_special_dir(which);

    if (xdg && g_file_test(xdg, G_FILE_TEST_IS_DIR))
        return g_strdup(xdg);

    char *path = g_build_filename(g_get_home_dir(), fallback, NULL);
    g_mkdir_with_parents(path, 0755);
    return path;
}

/* The freedesktop trash, so that anything else on this system which
 * throws a file away puts it where this row looks for it. */
static char *trash_dir(void)
{
    char *path = g_build_filename(g_get_user_data_dir(), "Trash", "files", NULL);

    g_mkdir_with_parents(path, 0755);
    return path;
}

/* Drives the system mounted, not drives it might have. README's
 * automount puts USB storage under /media, so that is where we look; an
 * empty /media means no USB row, which is the honest answer. */
static void add_usb_rows(App *app, GtkWidget *box)
{
    GDir *media = g_dir_open("/media", 0, NULL);
    if (!media)
        return;

    const char *name;
    while ((name = g_dir_read_name(media)) != NULL) {
        char *path = g_build_filename("/media", name, NULL);

        if (g_file_test(path, G_FILE_TEST_IS_DIR))
            gtk_box_append(GTK_BOX(box),
                           sidebar_row(app, name,
                                       "drive-removable-media-symbolic", path));
        g_free(path);
    }

    g_dir_close(media);
}

/* Tags come from ~/.config/lp-files/tags, one per line, name and path
 * separated by a tab. There is no tag database on this system to read,
 * and a group of dots that lead nowhere would break the same rule the
 * places follow - so with no file, there is no 태그 group. */
static void add_tag_rows(App *app, GtkWidget *box)
{
    char     *path = g_build_filename(g_get_user_config_dir(),
                                      "lp-files", "tags", NULL);
    char     *text = NULL;
    gboolean  read = g_file_get_contents(path, &text, NULL, NULL);

    g_free(path);
    if (!read)
        return;

    char     **lines  = g_strsplit(text, "\n", -1);
    GtkWidget *heading = NULL;

    for (int i = 0; lines[i] != NULL; i++) {
        char **field = g_strsplit(lines[i], "\t", 2);

        if (field[0] && field[0][0] != '\0' && field[1] &&
            g_file_test(field[1], G_FILE_TEST_IS_DIR)) {
            if (!heading) {
                heading = sidebar_heading(T("Tags", "태그"));
                gtk_box_append(GTK_BOX(box), heading);
            }
            gtk_box_append(GTK_BOX(box),
                           sidebar_row(app, field[0], NULL, field[1]));
        }
        g_strfreev(field);
    }

    g_strfreev(lines);
    g_free(text);
}

static GtkWidget *build_sidebar(App *app)
{
    GtkWidget *scroller = gtk_scrolled_window_new();
    GtkWidget *box      = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Both names. The theme styles .sidebar, which is what GTK's own
     * widgets carry and therefore what a theme is written against;
     * lp-sidebar stays so that a rule can reach this sidebar and not
     * every other one. */
    gtk_widget_add_css_class(box, "sidebar");
    gtk_widget_add_css_class(box, "lp-sidebar");

    /* Width, and it takes all three.
     *
     * set_size_request alone sets a minimum, and a child of a horizontal
     * box with nothing stopping it takes whatever is left over - which
     * is how a 96px sidebar comes out four hundred wide. The hexpand
     * says do not grow; size_request says do not shrink; and halign
     * FILL keeps it filling the width it was given rather than centring
     * inside it. */
    gtk_widget_set_size_request(box, SIDEBAR_WIDTH, -1);
    gtk_widget_set_hexpand(box, FALSE);
    gtk_widget_set_halign(box, GTK_ALIGN_FILL);

    gtk_box_append(GTK_BOX(box), sidebar_heading(T("Places", "위치")));

    char *documents = place_dir(G_USER_DIRECTORY_DOCUMENTS, "Documents");
    /* 다운로드, never 내려받기 - see the note at the top of the Makefile. */
    char *downloads = place_dir(G_USER_DIRECTORY_DOWNLOAD, "Downloads");
    char *pictures  = place_dir(G_USER_DIRECTORY_PICTURES, "Pictures");
    char *trash     = trash_dir();

    gtk_box_append(GTK_BOX(box), sidebar_row(app, T("Home", "홈"), "user-home-symbolic",
                                             g_get_home_dir()));
    gtk_box_append(GTK_BOX(box), sidebar_row(app, T("Documents", "문서"),
                                             "folder-documents-symbolic",
                                             documents));
    gtk_box_append(GTK_BOX(box), sidebar_row(app, T("Downloads", "다운로드"),
                                             "folder-download-symbolic",
                                             downloads));
    gtk_box_append(GTK_BOX(box), sidebar_row(app, T("Pictures", "사진"),
                                             "folder-pictures-symbolic",
                                             pictures));
    gtk_box_append(GTK_BOX(box), sidebar_row(app, T("Trash", "휴지통"),
                                             "user-trash-symbolic", trash));

    g_free(documents);
    g_free(downloads);
    g_free(pictures);
    g_free(trash);

    gtk_box_append(GTK_BOX(box), sidebar_heading(T("Drives", "드라이브")));
    gtk_box_append(GTK_BOX(box), sidebar_row(app, T("System", "시스템"),
                                             "drive-harddisk-symbolic", "/"));

    app->capacity = gtk_progress_bar_new();
    gtk_widget_add_css_class(app->capacity, "lp-capacity");
    gtk_box_append(GTK_BOX(box), app->capacity);

    app->capacity_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(app->capacity_label), 0.0);
    gtk_widget_add_css_class(app->capacity_label, "lp-capacity-label");
    gtk_box_append(GTK_BOX(box), app->capacity_label);

    add_usb_rows(app, box);
    add_tag_rows(app, box);

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), box);
    gtk_widget_add_css_class(scroller, "lp-sidebar-scroll");
    return scroller;
}

/* ------------------------------------------------------------------ */
/* The header bar                                                      */
/* ------------------------------------------------------------------ */

static GtkWidget *icon_button(const char *icon_name, const char *action)
{
    GtkWidget *button = gtk_button_new_from_icon_name(icon_name);

    gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
    gtk_widget_add_css_class(button, "lp-icon");
    if (action)
        gtk_actionable_set_action_name(GTK_ACTIONABLE(button), action);
    return button;
}

static void on_view_toggled(GtkToggleButton *button, gpointer data)
{
    App *app = data;

    gtk_stack_set_visible_child_name(GTK_STACK(app->stack),
                                     gtk_toggle_button_get_active(button)
                                         ? "list" : "grid");
}

static GtkWidget *build_header(App *app)
{
    GtkWidget *header = gtk_header_bar_new();

    /* The window's own controls are switched off and a close button is
     * packed by hand. The mockup puts the 1px separator immediately
     * before close with the header's own 8px gap around it, and
     * GtkHeaderBar's control group has its own spacing that no
     * stylesheet can reach. One button, drawn where the mockup draws it,
     * is worth losing the automatic one for. */
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), FALSE);
    gtk_widget_add_css_class(header, "lp-header");

    GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    app->back    = icon_button("go-previous-symbolic", "win.back");
    app->forward = icon_button("go-next-symbolic", "win.forward");
    gtk_widget_add_css_class(app->back, "lp-nav");
    gtk_widget_add_css_class(app->forward, "lp-nav");
    gtk_box_append(GTK_BOX(nav), app->back);
    gtk_box_append(GTK_BOX(nav), app->forward);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), nav);

    app->title = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(app->title), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(app->title), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(app->title, TRUE);
    gtk_widget_add_css_class(app->title, "lp-title");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), app->title);

    GtkWidget *tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    GtkWidget *search = gtk_toggle_button_new();
    gtk_button_set_child(GTK_BUTTON(search),
                         gtk_image_new_from_icon_name("system-search-symbolic"));
    gtk_button_set_has_frame(GTK_BUTTON(search), FALSE);
    gtk_widget_add_css_class(search, "lp-icon");
    gtk_box_append(GTK_BOX(tools), search);

    GtkWidget *list_view = gtk_toggle_button_new();
    gtk_button_set_child(GTK_BUTTON(list_view),
                         gtk_image_new_from_icon_name("view-list-symbolic"));
    gtk_button_set_has_frame(GTK_BUTTON(list_view), FALSE);
    gtk_widget_add_css_class(list_view, "lp-icon");
    gtk_widget_add_css_class(list_view, "lp-view");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(list_view), TRUE);
    gtk_box_append(GTK_BOX(tools), list_view);

    GtkWidget *grid_view = gtk_toggle_button_new();
    gtk_button_set_child(GTK_BUTTON(grid_view),
                         gtk_image_new_from_icon_name("view-grid-symbolic"));
    gtk_button_set_has_frame(GTK_BUTTON(grid_view), FALSE);
    gtk_widget_add_css_class(grid_view, "lp-icon");
    gtk_widget_add_css_class(grid_view, "lp-view");
    /* Grouping them makes the pair exclusive, so :checked is the whole
     * active/inactive difference and the theme needs no help from here. */
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(grid_view),
                                GTK_TOGGLE_BUTTON(list_view));
    gtk_box_append(GTK_BOX(tools), grid_view);
    g_signal_connect(list_view, "toggled", G_CALLBACK(on_view_toggled), app);

    GMenu *menu = g_menu_new();
    g_menu_append(menu, T("Hidden items", "숨김 항목"), "win.show-hidden");
    g_menu_append(menu, T("Refresh", "새로 고침"), "win.refresh");
    g_menu_append(menu, T("Parent folder", "상위 폴더"), "win.up");

    GtkWidget *menu_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button),
                                  "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button),
                                   G_MENU_MODEL(menu));
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(menu_button), FALSE);
    gtk_widget_add_css_class(menu_button, "lp-icon");
    gtk_box_append(GTK_BOX(tools), menu_button);
    g_object_unref(menu);

    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_add_css_class(separator, "lp-sep");
    gtk_box_append(GTK_BOX(tools), separator);

    GtkWidget *close = icon_button("window-close-symbolic", NULL);
    gtk_widget_add_css_class(close, "lp-close");
    g_signal_connect_swapped(close, "clicked",
                             G_CALLBACK(gtk_window_close), app->window);
    gtk_box_append(GTK_BOX(tools), close);

    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), tools);

    g_object_set_data(G_OBJECT(header), "search-button", search);
    return header;
}

/* ------------------------------------------------------------------ */
/* The list                                                            */
/* ------------------------------------------------------------------ */

static GtkColumnViewColumn *make_column(const char *title,
                                        GCallback setup, GCallback bind,
                                        gpointer setup_data, int width)
{
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();

    g_signal_connect(factory, "setup", setup, setup_data);
    g_signal_connect(factory, "bind", bind, NULL);

    GtkColumnViewColumn *column = gtk_column_view_column_new(title, factory);

    if (width > 0)
        gtk_column_view_column_set_fixed_width(column, width);
    else
        gtk_column_view_column_set_expand(column, TRUE);

    return column;
}

static GtkWidget *build_list(App *app)
{
    GtkWidget *scroller = gtk_scrolled_window_new();
    GtkWidget *view     = gtk_column_view_new(NULL);

    gtk_column_view_set_model(GTK_COLUMN_VIEW(view), app->selection);
    /* The mockup fixes the order of the three columns; dragging them
     * around would only put the numbers somewhere they were not drawn. */
    gtk_column_view_set_reorderable(GTK_COLUMN_VIEW(view), FALSE);
    gtk_widget_add_css_class(view, "lp-list");

    GtkColumnViewColumn *name = make_column(T("Name", "이름"),
                                            G_CALLBACK(name_setup),
                                            G_CALLBACK(name_bind), NULL, -1);
    GtkColumnViewColumn *size = make_column(T("Size", "크기"),
                                            G_CALLBACK(text_setup),
                                            G_CALLBACK(size_bind),
                                            (gpointer)"lp-size",
                                            SIZE_COL_WIDTH);
    GtkColumnViewColumn *date = make_column(T("Modified", "수정일"),
                                            G_CALLBACK(text_setup),
                                            G_CALLBACK(date_bind),
                                            (gpointer)"lp-date",
                                            DATE_COL_WIDTH);

    gtk_column_view_append_column(GTK_COLUMN_VIEW(view), name);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(view), size);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(view), date);
    g_object_unref(name);
    g_object_unref(size);
    g_object_unref(date);

    g_signal_connect(view, "activate", G_CALLBACK(on_row_activated), app);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view);
    return scroller;
}

static GtkWidget *build_grid(App *app)
{
    GtkWidget          *scroller = gtk_scrolled_window_new();
    GtkListItemFactory *factory  = gtk_signal_list_item_factory_new();

    g_signal_connect(factory, "setup", G_CALLBACK(tile_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(tile_bind), NULL);

    /* Same selection model as the list, so switching views keeps what
     * you had picked and the status bar does not flinch. */
    GtkWidget *view = gtk_grid_view_new(g_object_ref(app->selection), factory);

    gtk_widget_add_css_class(view, "lp-grid");
    g_signal_connect(view, "activate", G_CALLBACK(on_row_activated), app);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view);
    return scroller;
}

/* ------------------------------------------------------------------ */
/* Putting the window together                                         */
/* ------------------------------------------------------------------ */

static void on_window_destroy(GtkWidget *widget, gpointer data)
{
    App *app = data;
    (void)widget;

    /* The debounce timer holds a pointer to a window that is going away;
     * left running it fires into freed memory a third of a second later. */
    if (app->refresh_id != 0) {
        g_source_remove(app->refresh_id);
        app->refresh_id = 0;
    }
    g_clear_object(&app->monitor);

    /* The selection model outlives this signal by however long the views
     * take to let go of it, and it can still emit while they do. */
    g_signal_handlers_disconnect_by_data(app->selection, app);
}

static void free_place(gpointer data)
{
    Place *place = data;

    g_free(place->path);
    g_free(place);
}

/* Hung off the window as data rather than off its destroy signal, so it
 * runs when the window is finally gone - by then the views have let go
 * of the selection model, and unreffing it here takes the whole model
 * chain with it. */
static void free_app(gpointer data)
{
    App *app = data;

    g_ptr_array_free(app->history, TRUE);
    g_ptr_array_free(app->places, TRUE);
    g_clear_object(&app->selection);
    g_free(app->path);
    g_free(app);
}

static const GActionEntry ACTIONS[] = {
    { "back",    act_back,    NULL, NULL,    NULL, { 0 } },
    { "forward", act_forward, NULL, NULL,    NULL, { 0 } },
    { "up",      act_up,      NULL, NULL,    NULL, { 0 } },
    { "home",    act_home,    NULL, NULL,    NULL, { 0 } },
    { "refresh", act_refresh, NULL, NULL,    NULL, { 0 } },
    { "show-hidden", act_show_hidden, NULL, "false", NULL, { 0 } },
};

static void open_window(GtkApplication *application, const char *start)
{
    App *app = g_new0(App, 1);

    app->history = g_ptr_array_new_with_free_func(g_free);
    app->places  = g_ptr_array_new_with_free_func(free_place);

    app->window = GTK_WINDOW(gtk_application_window_new(application));
    gtk_window_set_default_size(app->window, WINDOW_WIDTH, WINDOW_HEIGHT);
    gtk_widget_add_css_class(GTK_WIDGET(app->window), "lp-window");

    g_action_map_add_action_entries(G_ACTION_MAP(app->window), ACTIONS,
                                    G_N_ELEMENTS(ACTIONS), app);

    /* store -> filter -> selection -> the two views. Each link takes the
     * reference of the one below it, so the whole chain is owned by the
     * one reference to the selection model that App keeps; `store` and
     * `filter` are borrowed pointers into it. */
    app->store  = g_list_store_new(LPF_TYPE_ITEM);
    app->filter = gtk_custom_filter_new(filter_match, app, NULL);

    GtkFilterListModel *filtered =
        gtk_filter_list_model_new(G_LIST_MODEL(app->store),
                                  GTK_FILTER(app->filter));

    app->selection = GTK_SELECTION_MODEL(
        gtk_multi_selection_new(G_LIST_MODEL(filtered)));

    g_signal_connect_swapped(app->selection, "selection-changed",
                             G_CALLBACK(update_status), app);
    g_signal_connect_swapped(app->selection, "items-changed",
                             G_CALLBACK(update_status), app);

    gtk_window_set_titlebar(app->window, build_header(app));

    GtkWidget *root  = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *split = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *main_column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    gtk_widget_set_vexpand(split, TRUE);
    gtk_widget_set_hexpand(main_column, TRUE);

    /* A path deep enough to overflow the window scrolls sideways rather
     * than squeezing the crumbs into nothing; the last crumb, which is
     * where you are, is the one that must stay readable. */
    GtkWidget *crumb_scroll = gtk_scrolled_window_new();
    app->crumbs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(app->crumbs, "lp-crumbs");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(crumb_scroll),
                                   GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(crumb_scroll),
                                  app->crumbs);
    gtk_widget_add_css_class(crumb_scroll, "lp-crumbbar");
    gtk_box_append(GTK_BOX(main_column), crumb_scroll);

    app->search_entry = gtk_search_entry_new();
    gtk_widget_set_hexpand(app->search_entry, TRUE);
    app->search_bar = gtk_search_bar_new();
    gtk_search_bar_set_child(GTK_SEARCH_BAR(app->search_bar),
                             app->search_entry);
    gtk_search_bar_connect_entry(GTK_SEARCH_BAR(app->search_bar),
                                 GTK_EDITABLE(app->search_entry));
    gtk_search_bar_set_key_capture_widget(GTK_SEARCH_BAR(app->search_bar),
                                          GTK_WIDGET(app->window));
    gtk_widget_add_css_class(app->search_bar, "lp-search");
    g_signal_connect(app->search_entry, "search-changed",
                     G_CALLBACK(on_search_changed), app);
    gtk_box_append(GTK_BOX(main_column), app->search_bar);

    GtkWidget *header = gtk_window_get_titlebar(app->window);
    g_object_bind_property(g_object_get_data(G_OBJECT(header),
                                             "search-button"),
                           "active", app->search_bar,
                           "search-mode-enabled",
                           G_BINDING_BIDIRECTIONAL);

    app->stack = gtk_stack_new();
    gtk_widget_set_vexpand(app->stack, TRUE);
    gtk_stack_add_named(GTK_STACK(app->stack), build_list(app), "list");
    gtk_stack_add_named(GTK_STACK(app->stack), build_grid(app), "grid");
    gtk_stack_set_visible_child_name(GTK_STACK(app->stack), "list");
    gtk_box_append(GTK_BOX(main_column), app->stack);

    gtk_box_append(GTK_BOX(split), build_sidebar(app));
    gtk_box_append(GTK_BOX(split), main_column);
    gtk_box_append(GTK_BOX(root), split);

    GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(status, "lp-status");
    app->status_left  = gtk_label_new(NULL);
    app->status_right = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(app->status_left), 0.0);
    gtk_label_set_xalign(GTK_LABEL(app->status_right), 1.0);
    gtk_widget_set_hexpand(app->status_left, TRUE);
    gtk_widget_add_css_class(app->status_left, "lp-status-left");
    gtk_widget_add_css_class(app->status_right, "lp-status-right");
    gtk_box_append(GTK_BOX(status), app->status_left);
    gtk_box_append(GTK_BOX(status), app->status_right);
    gtk_box_append(GTK_BOX(root), status);

    gtk_window_set_child(app->window, root);

    g_signal_connect(app->window, "destroy",
                     G_CALLBACK(on_window_destroy), app);
    g_object_set_data_full(G_OBJECT(app->window), "lp-app", app, free_app);

    navigate(app, start, TRUE);
    gtk_window_present(app->window);
}

static void on_activate(GtkApplication *application, gpointer data)
{
    (void)data;
    open_window(application, g_get_home_dir());
}

/* lp-files /어떤/폴더, and the directory a compositor hands us when it
 * opens inode/directory with this application. */
static void on_open(GApplication *application, GFile **files, int n,
                    const char *hint, gpointer data)
{
    (void)hint; (void)data;

    for (int i = 0; i < n; i++) {
        char *path = g_file_get_path(files[i]);

        if (!path)
            continue;

        /* Handed a file rather than a folder, open the folder it is in.
         * Refusing would be correct and useless. */
        if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
            char *parent = g_path_get_dirname(path);
            g_free(path);
            path = parent;
        }

        open_window(GTK_APPLICATION(application), path);
        g_free(path);
    }
}

int main(int argc, char **argv)
{
    GtkApplication *application =
        gtk_application_new(APP_ID, G_APPLICATION_HANDLES_OPEN);

    g_signal_connect(application, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(application, "open", G_CALLBACK(on_open), NULL);

    static const struct { const char *action; const char *key; } keys[] = {
        { "win.back",        "<Alt>Left"  },
        { "win.forward",     "<Alt>Right" },
        { "win.up",          "<Alt>Up"    },
        { "win.home",        "<Alt>Home"  },
        { "win.refresh",     "F5"         },
        { "win.show-hidden", "<Control>h" },
    };

    for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
        const char *accels[] = { keys[i].key, NULL };
        gtk_application_set_accels_for_action(application, keys[i].action,
                                              accels);
    }

    int status = g_application_run(G_APPLICATION(application), argc, argv);

    g_object_unref(application);
    return status;
}
