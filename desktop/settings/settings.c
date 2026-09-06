/*
 * settings.c - 설정.
 *
 * apps-and-settings.md 2-1 의 화면을 그대로 만든 것. 사이드바 158px,
 * 오른쪽에 제목·부제·행 목록. 행의 오른쪽에 오는 것은 세 가지뿐이다:
 * 토글 스위치, 현재 값, 하위 화면 화살표.
 *
 * ── 이 프로그램이 지키는 규칙 하나 ──
 *
 * 여기 보이는 값은 전부 기계에서 읽은 것이다. 못 읽는 값은 넣지
 * 않았고, 눌러도 아무 일이 없는 토글은 만들지 않았다.
 *
 * 설정 앱은 거짓말이 가장 비싼 곳이다. 목록을 채우려고 스위치를
 * 하나 그려 두면, 그것을 끄고 "왜 그대로지" 하는 사람이 생기고,
 * 그 다음부터는 진짜로 동작하는 스위치도 믿지 않게 된다. 그래서
 * 명세서 5절의 항목 중 이 기계에서 아직 읽거나 쓸 수 없는 것은
 * 값 자리에 그렇다고 적어 두었다 - 없는 것은 없다고 말하는 편이
 * 있는 척하는 것보다 낫다.
 *
 * ── 값을 어디서 읽는가 ──
 *
 *   네트워크    /sys/class/net, /proc/net/route, getifaddrs
 *   화면        swaymsg -t get_outputs
 *   소리        wpctl
 *   전원        /sys/class/power_supply
 *   저장 공간   statvfs, /proc/mounts
 *   키보드      ~/.config/sway/config 의 bindsym
 *   정보        uname, /proc/cpuinfo, /proc/meminfo, /etc/osname
 *
 * 명령을 부르는 것은 소리와 화면 둘뿐이고, 나머지는 파일을 읽는다.
 * 파일 쪽이 빠르고, 그 파일들은 이 커널이 항상 만든다.
 */

/* getpwent, getifaddrs, statvfs 는 -std=c11 만으로는 선언이 숨는다.
 * 숨은 채로 쓰면 컴파일러가 int 를 돌려주는 함수로 가정하고, 64비트
 * 포인터의 위쪽 절반이 잘려 나가 첫 호출에서 죽는다 - 경고 하나로
 * 지나가는 것치고는 비싼 실수다. */
#define _DEFAULT_SOURCE 1

#include <gtk/gtk.h>
#include "lp-i18n.h"
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <pwd.h>

#define SIDEBAR_WIDTH 158

/* ── 작은 읽기 도구들 ─────────────────────────────────────────────── */

/* 파일 하나를 통째로. 없으면 NULL. 돌려준 것은 부르는 쪽이 g_free. */
static char *slurp(const char *path)
{
    char  *buf = NULL;
    gsize  len = 0;
    if (!g_file_get_contents(path, &buf, &len, NULL))
        return NULL;
    /* 커널이 주는 값은 거의 다 줄바꿈으로 끝난다. */
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';
    return buf;
}

/* 명령 하나를 돌리고 표준출력을 받는다. 실패하면 NULL.
 *
 * g_spawn_sync 를 쓰는 이유는 셸을 거치지 않기 때문이다. system() 은
 * /bin/sh 를 부르고, 인자를 문자열 하나로 이어 붙이게 만든다. 배열로
 * 넘기면 인용 문제가 아예 생기지 않는다. */
static char *run_cmd(const char *const *argv)
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
    if (out)
        g_strchomp(out);
    return out;
}

/* 기다리지 않고 띄운다. 데몬을 다시 시작하는 것처럼 출력이 없고
 * 오래 사는 것에 run_cmd 를 쓰면, 자식이 파이프를 붙든 채 살아 있어서
 * 설정 창이 그대로 얼어붙는다. */
static void run_bg(const char *const *argv)
{
    g_spawn_async(NULL, (char **)argv, NULL,
                  G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                  G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
}

/* ~/.config/lp/<name> 하나를 쓴다. 이 앱이 기억해 두는 것은 전부
 * 여기 있고, 세션 스크립트와 상단바가 같은 자리를 읽는다. */
static void lp_conf_set(const char *name, const char *value)
{
    char *dir  = g_build_filename(g_get_home_dir(), ".config", "lp", NULL);
    char *path = g_build_filename(dir, name, NULL);
    g_mkdir_with_parents(dir, 0755);
    g_file_set_contents(path, value, -1, NULL);
    g_free(path);
    g_free(dir);
}

static char *lp_conf_get(const char *name)
{
    char *path = g_build_filename(g_get_home_dir(), ".config", "lp",
                                  name, NULL);
    char *v = slurp(path);
    g_free(path);
    return v;
}

/* 1024 단위. 사람이 읽는 쪽으로. */
static char *human(guint64 bytes)
{
    const char *unit[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int    u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    if (u == 0)
        return g_strdup_printf("%.0f%s", v, unit[u]);
    return g_strdup_printf(v < 10.0 ? "%.1f%s" : "%.0f%s", v, unit[u]);
}

/* ── 행 ───────────────────────────────────────────────────────────
 *
 * 명세서 2-1: 모든 항목이 같은 행 구조이고, 오른쪽 요소는 세 종류만.
 * 그 셋을 함수 셋으로 나눠 두면 새 항목을 넣을 때 네 번째 종류를
 * 만들 길이 없다. 그것이 여기서 노리는 것이다.
 */

static GtkWidget *row_shell(const char *title, const char *detail)
{
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);

    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(h, 12);
    gtk_widget_set_margin_end(h, 12);
    gtk_widget_set_margin_top(h, 7);
    gtk_widget_set_margin_bottom(h, 7);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(v, TRUE);
    gtk_widget_set_valign(v, GTK_ALIGN_CENTER);

    GtkWidget *l = gtk_label_new(title);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_box_append(GTK_BOX(v), l);

    /* 두 번째 줄은 설명이 필요한 항목에만. Wi-Fi 에는 붙이지 않고
     * '전체 화면일 때 자동으로 켜기' 에는 붙인다 - 이름만으로 무엇을
     * 하는지 알 수 있으면 설명은 화면만 길게 만든다. */
    if (detail && *detail) {
        GtkWidget *d = gtk_label_new(detail);
        gtk_widget_set_halign(d, GTK_ALIGN_START);
        gtk_widget_add_css_class(d, "dim-label");
        gtk_widget_add_css_class(d, "lp-detail");
        gtk_label_set_wrap(GTK_LABEL(d), TRUE);
        gtk_label_set_xalign(GTK_LABEL(d), 0.0);
        gtk_box_append(GTK_BOX(v), d);
    }

    gtk_box_append(GTK_BOX(h), v);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), h);
    g_object_set_data(G_OBJECT(row), "lp-hbox", h);
    return row;
}

/* 오른쪽에 현재 값. */
static GtkWidget *row_value(GtkWidget *list, const char *title,
                            const char *detail, const char *value)
{
    GtkWidget *row = row_shell(title, detail);
    GtkWidget *h   = g_object_get_data(G_OBJECT(row), "lp-hbox");

    GtkWidget *v = gtk_label_new(value ? value : "");
    gtk_widget_add_css_class(v, "dim-label");
    gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(v), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(v), 36);
    gtk_box_append(GTK_BOX(h), v);

    gtk_list_box_append(GTK_LIST_BOX(list), row);
    return row;
}

/* 오른쪽에 토글 스위치. cb 가 NULL 이면 스위치를 잠근다 - 읽을 수는
 * 있는데 아직 쓸 수 없는 항목이 그렇다. */
static GtkWidget *row_switch(GtkWidget *list, const char *title,
                             const char *detail, gboolean on,
                             GCallback cb, gpointer data)
{
    GtkWidget *row = row_shell(title, detail);
    GtkWidget *h   = g_object_get_data(G_OBJECT(row), "lp-hbox");

    GtkWidget *sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sw), on);
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    if (cb)
        g_signal_connect(sw, "notify::active", cb, data);
    else
        gtk_widget_set_sensitive(sw, FALSE);
    gtk_box_append(GTK_BOX(h), sw);

    gtk_list_box_append(GTK_LIST_BOX(list), row);
    return row;
}

/* 오른쪽에 누르는 단추. 상태가 아니라 한 번의 행동인 항목 - 잠그기,
 * 다시 시작하기 - 은 스위치로 그리면 안 된다. 스위치는 켜져 있다는
 * 뜻을 갖는데 이것들에는 켜져 있는 상태가 없다. */
static GtkWidget *row_button(GtkWidget *list, const char *title,
                             const char *detail, const char *label,
                             GCallback cb, gpointer data)
{
    GtkWidget *row = row_shell(title, detail);
    GtkWidget *h   = g_object_get_data(G_OBJECT(row), "lp-hbox");

    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    if (cb) g_signal_connect(b, "clicked", cb, data);
    else    gtk_widget_set_sensitive(b, FALSE);
    gtk_box_append(GTK_BOX(h), b);

    gtk_list_box_append(GTK_LIST_BOX(list), row);
    return row;
}

