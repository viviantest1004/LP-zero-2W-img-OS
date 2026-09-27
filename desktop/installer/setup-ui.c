/*
 * setup-ui.c - the look and the language switch of the installer and the
 * first-boot setup (setup-ui.h says why they share it).
 *
 * The look is the desktop's: the mockup's dark window colours, the
 * orange accent of its toggles, motion.css for press feedback. What is
 * added is size. Both programs are used with a finger on a 15.6" panel
 * at scale 2, often before any keyboard works (the on-screen one comes
 * up with them, but a person installing an OS is not yet at home here),
 * so every target is at least 56 logical pixels tall and the type is a
 * step larger than in an application window.
 */
#include "setup-ui.h"
#include "lp-motion.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

gboolean su_korean;

/* motion.css as a string in .rodata: the same file every other program
 * on the desktop styles its transitions with, not a copy of it. The
 * path is relative to the directory the compiler runs in (desktop/
 * installer or desktop/firstboot - both are one level below theme/). */
__asm__(".pushsection .rodata\n"
        ".global su_motion_css\n"
        ".type su_motion_css, @object\n"
        "su_motion_css:\n"
        ".incbin \"../theme/motion.css\"\n"
        ".byte 0\n"
        ".popsection\n");
extern const char su_motion_css[];

static const char CSS[] =
    "@define-color su_bg      #0e0e0e;\n"
    "@define-color su_card    #232323;\n"
    "@define-color su_edge    #3a3a3a;\n"
    "@define-color su_text    #e8e8e8;\n"
    "@define-color su_dim     #9a9a9a;\n"
    "@define-color su_accent  #e95420;\n"
    "@define-color su_accent2 #f06a38;\n"
    "@define-color su_danger  #c01c28;\n"
    "window.su-window, window.su-window.background {\n"
    "  background-color: @su_bg; color: @su_text; border-radius: 0; }\n"
    ".su-card { background-color: @su_card; border-radius: 18px;\n"
    "  box-shadow: 0 0 0 0.5px @su_edge, 0 12px 32px rgba(0,0,0,0.6);\n"
    "  padding: 36px 44px 28px 44px; }\n"
    ".su-brand { font-size: 15px; font-weight: 700; letter-spacing: 0.08em;\n"
    "  color: @su_accent; }\n"
    ".su-title { font-size: 30px; font-weight: 700; }\n"
    ".su-sub { font-size: 17px; color: @su_dim; }\n"
    ".su-body { font-size: 17px; }\n"
    ".su-note { font-size: 15px; color: @su_dim; }\n"
    ".su-warn { font-size: 17px; color: #ffb4a6; }\n"
    ".su-caption { font-size: 15px; font-weight: 600; color: @su_dim; }\n"
    /* The one line that says what is still missing on a form page. Amber
     * while something is, green once nothing is: the colour change is
     * the confirmation, so it eases (the press spring's 130 ms). */
    ".su-hint { font-size: 15px; color: #f5c16c; min-height: 22px;\n"
    "  transition: color 130ms cubic-bezier(0.25, 0.8, 0.35, 1); }\n"
    ".su-hint.su-ok { color: #8fd18f; }\n"
    "button.su-choice, button.su-primary, button.su-secondary,\n"
    "button.su-danger { background-image: none; }\n"
    "button.su-choice { min-height: 64px; padding: 10px 18px;\n"
    "  border-radius: 12px; background-color: alpha(white, 0.05);\n"
    "  box-shadow: inset 0 0 0 1px alpha(white, 0.08); }\n"
    "button.su-choice:hover { background-color: alpha(white, 0.08); }\n"
    "button.su-choice:checked { background-color: alpha(@su_accent, 0.22);\n"
    "  box-shadow: inset 0 0 0 2px @su_accent; }\n"
    "button.su-choice:disabled { opacity: 0.45; }\n"
    /* Every label on a button says its colour. Left to inherit, a title
     * took Adwaita's light-theme button text (#2e3436) and vanished into
     * the dark card - "English" and "한국어" were unreadable. */
    "button.su-choice, button.su-secondary { color: @su_text; }\n"
    ".su-choice-title { font-size: 19px; font-weight: 600; color: @su_text; }\n"
    ".su-choice-detail { font-size: 15px; color: @su_dim; }\n"
    "button.su-primary, button.su-secondary, button.su-danger {\n"
    "  min-height: 56px; min-width: 150px; padding: 0 28px;\n"
    "  border-radius: 12px; font-size: 17px; font-weight: 600; }\n"
    "button.su-primary { background-color: @su_accent; color: white; }\n"
    "button.su-primary:hover { background-color: @su_accent2; }\n"
    "button.su-danger { background-color: @su_danger; color: white; }\n"
    "button.su-primary:disabled, button.su-danger:disabled {\n"
    "  background-color: alpha(white, 0.08); color: alpha(white, 0.35); }\n"
    "button.su-secondary { background-color: alpha(white, 0.07); }\n"
    "entry.su-entry, passwordentry.su-entry { min-height: 52px; font-size: 18px;\n"
    "  border-radius: 10px; padding: 0 14px; }\n"
    /* The focus ring in the accent, not Adwaita's blue. */
    "entry.su-entry:focus-within, passwordentry.su-entry:focus-within {\n"
    "  outline: 2px solid alpha(@su_accent, 0.85); outline-offset: -2px; }\n"
    "checkbutton.su-check { font-size: 17px; padding: 8px 0; }\n"
    "checkbutton.su-check check { min-width: 26px; min-height: 26px; }\n"
    "progressbar.su-progress trough { min-height: 12px; border-radius: 6px;\n"
    "  background-color: alpha(white, 0.08); }\n"
    "progressbar.su-progress progress { min-height: 12px; border-radius: 6px;\n"
    "  background-color: @su_accent; }\n"
    "dropdown.su-drop > button { min-height: 52px; font-size: 17px; }\n"
    "scrolledwindow.su-list { background: transparent; }\n";

