#define _DEFAULT_SOURCE 1

/*
 * quick.c - 퀵메뉴.
 *
 * shell-design-spec.md 3절. 상단바 오른쪽의 상태 아이콘을 누르면
 * 그 아래로 떨어지는 320px 짜리 패널.
 *
 * ── 왜 타일이 아니라 행인가 (3-2) ──
 *
 * 윈도우 11 과 우분투가 공통으로 쓰는 2열 토글 타일이 두 OS 를 닮아
 * 보이게 하는 가장 큰 요소다. 행은 가로가 좁아도 되고, 무엇보다
 * 값을 같이 보여 줄 수 있다 - `Wi-Fi   iptime_ring` 은 한 줄이지만
 * 타일에서는 그 문자열을 놓을 자리가 없다.
 *
 * ── 왜 아코디언인가 (3-3) ──
 *
 * 우분투와 윈도우는 화살표를 눌러 하위 화면으로 슬라이드한다. 여기서는
 * 행이 제자리에서 아래로 펼쳐진다. 뒤로 가기가 필요 없고, 펼친 것과
 * 그 위아래가 같이 보인다.
 *
 * 행의 왼쪽(이름)을 누르면 켜고 끄고, 오른쪽(값)을 누르면 펼친다.
 * 그래서 각 행은 버튼 하나가 아니라 둘이다.
 *
 * ── 여기 없는 것은 없다고 말한다 ──
 *
 * 이 패널은 설정 앱과 같은 규칙을 따른다. 블루투스 스택이 이미지에
 * 없으면 블루투스 행은 스위치가 아니라 "설치되어 있지 않습니다" 라고
 * 적는다. 백라이트가 없는 기계에서는 밝기 슬라이더를 아예 만들지
 * 않는다 - 끌어도 아무 일이 없는 슬라이더는 고장으로 읽힌다.
 *
 * ── 창을 어떻게 그 자리에 두는가 ──
 *
 * 원래는 wlr-layer-shell 로 화면 모서리에 붙이는 것이 맞고, 그것을
 * GTK 에서 하는 방법은 gtk4-layer-shell 인데 데비안 12 에 없다.
 * 그래서 보통 창으로 만들고, 뜬 다음 sway 에게 자리를 알려 준다.
 * 차이는 화면 밖으로 나가는 애니메이션이 없다는 것 정도다.
 */

#include <gtk/gtk.h>
#include "lp-i18n.h"
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

#define PANEL_WIDTH 320
#define EDGE_GAP      8      /* 3-1: 화면 우측 8px */
#define BAR_GAP       6      /* 3-1: 상단바 아래 6px */
#define BAR_HEIGHT   33      /* waybar 가 실제로 잡는 높이 */
#define APP_ID "org.lpzero.QuickMenu"

typedef struct Row Row;

typedef struct {
    GtkWidget *window;
    GtkWidget *rows;         /* 행들이 들어가는 세로 상자 */
    GtkWidget *open;         /* 지금 펼쳐진 revealer, 없으면 NULL */
    Row       *power;        /* 아래쪽 전원 아이콘이 펴는 그 행 */
} Quick;

/* ── 도구 ──────────────────────────────────────────────────────── */

static char *run_out(const char *const *argv)
{
    char *out = NULL;
    int   status = 0;
    if (!g_spawn_sync(NULL, (char **)argv, NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, &out, NULL, &status, NULL)) {
        g_free(out);
        return NULL;
    }
    if (!g_spawn_check_wait_status(status, NULL)) {
        g_free(out);
        return NULL;
    }
    if (out) g_strchomp(out);
    return out;
}

static void run_bg(const char *const *argv)
{
    g_spawn_async(NULL, (char **)argv, NULL,
                  G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                  G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL);
}

static char *slurp(const char *path)
{
    char *buf = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &buf, &len, NULL)) return NULL;
    while (len && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = 0;
    return buf;
}

/* ── 행 ────────────────────────────────────────────────────────── */

typedef void (*ToggleFn)(Quick *q);
typedef GtkWidget *(*ExpandFn)(Quick *q);

struct Row {
    Quick     *q;
    GtkWidget *revealer;
    GtkWidget *value;
    ExpandFn   build;
    ToggleFn   toggle;
    gboolean   built;
};

/* 펼침은 한 번에 하나만. 둘이 동시에 열리면 패널이 화면 절반이
 * 되고, 그때는 아코디언인 이유가 없어진다. */
