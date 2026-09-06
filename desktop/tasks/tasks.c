/* getpwuid, statvfs 등이 -std=c11 만으로는 선언이 숨는다. 숨은 채로
 * 쓰면 컴파일러가 int 를 돌려주는 함수로 가정해 64비트 포인터의
 * 위쪽 절반이 잘려 나간다. */
#define _DEFAULT_SOURCE 1

/*
 * tasks.c - 작업 관리자.
 *
 * apps-and-settings.md 2-2. 탭은 앱 / 프로세스 / CPU / 메모리 /
 * 드라이브 / 네트워크.
 *
 * ── 명세서가 요구한 세 가지 ──
 *
 * 1. 앱 탭과 프로세스 탭을 나눈다. 대부분은 "브라우저가 느려졌다"를
 *    확인하러 오지 systemd 를 보러 오지 않는다. 앱 탭은 창이 있는
 *    프로그램만 - 창 목록은 컴포지터가 알고 있으므로 swaymsg 에게
 *    묻고, 거기서 얻은 pid 로 /proc 을 읽는다. 이름으로 거르면 그
 *    목록이 곧 하드코딩된 앱 이름 목록이 된다.
 *
 * 2. 전력 열을 기본 표시한다. 노트북에서는 CPU% 보다 중요한 지표다.
 *    이 기계가 실제로 잴 수 있는 것은 배터리 전체의 소비 전력뿐
 *    (power_supply 의 power_now) 이라, 프로세스별 전력은
 *    그 값을 CPU 점유로 나눈 몫이다. 배터리가 없으면 열이 '—' 로
 *    남는다 - 없는 값을 그럴듯한 숫자로 채우지 않는다.
 *
 * 3. 숫자 옆에 해석을 붙인다. 남은 공간 15% 미만이면 그 줄이 무엇을
 *    하라는 문장으로 바뀐다. 숫자만 보여 주면 그게 나쁜 상태인지 알
 *    수 없다.
 *
 * ── CPU 를 어떻게 재는가 ──
 *
 * /proc/<pid>/stat 의 utime+stime 은 부팅 이후의 누적값이다. 그것
 * 하나만 보면 "이 프로세스가 평생 쓴 시간" 이 나오는데, 지금 무엇이
 * 느린지와는 상관이 없다 - 오래 떠 있던 프로세스가 늘 1등이 된다.
 * 그래서 두 번 읽어 차이를 본다. 표본 사이의 tick 증가를 같은 동안의
 * 전체 jiffies 증가로 나눈 것이 이 표의 CPU% 다.
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <pwd.h>

#define REFRESH_MS  1500
#define HISTORY     60          /* 명세서의 "60초 그래프" */
#define MAX_ROWS    64

/* ── 읽기 도구 ─────────────────────────────────────────────────── */

static char *slurp(const char *path)
{
    char *buf = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &buf, &len, NULL))
        return NULL;
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';
    return buf;
}

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
    return out;
}