/* 오른쪽에 값과 화살표. 하위 화면이 있는 항목. */
static GtkWidget *row_chevron(GtkWidget *list, const char *title,
                              const char *detail, const char *value)
{
    GtkWidget *row = row_shell(title, detail);
    GtkWidget *h   = g_object_get_data(G_OBJECT(row), "lp-hbox");

    if (value && *value) {
        GtkWidget *v = gtk_label_new(value);
        gtk_widget_add_css_class(v, "dim-label");
        gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(h), v);
    }

    GtkWidget *arrow = gtk_image_new_from_icon_name("go-next-symbolic");
    gtk_widget_add_css_class(arrow, "dim-label");
    gtk_widget_set_valign(arrow, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(h), arrow);

    gtk_list_box_append(GTK_LIST_BOX(list), row);
    return row;
}

/* 목록 하나. 행들이 한 덩어리로 보이도록 카드에 담는다. */
static GtkWidget *group_new(GtkWidget *page, const char *heading)
{
    if (heading && *heading) {
        GtkWidget *h = gtk_label_new(heading);
        gtk_widget_set_halign(h, GTK_ALIGN_START);
        gtk_widget_add_css_class(h, "heading");
        gtk_widget_set_margin_top(h, 16);
        gtk_widget_set_margin_bottom(h, 6);
        gtk_box_append(GTK_BOX(page), h);
    }

    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");
    gtk_widget_add_css_class(list, "card");
    gtk_box_append(GTK_BOX(page), list);
    return list;
}

/* 페이지 껍데기: 제목 + 부제 + 그 아래 내용.
 * 부제에는 현재 상태를 넣는다 (`배터리 38% · 1시간 24분 남음`). */
static GtkWidget *page_new(const char *title, const char *subtitle)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(page, 24);
    gtk_widget_set_margin_end(page, 24);
    gtk_widget_set_margin_top(page, 20);
    gtk_widget_set_margin_bottom(page, 24);

    GtkWidget *t = gtk_label_new(title);
    gtk_widget_set_halign(t, GTK_ALIGN_START);
    gtk_widget_add_css_class(t, "lp-title");
    gtk_box_append(GTK_BOX(page), t);

    if (subtitle && *subtitle) {
        GtkWidget *s = gtk_label_new(subtitle);
        gtk_widget_set_halign(s, GTK_ALIGN_START);
        gtk_widget_add_css_class(s, "dim-label");
        gtk_widget_add_css_class(s, "lp-subtitle");
        gtk_label_set_wrap(GTK_LABEL(s), TRUE);
        gtk_label_set_xalign(GTK_LABEL(s), 0.0);
        gtk_widget_set_margin_top(s, 2);
        gtk_box_append(GTK_BOX(page), s);
    }

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep, 14);
    gtk_box_append(GTK_BOX(page), sep);

    return page;
}

/* ── 각 화면 ──────────────────────────────────────────────────────
 *
 * 명세서 5절의 순서를 그대로 따른다. 사이드바의 순서와 이 함수들의
 * 순서가 같아야 나중에 항목을 넣을 자리를 찾을 수 있다.
 */

/* 5-1 네트워크 ─────────────────────────────────────────────────── */

static char *net_default_iface(void)
{
    /* /proc/net/route 의 두 번째 칸이 목적지이고 00000000 이 기본
     * 경로다. ip 명령을 부르는 것보다 빠르고, 이 파일은 커널이 항상
     * 만든다. */
    char *r = slurp("/proc/net/route");
    if (!r) return NULL;

    char **lines = g_strsplit(r, "\n", -1);
    char  *found = NULL;
    for (int i = 1; lines[i] && !found; i++) {
        char **f = g_strsplit_set(lines[i], "\t ", -1);
        int    n = 0;
        while (f[n]) n++;
        if (n >= 2 && f[1] && g_strcmp0(f[1], "00000000") == 0)
            found = g_strdup(f[0]);
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(r);
    return found;
}

static char *net_address_of(const char *iface)
{
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0)
        return NULL;

    char *out = NULL;
    for (struct ifaddrs *p = ifa; p && !out; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
            continue;
        if (iface && g_strcmp0(p->ifa_name, iface) != 0)
            continue;
        if (g_strcmp0(p->ifa_name, "lo") == 0)
            continue;
        char buf[INET_ADDRSTRLEN] = "";
        struct sockaddr_in *s = (struct sockaddr_in *)p->ifa_addr;
        if (inet_ntop(AF_INET, &s->sin_addr, buf, sizeof buf))
            out = g_strdup(buf);
    }
    freeifaddrs(ifa);
    return out;
}