static void css_error(GtkCssProvider *p, GtkCssSection *s, const GError *e,
                      gpointer name)
{
    (void)p; (void)s;
    g_printerr("setup-ui: %s: %s\n", (const char *)name, e->message);
}

void su_load_css(void)
{
    GdkDisplay *dpy = gdk_display_get_default();
    GtkCssProvider *motion = gtk_css_provider_new();
    GtkCssProvider *ours = gtk_css_provider_new();
    g_signal_connect(motion, "parsing-error", G_CALLBACK(css_error), "motion.css");
    g_signal_connect(ours, "parsing-error", G_CALLBACK(css_error), "setup-ui.c");
    gtk_css_provider_load_from_data(motion, su_motion_css, -1);
    gtk_css_provider_load_from_data(ours, CSS, -1);
    /* Above GTK_STYLE_PROVIDER_PRIORITY_USER (800), where the desktop
     * theme's ~/.config/gtk-4.0/gtk.css sits: at equal priority its 36px
     * rows would win over the finger-sized ones here. */
    gtk_style_context_add_provider_for_display(dpy, GTK_STYLE_PROVIDER(motion), 815);
    gtk_style_context_add_provider_for_display(dpy, GTK_STYLE_PROVIDER(ours), 820);
}

/* ── two languages ─────────────────────────────────────────────────── */

static GPtrArray *g_texts;          /* every widget that follows the language */

static void apply_text(GtkWidget *w)
{
    const char *s = g_object_get_data(G_OBJECT(w), su_korean ? "su-ko" : "su-en");
    if (!s)
        return;
    if (GTK_IS_LABEL(w))
        gtk_label_set_text(GTK_LABEL(w), s);
    else if (GTK_IS_BUTTON(w))
        gtk_button_set_label(GTK_BUTTON(w), s);
    else if (GTK_IS_CHECK_BUTTON(w))
        gtk_check_button_set_label(GTK_CHECK_BUTTON(w), s);
}

static void forget(gpointer data, GObject *gone)
{
    (void)data;
    g_ptr_array_remove_fast(g_texts, gone);
}

static void remember(GtkWidget *w, const char *en, const char *ko)
{
    if (!g_texts)
        g_texts = g_ptr_array_new();
    if (!g_object_get_data(G_OBJECT(w), "su-en")) {
        g_ptr_array_add(g_texts, w);
        g_object_weak_ref(G_OBJECT(w), forget, NULL);
    }
    g_object_set_data_full(G_OBJECT(w), "su-en", g_strdup(en), g_free);
    g_object_set_data_full(G_OBJECT(w), "su-ko", g_strdup(ko ? ko : en), g_free);
    apply_text(w);
}

void su_retext(GtkWidget *w, const char *en, const char *ko)
{
    remember(w, en, ko);
}

void su_set_korean(gboolean ko)
{
    su_korean = ko;
    if (g_texts)
        for (guint i = 0; i < g_texts->len; i++)
            apply_text(g_ptr_array_index(g_texts, i));
}