static void on_expand(GtkButton *b, gpointer data)
{
    (void)b;
    Row   *row = data;
    Quick *q   = row->q;

    if (q->open && q->open != row->revealer)
        gtk_revealer_set_reveal_child(GTK_REVEALER(q->open), FALSE);

    gboolean now = !gtk_revealer_get_reveal_child(GTK_REVEALER(row->revealer));

    if (now && !row->built && row->build) {
        gtk_revealer_set_child(GTK_REVEALER(row->revealer), row->build(q));
        row->built = TRUE;
    }
    gtk_revealer_set_reveal_child(GTK_REVEALER(row->revealer), now);
    q->open = now ? row->revealer : NULL;
}

static void on_toggle(GtkButton *b, gpointer data)
{
    (void)b;
    Row *row = data;
    if (row->toggle) row->toggle(row->q);
}

/* 이름 | 값 한 줄, 그리고 그 아래 접힌 자리.
 *
 * detail 은 이름만으로 무엇인지 알 수 없는 항목에만 붙는다 (3-4 의
 * 마지막 문단). Wi-Fi 에는 붙이지 않고 '절전' 에는 붙인다. */
static Row *add_row(Quick *q, const char *name, const char *detail,
                    const char *value, ToggleFn toggle, ExpandFn build)
{
    Row *row = g_new0(Row, 1);
    row->q = q;
    row->toggle = toggle;
    row->build = build;

    GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(line, "lp-quick-row");

    /* 왼쪽: 이름. 누르면 켜고 끈다. */
    GtkWidget *left = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(left), FALSE);
    gtk_widget_set_hexpand(left, TRUE);

    GtkWidget *label_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *l = gtk_label_new(name);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(label_box), l);
    if (detail && *detail) {
        GtkWidget *d = gtk_label_new(detail);
        gtk_widget_set_halign(d, GTK_ALIGN_START);
        gtk_widget_add_css_class(d, "dim-label");
        gtk_widget_add_css_class(d, "lp-detail");
        gtk_box_append(GTK_BOX(label_box), d);
    }
    gtk_button_set_child(GTK_BUTTON(left), label_box);
    if (toggle)
        g_signal_connect(left, "clicked", G_CALLBACK(on_toggle), row);
    gtk_box_append(GTK_BOX(line), left);

    /* 오른쪽: 지금 값. 누르면 제자리에서 펼친다. */
    GtkWidget *right = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(right), FALSE);
    row->value = gtk_label_new(value ? value : "");
    gtk_widget_add_css_class(row->value, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(row->value), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(row->value), 18);
    gtk_button_set_child(GTK_BUTTON(right), row->value);
    if (build)
        g_signal_connect(right, "clicked", G_CALLBACK(on_expand), row);
    gtk_box_append(GTK_BOX(line), right);

    gtk_box_append(GTK_BOX(q->rows), line);

    row->revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(row->revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    /* 1-5: 대부분 150~250ms. 이건 작은 동작이라 아래쪽이다. */
    gtk_revealer_set_transition_duration(GTK_REVEALER(row->revealer), 180);
    gtk_box_append(GTK_BOX(q->rows), row->revealer);

    return row;
}

static GtkWidget *note(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_widget_add_css_class(l, "lp-detail");
    gtk_widget_set_margin_start(l, 14);
    gtk_widget_set_margin_end(l, 14);
    gtk_widget_set_margin_bottom(l, 8);
    return l;
}

static GtkWidget *link_row(const char *text, GCallback cb, gpointer data)
{
    GtkWidget *b = gtk_button_new_with_label(text);
    gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
    gtk_widget_set_halign(b, GTK_ALIGN_START);
    gtk_widget_add_css_class(b, "lp-quick-link");
    g_signal_connect(b, "clicked", cb, data);
    return b;
}

/* ── 볼륨과 밝기 ───────────────────────────────────────────────── */

static void on_volume(GtkRange *r, gpointer d)
{
    (void)d;
    char v[32];
    g_snprintf(v, sizeof v, "%.2f", gtk_range_get_value(r) / 100.0);
    const char *a[] = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", v, NULL };
    char *o = run_out(a); g_free(o);
}

static void on_brightness(GtkRange *r, gpointer d)
{
    (void)d;
    char v[32];
    g_snprintf(v, sizeof v, "%d%%", (int)gtk_range_get_value(r));
    const char *a[] = { "brightnessctl", "-q", "set", v, NULL };
    char *o = run_out(a); g_free(o);
}

/* 슬라이더 한 줄.
 *
 * value 가 음수면 이 기계에 그 손잡이가 없다는 뜻이고, 그때도 줄은
 * 만든다 - 없애 버리면 패널이 그때그때 다른 모양이 되고, 소리 조절이
 * 어디 갔는지 찾게 된다. 대신 잠그고 왜 못 쓰는지 옆에 적는다.
 * 끌어도 아무 일이 없는 손잡이가 제일 나쁘고, 그 다음이 사라진
 * 손잡이이며, 이유가 적힌 잠긴 손잡이가 낫다. */