static char *net_gateway(void)
{
    char *r = slurp("/proc/net/route");
    if (!r) return NULL;

    char **lines = g_strsplit(r, "\n", -1);
    char  *gw = NULL;
    for (int i = 1; lines[i] && !gw; i++) {
        char **f = g_strsplit_set(lines[i], "\t ", -1);
        int n = 0; while (f[n]) n++;
        if (n >= 3 && g_strcmp0(f[1], "00000000") == 0) {
            /* 리틀엔디언 16진수로 적혀 있으므로 바이트를 뒤집어야
             * 사람이 아는 순서가 된다. */
            guint32 v = (guint32)g_ascii_strtoull(f[2], NULL, 16);
            gw = g_strdup_printf("%u.%u.%u.%u",
                                 v & 0xff, (v >> 8) & 0xff,
                                 (v >> 16) & 0xff, (v >> 24) & 0xff);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(r);
    return gw;
}

/* 비행기 모드는 알림 절에 있는 두 함수를 쓴다. 그 절이 아래에
 * 있어서 여기서는 앞선 선언만 둔다 - 순서를 바꾸면 5절의 번호와
 * 파일의 순서가 어긋난다. */
static gboolean airplane_on(void);
static void     on_airplane(GObject *sw, GParamSpec *spec, gpointer data);

static GtkWidget *page_network(void)
{
    char *iface = net_default_iface();
    char *addr  = iface ? net_address_of(iface) : net_address_of(NULL);

    char *sub;
    if (addr && iface)
        sub = g_strdup_printf(T("Connected over %s · %s", "%s 로 연결되어 있습니다 · %s"), iface, addr);
    else if (iface)
        sub = g_strdup_printf(T("%s is up but has no address", "%s 가 있지만 주소를 받지 못했습니다"), iface);
    else
        sub = g_strdup(T("no network is connected", "연결된 네트워크가 없습니다"));

    GtkWidget *page = page_new(T("Network", "네트워크"), sub);
    g_free(sub);

    /* 5-1 의 '비행기 모드'. 표시만 바꾸는 스위치가 아니라 정말로
     * 인터페이스를 내린다 - 켜 두었는데 여전히 연결되어 있는 비행기
     * 모드는 비행기 모드라는 그림일 뿐이다. */
    GtkWidget *g0 = group_new(page, NULL);
    row_switch(g0, T("Airplane mode", "비행기 모드"),
               T("Brings every network interface down",
                 "네트워크 장치를 전부 내립니다"),
               airplane_on(), G_CALLBACK(on_airplane), NULL);

    GtkWidget *g1 = group_new(page, NULL);

    /* 이 기계에 있는 인터페이스를 전부. 없는 Wi-Fi 를 그려 두지
     * 않는다 - 가상 머신에는 유선 하나뿐인 경우가 대부분이고,
     * 그때 Wi-Fi 행이 '꺼짐' 으로 있으면 켜려고 누르게 된다. */
    GDir *d = g_dir_open("/sys/class/net", 0, NULL);
    if (d) {
        const char *name;
        while ((name = g_dir_read_name(d))) {
            if (g_strcmp0(name, "lo") == 0)
                continue;

            char *p  = g_strdup_printf("/sys/class/net/%s/operstate", name);
            char *st = slurp(p);
            g_free(p);

            char *ip = net_address_of(name);
            char *val;
            if (ip)
                val = g_strdup(ip);
            else if (st && g_strcmp0(st, "up") == 0)
                val = g_strdup(T("no address", "주소 없음"));
            else
                val = g_strdup(T("not connected", "연결 안 됨"));

            /* 무선인지 유선인지는 wireless 디렉터리가 있는지로 안다. */
            char *w = g_strdup_printf("/sys/class/net/%s/wireless", name);
            gboolean wifi = g_file_test(w, G_FILE_TEST_IS_DIR);
            g_free(w);

            char *title = g_strdup_printf("%s (%s)",
                                          wifi ? "Wi-Fi" : T("Wired", "유선"), name);
            row_value(g1, title, NULL, val);

            g_free(title); g_free(val); g_free(ip); g_free(st);
        }
        g_dir_close(d);
    }

    GtkWidget *g2 = group_new(page, T("Connection", "연결 정보"));
    row_value(g2, T("IP address", "IP 주소"), NULL, addr ? addr : T("none", "없음"));

    char *gw = net_gateway();
    row_value(g2, T("Gateway", "게이트웨이"), NULL, gw ? gw : T("none", "없음"));
    g_free(gw);

    char *dns = slurp("/etc/resolv.conf");
    if (dns) {
        char **l = g_strsplit(dns, "\n", -1);
        GString *acc = g_string_new(NULL);
        for (int i = 0; l[i]; i++) {
            if (!g_str_has_prefix(l[i], "nameserver")) continue;
            const char *v = l[i] + 10;
            while (*v == ' ' || *v == '\t') v++;
            if (acc->len) g_string_append(acc, ", ");
            g_string_append(acc, v);
        }
        row_value(g2, "DNS", NULL, acc->len ? acc->str : T("none", "없음"));
        g_string_free(acc, TRUE);
        g_strfreev(l);
        g_free(dns);
    }

    if (iface) {
        char *p = g_strdup_printf("/sys/class/net/%s/address", iface);
        char *mac = slurp(p);
        g_free(p);
        row_value(g2, T("MAC address", "MAC 주소"), NULL, mac ? mac : T("unknown", "알 수 없음"));
        g_free(mac);
    }

    g_free(addr);
    g_free(iface);
    return page;
}

/* 비행기 모드: 지금 정말로 그런가.
 *
 * 어딘가에 기록해 둔 상태가 아니라 인터페이스를 본다. 기록해 두면
 * 명령줄에서 ifconfig 로 올린 순간 설정 앱만 옛말을 하게 된다. */
/* 켜져 있다는 기록. 인터페이스가 내려가 있는지로 알아낼 수는 없다 -
 * 랜선을 뽑아도 같은 모습이고, 그것은 사람이 연결을 끊은 것과 다른
 * 일이다. 상단바와 퀵메뉴가 같은 파일을 본다. */
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

static void on_airplane(GObject *sw, GParamSpec *spec, gpointer data)
{
    (void)spec; (void)data;
    gboolean want = gtk_switch_get_active(GTK_SWITCH(sw));

    GDir *d = g_dir_open("/sys/class/net", 0, NULL);
    if (d) {
        const char *n;
        while ((n = g_dir_read_name(d))) {
            if (g_strcmp0(n, "lo") == 0) continue;
            const char *a[] = { "ifconfig", n, want ? "down" : "up", NULL };
            char *o = run_cmd(a);
            g_free(o);
        }
        g_dir_close(d);
    }

    char *p = airplane_flag();
    if (want) {
        char *dir = g_path_get_dirname(p);
        g_mkdir_with_parents(dir, 0755);
        g_free(dir);
        g_file_set_contents(p, "on\n", -1, NULL);
    } else {
        g_unlink(p);
    }
    g_free(p);
}

/* 5-5 알림 ─────────────────────────────────────────────────────── */

/* 알림을 실제로 그리는 것은 mako 이고, 방해 금지는 mako 의 모드다.
 * 우리가 따로 기억해 두지 않으므로, 명령줄에서 makoctl 로 바꿔도
 * 이 화면과 상단바가 같은 답을 한다. */
static gboolean dnd_on(void)
{
    const char *a[] = { "makoctl", "mode", NULL };
    char *out = run_cmd(a);
    gboolean on = out && strstr(out, "do-not-disturb") != NULL;
    g_free(out);
    return on;
}

static void on_dnd(GObject *sw, GParamSpec *spec, gpointer data)
{
    (void)spec; (void)data;
    gboolean want = gtk_switch_get_active(GTK_SWITCH(sw));
    const char *a[] = { "makoctl", "mode", want ? "-a" : "-r",
                        "do-not-disturb", NULL };
    char *o = run_cmd(a);
    g_free(o);
}

static GtkWidget *page_notify(void)
{
    /* mako 가 떠 있는지부터 본다. 없으면 이 화면의 스위치는 켤 것이
     * 없고, 그 사실을 스위치를 잠그는 것으로 말한다. */
    const char *probe[] = { "makoctl", "mode", NULL };
    char *have = run_cmd(probe);
    gboolean running = have != NULL;
    g_free(have);
    gboolean on = running && dnd_on();

    GtkWidget *page = page_new(
        T("Notifications", "알림"),
        running
            ? (on ? T("Do not disturb is on", "방해 금지가 켜져 있습니다")
                  : T("Notifications appear at the top of the screen",
                      "알림은 화면 위쪽에 나타납니다"))
            : T("The notification daemon is not running",
                "알림 데몬이 돌고 있지 않습니다"));

    GtkWidget *g1 = group_new(page, NULL);

    row_switch(g1, T("Do not disturb", "방해 금지"),
               T("Notifications are held instead of shown."
                 " The top bar says so while it is on.",
                 "알림이 뜨지 않고 쌓입니다. 켜져 있는 동안 상단바가"
                 " 그렇다고 알려 줍니다."),
               on, running ? G_CALLBACK(on_dnd) : NULL, NULL);

    if (!running)
        row_value(g1, T("Daemon", "데몬"),
                  T("mako draws the notifications. The session starts it;"
                    " if this says nothing is running, look at the log.",
                    "알림을 그리는 것은 mako 입니다. 세션이 띄우며,"
                    " 여기서 돌지 않는다고 나오면 로그를 보십시오."),
                  T("not running", "돌지 않음"));

    return page;
}

/* 5-3 화면 ─────────────────────────────────────────────────────── */

/* 여기서부터 아래 몇 절은 값을 읽기만 하던 것을 바꾸는 것으로
 * 만든 부분이다.
 *
 * 바꿀 수 있는 것과 없는 것을 가르는 선은 하나다: 이 세션이 uid 1000
 * 으로 할 수 있는가. 컴포지터에게 말하는 것(해상도, 배율, 자판),
 * 소리 서버에게 말하는 것(볼륨, 음소거, 출력 장치), 홈에 파일을
 * 쓰는 것(언어, 비행기 모드)은 할 수 있다. /etc 를 고치는 것(시간대,
 * 기기 이름)은 할 수 없고, 그런 항목은 값만 보여 주고 왜 못 바꾸는지
 * 적어 둔다.
 *
 * 바꾼 것은 그 자리에서 적용된다. '적용' 버튼을 두지 않은 이유는,
 * 그 버튼이 있으면 누르지 않고 창을 닫은 사람이 바꾼 줄 알고 나가기
 * 때문이다. */

/* 지금 켜진 출력의 이름. swaymsg 로 무언가를 바꾸려면 이것이 필요하다. */
static char *output_name(void)
{
    const char *argv[] = { "swaymsg", "-t", "get_outputs", "-r", NULL };
    char *json = run_cmd(argv);
    if (!json) return NULL;

    char b[128] = "";
    char *p = strstr(json, "\"name\"");
    char *out = NULL;
    if (p && sscanf(p, "\"name\": \"%127[^\"]", b) == 1)
        out = g_strdup(b);
    g_free(json);
    return out;
}

/* 이 출력이 낼 수 있는 모드 전부. "1920x1080@60.000Hz" 꼴. */
static char **output_modes(int *count)
{
    *count = 0;
    const char *argv[] = { "swaymsg", "-t", "get_outputs", "-r", NULL };
    char *json = run_cmd(argv);
    if (!json) return NULL;

    /* modes 배열 안의 width/height/refresh 세 쌍을 훑는다. JSON 파서를
     * 들이는 것보다 이쪽이 작고, 형식은 sway 가 고정해 두었다. */
    GPtrArray *list = g_ptr_array_new();
    char *m = strstr(json, "\"modes\"");
    char *end = m ? strstr(m, "]") : NULL;

    for (char *p = m; p && end && p < end; ) {
        char *w = strstr(p, "\"width\"");
        if (!w || w > end) break;
        int width = 0, height = 0, refresh = 0;
        sscanf(w, "\"width\": %d", &width);
        char *h = strstr(w, "\"height\"");
        if (h) sscanf(h, "\"height\": %d", &height);
        char *r = h ? strstr(h, "\"refresh\"") : NULL;
        if (r) sscanf(r, "\"refresh\": %d", &refresh);

        if (width && height) {
            char *text = refresh
                ? g_strdup_printf("%dx%d@%.3fHz", width, height, refresh / 1000.0)
                : g_strdup_printf("%dx%d", width, height);
            /* 같은 모드가 두 번 나오는 화면이 있다. */
            gboolean dup = FALSE;
            for (guint i = 0; i < list->len && !dup; i++)
                dup = g_strcmp0(g_ptr_array_index(list, i), text) == 0;
            if (dup) g_free(text);
            else     g_ptr_array_add(list, text);
        }
        p = r ? r + 8 : w + 8;
    }
    g_free(json);

    *count = (int)list->len;
    g_ptr_array_add(list, NULL);
    return (char **)g_ptr_array_free(list, FALSE);
}

static void on_mode_changed(GObject *dd, GParamSpec *spec, gpointer data)
{
    (void)spec;
    char **modes = data;
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (!modes || !modes[i]) return;

    char *name = output_name();
    if (!name) return;

    const char *a[] = { "swaymsg", "output", name, "mode", modes[i], NULL };
    char *o = run_cmd(a);
    g_free(o);
    g_free(name);
}

static void on_scale_changed(GObject *dd, GParamSpec *spec, gpointer data)
{
    (void)spec; (void)data;
    static const char *scales[] = { "1", "1.25", "1.5", "2" };
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (i >= G_N_ELEMENTS(scales)) return;

    char *name = output_name();
    if (!name) return;

    const char *a[] = { "swaymsg", "output", name, "scale", scales[i], NULL };
    char *o = run_cmd(a);
    g_free(o);
    g_free(name);
}

/* 바탕. 5-7 의 '배경 화면' 은 아직 그림을 고르지 못하고 색만 고른다 -
 * 그림을 고르려면 파일 선택 대화상자가 필요하고 그것은 portal 이
 * 하는 일인데, 이 이미지에서 portal 은 아직 반쯤만 산다. */
static void on_bg_changed(GObject *dd, GParamSpec *spec, gpointer data)
{
    (void)spec; (void)data;
    static const char *colors[] = { "#0e0e0e", "#1a1a1a", "#232323",
                                    "#1e2a2a", "#2a2320" };
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (i >= G_N_ELEMENTS(colors)) return;

    const char *a[] = { "swaymsg", "output", "*", "bg",
                        colors[i], "solid_color", NULL };
    char *o = run_cmd(a);
    g_free(o);
}

/* 드롭다운 한 줄. 고른 값이 그 자리에서 적용된다. */
static GtkWidget *row_choice(GtkWidget *list, const char *title,
                             const char *detail, const char *const *items,
                             guint selected, GCallback cb, gpointer data)
{
    GtkWidget *row = row_shell(title, detail);
    GtkWidget *h   = g_object_get_data(G_OBJECT(row), "lp-hbox");

    GtkWidget *dd = gtk_drop_down_new_from_strings(items);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), selected);
    gtk_widget_set_valign(dd, GTK_ALIGN_CENTER);
    if (cb)
        g_signal_connect(dd, "notify::selected", cb, data);
    else
        gtk_widget_set_sensitive(dd, FALSE);
    gtk_box_append(GTK_BOX(h), dd);

    gtk_list_box_append(GTK_LIST_BOX(list), row);
    return row;
}

