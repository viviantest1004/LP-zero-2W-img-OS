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

static GtkWidget *page_network(void)
{
    char *iface = net_default_iface();
    char *addr  = iface ? net_address_of(iface) : net_address_of(NULL);

    char *sub;
    if (addr && iface)
        sub = g_strdup_printf("%s 로 연결되어 있습니다 · %s", iface, addr);
    else if (iface)
        sub = g_strdup_printf("%s 가 있지만 주소를 받지 못했습니다", iface);
    else
        sub = g_strdup("연결된 네트워크가 없습니다");

    GtkWidget *page = page_new("네트워크", sub);
    g_free(sub);

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
                val = g_strdup("주소 없음");
            else
                val = g_strdup("연결 안 됨");

            /* 무선인지 유선인지는 wireless 디렉터리가 있는지로 안다. */
            char *w = g_strdup_printf("/sys/class/net/%s/wireless", name);
            gboolean wifi = g_file_test(w, G_FILE_TEST_IS_DIR);
            g_free(w);

            char *title = g_strdup_printf("%s (%s)",
                                          wifi ? "Wi-Fi" : "유선", name);
            row_value(g1, title, NULL, val);

            g_free(title); g_free(val); g_free(ip); g_free(st);
        }
        g_dir_close(d);
    }

    GtkWidget *g2 = group_new(page, "연결 정보");
    row_value(g2, "IP 주소", NULL, addr ? addr : "없음");

    char *gw = net_gateway();
    row_value(g2, "게이트웨이", NULL, gw ? gw : "없음");
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
        row_value(g2, "DNS", NULL, acc->len ? acc->str : "없음");
        g_string_free(acc, TRUE);
        g_strfreev(l);
        g_free(dns);
    }

    if (iface) {
        char *p = g_strdup_printf("/sys/class/net/%s/address", iface);
        char *mac = slurp(p);
        g_free(p);
        row_value(g2, "MAC 주소", NULL, mac ? mac : "알 수 없음");
        g_free(mac);
    }

    g_free(addr);
    g_free(iface);
    return page;
}

/* 5-3 화면 ─────────────────────────────────────────────────────── */

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
            sub = g_strdup_printf("%d × %d · %.0fHz", w, h, mhz / 1000.0);
        else if (w && h)
            sub = g_strdup_printf("%d × %d", w, h);
    }

    GtkWidget *page = page_new("화면",
                               sub ? sub : "화면 정보를 읽지 못했습니다");
    g_free(sub);

    GtkWidget *g1 = group_new(page, NULL);

    if (json) {
        char b[160];
        char *p = strstr(json, "\"name\"");
        if (p && sscanf(p, "\"name\": \"%159[^\"]", b) == 1)
            row_value(g1, "출력", NULL, b);

        p = strstr(json, "\"model\"");
        if (p && sscanf(p, "\"model\": \"%159[^\"]", b) == 1)
            row_value(g1, "모니터", NULL, b);

        if (w && h) {
            char *v = g_strdup_printf("%d × %d", w, h);
            row_value(g1, "해상도", NULL, v);
            g_free(v);
        }
        if (mhz) {
            char *v = g_strdup_printf("%.0fHz", mhz / 1000.0);
            row_value(g1, "주사율", NULL, v);
            g_free(v);
        }

        p = strstr(json, "\"scale\"");
        double sc = 0;
        if (p && sscanf(p, "\"scale\": %lf", &sc) == 1 && sc > 0) {
            char *v = g_strdup_printf("%d%%", (int)(sc * 100));
            row_value(g1, "배율", NULL, v);
            g_free(v);
        }
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
                row_value(g1, "밝기", NULL, v);
                g_free(v);
                has_bl = TRUE;
            }
            g_free(c); g_free(m2); g_free(cur); g_free(max);
        }
        g_dir_close(bl);
    }
    if (!has_bl)
        row_value(g1, "밝기",
                  "이 기계에는 조절할 수 있는 백라이트가 없습니다", "해당 없음");

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
        sub = g_strdup_printf("출력 %d%%%s", pct, muted ? " · 음소거" : "");
    else
        sub = g_strdup("소리 장치를 찾지 못했습니다");

    GtkWidget *page = page_new("소리", sub);
    g_free(sub);

    GtkWidget *g1 = group_new(page, NULL);

    if (pct >= 0) {
        GtkWidget *row = row_shell("출력 볼륨", NULL);
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

        row_switch(g1, "음소거", NULL, muted, NULL, NULL);
    } else {
        row_value(g1, "출력 볼륨",
                  "pipewire 가 아직 장치를 잡지 못했습니다", "없음");
    }

    /* 어떤 장치로 나가는지. */
    const char *nargv[] = { "wpctl", "status", NULL };
    char *st = run_cmd(nargv);
    if (st) {
        char *sink = strstr(st, "Sinks:");
        if (sink) {
            char *star = strchr(sink, '*');
            if (star) {
                char buf[160] = "";
                if (sscanf(star, "* %*d. %159[^[\n]", buf) == 1) {
                    g_strchomp(buf);
                    if (buf[0])
                        row_value(g1, "출력 장치", NULL, buf);
                }
            }
        }
        g_free(st);
    }

    GtkWidget *g2 = group_new(page, "앱별 볼륨");
    row_chevron(g2, "앱마다 따로 맞추기",
                "pavucontrol 에서 다룹니다", NULL);

    g_free(vol);
    return page;
}