static void update_readout(GtkRange *r, gpointer label)
{
    char text[16];
    g_snprintf(text, sizeof text, "%d%%", (int)gtk_range_get_value(r));
    gtk_label_set_text(GTK_LABEL(label), text);
}

static GtkWidget *slider(const char *icon, int value, GCallback cb,
                         const char *why)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(box, "lp-quick-slider");

    GtkWidget *im = gtk_image_new_from_icon_name(icon);
    gtk_box_append(GTK_BOX(box), im);

    if (value < 0) {
        GtkWidget *l = gtk_label_new(why);
        gtk_widget_add_css_class(l, "dim-label");
        gtk_widget_add_css_class(l, "lp-detail");
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
        gtk_widget_set_halign(l, GTK_ALIGN_START);
        gtk_widget_set_hexpand(l, TRUE);
        gtk_box_append(GTK_BOX(box), l);
        gtk_widget_set_sensitive(box, FALSE);
        return box;
    }

    GtkWidget *s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                            0, 100, 1);
    gtk_range_set_value(GTK_RANGE(s), value);
    gtk_scale_set_draw_value(GTK_SCALE(s), FALSE);
    gtk_widget_set_hexpand(s, TRUE);
    g_signal_connect(s, "value-changed", cb, NULL);
    gtk_box_append(GTK_BOX(box), s);

    /* 값은 숫자로도 적는다. 손잡이의 자리만 보고 62% 인지 68% 인지
     * 알 수 있는 사람은 없고, 상단바가 숫자를 찍는 것과 같은 이유다. */
    char text[16];
    g_snprintf(text, sizeof text, "%d%%", value);
    GtkWidget *n = gtk_label_new(text);
    gtk_widget_add_css_class(n, "dim-label");
    gtk_label_set_width_chars(GTK_LABEL(n), 4);
    gtk_label_set_xalign(GTK_LABEL(n), 1.0);
    gtk_box_append(GTK_BOX(box), n);

    /* 끌면 숫자도 같이 움직여야 한다. 안 그러면 처음 열었을 때의
     * 값이 남아서 손잡이와 숫자가 서로 다른 말을 한다. */
    g_signal_connect(s, "value-changed", G_CALLBACK(update_readout), n);
    return box;
}

/* ── 각 행이 아는 것 ───────────────────────────────────────────── */

/* 지금 붙어 있는 무선 네트워크의 이름. 없으면 NULL. */
static char *wifi_ssid(void)
{
    const char *a[] = { "wpa_cli", "status", NULL };
    char *out = run_out(a);
    if (!out) return NULL;

    char *ssid = NULL;
    char **l = g_strsplit(out, "\n", -1);
    for (int i = 0; l[i] && !ssid; i++)
        if (g_str_has_prefix(l[i], "ssid="))
            ssid = g_strdup(l[i] + 5);
    g_strfreev(l);
    g_free(out);
    return ssid;
}

/* 무선 인터페이스가 이 기계에 있는가. */
static char *wifi_iface(void)
{
    GDir *d = g_dir_open("/sys/class/net", 0, NULL);
    if (!d) return NULL;
    const char *n;
    char *found = NULL;
    while ((n = g_dir_read_name(d)) && !found) {
        char *w = g_strdup_printf("/sys/class/net/%s/wireless", n);
        if (g_file_test(w, G_FILE_TEST_IS_DIR))
            found = g_strdup(n);
        g_free(w);
    }
    g_dir_close(d);
    return found;
}

static char *first_iface(void)
{
    GDir *d = g_dir_open("/sys/class/net", 0, NULL);
    if (!d) return NULL;
    const char *n;
    char *found = NULL;
    while ((n = g_dir_read_name(d)) && !found)
        if (g_strcmp0(n, "lo") != 0)
            found = g_strdup(n);
    g_dir_close(d);
    return found;
}

/* ── 각 행의 동작 ──────────────────────────────────────────────── */

static void open_settings(GtkButton *b, gpointer d)
{
    (void)b;
    Quick *q = d;
    const char *a[] = { "lp-settings", NULL };
    run_bg(a);
    gtk_window_close(GTK_WINDOW(q->window));
}