static GtkWidget *page_display(void)
{
    /* sway 에게 묻는다. 컴포지터가 실제로 무엇을 켰는지 아는 것은
     * 컴포지터뿐이고, /sys 의 DRM 정보는 sway 가 고른 모드를 모른다. */
    const char *argv[] = { "swaymsg", "-t", "get_outputs", "-r", NULL };
    char *json = run_cmd(argv);

    char *sub = NULL;
    int   w = 0, h = 0, mhz = 0;

    if (json) {
        /* 아주 작은 파싱: current_mode 뒤의 숫자 셋. JSON 파서를
         * 끌어오는 것보다 이 세 값을 찾는 편이 싸다. */
        char *m = strstr(json, "\"current_mode\"");
        if (m) {
            char *wp = strstr(m, "\"width\"");
            char *hp = strstr(m, "\"height\"");
            char *rp = strstr(m, "\"refresh\"");
            if (wp) sscanf(wp, "\"width\": %d", &w);
            if (hp) sscanf(hp, "\"height\": %d", &h);
            if (rp) sscanf(rp, "\"refresh\": %d", &mhz);
        }
        if (w && h && mhz)
            sub = g_strdup_printf(T("%d × %d · %.0fHz", "%d × %d · %.0fHz"), w, h, mhz / 1000.0);
        else if (w && h)
            sub = g_strdup_printf(T("%d × %d", "%d × %d"), w, h);
    }

    GtkWidget *page = page_new(T("Display", "화면"),
                               sub ? sub : T("could not read the display", "화면 정보를 읽지 못했습니다"));
    g_free(sub);

    GtkWidget *g1 = group_new(page, NULL);

    if (json) {
        char b[160];
        char *p = strstr(json, "\"name\"");
        if (p && sscanf(p, "\"name\": \"%159[^\"]", b) == 1)
            row_value(g1, T("Output", "출력"), NULL, b);

        p = strstr(json, "\"model\"");
        if (p && sscanf(p, "\"model\": \"%159[^\"]", b) == 1)
            row_value(g1, T("Monitor", "모니터"), NULL, b);

        /* 해상도. sway 가 낼 수 있다고 말한 모드만 목록에 올린다 -
         * 없는 모드를 고를 수 있게 두면 화면이 꺼진다. */
        int nmodes = 0;
        char **modes = output_modes(&nmodes);
        if (modes && nmodes > 0) {
            char *cur = NULL;
            if (w && h && mhz)
                cur = g_strdup_printf("%dx%d@%.3fHz", w, h, mhz / 1000.0);
            else if (w && h)
                cur = g_strdup_printf("%dx%d", w, h);

            guint sel = 0;
            for (int i = 0; i < nmodes; i++)
                if (cur && g_strcmp0(modes[i], cur) == 0) { sel = (guint)i; break; }
            g_free(cur);

            GtkWidget *row = row_choice(g1, T("Resolution", "해상도"), NULL,
                                        (const char *const *)modes, sel,
                                        G_CALLBACK(on_mode_changed), modes);
            /* 목록은 콜백이 들고 있어야 하므로 줄과 수명을 같이 한다. */
            g_object_set_data_full(G_OBJECT(row), "lp-modes",
                                   modes, (GDestroyNotify)g_strfreev);
        } else {
            if (modes) g_strfreev(modes);
            if (w && h) {
                char *v = g_strdup_printf("%d × %d", w, h);
                row_value(g1, T("Resolution", "해상도"), NULL, v);
                g_free(v);
            }
        }

        if (mhz) {
            char *v = g_strdup_printf("%.0fHz", mhz / 1000.0);
            row_value(g1, T("Refresh rate", "주사율"), NULL, v);
            g_free(v);
        }

        /* 배율. sway 는 아무 소수나 받지만 목록에는 쓸 만한 넷만 둔다. */
        p = strstr(json, "\"scale\"");
        double sc = 0;
        if (p) sscanf(p, "\"scale\": %lf", &sc);
        if (sc <= 0) sc = 1.0;

        static const char *const scale_items[] =
            { "100%", "125%", "150%", "200%", NULL };
        guint ssel = 0;
        if      (sc >= 1.9) ssel = 3;
        else if (sc >= 1.4) ssel = 2;
        else if (sc >= 1.2) ssel = 1;
        row_choice(g1, T("Scale", "배율"),
                   T("takes effect at once", "고르면 바로 적용됩니다"),
                   scale_items, ssel, G_CALLBACK(on_scale_changed), NULL);
    }

    /* 밝기. 백라이트가 있는 기계에서만 값이 나온다 - 가상 머신에는
     * 없고, 없는 슬라이더를 그려 두면 끌어도 아무 일이 없다. */
    GDir *bl = g_dir_open("/sys/class/backlight", 0, NULL);
    gboolean has_bl = FALSE;
    if (bl) {
        const char *name;
        while ((name = g_dir_read_name(bl))) {
            char *cur = g_strdup_printf("/sys/class/backlight/%s/brightness", name);
            char *max = g_strdup_printf("/sys/class/backlight/%s/max_brightness", name);
            char *c = slurp(cur), *m2 = slurp(max);
            if (c && m2 && atoi(m2) > 0) {
                char *v = g_strdup_printf("%d%%", atoi(c) * 100 / atoi(m2));
                row_value(g1, T("Brightness", "밝기"), NULL, v);
                g_free(v);
                has_bl = TRUE;
            }
            g_free(c); g_free(m2); g_free(cur); g_free(max);
        }
        g_dir_close(bl);
    }
    if (!has_bl)
        row_value(g1, T("Brightness", "밝기"),
                  T("this machine has no adjustable backlight", "이 기계에는 조절할 수 있는 백라이트가 없습니다"), T("not applicable", "해당 없음"));

    /* 모양. 배경은 아직 단색만 고른다 - 그림을 고르려면 파일 선택
     * 대화상자가 필요하고 그것은 portal 이 하는 일이다. */
    GtkWidget *g2 = group_new(page, T("Appearance", "모양"));

    const char *bg_items[] = {
        T("Black",      "검정"),
        T("Charcoal",   "숯"),
        T("Slate",      "잿빛"),
        T("Deep teal",  "짙은 청록"),
        T("Warm brown", "따뜻한 갈색"),
        NULL
    };
    row_choice(g2, T("Background", "배경"),
               T("solid colour", "단색"),
               bg_items, 1, G_CALLBACK(on_bg_changed), NULL);

    g_free(json);
    return page;
}

/* 5-4 소리 ─────────────────────────────────────────────────────── */

static void on_volume(GtkRange *r, gpointer user)
{
    (void)user;
    char v[32];
    g_snprintf(v, sizeof v, "%.2f", gtk_range_get_value(r) / 100.0);
    const char *a[] = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", v, NULL };
    char *o = run_cmd(a);
    g_free(o);
}

static void on_mute(GObject *sw, GParamSpec *spec, gpointer data)
{
    (void)spec; (void)data;
    gboolean want = gtk_switch_get_active(GTK_SWITCH(sw));
    const char *a[] = { "wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@",
                        want ? "1" : "0", NULL };
    char *o = run_cmd(a);
    g_free(o);
}