/* 5-6 전원 ─────────────────────────────────────────────────────── */

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
        sub = g_strdup_printf("배터리 %s%%%s", cap,
                              (status && g_strcmp0(status, "Charging") == 0)
                              ? " · 충전 중" : "");
    else
        sub = g_strdup("배터리가 없습니다. 전원에 연결된 채로 동작합니다");

    GtkWidget *page = page_new("전원", sub);
    g_free(sub);

    GtkWidget *g1 = group_new(page, NULL);
    if (cap) {
        char *v = g_strdup_printf("%s%%", cap);
        row_value(g1, "배터리 잔량", NULL, v);
        g_free(v);
        row_value(g1, "상태", NULL, status ? status : "알 수 없음");
    }

    /* 이 기계가 실제로 쓰는 값. sway 가 swayidle 로 화면을 끄고,
     * 그 시간은 세션 설정에 들어 있다. */
    row_value(g1, "화면 끄기", "아무 입력이 없을 때 화면을 끕니다", "10분");
    row_value(g1, "절전 대기",
              "이 기계에서는 아직 절전 대기로 들어가지 않습니다", "사용 안 함");

    g_free(cap); g_free(status);
    return page;
}

/* 5-9 키보드 ───────────────────────────────────────────────────── */

/* 문서에 적힌 목록이 아니라 지금 걸려 있는 것을 보여 준다. sway 의
 * 설정 파일이 곧 진실이고, 그것을 읽으면 둘이 어긋날 수가 없다. */
static GtkWidget *page_keyboard(void)
{
    GtkWidget *page = page_new("키보드",
                               "지금 이 기계에 걸려 있는 단축키입니다");

    GtkWidget *g0 = group_new(page, "입력 소스");
    const char *iargv[] = { "swaymsg", "-t", "get_inputs", "-r", NULL };
    char *inp = run_cmd(iargv);
    if (inp) {
        char *p = strstr(inp, "xkb_active_layout_name");
        char b[128] = "";
        if (p && sscanf(p, "xkb_active_layout_name\": \"%127[^\"]", b) == 1)
            row_value(g0, "현재 배열", NULL, b);
        g_free(inp);
    }
    row_value(g0, "입력 전환", "Alt+Shift 로 배열을 넘깁니다", "Alt+Shift");

    GtkWidget *g1 = group_new(page, "단축키");
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
        row_value(g1, "설정 파일",
                  "~/.config/sway/config 를 읽지 못했습니다", "없음");
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

    char *sub = g_strdup_printf("설치된 앱 %d개", n);
    GtkWidget *page = page_new("앱", sub);
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
        sub = g_strdup_printf("%s 중 %s 사용 · %s 남음", t, u, f);
        g_free(t); g_free(u); g_free(f);
    }

    GtkWidget *page = page_new("저장 공간",
                               sub ? sub : "드라이브를 읽지 못했습니다");
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
                val = g_strdup_printf("%s / %s 남음", a, b);
                g_free(a); g_free(b);

                /* 명세서 2-2: 남은 공간 15% 미만이면 경고. 숫자만
                 * 보여 주면 그게 나쁜 상태인지 알 수 없다. */
                if (tot && fr * 100 / tot < 15) {
                    char *w = g_strdup_printf("%s · 곧 가득 찹니다", val);
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

static GtkWidget *page_datetime(void)
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    char buf[160];
    strftime(buf, sizeof buf, "%Y년 %m월 %d일 %H:%M", &tmv);

    GtkWidget *page = page_new("날짜와 시간", buf);
    GtkWidget *g1   = group_new(page, NULL);

    /* 시간대는 /etc/localtime 이 어디를 가리키는지로 안다. */
    char *tz = g_file_read_link("/etc/localtime", NULL);
    const char *shown = "알 수 없음";
    if (tz) {
        const char *p = strstr(tz, "zoneinfo/");
        shown = p ? p + 9 : tz;
    }
    row_value(g1, "시간대", NULL, shown);
    row_value(g1, "형식", NULL, "24시간");
    g_free(tz);

    char *pl = slurp("/proc/uptime");
    if (pl) {
        double up = g_ascii_strtod(pl, NULL);
        char *v = g_strdup_printf("%d시간 %d분",
                                  (int)up / 3600, ((int)up % 3600) / 60);
        row_value(g1, "가동 시간", NULL, v);
        g_free(v); g_free(pl);
    }
    return page;
}