static GtkWidget *expand_network(Quick *q)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    char *iface = wifi_iface();
    if (!iface) iface = first_iface();

    if (iface) {
        char *p = g_strdup_printf("/sys/class/net/%s/address", iface);
        char *mac = slurp(p);
        g_free(p);
        char *line = g_strdup_printf("%s · %s", iface,
                                     mac ? mac : T("no address", "주소 없음"));
        gtk_box_append(GTK_BOX(box), note(line));
        g_free(line); g_free(mac); g_free(iface);
    } else {
        gtk_box_append(GTK_BOX(box),
                       note(T("This machine has no network interface.",
                              "이 기계에는 네트워크 장치가 없습니다.")));
    }

    gtk_box_append(GTK_BOX(box),
        link_row(T("Network settings →", "네트워크 설정 →"),
                 G_CALLBACK(open_settings), q));
    return box;
}

static GtkWidget *expand_bluetooth(Quick *q)
{
    (void)q;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(box),
        note(T("No Bluetooth stack is installed on this image, so there is"
               " nothing to turn on. `apt install bluez` adds it.",
               "이 이미지에는 블루투스 스택이 없어서 켤 것이 없습니다."
               " `apt install bluez` 로 넣을 수 있습니다.")));
    return box;
}

/* 방해 금지. mako 가 알림을 실제로 그리므로, 이 스위치는 mako 의
 * 모드를 바꾼다 - 아무것도 하지 않는 스위치가 아니다. */
static gboolean dnd_on(void)
{
    const char *a[] = { "makoctl", "mode", NULL };
    char *out = run_out(a);
    gboolean on = out && strstr(out, "do-not-disturb") != NULL;
    g_free(out);
    return on;
}

static void toggle_dnd(Quick *q)
{
    gboolean on = dnd_on();
    const char *set[] = { "makoctl", "mode", on ? "-r" : "-a",
                          "do-not-disturb", NULL };
    char *o = run_out(set);
    g_free(o);
    gtk_window_close(GTK_WINDOW(q->window));
}

static void dnd_30(GtkButton *b, gpointer d)   { (void)b; (void)d;
    const char *a[] = { "makoctl", "mode", "-a", "do-not-disturb", NULL };
    char *o = run_out(a); g_free(o); }

static GtkWidget *expand_dnd(Quick *q)
{
    (void)q;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(box),
        note(T("While this is on, notifications are held instead of shown.",
               "켜져 있는 동안 알림은 뜨지 않고 쌓입니다.")));
    gtk_box_append(GTK_BOX(box),
        link_row(T("Turn on until I turn it off", "끌 때까지 켜기"),
                 G_CALLBACK(dnd_30), NULL));
    return box;
}

/* 배터리. 있는 기계에서만 값이 있다.
 *
 * 없으면 NULL 을 돌려주고, 부르는 쪽은 아무것도 그리지 않는다.
 * "배터리 없음" 이라고 적는 자리를 두지 않는 것이 요점이다 -
 * 데스크탑에서 그 줄은 읽을 이유가 없고, 노트북에서만 의미가 있는
 * 것은 노트북에서만 보이면 된다. */
static char *battery_now(void)
{
    GDir *d = g_dir_open("/sys/class/power_supply", 0, NULL);
    if (!d)
        return NULL;

    char *out = NULL;
    const char *name;
    while ((name = g_dir_read_name(d)) && !out) {
        char *tp = g_strdup_printf("/sys/class/power_supply/%s/type", name);
        char *t  = slurp(tp);
        g_free(tp);
        if (t && g_strcmp0(t, "Battery") == 0) {
            char *pp = g_strdup_printf("/sys/class/power_supply/%s/present",
                                       name);
            char *cp = g_strdup_printf("/sys/class/power_supply/%s/capacity",
                                       name);
            char *sp = g_strdup_printf("/sys/class/power_supply/%s/status",
                                       name);
            char *pr = slurp(pp), *c = slurp(cp), *st = slurp(sp);

            /* 배터리라고 적힌 자리가 있다고 배터리가 있는 것은 아니다.
             *
             * 가상 머신은 ACPI 배터리 슬롯을 만들어 두고 그 안을 비워
             * 둔다: present 는 0 이거나, 용량이 0 이고 상태가 Unknown
             * 이다. 그것을 그대로 그리면 화면에 "0%" 가 떠서, 배터리가
             * 없는 기계가 다 닳은 노트북처럼 보인다. */
            gboolean real = TRUE;
            if (pr && g_strcmp0(pr, "0") == 0)
                real = FALSE;
            if (!c || !*c)
                real = FALSE;
            else if (atoi(c) == 0 &&
                     (!st || g_strcmp0(st, "Unknown") == 0 || !*st))
                real = FALSE;

            if (real)
                out = g_strdup_printf("%s%s%%",
                                      (st && g_strcmp0(st, "Charging") == 0)
                                      ? "⚡ " : "", c);
            g_free(pr); g_free(c); g_free(st);
            g_free(pp); g_free(cp); g_free(sp);
        }
        g_free(t);
    }
    g_dir_close(d);
    return out;
}