/* 소리가 나갈 수 있는 곳 전부. wpctl status 의 Sinks: 아래를 읽는다.
 * 이름만으로는 고를 수 없다 - set-default 가 받는 것은 번호이고,
 * 그래서 이름과 번호를 짝지어 들고 있어야 한다. */
typedef struct {
    GPtrArray *names;   /* NULL 로 끝난다. 드롭다운에 그대로 넘긴다. */
    GPtrArray *ids;
    guint      cur;
} sinks_t;

static void sinks_free(gpointer p)
{
    sinks_t *s = p;
    if (!s) return;
    g_ptr_array_free(s->names, TRUE);
    g_ptr_array_free(s->ids, TRUE);
    g_free(s);
}

static sinks_t *sinks_read(void)
{
    const char *a[] = { "wpctl", "status", NULL };
    char *st = run_cmd(a);
    if (!st) return NULL;

    sinks_t *s = g_new0(sinks_t, 1);
    s->names = g_ptr_array_new_with_free_func(g_free);
    s->ids   = g_ptr_array_new_with_free_func(g_free);

    char  *sec = strstr(st, "Sinks:");
    char **l   = sec ? g_strsplit(sec, "\n", -1) : NULL;
    for (int i = 1; l && l[i]; i++) {
        /* 다음 절이 시작되면 멈춘다. */
        if (strstr(l[i], "Sources:") || strstr(l[i], "Filters:") ||
            strstr(l[i], "Streams:"))
            break;

        /* '│  *   47. Built-in Audio [vol: 0.40]' */
        char *dot = strchr(l[i], '.');
        if (!dot) continue;
        char *num = dot;
        while (num > l[i] && g_ascii_isdigit(num[-1])) num--;
        if (num == dot) continue;

        char *nm = dot + 1;
        while (*nm == ' ') nm++;
        char *br   = strstr(nm, "[vol:");
        char *name = br ? g_strndup(nm, br - nm) : g_strdup(nm);
        g_strchomp(name);
        if (!*name) { g_free(name); continue; }

        if (strchr(l[i], '*')) s->cur = s->names->len;
        g_ptr_array_add(s->names, name);
        g_ptr_array_add(s->ids, g_strndup(num, dot - num));
    }
    if (l) g_strfreev(l);
    g_free(st);

    if (!s->names->len) { sinks_free(s); return NULL; }
    g_ptr_array_add(s->names, NULL);
    return s;
}

static void on_sink_changed(GObject *dd, GParamSpec *spec, gpointer data)
{
    (void)spec;
    sinks_t *s = data;
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (!s || i >= s->ids->len) return;

    const char *a[] = { "wpctl", "set-default",
                        g_ptr_array_index(s->ids, i), NULL };
    char *o = run_cmd(a);
    g_free(o);
}

static GtkWidget *page_sound(void)
{
    const char *vargv[] = { "wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL };
    char *vol = run_cmd(vargv);

    int  pct   = -1;
    gboolean muted = FALSE;
    if (vol) {
        double v = 0;
        if (sscanf(vol, "Volume: %lf", &v) == 1)
            pct = (int)(v * 100 + 0.5);
        muted = strstr(vol, "MUTED") != NULL;
    }

    char *sub;
    if (pct >= 0)
        sub = g_strdup_printf(T("Output %d%%%s", "출력 %d%%%s"), pct, muted ? T(" · muted", " · 음소거") : "");
    else
        sub = g_strdup(T("no sound device was found", "소리 장치를 찾지 못했습니다"));

    GtkWidget *page = page_new(T("Sound", "소리"), sub);
    g_free(sub);

    GtkWidget *g1 = group_new(page, NULL);

    if (pct >= 0) {
        GtkWidget *row = row_shell(T("Output volume", "출력 볼륨"), NULL);
        GtkWidget *h   = g_object_get_data(G_OBJECT(row), "lp-hbox");
        GtkWidget *sc  = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                                  0, 100, 1);
        gtk_range_set_value(GTK_RANGE(sc), pct);
        gtk_scale_set_draw_value(GTK_SCALE(sc), FALSE);
        gtk_widget_set_size_request(sc, 180, -1);
        gtk_widget_set_valign(sc, GTK_ALIGN_CENTER);
        g_signal_connect(sc, "value-changed", G_CALLBACK(on_volume), NULL);
        gtk_box_append(GTK_BOX(h), sc);
        gtk_list_box_append(GTK_LIST_BOX(g1), row);

        row_switch(g1, T("Muted", "음소거"), NULL, muted,
                   G_CALLBACK(on_mute), NULL);
    } else {
        row_value(g1, T("Output volume", "출력 볼륨"),
                  T("pipewire has not claimed a device yet", "pipewire 가 아직 장치를 잡지 못했습니다"), T("none", "없음"));
    }

    /* 어떤 장치로 나갈지. 하나뿐인 기계에서도 목록을 그린다 -
     * 고를 것이 없다는 사실도 답이다. */
    sinks_t *sinks = sinks_read();
    if (sinks) {
        GtkWidget *row = row_choice(g1, T("Output device", "출력 장치"), NULL,
                                    (const char *const *)sinks->names->pdata,
                                    sinks->cur,
                                    G_CALLBACK(on_sink_changed), sinks);
        g_object_set_data_full(G_OBJECT(row), "lp-sinks", sinks, sinks_free);
    } else {
        row_value(g1, T("Output device", "출력 장치"),
                  T("wireplumber is not running", "wireplumber 가 떠 있지 않습니다"),
                  T("none", "없음"));
    }

    GtkWidget *g2 = group_new(page, T("Per-application volume", "앱별 볼륨"));
    row_chevron(g2, T("Set the level per application", "앱마다 따로 맞추기"),
                T("pavucontrol handles per-application volume", "pavucontrol 에서 다룹니다"), NULL);

    g_free(vol);
    return page;
}

/* 5-6 전원 ─────────────────────────────────────────────────────── */

/* 화면을 언제 끌지. 초 단위로 ~/.config/lp/idle 에 적고 lp-idle 을
 * 다시 띄운다 - swayidle 은 시간을 명령줄에서만 받고 다시 읽지
 * 않으므로, 바꾸려면 그 프로세스를 갈아 끼우는 수밖에 없다. */
static void on_lock_now(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    const char *a[] = { "swaylock", "-f", "-c", "0e0e0e", NULL };
    run_bg(a);
}

static const char *const IDLE_SECS[] = { "0", "120", "300", "600", "1800", NULL };

static void on_idle_changed(GObject *dd, GParamSpec *spec, gpointer data)
{
    (void)spec; (void)data;
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (i >= G_N_ELEMENTS(IDLE_SECS) - 1) return;

    lp_conf_set("idle", IDLE_SECS[i]);
    const char *a[] = { "/usr/local/bin/lp-idle", NULL };
    run_bg(a);
}

static GtkWidget *page_power(void)
{
    char *cap = NULL, *status = NULL;

    GDir *d = g_dir_open("/sys/class/power_supply", 0, NULL);
    if (d) {
        const char *name;
        while ((name = g_dir_read_name(d)) && !cap) {
            char *tp = g_strdup_printf("/sys/class/power_supply/%s/type", name);
            char *t  = slurp(tp);
            g_free(tp);
            if (t && g_strcmp0(t, "Battery") == 0) {
                char *cp = g_strdup_printf("/sys/class/power_supply/%s/capacity", name);
                char *sp = g_strdup_printf("/sys/class/power_supply/%s/status", name);
                cap    = slurp(cp);
                status = slurp(sp);
                g_free(cp); g_free(sp);
            }
            g_free(t);
        }
        g_dir_close(d);
    }

    char *sub;
    if (cap)
        sub = g_strdup_printf(T("Battery %s%%%s", "배터리 %s%%%s"), cap,
                              (status && g_strcmp0(status, "Charging") == 0)
                              ? T(" · charging", " · 충전 중") : "");
    else
        sub = g_strdup(T("No battery. This machine runs on mains power", "배터리가 없습니다. 전원에 연결된 채로 동작합니다"));

    GtkWidget *page = page_new(T("Power", "전원"), sub);
    g_free(sub);

    GtkWidget *g1 = group_new(page, NULL);
    if (cap) {
        char *v = g_strdup_printf("%s%%", cap);
        row_value(g1, T("Battery level", "배터리 잔량"), NULL, v);
        g_free(v);
        row_value(g1, T("State", "상태"), NULL, status ? status : T("unknown", "알 수 없음"));
    }

    /* 이 기계가 실제로 쓰는 값. swayidle 이 화면을 끄고, 그 시간은
     * 여기서 고른 것이 그대로 들어간다. */
    char *idle = lp_conf_get("idle");
    guint isel = 3;                          /* 기본 10분 */
    for (guint i = 0; IDLE_SECS[i]; i++)
        if (idle && g_strcmp0(idle, IDLE_SECS[i]) == 0) { isel = i; break; }
    g_free(idle);

    const char *idle_items[] = {
        T("Never",      "끄지 않음"),
        T("2 minutes",  "2분"),
        T("5 minutes",  "5분"),
        T("10 minutes", "10분"),
        T("30 minutes", "30분"),
        NULL
    };
    row_choice(g1, T("Turn the screen off", "화면 끄기"),
               T("after no input", "아무 입력이 없을 때"),
               idle_items, isel, G_CALLBACK(on_idle_changed), NULL);

    row_button(g1, T("Lock the screen now", "지금 화면 잠그기"), NULL,
               T("Lock", "잠그기"), G_CALLBACK(on_lock_now), NULL);

    row_value(g1, T("Suspend", "절전 대기"),
              T("this machine does not suspend yet", "이 기계에서는 아직 절전 대기로 들어가지 않습니다"), T("Off", "사용 안 함"));

    g_free(cap); g_free(status);
    return page;
}