static char *human(guint64 bytes)
{
    const char *unit[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int    u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    if (u == 0) return g_strdup_printf("%.0f%s", v, unit[u]);
    return g_strdup_printf(v < 10.0 ? "%.1f%s" : "%.0f%s", v, unit[u]);
}

/* ── 프로세스 한 줄 ────────────────────────────────────────────── */

typedef struct {
    int      pid;
    char     name[64];
    guint64  ticks;        /* utime + stime, 누적 */
    guint64  prev_ticks;
    double   cpu;          /* 지난 표본 이후의 % */
    guint64  rss;          /* 바이트 */
} proc_t;

/* 표를 GtkColumnView 로 만들지 않은 이유.
 *
 * ColumnView 에 실으려면 항목 하나하나가 GObject 여야 하고, 그러려면
 * 타입을 선언하고 프로퍼티를 달아야 한다. 표가 둘이라 그것을 두 번
 * 한다. 여기서는 GtkGrid 에 라벨을 미리 깔아 두고 1.5초마다 글자만
 * 바꾼다 - 행이 예순 개 수준이면 이쪽이 더 싸고, 무엇보다 정렬이
 * 바뀌어도 스크롤 위치가 튀지 않는다. */
typedef struct {
    GtkWidget *grid;
    GtkWidget *cell[MAX_ROWS][5];
} table_t;

typedef struct {
    GHashTable *procs;         /* pid -> proc_t*, 누적값을 들고 있다 */
    guint64     prev_total;    /* /proc/stat 의 전체 jiffies */
    guint64     prev_idle;

    table_t     apps;
    table_t     all;

    GtkWidget  *cpu_label,  *cpu_bar,  *cpu_detail, *cpu_graph;
    GtkWidget  *mem_label,  *mem_bar,  *mem_detail;

    double      cpu_hist[HISTORY];
    int         hist_at;
} app_t;

/* ── 표 ────────────────────────────────────────────────────────── */

static const char *COLS[] = { "이름", "CPU", "메모리", "전력", "PID" };

static void table_init(table_t *t, const char *first_col)
{
    t->grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(t->grid), 18);
    gtk_widget_set_margin_start(t->grid, 16);
    gtk_widget_set_margin_end(t->grid, 16);
    gtk_widget_set_margin_top(t->grid, 12);
    gtk_widget_set_margin_bottom(t->grid, 12);

    for (int c = 0; c < 5; c++) {
        GtkWidget *h = gtk_label_new(c == 0 ? first_col : COLS[c]);
        gtk_widget_add_css_class(h, "dim-label");
        gtk_widget_add_css_class(h, "heading");
        gtk_widget_set_halign(h, c == 0 ? GTK_ALIGN_START : GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(t->grid), h, c, 0, 1, 1);
    }

    for (int r = 0; r < MAX_ROWS; r++) {
        for (int c = 0; c < 5; c++) {
            GtkWidget *l = gtk_label_new("");
            gtk_widget_set_halign(l, c == 0 ? GTK_ALIGN_START : GTK_ALIGN_END);
            gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
            gtk_label_set_xalign(GTK_LABEL(l), c == 0 ? 0.0 : 1.0);
            if (c == 0) {
                gtk_widget_set_hexpand(l, TRUE);
                gtk_label_set_max_width_chars(GTK_LABEL(l), 40);
            } else {
                /* 숫자 열은 자리를 고정한다. 값이 바뀔 때마다 열
                 * 너비가 흔들리면 눈으로 따라갈 수 없다. */
                gtk_label_set_width_chars(GTK_LABEL(l), 9);
            }
            gtk_widget_set_visible(l, FALSE);
            gtk_grid_attach(GTK_GRID(t->grid), l, c, r + 1, 1, 1);
            t->cell[r][c] = l;
        }
    }
}

static void table_set(table_t *t, int row, const char *a, const char *b,
                      const char *c, const char *d, const char *e)
{
    if (row >= MAX_ROWS) return;
    const char *v[5] = { a, b, c, d, e };
    for (int i = 0; i < 5; i++) {
        gtk_label_set_text(GTK_LABEL(t->cell[row][i]), v[i] ? v[i] : "");
        gtk_widget_set_visible(t->cell[row][i], TRUE);
    }
}

static void table_trim(table_t *t, int used)
{
    for (int r = used; r < MAX_ROWS; r++)
        for (int c = 0; c < 5; c++)
            gtk_widget_set_visible(t->cell[r][c], FALSE);
}

/* ── /proc 읽기 ────────────────────────────────────────────────── */

static guint64 total_jiffies(guint64 *idle_out)
{
    char *s = slurp("/proc/stat");
    if (!s) return 0;

    guint64 sum = 0, idle = 0;
    if (g_str_has_prefix(s, "cpu ")) {
        const char *p = s + 4;
        for (int i = 0; ; i++) {
            char *end;
            guint64 v = g_ascii_strtoull(p, &end, 10);
            if (end == p) break;
            sum += v;
            if (i == 3 || i == 4) idle += v;   /* idle + iowait */
            p = end;
        }
    }
    g_free(s);
    if (idle_out) *idle_out = idle;
    return sum;
}