/* 전원 모드. cpufreq 가 있는 기계에서만 값이 있다. */
static char *governor(void)
{
    return slurp("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
}

static GtkWidget *expand_power_mode(Quick *q)
{
    (void)q;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    char *g = governor();

    if (!g) {
        gtk_box_append(GTK_BOX(box),
            note(T("This machine does not expose CPU frequency scaling, so"
                   " there is one mode and it cannot be changed.",
                   "이 기계는 CPU 주파수 조절을 내놓지 않습니다."
                   " 모드가 하나뿐이고 바꿀 수 없습니다.")));
    } else {
        /* 3-4: 이름만으로 차이를 알 수 없는 항목에만 설명을 단다.
         * 전원 모드가 바로 그것이다. */
        gtk_box_append(GTK_BOX(box),
            note(T("powersave — makes the battery last longer\n"
                   "performance — runs at full speed",
                   "절전 — 배터리를 오래 씁니다\n"
                   "성능 — 최대 속도로 동작합니다")));
        g_free(g);
    }
    return box;
}

/* ── 아래쪽 아이콘 (3-2 의 마지막 줄) ──────────────────────────── */

/* 비행기 모드가 켜져 있다는 기록이 놓이는 자리.
 *
 * 인터페이스가 내려가 있는지로 알아낼 수는 없다 - 랜선을 뽑아도 같은
 * 모습이고, 그것은 사람이 연결을 끊은 것과 다른 일이다. 상단바도
 * 설정 앱도 이 파일을 본다. */
static char *airplane_flag(void)
{
    return g_build_filename(g_get_home_dir(), ".config", "lp", "airplane",
                            NULL);
}

static gboolean airplane_on(void)
{
    char *p = airplane_flag();
    gboolean on = g_file_test(p, G_FILE_TEST_EXISTS);
    g_free(p);
    return on;
}

static void toggle_airplane(GtkButton *b, gpointer d)
{
    (void)b;
    Quick *q = d;
    gboolean want = !airplane_on();

    /* 진짜로 내린다. 표시만 바꾸면 비행기 모드가 아니라 비행기
     * 모드라는 그림이다. */
    GDir *dir = g_dir_open("/sys/class/net", 0, NULL);
    if (dir) {
        const char *n;
        while ((n = g_dir_read_name(dir))) {
            if (g_strcmp0(n, "lo") == 0) continue;
            const char *a[] = { "ifconfig", n, want ? "down" : "up", NULL };
            char *o = run_out(a);
            g_free(o);
        }
        g_dir_close(dir);
    }

    char *p = airplane_flag();
    if (want) {
        char *dirn = g_path_get_dirname(p);
        g_mkdir_with_parents(dirn, 0755);
        g_free(dirn);
        g_file_set_contents(p, "on\n", -1, NULL);
    } else {
        g_unlink(p);
    }
    g_free(p);

    gtk_window_close(GTK_WINDOW(q->window));
}

static void do_lock(GtkButton *b, gpointer d)
{ (void)b; Quick *q = d;
  const char *a[] = { "swaylock", "-f", "-c", "0e0e0e", NULL };
  run_bg(a); gtk_window_close(GTK_WINDOW(q->window)); }

static void do_logout(GtkButton *b, gpointer d)
{ (void)b; (void)d;
  const char *a[] = { "swaymsg", "exit", NULL };
  run_bg(a); }

/* /bin/reboot 과 /bin/poweroff 가 아니라 lp-power 다.
 *
 * 저 둘은 uid 0 만 받는다 - init 에 신호를 보내는 일이고, init 에
 * 신호를 보낼 수 있다는 것은 기계를 멈출 수 있다는 뜻이라 그렇게
 * 되어 있는 것이 옳다. 그런데 이 세션은 uid 1000 으로 돈다. 그래서
 * 메뉴에 '시스템 종료' 가 있어도 눌리면 아무 일이 없었다.
 *
 * lp-power 는 그 셋만 하는 setuid 프로그램이고, 이 기계에서 setuid
 * 인 것은 그것 하나뿐이다. 자세한 이유는 userland/lp-power 에 있다. */
static void do_reboot(GtkButton *b, gpointer d)
{ (void)b; (void)d;
  const char *a[] = { "lp-power", "restart", NULL }; run_bg(a); }

static void do_poweroff(GtkButton *b, gpointer d)
{ (void)b; (void)d;
  const char *a[] = { "lp-power", "off", NULL }; run_bg(a); }

/* 3-7 의 전원 메뉴. 하위 화면이 아니라 여기서 펼쳐진다. */
static GtkWidget *expand_power_menu(Quick *q)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box),
        link_row(T("Lock the screen", "화면 잠금"), G_CALLBACK(do_lock), q));
    gtk_box_append(GTK_BOX(box),
        note(T("Suspend is not available on this machine yet.",
               "이 기계는 아직 절전 대기로 들어가지 못합니다.")));
    gtk_box_append(GTK_BOX(box),
        link_row(T("Log out", "로그아웃"), G_CALLBACK(do_logout), q));

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep, 4);
    gtk_widget_set_margin_bottom(sep, 4);
    gtk_box_append(GTK_BOX(box), sep);

    gtk_box_append(GTK_BOX(box),
        link_row(T("Restart", "다시 시작"), G_CALLBACK(do_reboot), q));
    gtk_box_append(GTK_BOX(box),
        link_row(T("Shut down", "시스템 종료"), G_CALLBACK(do_poweroff), q));
    return box;
}