GtkWidget *su_label(const char *en, const char *ko, const char *css)
{
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    if (css)
        gtk_widget_add_css_class(l, css);
    remember(l, en, ko);
    return l;
}

GtkWidget *su_button(const char *en, const char *ko, const char *css)
{
    GtkWidget *b = gtk_button_new();
    if (css)
        gtk_widget_add_css_class(b, css);
    remember(b, en, ko);
    return b;
}

/* ── pages ─────────────────────────────────────────────────────────── */

void su_page(SuPage *p, const char *en_title, const char *ko_title,
             const char *en_sub, const char *ko_sub)
{
    p->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *brand = gtk_label_new("LP");
    gtk_widget_add_css_class(brand, "su-brand");
    gtk_label_set_xalign(GTK_LABEL(brand), 0.0);
    gtk_box_append(GTK_BOX(p->root), brand);

    p->title = su_label(en_title, ko_title, "su-title");
    gtk_box_append(GTK_BOX(p->root), p->title);
    p->subtitle = su_label(en_sub ? en_sub : "", ko_sub ? ko_sub : "", "su-sub");
    gtk_widget_set_visible(p->subtitle, en_sub != NULL);
    gtk_box_append(GTK_BOX(p->root), p->subtitle);

    p->body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(p->body, "su-body");
    gtk_widget_set_margin_top(p->body, 18);
    gtk_widget_set_vexpand(p->body, TRUE);
    gtk_box_append(GTK_BOX(p->root), p->body);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(bar, 16);
    p->left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    p->right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_hexpand(p->left, TRUE);
    gtk_box_append(GTK_BOX(bar), p->left);
    gtk_box_append(GTK_BOX(bar), p->right);
    gtk_box_append(GTK_BOX(p->root), bar);
}

GtkWidget *su_choice(const char *en, const char *ko,
                     const char *en_detail, const char *ko_detail,
                     const char *icon)
{
    GtkWidget *b = gtk_toggle_button_new();
    gtk_widget_add_css_class(b, "su-choice");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    if (icon) {
        GtkWidget *img = gtk_image_new_from_icon_name(icon);
        gtk_image_set_pixel_size(GTK_IMAGE(img), 32);
        gtk_box_append(GTK_BOX(row), img);
    }
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(col, GTK_ALIGN_CENTER);
    /* Without this a wrapping label is given its minimum width inside a
     * button and "English (United States)" breaks after two words. */
    gtk_widget_set_hexpand(col, TRUE);
    gtk_box_append(GTK_BOX(col), su_label(en, ko, "su-choice-title"));
    if (en_detail)
        gtk_box_append(GTK_BOX(col), su_label(en_detail, ko_detail, "su-choice-detail"));
    gtk_box_append(GTK_BOX(row), col);
    gtk_button_set_child(GTK_BUTTON(b), row);
    return b;
}

GtkWidget *su_stack(void)
{
    GtkWidget *s = gtk_stack_new();
    gtk_stack_set_hhomogeneous(GTK_STACK(s), TRUE);
    gtk_stack_set_vhomogeneous(GTK_STACK(s), FALSE);
    gtk_stack_set_interpolate_size(GTK_STACK(s), TRUE);
    /* feel.md's page transition, 260 ms; a crossfade under 100 ms when
     * the person asked for less motion - the page must still visibly
     * change, it must not travel. */
    if (lp_motion_reduced()) {
        gtk_stack_set_transition_type(GTK_STACK(s), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
        gtk_stack_set_transition_duration(GTK_STACK(s), 90);
    } else {
        gtk_stack_set_transition_duration(GTK_STACK(s), 260);
    }
    return s;
}

void su_go(GtkWidget *stack, const char *name, gboolean forward)
{
    GtkStackTransitionType t = lp_motion_reduced()
        ? GTK_STACK_TRANSITION_TYPE_CROSSFADE
        : forward ? GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT
                  : GTK_STACK_TRANSITION_TYPE_SLIDE_RIGHT;
    gtk_stack_set_visible_child_full(GTK_STACK(stack), name, t);
}

/* ── the progress bar ──────────────────────────────────────────────── */

struct _SuProgress {
    GtkWidget *bar;
    LpSpring   s;
    LpMotion  *m;
};

static void progress_frame(GtkWidget *w, gpointer data)
{
    (void)w;
    SuProgress *p = data;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(p->bar),
                                  CLAMP(p->s.x, 0.0, 1.0));
}