/* /proc 을 한 바퀴 돌아 pid 별 누적 tick 과 rss 를 갱신한다. */
static GList *scan_procs(app_t *app, double *busy_out)
{
    guint64 idle = 0;
    guint64 now  = total_jiffies(&idle);
    guint64 dt   = (app->prev_total && now > app->prev_total)
                 ? now - app->prev_total : 0;

    if (busy_out) {
        *busy_out = dt
            ? 100.0 * (1.0 - (double)(idle - app->prev_idle) / (double)dt)
            : 0.0;
        if (*busy_out < 0)   *busy_out = 0;
        if (*busy_out > 100) *busy_out = 100;
    }
    app->prev_total = now;
    app->prev_idle  = idle;

    long page = sysconf(_SC_PAGESIZE);

    GDir *d = g_dir_open("/proc", 0, NULL);
    if (!d) return NULL;

    /* 이번 바퀴에 보이지 않은 pid 는 죽은 것이다. 지워 두지 않으면
     * 해시가 부팅 이후의 모든 pid 를 들고 있게 된다. */
    GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);

    GList *list = NULL;
    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (!g_ascii_isdigit(name[0])) continue;
        int pid = atoi(name);

        char *sp = g_strdup_printf("/proc/%d/stat", pid);
        char *st = slurp(sp);
        g_free(sp);
        if (!st) continue;

        /* comm 은 괄호 안에 있고 공백을 담을 수 있으므로 마지막 ')'
         * 를 기준으로 자른다. 앞에서부터 칸을 세면 `(Web Content)`
         * 같은 이름에서 그 뒤가 전부 밀린다. */
        char *close = strrchr(st, ')');
        char *open  = strchr(st, '(');
        char  comm[64] = "";
        if (open && close && close > open) {
            gsize n = (gsize)(close - open - 1);
            if (n >= sizeof comm) n = sizeof comm - 1;
            memcpy(comm, open + 1, n);
            comm[n] = '\0';
        }

        guint64 ut = 0, stime = 0, rss = 0;
        if (close) {
            /* close + 2 부터가 state 이고 글자 하나다. 그것을 건너뛰면
             * 다음 숫자가 proc(5) 의 4번 항목(ppid)이므로, 여기서 세는
             * i 는 항목 번호에서 4를 뺀 값이 된다:
             *
             *   utime  14번 → i = 10
             *   stime  15번 → i = 11
             *   rss    24번 → i = 20
             *
             * 처음에 11, 12, 21 로 적었다가 cutime, cstime, vsize 를
             * 읽었다. vsize 는 바이트인데 rss 처럼 페이지 크기를 곱하니
             * 메모리 열이 전부 16777216TB 로 나왔다 - 자리 하나가 밀린
             * 것이 화면에서는 단위를 잘못 쓴 것처럼 보였다. */
            const char *p = close + 2;
            while (*p && *p != ' ') p++;        /* state 를 건너뛴다 */
            for (int i = 0; i <= 20; i++) {
                while (*p == ' ') p++;
                char *end;
                guint64 v = g_ascii_strtoull(p, &end, 10);
                if (end == p) break;
                if (i == 10) ut    = v;
                if (i == 11) stime = v;
                if (i == 20) rss   = v;
                p = end;
            }
        }
        g_free(st);

        proc_t *pr = g_hash_table_lookup(app->procs, GINT_TO_POINTER(pid));
        if (!pr) {
            pr = g_new0(proc_t, 1);
            pr->pid   = pid;
            pr->ticks = ut + stime;    /* 첫 표본은 0% 로 시작한다 */
            g_hash_table_insert(app->procs, GINT_TO_POINTER(pid), pr);
        }
        g_strlcpy(pr->name, comm, sizeof pr->name);
        pr->prev_ticks = pr->ticks;
        pr->ticks      = ut + stime;
        pr->rss        = rss * (guint64)page;
        pr->cpu = dt ? (double)(pr->ticks - pr->prev_ticks) * 100.0 / (double)dt
                     : 0.0;
        if (pr->cpu < 0) pr->cpu = 0;

        g_hash_table_add(seen, GINT_TO_POINTER(pid));
        list = g_list_prepend(list, pr);
    }
    g_dir_close(d);

    GHashTableIter it;
    gpointer key, val;
    GList *dead = NULL;
    g_hash_table_iter_init(&it, app->procs);
    while (g_hash_table_iter_next(&it, &key, &val))
        if (!g_hash_table_contains(seen, key))
            dead = g_list_prepend(dead, key);
    for (GList *l = dead; l; l = l->next)
        g_hash_table_remove(app->procs, l->data);
    g_list_free(dead);
    g_hash_table_destroy(seen);

    return list;
}