/* ── 창 ────────────────────────────────────────────────────────── */

/* ── 언제 닫히는가 ──
 *
 * 처음에는 초점을 잃으면 닫도록 했다. 대부분의 퀵메뉴가 그렇게 하고,
 * 여기서는 동작하지 않았다: 이 패널은 레이어 셸이 아니라 보통 창이고,
 * 컴포지터가 포인터와 초점을 다루는 사이에 is-active 가 한 번 거짓이
 * 되는 순간이 있다. 그때마다 패널이 사라졌다 - 손이 닿기도 전에.
 *
 * 그래서 닫는 조건을 사람이 한 일로만 한정한다: Esc 를 누르거나,
 * 안에서 무언가를 고르거나. 예측할 수 있는 편이 낫고, 잘못 닫히는
 * 패널은 다시 열면 그만이 아니라 무엇을 눌렀는지 모르게 만든다. */
static gboolean on_key(GtkEventControllerKey *k, guint keyval,
                       guint code, GdkModifierType state, gpointer data)
{
    (void)k; (void)code; (void)state;
    if (keyval == GDK_KEY_Escape) {
        gtk_window_close(GTK_WINDOW(data));
        return TRUE;
    }
    return FALSE;
}

/* 화면 오른쪽 위로 옮긴다. 레이어 셸이 없으니 컴포지터에게 말한다. */
static gboolean place_window(gpointer data)
{
    (void)data;
    const char *argv[] = { "swaymsg", "-t", "get_outputs", "-r", NULL };
    char *json = run_out(argv);
    int width = 0;

    if (json) {
        char *m = strstr(json, "\"current_mode\"");
        char *wp = m ? strstr(m, "\"width\"") : NULL;
        if (wp) sscanf(wp, "\"width\": %d", &width);
        g_free(json);
    }
    if (width <= 0) width = 1280;

    char pos[64];
    g_snprintf(pos, sizeof pos, "move position %d %d",
               width - PANEL_WIDTH - EDGE_GAP, BAR_HEIGHT + BAR_GAP);

    const char *mv[] = { "swaymsg", "[app_id=\"" APP_ID "\"]", pos, NULL };
    char *o = run_out(mv);
    g_free(o);
    return G_SOURCE_REMOVE;
}