/* 5-16 지역과 언어 ─────────────────────────────────────────────── */

static GtkWidget *page_locale(void)
{
    const char *lang = g_getenv("LANG");
    GtkWidget *page = page_new("지역과 언어",
                               lang ? lang : "LANG 이 설정되어 있지 않습니다");
    GtkWidget *g1 = group_new(page, NULL);
    row_value(g1, "시스템 언어", NULL, "한국어");
    row_value(g1, "문자 인코딩", NULL, "UTF-8");
    row_value(g1, "LANG", NULL, lang ? lang : "없음");
    return page;
}

/* 5-12 사용자 ──────────────────────────────────────────────────── */

static GtkWidget *page_users(void)
{
    GtkWidget *page = page_new("사용자", NULL);
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
                  pw->pw_uid == 0 ? "관리자" : "표준");
        g_free(detail);
    }
    endpwent();

    GtkWidget *g2 = group_new(page, "지금 로그인한 계정");
    const char *me = g_get_user_name();
    row_value(g2, me ? me : "알 수 없음", g_get_home_dir(), NULL);
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
    GtkWidget *page = page_new("정보", sub);
    g_free(sub);

    GtkWidget *g1 = group_new(page, "기기");
    row_value(g1, "기기 이름", NULL, host ? host : u.nodename);

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

        char *v = g_strdup_printf("%s · %d코어",
                                  model[0] ? model : "알 수 없음", cores);
        row_value(g1, "프로세서", NULL, v);
        g_free(v);
    }

    char *mi = slurp("/proc/meminfo");
    if (mi) {
        unsigned long kb = 0;
        sscanf(mi, "MemTotal: %lu kB", &kb);
        char *v = human((guint64)kb * 1024);
        row_value(g1, "메모리", NULL, v);
        g_free(v); g_free(mi);
    }

    struct statvfs st;
    if (statvfs("/", &st) == 0) {
        char *v = human((guint64)st.f_blocks * st.f_frsize);
        row_value(g1, "저장 공간", NULL, v);
        g_free(v);
    }

    GtkWidget *g2 = group_new(page, "소프트웨어");
    row_value(g2, "운영체제", NULL, osname ? osname : "linux-LP");
    row_value(g2, "커널", NULL, u.release);
    row_value(g2, "화면 서버", NULL, "Wayland");
    row_value(g2, "셸", "이 기계의 /bin/sh 는 직접 만든 것입니다", "sh");

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

typedef struct {
    const char *name;
    const char *icon;
    GtkWidget *(*build)(void);
} section_t;

static const section_t SECTIONS[] = {
    { "네트워크",     "network-wireless-symbolic",     page_network  },
    { "화면",         "video-display-symbolic",        page_display  },
    { "소리",         "audio-volume-high-symbolic",    page_sound    },
    { "전원",         "battery-symbolic",              page_power    },
    { "키보드",       "input-keyboard-symbolic",       page_keyboard },
    { "앱",           "view-grid-symbolic",            page_apps     },
    { "저장 공간",    "drive-harddisk-symbolic",       page_storage  },
    { "날짜와 시간",  "preferences-system-time-symbolic", page_datetime },
    { "지역과 언어",  "preferences-desktop-locale-symbolic", page_locale },
    { "사용자",       "system-users-symbolic",         page_users    },
    { "정보",         "help-about-symbolic",           page_about    },
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
    gtk_window_set_title(GTK_WINDOW(win), "설정");
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

        GtkWidget *l = gtk_label_new(SECTIONS[i].name);
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