static gint by_cpu(gconstpointer a, gconstpointer b)
{
    const proc_t *x = a, *y = b;
    if (y->cpu > x->cpu) return 1;
    if (y->cpu < x->cpu) return -1;
    return (y->rss > x->rss) ? 1 : (y->rss < x->rss) ? -1 : 0;
}

/* 창이 있는 pid 를 컴포지터에게 묻는다. 앱과 프로세스를 가르는
 * 유일하게 정직한 기준이다. */
static GHashTable *window_pids(void)
{
    GHashTable *set = g_hash_table_new(g_direct_hash, g_direct_equal);
    const char *argv[] = { "swaymsg", "-t", "get_tree", "-r", NULL };
    char *json = run_cmd(argv);
    if (!json) return set;

    const char *p = json;
    while ((p = strstr(p, "\"pid\":"))) {
        int pid = atoi(p + 6);
        if (pid > 0)
            g_hash_table_add(set, GINT_TO_POINTER(pid));
        p += 6;
    }
    g_free(json);
    return set;
}

/* 지금 배터리가 몇 W 를 쓰는지. 없으면 0. */
static double watts_now(void)
{
    double w = 0;
    GDir *ps = g_dir_open("/sys/class/power_supply", 0, NULL);
    if (!ps) return 0;
    const char *n;
    while ((n = g_dir_read_name(ps))) {
        char *p = g_strdup_printf("/sys/class/power_supply/%s/power_now", n);
        char *v = slurp(p);
        g_free(p);
        if (v) { w = g_ascii_strtod(v, NULL) / 1e6; g_free(v); }
    }
    g_dir_close(ps);
    return w;
}

/* ── 갱신 ──────────────────────────────────────────────────────── */