static void build_panel(GtkApplication *gapp, gpointer data)
{
    (void)data;
    Quick *q = g_new0(Quick, 1);

    q->window = gtk_application_window_new(gapp);
    gtk_window_set_title(GTK_WINDOW(q->window), T("Quick menu", "퀵메뉴"));
    gtk_window_set_decorated(GTK_WINDOW(q->window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(q->window), FALSE);
    gtk_widget_add_css_class(q->window, "lp-quick");

    /* 3-1: 폭 320px 고정. 비율이 아니다 - 4K 에서 30% 는 1152px 이고,
     * 내용은 슬라이더 둘과 행 넷뿐인데 그릇만 커진다. */
    gtk_widget_set_size_request(q->window, PANEL_WIDTH, -1);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 10);

    /* 닫기.
     *
     * Esc 로도 닫히고 상단바의 버튼을 다시 눌러도 닫히지만, 그 둘 다
     * 화면에 보이지 않는다. 마우스로 연 사람이 마우스로 닫을 자리가
     * 없으면 그 패널은 닫는 법을 아는 사람만 쓸 수 있는 것이 된다. */
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_start(top, 14);
    gtk_widget_set_margin_end(top, 8);
    gtk_widget_set_margin_bottom(top, 2);

    GtkWidget *heading = gtk_label_new(T("Quick menu", "퀵메뉴"));
    gtk_widget_add_css_class(heading, "dim-label");
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    gtk_widget_set_hexpand(heading, TRUE);
    gtk_box_append(GTK_BOX(top), heading);

    /* 배터리는 있을 때만. 3-2 의 왼쪽 위 자리다. */
    char *bat = battery_now();
    if (bat) {
        GtkWidget *chip = gtk_label_new(bat);
        gtk_widget_add_css_class(chip, "lp-chip");
        gtk_widget_set_valign(chip, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_end(chip, 6);
        gtk_box_append(GTK_BOX(top), chip);
        g_free(bat);
    }

    GtkWidget *shut = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(shut), FALSE);
    gtk_widget_set_tooltip_text(shut, T("Close", "닫기"));
    g_signal_connect_swapped(shut, "clicked",
                             G_CALLBACK(gtk_window_close), q->window);
    gtk_box_append(GTK_BOX(top), shut);

    gtk_box_append(GTK_BOX(box), top);

    /* 볼륨. 3-2 는 이것을 패널의 첫 줄로 두었고, 그것이 퀵메뉴를
     * 여는 가장 흔한 이유이기 때문이다.
     *
     * @DEFAULT_AUDIO_SINK@ 만 물으면 기본 출력이 아직 정해지지 않은
     * 기계에서 실패한다 - 카드는 있는데 wireplumber 가 기본을 고르기
     * 전이거나, 출력이 하나뿐이라 고를 것이 없었던 경우다. 그때
     * "소리 장치가 없습니다" 라고 말하는 것은 틀린 말이므로, 목록의
     * 첫 출력으로 한 번 더 물어본다. */
    double v = -1;
    const char *vargv[] = { "wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL };
    char *vol = run_out(vargv);

    if (!vol) {
        const char *st[] = { "wpctl", "status", NULL };
        char *status = run_out(st);
        if (status) {
            char *sinks = strstr(status, "Sinks:");
            int id = 0;
            if (sinks) {
                /* "│  *   32. Dummy Output" 에서 32 를 집는다. */
                for (char *p = sinks; *p && !id; p++)
                    if (g_ascii_isdigit(*p) && (p == sinks || !g_ascii_isdigit(p[-1])))
                        id = atoi(p);
            }
            g_free(status);
            if (id > 0) {
                char idbuf[16];
                g_snprintf(idbuf, sizeof idbuf, "%d", id);
                const char *again[] = { "wpctl", "get-volume", idbuf, NULL };
                vol = run_out(again);
            }
        }
    }

    if (vol) {
        double got = 0;
        if (sscanf(vol, "Volume: %lf", &got) == 1)
            v = got * 100;
        g_free(vol);
    }
    gtk_box_append(GTK_BOX(box),
        slider("audio-volume-high-symbolic",
               v < 0 ? -1 : (int)(v + 0.5), G_CALLBACK(on_volume),
               T("No sound device", "소리 장치가 없습니다")));

    /* 밝기. 백라이트가 없으면 잠긴 채로 남는다 - 가상 머신에는 없고,
     * 노트북에는 있다. 같은 이미지가 두 곳에서 같은 자리를 지킨다. */
    int bright = -1;
    GDir *bl = g_dir_open("/sys/class/backlight", 0, NULL);
    if (bl) {
        const char *name = g_dir_read_name(bl);
        if (name) {
            char *cp = g_strdup_printf("/sys/class/backlight/%s/brightness", name);
            char *mp = g_strdup_printf("/sys/class/backlight/%s/max_brightness", name);
            char *c = slurp(cp), *m = slurp(mp);
            if (c && m && atoi(m) > 0)
                bright = atoi(c) * 100 / atoi(m);
            g_free(c); g_free(m); g_free(cp); g_free(mp);
        }
        g_dir_close(bl);
    }
    gtk_box_append(GTK_BOX(box),
        slider("display-brightness-symbolic", bright, G_CALLBACK(on_brightness),
               T("No adjustable backlight", "조절할 수 있는 백라이트가 없습니다")));

    GtkWidget *sep1 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep1, 8);
    gtk_widget_set_margin_bottom(sep1, 4);
    gtk_box_append(GTK_BOX(box), sep1);

    q->rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box), q->rows);

    /* Wi-Fi */
    char *ssid  = wifi_ssid();
    char *wif   = wifi_iface();
    char *value;
    if (ssid && *ssid)      value = g_strdup(ssid);
    else if (wif)           value = g_strdup(T("off", "꺼짐"));
    else {
        char *any = first_iface();
        value = any ? g_strdup_printf(T("wired · %s", "유선 · %s"), any)
                    : g_strdup(T("none", "없음"));
        g_free(any);
    }
    add_row(q, "Wi-Fi", NULL, value, NULL, expand_network);
    g_free(value); g_free(ssid); g_free(wif);

    add_row(q, T("Bluetooth", "블루투스"), NULL,
            T("not installed", "설치 안 됨"), NULL, expand_bluetooth);

    add_row(q, T("Do not disturb", "방해 금지"), NULL,
            dnd_on() ? T("on", "켜짐") : T("off", "꺼짐"),
            toggle_dnd, expand_dnd);

    /* 전원 모드는 cpufreq 가 있는 기계에만 있다. 없을 때 "해당 없음"
     * 이라고 적어 두었었는데, 그 줄은 아무에게도 쓸모가 없다 - 읽고
     * 나서 할 수 있는 일이 없고, 자리는 차지한다. 없는 것은 없는
     * 채로 둔다. 배터리도 같은 이유로 배터리가 있는 기계에서만
     * 나온다. */
    char *gov = governor();
    if (gov) {
        add_row(q, T("Power mode", "전원 모드"), NULL, gov,
                NULL, expand_power_mode);
        g_free(gov);
    }

    GtkWidget *sep2 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep2, 4);
    gtk_widget_set_margin_bottom(sep2, 6);
    gtk_box_append(GTK_BOX(box), sep2);

    /* 아래쪽 줄 */
    GtkWidget *feet = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(feet, 10);
    gtk_widget_set_margin_end(feet, 10);

    GtkWidget *air = gtk_button_new_from_icon_name("airplane-mode-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(air), FALSE);
    gtk_widget_set_tooltip_text(air, T("Airplane mode", "비행기 모드"));
    g_signal_connect(air, "clicked", G_CALLBACK(toggle_airplane), q);
    gtk_box_append(GTK_BOX(feet), air);

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(feet), spacer);

    GtkWidget *prefs = gtk_button_new_from_icon_name("emblem-system-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(prefs), FALSE);
    gtk_widget_set_tooltip_text(prefs, T("Settings", "설정"));
    g_signal_connect(prefs, "clicked", G_CALLBACK(open_settings), q);
    gtk_box_append(GTK_BOX(feet), prefs);

    /* 3-2 의 마지막 줄은 [절전][비행기] ... [설정][전원] 이고, 전원이
     * 그 오른쪽 끝이다. 처음에는 행으로만 두었는데, 끄거나 다시
     * 시작하려는 사람이 목록을 읽어 내려가야 했다 - 그것은 자주 쓰는
     * 것을 가장 먼 곳에 둔 것이다. 아이콘과 행이 같은 것을 편다. */
    GtkWidget *power_icon =
        gtk_button_new_from_icon_name("system-shutdown-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(power_icon), FALSE);
    gtk_widget_set_tooltip_text(power_icon,
                                T("Power", "전원"));
    gtk_box_append(GTK_BOX(feet), power_icon);

    gtk_box_append(GTK_BOX(box), feet);

    /* 3-7 의 항목이 여섯 개라 아이콘 하나에 담기지 않는다. 아코디언이
     * 이미 있는 자리에 넣고, 위의 아이콘이 그것을 편다. */
    q->power = add_row(q, T("Power", "전원"), NULL, "", NULL,
                       expand_power_menu);
    g_signal_connect(power_icon, "clicked", G_CALLBACK(on_expand), q->power);

    gtk_window_set_child(GTK_WINDOW(q->window), box);

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), q->window);
    gtk_widget_add_controller(q->window, keys);

    gtk_window_present(GTK_WINDOW(q->window));

    /* 창이 뜬 다음에야 컴포지터가 그것을 안다. */
    g_timeout_add(60, place_window, NULL);
}

/* 두 번째로 부르면 닫힌다.
 *
 * 상단바의 버튼은 누를 때마다 lp-quick 을 실행한다. GtkApplication 은
 * 같은 app id 가 이미 돌고 있으면 새 프로세스를 만들지 않고 그쪽의
 * activate 를 다시 부르므로, 여기서 창이 이미 있는지 보고 있으면
 * 닫는다 - 그것이 사람이 기대하는 토글이다.
 *
 * 이것이 없으면 버튼을 두 번 누른 사람은 패널이 그대로 있는 것을
 * 보게 되고, 닫는 방법을 따로 배워야 한다. */
static void activate(GtkApplication *gapp, gpointer data)
{
    GtkWindow *existing = gtk_application_get_active_window(gapp);
    if (existing) {
        gtk_window_close(existing);
        return;
    }
    build_panel(gapp, data);
}

int main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new(APP_ID,
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int r = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return r;
}