/* 5-9 키보드 ───────────────────────────────────────────────────── */

/* 고를 수 있는 배열. xkb 이름과 화면에 적을 이름은 다르다 -
 * 사람은 'kr' 이 무엇인지 모르고 'Korean' 은 안다. */
static const char *const XKB_LAYOUTS[] = { "us", "us,kr", "kr", "us,jp",
                                           "us,de", "us,fr", NULL };

/* 고른 배열은 두 곳에 간다: swaymsg 로 지금 세션에, 그리고
 * ~/.config/sway/input.conf 로 다음 세션에. 한 곳만 하면 다시
 * 로그인했을 때 되돌아가거나 지금 화면이 그대로다. */
static void on_layout_changed(GObject *dd, GParamSpec *spec, gpointer data)
{
    (void)spec; (void)data;
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (i >= G_N_ELEMENTS(XKB_LAYOUTS) - 1) return;

    const char *a[] = { "swaymsg", "input", "type:keyboard",
                        "xkb_layout", XKB_LAYOUTS[i], NULL };
    char *o = run_cmd(a);
    g_free(o);

    char *dir  = g_build_filename(g_get_home_dir(), ".config", "sway", NULL);
    char *path = g_build_filename(dir, "input.conf", NULL);
    char *body = g_strdup_printf(
        "# 설정 앱이 쓴 파일. 직접 고쳐도 되고, 그러면 설정 앱이\n"
        "# 그 값을 읽어 보여 준다.\n"
        "input * {\n"
        "    xkb_layout %s\n"
        "    xkb_options grp:alt_shift_toggle\n"
        "}\n", XKB_LAYOUTS[i]);
    g_mkdir_with_parents(dir, 0755);
    g_file_set_contents(path, body, -1, NULL);
    g_free(body); g_free(path); g_free(dir);
}

/* 지금 걸려 있는 배열. input.conf 를 먼저 보고, 없으면 us. */
static guint layout_selected(void)
{
    char *path = g_build_filename(g_get_home_dir(), ".config", "sway",
                                  "input.conf", NULL);
    char *cfg = slurp(path);
    g_free(path);
    if (!cfg) return 0;

    guint sel = 0;
    char *p = strstr(cfg, "xkb_layout");
    if (p) {
        char b[64] = "";
        if (sscanf(p, "xkb_layout %63s", b) == 1)
            for (guint i = 0; XKB_LAYOUTS[i]; i++)
                if (g_strcmp0(b, XKB_LAYOUTS[i]) == 0) { sel = i; break; }
    }
    g_free(cfg);
    return sel;
}

/* 문서에 적힌 목록이 아니라 지금 걸려 있는 것을 보여 준다. sway 의
 * 설정 파일이 곧 진실이고, 그것을 읽으면 둘이 어긋날 수가 없다. */
static GtkWidget *page_keyboard(void)
{
    GtkWidget *page = page_new(T("Keyboard", "키보드"),
                               T("The shortcuts this machine actually has", "지금 이 기계에 걸려 있는 단축키입니다"));

    GtkWidget *g0 = group_new(page, T("Input sources", "입력 소스"));

    const char *layout_items[] = {
        T("English (US)",            "영어 (US)"),
        T("English (US) + Korean",   "영어 (US) + 한국어"),
        T("Korean",                  "한국어"),
        T("English (US) + Japanese", "영어 (US) + 일본어"),
        T("English (US) + German",   "영어 (US) + 독일어"),
        T("English (US) + French",   "영어 (US) + 프랑스어"),
        NULL
    };
    row_choice(g0, T("Layout", "배열"),
               T("takes effect at once", "고르면 바로 적용됩니다"),
               layout_items, layout_selected(),
               G_CALLBACK(on_layout_changed), NULL);

    /* 지금 실제로 활성인 배열. 위에서 고른 것과 다를 수 있다 -
     * 둘을 걸어 두면 Alt+Shift 로 오가기 때문이다. */
    const char *iargv[] = { "swaymsg", "-t", "get_inputs", "-r", NULL };
    char *inp = run_cmd(iargv);
    if (inp) {
        char *p = strstr(inp, "xkb_active_layout_name");
        char b[128] = "";
        if (p && sscanf(p, "xkb_active_layout_name\": \"%127[^\"]", b) == 1)
            row_value(g0, T("Active now", "지금 쓰는 배열"), NULL, b);
        g_free(inp);
    }
    row_value(g0, T("Switch input", "입력 전환"), T("Alt+Shift moves to the next layout", "Alt+Shift 로 배열을 넘깁니다"), "Alt+Shift");

    GtkWidget *g1 = group_new(page, T("Shortcuts", "단축키"));
    char *path = g_build_filename(g_get_home_dir(), ".config", "sway",
                                  "config", NULL);
    char *cfg  = slurp(path);
    g_free(path);

    if (cfg) {
        char **l = g_strsplit(cfg, "\n", -1);
        for (int i = 0; l[i]; i++) {
            if (!g_str_has_prefix(l[i], "bindsym ")) continue;

            char **f = g_strsplit(l[i] + 8, " ", 2);
            if (f[0] && f[1]) {
                /* $mod 는 화면에 Super 라고 적는다. 설정 파일의 변수
                 * 이름을 사용자에게 보여 줄 이유가 없다. */
                char **parts = g_strsplit(f[0], "$mod", -1);
                char  *key   = g_strjoinv("Super", parts);
                g_strfreev(parts);

                char *act = g_strdup(f[1]);
                if (g_str_has_prefix(act, "exec ")) {
                    char *base = g_path_get_basename(act + 5);
                    g_free(act);
                    act = base;
                }
                row_value(g1, act, NULL, key);
                g_free(key); g_free(act);
            }
            g_strfreev(f);
        }
        g_strfreev(l);
        g_free(cfg);
    } else {
        row_value(g1, T("Config file", "설정 파일"),
                  T("could not read ~/.config/sway/config", "~/.config/sway/config 를 읽지 못했습니다"), T("none", "없음"));
    }
    return page;
}

/* 5-13 앱 ──────────────────────────────────────────────────────── */

static int add_apps_from(GtkWidget *list, const char *dir)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return 0;

    int count = 0;
    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (!g_str_has_suffix(name, ".desktop")) continue;

        char *p = g_build_filename(dir, name, NULL);
        char *c = slurp(p);
        g_free(p);
        if (!c) continue;

        char    *app = NULL, *comment = NULL;
        gboolean hidden = FALSE;
        char   **l = g_strsplit(c, "\n", -1);
        for (int i = 0; l[i]; i++) {
            if (!app && g_str_has_prefix(l[i], "Name="))
                app = g_strdup(l[i] + 5);
            if (!comment && g_str_has_prefix(l[i], "Comment="))
                comment = g_strdup(l[i] + 8);
            if (g_str_has_prefix(l[i], "NoDisplay=true") ||
                g_str_has_prefix(l[i], "Hidden=true"))
                hidden = TRUE;
        }
        g_strfreev(l);

        if (app && !hidden) {
            row_value(list, app, comment, NULL);
            count++;
        }
        g_free(app); g_free(comment); g_free(c);
    }
    g_dir_close(d);
    return count;
}

static GtkWidget *page_apps(void)
{
    /* 목록을 먼저 만들어야 몇 개인지 알 수 있고, 부제에 그 수를 넣고
     * 싶으므로 페이지를 나중에 만든다. */
    GtkWidget *tmp = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(tmp), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(tmp, "boxed-list");
    gtk_widget_add_css_class(tmp, "card");

    int n = add_apps_from(tmp, "/usr/local/share/applications");
    n    += add_apps_from(tmp, "/usr/share/applications");

    char *sub = g_strdup_printf(T("%d applications installed", "설치된 앱 %d개"), n);
    GtkWidget *page = page_new(T("Apps", "앱"), sub);
    g_free(sub);

    gtk_widget_set_margin_top(tmp, 8);
    gtk_box_append(GTK_BOX(page), tmp);
    return page;
}

/* 5-14 저장 공간 ───────────────────────────────────────────────── */