static gboolean tick(gpointer data)
{
    app_t *app  = data;
    double busy = 0;

    GList      *list = scan_procs(app, &busy);
    GHashTable *wins = window_pids();
    double      watts = watts_now();

    list = g_list_sort(list, by_cpu);

    int rows_all = 0, rows_app = 0;
    for (GList *l = list; l; l = l->next) {
        proc_t *pr = l->data;

        char cpu[16], mem[16], pw[16], pid[16];
        g_snprintf(cpu, sizeof cpu, "%.1f%%", pr->cpu);
        char *m = human(pr->rss);
        g_strlcpy(mem, m, sizeof mem);
        g_free(m);
        if (watts > 0)
            g_snprintf(pw, sizeof pw, "%.2fW", watts * pr->cpu / 100.0);
        else
            g_strlcpy(pw, "—", sizeof pw);
        g_snprintf(pid, sizeof pid, "%d", pr->pid);

        if (g_hash_table_contains(wins, GINT_TO_POINTER(pr->pid)) &&
            rows_app < MAX_ROWS)
            table_set(&app->apps, rows_app++, pr->name, cpu, mem, pw, pid);

        if (rows_all < MAX_ROWS)
            table_set(&app->all, rows_all++, pr->name, cpu, mem, pw, pid);
    }

    if (rows_app == 0)
        table_set(&app->apps, rows_app++,
                  "창이 열려 있는 프로그램이 없습니다", "", "", "", "");

    table_trim(&app->apps, rows_app);
    table_trim(&app->all,  rows_all);

    /* ── CPU ── */
    app->cpu_hist[app->hist_at] = busy;
    app->hist_at = (app->hist_at + 1) % HISTORY;
    gtk_widget_queue_draw(app->cpu_graph);

    char *cl = g_strdup_printf("%.0f%%", busy);
    gtk_label_set_text(GTK_LABEL(app->cpu_label), cl);
    g_free(cl);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->cpu_bar), busy / 100.0);

    {
        struct utsname u;
        uname(&u);
        char  *up  = slurp("/proc/uptime");
        double sec = up ? g_ascii_strtod(up, NULL) : 0;
        g_free(up);
        char *d2 = g_strdup_printf("프로세스 %d개 · 가동 %d시간 %d분 · %s",
                                   (int)g_hash_table_size(app->procs),
                                   (int)sec / 3600, ((int)sec % 3600) / 60,
                                   u.machine);
        gtk_label_set_text(GTK_LABEL(app->cpu_detail), d2);
        g_free(d2);
    }

    /* ── 메모리 ── */
    {
        char *mi = slurp("/proc/meminfo");
        if (mi) {
            unsigned long total = 0, avail = 0, cached = 0,
                          swapt = 0, swapf = 0;
            char **l = g_strsplit(mi, "\n", -1);
            for (int i = 0; l[i]; i++) {
                sscanf(l[i], "MemTotal: %lu kB", &total);
                sscanf(l[i], "MemAvailable: %lu kB", &avail);
                sscanf(l[i], "Cached: %lu kB", &cached);
                sscanf(l[i], "SwapTotal: %lu kB", &swapt);
                sscanf(l[i], "SwapFree: %lu kB", &swapf);
            }
            g_strfreev(l);
            g_free(mi);

            if (total) {
                char *a = human((guint64)(total - avail) * 1024);
                char *b = human((guint64)total * 1024);
                char *t = g_strdup_printf("%s / %s", a, b);
                gtk_label_set_text(GTK_LABEL(app->mem_label), t);
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->mem_bar),
                                              (double)(total - avail) / total);
                g_free(a); g_free(b); g_free(t);

                char *c2 = human((guint64)cached * 1024);
                char *sw = swapt
                    ? g_strdup_printf("스왑 %lu%% 사용",
                                      (swapt - swapf) * 100 / swapt)
                    : g_strdup("스왑 없음");
                char *d2 = g_strdup_printf("캐시 %s · %s", c2, sw);
                gtk_label_set_text(GTK_LABEL(app->mem_detail), d2);
                g_free(c2); g_free(sw); g_free(d2);
            }
        }
    }

    g_hash_table_destroy(wins);
    g_list_free(list);
    return G_SOURCE_CONTINUE;
}

/* ── CPU 그래프 ────────────────────────────────────────────────── */

static void draw_graph(GtkDrawingArea *area, cairo_t *cr,
                       int width, int height, gpointer data)
{
    (void)area;
    app_t *app = data;

    cairo_set_source_rgb(cr, 0.106, 0.106, 0.106);
    cairo_paint(cr);

    /* 25% 마다 눈금. 선이 없으면 40% 와 60% 를 눈으로 구별할 수 없다. */
    cairo_set_source_rgba(cr, 1, 1, 1, 0.07);
    cairo_set_line_width(cr, 1);
    for (int i = 1; i < 4; i++) {
        double y = height * i / 4.0;
        cairo_move_to(cr, 0, y);
        cairo_line_to(cr, width, y);
    }
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, 0.50, 0.72, 0.63);
    cairo_set_line_width(cr, 1.5);
    for (int i = 0; i < HISTORY; i++) {
        int    idx = (app->hist_at + i) % HISTORY;
        double x = (double)i * width / (HISTORY - 1);
        double y = height - (app->cpu_hist[idx] / 100.0) * height;
        if (i == 0) cairo_move_to(cr, x, y);
        else        cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);
}