SuProgress *su_progress_new(void)
{
    SuProgress *p = g_new0(SuProgress, 1);
    p->bar = gtk_progress_bar_new();
    gtk_widget_add_css_class(p->bar, "su-progress");
    /* The expand spring: critically damped, so the bar never runs past
     * the real number and comes back. */
    lp_spring_init(&p->s, LP_SPRING_EXPAND, 0.0);
    p->m = lp_motion_new(p->bar, progress_frame, p);
    lp_motion_add(p->m, &p->s);
    return p;
}

GtkWidget *su_progress_widget(SuProgress *p)
{
    return p->bar;
}

void su_progress_set(SuProgress *p, double fraction)
{
    lp_spring_set_target(&p->s, CLAMP(fraction, 0.0, 1.0));
    lp_motion_kick(p->m);
}

/* ── small system pieces ───────────────────────────────────────────── */

char *su_run(const char *const *argv, int *status)
{
    char *out = NULL;
    int st = -1;
    GError *err = NULL;
    if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH |
                      G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL,
                      &st, &err)) {
        g_clear_error(&err);
        if (status)
            *status = -1;
        return NULL;
    }
    if (status)
        *status = st;
    return out;
}

gboolean su_write_atomic(const char *path, const char *data, int mode,
                         GError **err)
{
    /* GLib's CONSISTENT|DURABLE is write + fsync + rename + fsync(dir),
     * the Persistence rule of COMMON.md exactly. */
    if (!g_file_set_contents_full(path, data, -1,
                                  G_FILE_SET_CONTENTS_CONSISTENT |
                                  G_FILE_SET_CONTENTS_DURABLE, mode, err))
        return FALSE;
    return TRUE;
}

/* ── the on-screen keyboard ───────────────────────────────────────
 *
 * The keyboard normally appears on its own: a field that takes focus
 * enables text-input-v3, the compositor activates lp-osk's input
 * method, and lp-osk slides up. In the setup kiosk that activation was
 * never seen - tested at 3840x2160 in QEMU, lp-osk status stayed
 * "im":"inactive" with a field focused by touch - and a person at the
 * installer on a touch-only laptop was left with nothing to type on.
 *
 * So the setup windows ask for it themselves: a tap on a field (a touch,
 * not a click - a laptop keyboard user does not want half the screen
 * covered) runs `lp-osk show`, and when focus has left every field for
 * 200ms, `lp-osk hide`. The short delay is what keeps it from dropping
 * and rising again as focus moves from one field to the next. lp-osk
 * types through its virtual keyboard when no input method is active, so
 * the keys reach the focused field either way. */
static guint osk_hide_id;

static void osk_run(const char *what)
{
    char *argv[] = { (char *)"lp-osk", (char *)what, NULL };
    g_spawn_async(NULL, argv, NULL,
                  G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                  G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL);
}

static gboolean osk_hide_later(gpointer data)
{
    (void)data;
    osk_hide_id = 0;
    osk_run("hide");
    return G_SOURCE_REMOVE;
}

static void osk_tapped(GtkGestureClick *g, int n, double x, double y, gpointer data)
{
    (void)g; (void)n; (void)x; (void)y; (void)data;
    if (osk_hide_id) {
        g_source_remove(osk_hide_id);
        osk_hide_id = 0;
    }
    osk_run("show");
}

static void osk_focus_in(GtkEventControllerFocus *c, gpointer data)
{
    (void)c; (void)data;
    if (osk_hide_id) {                 /* moved to the next field */
        g_source_remove(osk_hide_id);
        osk_hide_id = 0;
    }
}

static void osk_focus_out(GtkEventControllerFocus *c, gpointer data)
{
    (void)c; (void)data;
    if (!osk_hide_id)
        osk_hide_id = g_timeout_add(200, osk_hide_later, NULL);
}

void su_osk_attach(GtkWidget *field)
{
    GtkGesture *tap = gtk_gesture_click_new();
    gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(tap), TRUE);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(tap),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(tap, "pressed", G_CALLBACK(osk_tapped), NULL);
    gtk_widget_add_controller(field, GTK_EVENT_CONTROLLER(tap));

    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "enter", G_CALLBACK(osk_focus_in), NULL);
    g_signal_connect(focus, "leave", G_CALLBACK(osk_focus_out), NULL);
    gtk_widget_add_controller(field, focus);
}