static GtkWidget *page_storage(void)
{
    struct statvfs st;
    char *sub = NULL;
    if (statvfs("/", &st) == 0) {
        guint64 total = (guint64)st.f_blocks * st.f_frsize;
        guint64 avail = (guint64)st.f_bavail * st.f_frsize;
        char *t = human(total), *u = human(total - avail), *f = human(avail);
        sub = g_strdup_printf(T("%s of %s used · %s free", "%s 중 %s 사용 · %s 남음"), t, u, f);
        g_free(t); g_free(u); g_free(f);
    }

    GtkWidget *page = page_new(T("Storage", "저장 공간"),
                               sub ? sub : T("could not read the drives", "드라이브를 읽지 못했습니다"));
    g_free(sub);

    GtkWidget *g1 = group_new(page, NULL);

    /* 마운트된 것 전부. /proc/mounts 가 진실이고, 여기에 없는 것은
     * 마운트되어 있지 않다. */
    char *m = slurp("/proc/mounts");
    if (m) {
        char **l = g_strsplit(m, "\n", -1);
        for (int i = 0; l[i]; i++) {
            char dev[128] = "", mnt[128] = "", type[64] = "";
            if (sscanf(l[i], "%127s %127s %63s", dev, mnt, type) != 3)
                continue;
            /* 실제 저장 장치만. proc, sysfs, tmpfs 는 사람이 파일을
             * 두는 공간이 아니다. */
            if (!g_str_has_prefix(dev, "/dev/"))
                continue;

            struct statvfs s2;
            char *val = NULL;
            if (statvfs(mnt, &s2) == 0) {
                guint64 tot = (guint64)s2.f_blocks * s2.f_frsize;
                guint64 fr  = (guint64)s2.f_bavail * s2.f_frsize;
                char *a = human(fr), *b = human(tot);
                val = g_strdup_printf(T("%s of %s free", "%s / %s 남음"), a, b);
                g_free(a); g_free(b);

                /* 명세서 2-2: 남은 공간 15% 미만이면 경고. 숫자만
                 * 보여 주면 그게 나쁜 상태인지 알 수 없다. */
                if (tot && fr * 100 / tot < 15) {
                    char *w = g_strdup_printf(T("%s · nearly full", "%s · 곧 가득 찹니다"), val);
                    g_free(val);
                    val = w;
                }
            }
            char *detail = g_strdup_printf("%s · %s", dev, type);
            row_value(g1, mnt, detail, val ? val : "");
            g_free(detail); g_free(val);
        }
        g_strfreev(l);
        g_free(m);
    }
    return page;
}

/* 5-15 날짜와 시간 ─────────────────────────────────────────────── */

/* 12시간제인지. 상단바의 시계도 이 값을 본다 - 설정에서 바꿨는데
 * 바가 그대로면 바꾼 것이 아니다. waybar 의 설정 파일에서 시계
 * 형식만 갈아 끼우고 SIGUSR2 로 다시 읽게 한다. */
static gboolean clock12_on(void)
{
    char *v = lp_conf_get("clock");
    gboolean on = v && g_strcmp0(v, "12") == 0;
    g_free(v);
    return on;
}

static void on_clock12(GObject *sw, GParamSpec *spec, gpointer data)
{
    (void)spec; (void)data;
    gboolean want = gtk_switch_get_active(GTK_SWITCH(sw));
    lp_conf_set("clock", want ? "12" : "24");

    char *path = g_build_filename(g_get_home_dir(), ".config", "waybar",
                                  "config", NULL);
    char *cfg  = slurp(path);
    if (cfg) {
        const char *from = want ? "{:%a %H:%M}" : "{:%a %I:%M %p}";
        const char *to   = want ? "{:%a %I:%M %p}" : "{:%a %H:%M}";
        if (strstr(cfg, from)) {
            char **parts = g_strsplit(cfg, from, -1);
            char  *fixed = g_strjoinv(to, parts);
            g_strfreev(parts);
            g_file_set_contents(path, fixed, -1, NULL);
            g_free(fixed);

            /* waybar 는 SIGUSR2 를 받으면 설정을 다시 읽는다. */
            const char *k[] = { "sh", "-c",
                                "kill -USR2 $(pidof waybar)", NULL };
            run_bg(k);
        }
        g_free(cfg);
    }
    g_free(path);
}

static GtkWidget *page_datetime(void)
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    char buf[160];
    strftime(buf, sizeof buf,
             clock12_on() ? T("%d %B %Y  %I:%M %p", "%Y년 %m월 %d일 %p %I:%M")
                          : T("%d %B %Y  %H:%M",    "%Y년 %m월 %d일 %H:%M"),
             &tmv);

    GtkWidget *page = page_new(T("Date & Time", "날짜와 시간"), buf);
    GtkWidget *g1   = group_new(page, NULL);

    /* 시간대는 /etc/localtime 이 어디를 가리키는지로 안다. */
    char *tz = g_file_read_link("/etc/localtime", NULL);
    const char *shown = T("unknown", "알 수 없음");
    if (tz) {
        const char *p = strstr(tz, "zoneinfo/");
        shown = p ? p + 9 : tz;
    }
    row_value(g1, T("Time zone", "시간대"), NULL, shown);
    row_switch(g1, T("12-hour clock", "12시간제"),
               T("the top bar follows this", "상단바도 이 값을 따릅니다"),
               clock12_on(), G_CALLBACK(on_clock12), NULL);
    g_free(tz);

    char *pl = slurp("/proc/uptime");
    if (pl) {
        double up = g_ascii_strtod(pl, NULL);
        char *v = g_strdup_printf(T("%dh %dm", "%d시간 %d분"),
                                  (int)up / 3600, ((int)up % 3600) / 60);
        row_value(g1, T("Uptime", "가동 시간"), NULL, v);
        g_free(v); g_free(pl);
    }
    return page;
}

/* 5-16 지역과 언어 ─────────────────────────────────────────────── */

/* 이 기계가 쓸 로케일이 적힌 파일.
 *
 * 세션이 뜰 때 session-run 이 이것을 읽고, 없으면 영어로 간다.
 * /etc/default/locale 이 아니라 홈에 두는 이유는 이 화면이 표준
 * 계정으로도 열리기 때문이다 - 계정마다 다른 언어를 쓸 수 있어야
 * 하고, 그러자고 설정 앱에 관리자 권한을 요구하고 싶지는 않다. */
static char *locale_file(void)
{
    return g_build_filename(g_get_home_dir(), ".config", "lp", "locale", NULL);
}

static void set_language(const char *locale)
{
    char *path = locale_file();
    char *dir  = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_file_set_contents(path, locale, -1, NULL);
    g_free(dir);
    g_free(path);
}

static void on_lang_english(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    set_language("en_US.UTF-8");
}

static void on_lang_korean(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    set_language("ko_KR.UTF-8");
}

static GtkWidget *page_locale(void)
{
    const char *lang = g_getenv("LANG");
    gboolean ko = lp_korean();

    GtkWidget *page = page_new(T("Region & Language", "지역과 언어"),
                               ko ? "한국어 · ko_KR.UTF-8"
                                  : "English · en_US.UTF-8");

    GtkWidget *g1 = group_new(page, NULL);

    /* 두 언어뿐이므로 목록이 아니라 버튼 둘이다. 항목이 둘일 때
     * 드롭다운을 쓰면 무엇이 있는지 보려고 한 번 더 눌러야 한다. */
    GtkWidget *row = row_shell(T("System language", "시스템 언어"),
                               T("Applies the next time you sign in",
                                 "다음에 로그인할 때 적용됩니다"));
    GtkWidget *h = g_object_get_data(G_OBJECT(row), "lp-hbox");

    GtkWidget *pick = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(pick, "linked");
    gtk_widget_set_valign(pick, GTK_ALIGN_CENTER);

    GtkWidget *en = gtk_toggle_button_new_with_label("English");
    GtkWidget *kb = gtk_toggle_button_new_with_label("한국어");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ko ? kb : en), TRUE);
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(kb), GTK_TOGGLE_BUTTON(en));
    g_signal_connect(en, "clicked", G_CALLBACK(on_lang_english), NULL);
    g_signal_connect(kb, "clicked", G_CALLBACK(on_lang_korean), NULL);
    gtk_box_append(GTK_BOX(pick), en);
    gtk_box_append(GTK_BOX(pick), kb);
    gtk_box_append(GTK_BOX(h), pick);
    gtk_list_box_append(GTK_LIST_BOX(g1), row);

    row_value(g1, T("Character encoding", "문자 인코딩"), NULL, "UTF-8");
    row_value(g1, "LANG", NULL, lang ? lang : T("none", "없음"));

    /* 설치되어 있는 것을 그대로 보여 준다. 고를 수 있는 언어가 둘인
     * 이유가 두 개만 생성해 두었기 때문이라는 것을, 목록을 보면
     * 알 수 있다. */
    GtkWidget *g2 = group_new(page, T("Installed languages", "설치된 언어"));
    const char *gargv[] = { "locale", "-a", NULL };
    char *all = run_cmd(gargv);
    if (all) {
        char **l = g_strsplit(all, "\n", -1);
        for (int i = 0; l[i]; i++) {
            if (!strstr(l[i], ".utf8") && !strstr(l[i], ".UTF-8")) continue;
            row_value(g2, l[i], NULL, NULL);
        }
        g_strfreev(l);
        g_free(all);
    }
    return page;
}

/* 5-12 사용자 ──────────────────────────────────────────────────── */