/* ── 드라이브와 네트워크 ───────────────────────────────────────── */

static GtkWidget *build_drives(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);

    char *m = slurp("/proc/mounts");
    if (!m) return box;

    char **l = g_strsplit(m, "\n", -1);
    for (int i = 0; l[i]; i++) {
        char dev[128] = "", mnt[128] = "", type[64] = "";
        if (sscanf(l[i], "%127s %127s %63s", dev, mnt, type) != 3) continue;
        if (!g_str_has_prefix(dev, "/dev/")) continue;

        struct statvfs s;
        if (statvfs(mnt, &s) != 0) continue;
        guint64 tot = (guint64)s.f_blocks * s.f_frsize;
        guint64 fr  = (guint64)s.f_bavail * s.f_frsize;
        if (!tot) continue;

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

        char *a = human(tot - fr), *b = human(tot), *c = human(fr);
        char *head = g_strdup_printf("%s   %s · %s 중 %s 사용", mnt, dev, b, a);
        GtkWidget *h = gtk_label_new(head);
        gtk_widget_set_halign(h, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(row), h);
        g_free(head);

        GtkWidget *bar = gtk_progress_bar_new();
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar),
                                      1.0 - (double)fr / (double)tot);
        gtk_box_append(GTK_BOX(row), bar);

        /* 명세서 2-2: 숫자 옆에 해석을. 15% 미만은 행동으로 이어져야
         * 하는 상태이므로 문장이 바뀐다. */
        gboolean low = fr * 100 / tot < 15;
        char *note = low
            ? g_strdup_printf("%s 남음 · 15%% 아래입니다."
                              " 휴지통을 비우거나 큰 파일을 옮기십시오", c)
            : g_strdup_printf("%s 남음", c);
        GtkWidget *n = gtk_label_new(note);
        gtk_widget_set_halign(n, GTK_ALIGN_START);
        gtk_widget_add_css_class(n, low ? "lp-warning" : "dim-label");
        gtk_box_append(GTK_BOX(row), n);
        g_free(note); g_free(a); g_free(b); g_free(c);

        gtk_box_append(GTK_BOX(box), row);
    }
    g_strfreev(l);
    g_free(m);
    return box;
}

static GtkWidget *build_network(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);

    char *d = slurp("/proc/net/dev");
    if (!d) return box;

    char **l = g_strsplit(d, "\n", -1);
    for (int i = 2; l[i]; i++) {
        char iface[64] = "";
        unsigned long rx = 0, tx = 0;
        /* 형식: "  eth0: 12345 67 0 0 0 0 0 0 8901 23 ..."
         * 받은 바이트가 첫 숫자, 보낸 바이트가 아홉 번째다. */
        if (sscanf(l[i], " %63[^:]: %lu %*u %*u %*u %*u %*u %*u %*u %lu",
                   iface, &rx, &tx) != 3)
            continue;
        g_strstrip(iface);
        if (g_strcmp0(iface, "lo") == 0) continue;

        char *a = human(rx), *b = human(tx);
        char *t = g_strdup_printf("%s   받음 %s · 보냄 %s", iface, a, b);
        GtkWidget *lb = gtk_label_new(t);
        gtk_widget_set_halign(lb, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(box), lb);
        g_free(t); g_free(a); g_free(b);
    }
    g_strfreev(l);
    g_free(d);
    return box;
}

/* ── 창 ────────────────────────────────────────────────────────── */

static GtkWidget *scrolled(GtkWidget *child)
{
    GtkWidget *s = gtk_scrolled_window_new();
    /* 가로도 AUTOMATIC 이어야 한다. NEVER 는 "가로로 스크롤하지
     * 않는다" 가 아니라 "내용이 요구하는 만큼 넓어진다" 는 뜻이고,
     * 다섯 열이 고정 폭을 요구하는 이 표에서는 창이 제목 표시줄보다
     * 넓어져 오른쪽 모서리가 어긋난다. */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s), child);
    gtk_widget_set_vexpand(s, TRUE);
    return s;
}

static GtkWidget *meter_page(const char *title, GtkWidget **label,
                             GtkWidget **bar, GtkWidget **detail,
                             GtkWidget **graph, app_t *app)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);

    GtkWidget *t = gtk_label_new(title);
    gtk_widget_set_halign(t, GTK_ALIGN_START);
    gtk_widget_add_css_class(t, "lp-title");
    gtk_box_append(GTK_BOX(box), t);

    *label = gtk_label_new("—");
    gtk_widget_set_halign(*label, GTK_ALIGN_START);
    gtk_widget_add_css_class(*label, "lp-big");
    gtk_box_append(GTK_BOX(box), *label);

    if (graph) {
        *graph = gtk_drawing_area_new();
        gtk_widget_set_size_request(*graph, -1, 120);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(*graph),
                                       draw_graph, app, NULL);
        gtk_widget_set_margin_top(*graph, 10);
        gtk_box_append(GTK_BOX(box), *graph);

        GtkWidget *cap = gtk_label_new("지난 60초");
        gtk_widget_set_halign(cap, GTK_ALIGN_START);
        gtk_widget_add_css_class(cap, "dim-label");
        gtk_box_append(GTK_BOX(box), cap);
    }

    *bar = gtk_progress_bar_new();
    gtk_widget_set_margin_top(*bar, 10);
    gtk_box_append(GTK_BOX(box), *bar);

    *detail = gtk_label_new("");
    gtk_widget_set_halign(*detail, GTK_ALIGN_START);
    gtk_widget_add_css_class(*detail, "dim-label");
    gtk_widget_set_margin_top(*detail, 4);
    gtk_box_append(GTK_BOX(box), *detail);

    return box;
}

static void activate(GtkApplication *gapp, gpointer data)
{
    (void)data;
    app_t *app = g_new0(app_t, 1);
    app->procs = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                       NULL, g_free);

    GtkWidget *win = gtk_application_window_new(gapp);
    gtk_window_set_title(GTK_WINDOW(win), "작업 관리자");
    gtk_window_set_default_size(GTK_WINDOW(win), 820, 620);

    GtkWidget *head = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(win), head);

    GtkWidget *nb = gtk_notebook_new();

    table_init(&app->apps, "앱");
    table_init(&app->all,  "프로세스");

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), scrolled(app->apps.grid),
                             gtk_label_new("앱"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), scrolled(app->all.grid),
                             gtk_label_new("프로세스"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb),
        meter_page("CPU", &app->cpu_label, &app->cpu_bar, &app->cpu_detail,
                   &app->cpu_graph, app),
        gtk_label_new("CPU"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb),
        meter_page("메모리", &app->mem_label, &app->mem_bar, &app->mem_detail,
                   NULL, app),
        gtk_label_new("메모리"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), scrolled(build_drives()),
                             gtk_label_new("드라이브"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), scrolled(build_network()),
                             gtk_label_new("네트워크"));

    gtk_window_set_child(GTK_WINDOW(win), nb);

    /* 첫 표본을 바로 뜨게 한다. 첫 1.5초를 빈 표로 기다리면 앱이
     * 멈춘 것처럼 보인다. 그 표본의 CPU% 는 전부 0 인데, 그것은
     * 비교할 이전 값이 아직 없기 때문이고 곧 채워진다. */
    tick(app);
    g_timeout_add(REFRESH_MS, tick, app);

    gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new("org.lpzero.Tasks",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int r = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return r;
}