static GtkWidget *page_users(void)
{
    GtkWidget *page = page_new(T("Users", "사용자"), NULL);
    GtkWidget *g1   = group_new(page, NULL);

    /* 사람이 쓰는 계정만. root 와 uid 1000 위다. 시스템 계정 스무
     * 개를 늘어놓으면 정작 찾는 것이 묻힌다. */
    setpwent();
    struct passwd *pw;
    while ((pw = getpwent())) {
        if (pw->pw_uid != 0 && pw->pw_uid < 1000)
            continue;
        if (pw->pw_uid >= 65534)
            continue;

        char *detail = g_strdup_printf("%s · %s", pw->pw_dir, pw->pw_shell);
        row_value(g1, pw->pw_name, detail,
                  pw->pw_uid == 0 ? T("Administrator", "관리자") : T("Standard", "표준"));
        g_free(detail);
    }
    endpwent();

    GtkWidget *g2 = group_new(page, T("Signed in as", "지금 로그인한 계정"));
    const char *me = g_get_user_name();
    row_value(g2, me ? me : T("unknown", "알 수 없음"), g_get_home_dir(), NULL);
    return page;
}

/* 5-19 정보 ────────────────────────────────────────────────────── */

static GtkWidget *page_about(void)
{
    struct utsname u;
    uname(&u);

    char *osname = slurp("/etc/osname");
    char *host   = slurp("/etc/hostname");

    char *sub = g_strdup_printf("%s · %s",
                                osname ? osname : "linux-LP", u.machine);
    GtkWidget *page = page_new(T("About", "정보"), sub);
    g_free(sub);

    GtkWidget *g1 = group_new(page, T("Device", "기기"));
    row_value(g1, T("Device name", "기기 이름"), NULL, host ? host : u.nodename);

    char *ci = slurp("/proc/cpuinfo");
    if (ci) {
        char  model[256] = "";
        int   cores = 0;
        char **l = g_strsplit(ci, "\n", -1);
        for (int i = 0; l[i]; i++) {
            if (g_str_has_prefix(l[i], "processor")) cores++;
            if (!model[0] && g_str_has_prefix(l[i], "model name")) {
                const char *c = strchr(l[i], ':');
                if (c) {
                    c++;
                    while (*c == ' ') c++;
                    g_strlcpy(model, c, sizeof model);
                }
            }
        }
        g_strfreev(l);
        g_free(ci);

        char *v = g_strdup_printf(T("%s · %d cores", "%s · %d코어"),
                                  model[0] ? model : T("unknown", "알 수 없음"), cores);
        row_value(g1, T("Processor", "프로세서"), NULL, v);
        g_free(v);
    }

    char *mi = slurp("/proc/meminfo");
    if (mi) {
        unsigned long kb = 0;
        sscanf(mi, "MemTotal: %lu kB", &kb);
        char *v = human((guint64)kb * 1024);
        row_value(g1, T("Memory", "메모리"), NULL, v);
        g_free(v); g_free(mi);
    }

    struct statvfs st;
    if (statvfs("/", &st) == 0) {
        char *v = human((guint64)st.f_blocks * st.f_frsize);
        row_value(g1, T("Storage", "저장 공간"), NULL, v);
        g_free(v);
    }

    GtkWidget *g2 = group_new(page, T("Software", "소프트웨어"));
    row_value(g2, T("Operating system", "운영체제"), NULL, osname ? osname : "linux-LP");
    row_value(g2, T("Kernel", "커널"), NULL, u.release);
    row_value(g2, T("Display server", "화면 서버"), NULL, "Wayland");
    row_value(g2, T("Shell", "셸"), T("the /bin/sh on this machine was written here", "이 기계의 /bin/sh 는 직접 만든 것입니다"), "sh");

    g_free(osname); g_free(host);
    return page;
}

/* ── 창 ───────────────────────────────────────────────────────────
 *
 * 사이드바 158px 에 항목 이름, 오른쪽에 그 항목의 화면. 명세서
 * 2-1 의 그림 그대로다.
 *
 * 화면을 미리 다 만들지 않고 고를 때 만든다. 열 몇 개의 화면이 전부
 * /proc 과 /sys 를 읽고 swaymsg 를 부르는데, 그것을 시작할 때 한꺼번에
 * 하면 창이 뜨는 데 1초가 걸린다. 그리고 다시 고르면 다시 읽으므로
 * 값이 늘 지금 것이다 - 설정 앱이 30분 전의 배터리 잔량을 보여 주는
 * 것만큼 쓸모없는 것도 없다.
 */

/* 이름이 두 벌이라 표에 둘 다 담는다.
 *
 * T() 를 여기 쓸 수는 없다. 이 배열은 정적 초기화이고 T() 는 실행할
 * 때 환경을 읽는 함수 호출이라, C 가 받아 주지 않는다. 어느 쪽을
 * 쓸지는 사이드바를 만들면서 정한다. */
typedef struct {
    const char *en;
    const char *ko;
    const char *icon;
    GtkWidget *(*build)(void);
} section_t;

static const char *section_name(const section_t *s)
{
    return lp_korean() ? s->ko : s->en;
}

static const section_t SECTIONS[] = {
    { "Network", "네트워크", "network-wireless-symbolic",     page_network  },
    { "Display", "화면", "video-display-symbolic",        page_display  },
    { "Sound", "소리", "audio-volume-high-symbolic",    page_sound    },
    { "Notifications", "알림", "preferences-system-notifications-symbolic",
      page_notify },
    { "Power", "전원", "battery-symbolic",              page_power    },
    { "Keyboard", "키보드", "input-keyboard-symbolic",       page_keyboard },
    { "Apps", "앱", "view-grid-symbolic",            page_apps     },
    { "Storage", "저장 공간", "drive-harddisk-symbolic",       page_storage  },
    { "Date & Time", "날짜와 시간", "preferences-system-time-symbolic", page_datetime },
    { "Region & Language", "지역과 언어", "preferences-desktop-locale-symbolic", page_locale },
    { "Users", "사용자", "system-users-symbolic",         page_users    },
    { "About", "정보", "help-about-symbolic",           page_about    },
};

typedef struct {
    GtkWidget *content;    /* 오른쪽. 화면 하나를 담는 스크롤 창 */
} app_t;

static void show_section(app_t *app, int index)
{
    if (index < 0 || index >= (int)G_N_ELEMENTS(SECTIONS))
        return;

    GtkWidget *page = SECTIONS[index].build();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(app->content), page);
}

static void on_sidebar_row(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    (void)box;
    if (!row) return;
    show_section((app_t *)data, gtk_list_box_row_get_index(row));
}

static void activate(GtkApplication *gapp, gpointer data)
{
    (void)data;
    app_t *app = g_new0(app_t, 1);

    GtkWidget *win = gtk_application_window_new(gapp);
    gtk_window_set_title(GTK_WINDOW(win), T("Settings", "설정"));
    gtk_window_set_default_size(GTK_WINDOW(win), 860, 620);

    /* 헤더 바는 창과 한 몸이고 오른쪽에 닫기 하나뿐. 4-1 이다. */
    GtkWidget *head = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(head), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(win), head);

    GtkWidget *split = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    /* ── 사이드바 ── */
    GtkWidget *sidebar = gtk_list_box_new();
    gtk_widget_add_css_class(sidebar, "navigation-sidebar");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(sidebar), GTK_SELECTION_SINGLE);

    for (guint i = 0; i < G_N_ELEMENTS(SECTIONS); i++) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *h   = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(h, 8);
        gtk_widget_set_margin_end(h, 8);
        gtk_widget_set_margin_top(h, 5);
        gtk_widget_set_margin_bottom(h, 5);

        GtkWidget *ic = gtk_image_new_from_icon_name(SECTIONS[i].icon);
        gtk_box_append(GTK_BOX(h), ic);

        GtkWidget *l = gtk_label_new(section_name(&SECTIONS[i]));
        gtk_widget_set_halign(l, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(h), l);

        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), h);
        gtk_list_box_append(GTK_LIST_BOX(sidebar), row);
    }

    GtkWidget *side_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(side_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(side_scroll), sidebar);
    gtk_widget_set_size_request(side_scroll, SIDEBAR_WIDTH, -1);
    gtk_widget_set_hexpand(side_scroll, FALSE);
    gtk_widget_add_css_class(side_scroll, "sidebar");
    gtk_box_append(GTK_BOX(split), side_scroll);

    gtk_box_append(GTK_BOX(split),
                   gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    /* ── 오른쪽 ── */
    app->content = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(app->content, TRUE);
    gtk_widget_set_vexpand(app->content, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(app->content),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(split), app->content);

    g_signal_connect(sidebar, "row-selected",
                     G_CALLBACK(on_sidebar_row), app);

    gtk_window_set_child(GTK_WINDOW(win), split);

    /* 첫 화면은 네트워크. 설정을 열러 오는 가장 흔한 이유다. */
    gtk_list_box_select_row(GTK_LIST_BOX(sidebar),
                            gtk_list_box_get_row_at_index(GTK_LIST_BOX(sidebar), 0));

    gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new("org.lpzero.Settings",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int r = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return r;
}
