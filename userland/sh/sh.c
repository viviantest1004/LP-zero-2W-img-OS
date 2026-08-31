/* sh.c - LP-zero 셸.
 *
 * 지원: 내장 명령, PATH 탐색, 리다이렉션(< > >>), 파이프(|).
 * 미지원: 잡 컨트롤, 변수 확장, 와일드카드, 서브셸.
 *   이것들은 셸 자체보다 큰 기능이라 필요해지면 그때 붙인다. */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define MAX_LINE   1024
#define MAX_ARGS   64
#define MAX_CMDS   8        /* 파이프라인 최대 단계 */
#define MAX_PIPES  8        /* && || ; 로 이어붙일 수 있는 파이프라인 수 */

typedef struct {
    char *argv[MAX_ARGS + 1];   /* execve 를 위해 NULL 로 끝난다 */
    int   argc;
    char *redir_in;
    char *redir_out;
    bool  out_append;
} cmd_t;

/* 앞선 파이프라인과 어떻게 이어지는가 */
typedef enum { LINK_NONE, LINK_AND, LINK_OR, LINK_SEQ } link_t;

typedef struct {
    cmd_t  cmds[MAX_CMDS];
    int    ncmds;
    link_t link;        /* 이 파이프라인 "앞"의 연결자 */
} pipeline_t;

static const char *DEFAULT_PATH = "/bin:/sbin:/usr/bin:/usr/sbin";
static bool shell_running = true;
static int  last_status   = 0;

/* ── 토크나이저 ───────────────────────────────────────────────────
 *
 * 토큰을 입력 버퍼에서 제자리로 자르지 않고 아레나에 복사한다.
 * 제자리 방식은 두 곳에서 깨진다:
 *   1) 구분자 자리에 종료 NUL 을 쓰면 다음 토큰의 시작을 잃는다
 *   2) "echo hi|cat" 처럼 단어 뒤에 바로 연산자가 오면 NUL 을 쓸 자리가
 *      연산자 자리뿐이라 연산자를 덮어쓴다
 * 아레나는 줄마다 초기화되고, 토큰은 execve 까지 살아 있어야 하므로
 * 정적으로 둔다. */

typedef enum {
    TOK_END,
    TOK_WORD,
    TOK_PIPE,
    TOK_AND,            /* && */
    TOK_OR,             /* || */
    TOK_SEMI,           /* ;  */
    TOK_REDIR_IN,
    TOK_REDIR_OUT,
    TOK_REDIR_APPEND,
} tok_type_t;

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static char   arena[MAX_LINE * 2];
static size_t arena_used;

/* 토큰 문자열을 아레나에 복사하고 그 포인터를 돌려준다. 부족하면 NULL. */
static char *arena_push(const char *src, size_t len)
{
    if (arena_used + len + 1 > sizeof(arena))
        return NULL;

    char *dst = &arena[arena_used];
    memcpy(dst, src, len);
    dst[len] = '\0';
    arena_used += len + 1;
    return dst;
}

/* 다음 토큰 하나를 읽는다. TOK_WORD 면 *word_out 에 아레나 포인터가 들어간다. */
static tok_type_t next_token(char **p, char **word_out)
{
    char *s = *p;

    while (is_space(*s)) s++;
    if (*s == '\0') { *p = s; return TOK_END; }

    /* 연산자는 그 자체로 토큰이다.
     * 두 글자짜리(&& ||)를 한 글자짜리(|)보다 먼저 본다. */
    if (*s == '&' && s[1] == '&') { *p = s + 2; return TOK_AND;  }
    if (*s == '|' && s[1] == '|') { *p = s + 2; return TOK_OR;   }
    if (*s == '|')                { *p = s + 1; return TOK_PIPE; }
    if (*s == ';')                { *p = s + 1; return TOK_SEMI; }
    if (*s == '<') { *p = s + 1; return TOK_REDIR_IN; }
    if (*s == '>') {
        if (s[1] == '>') { *p = s + 2; return TOK_REDIR_APPEND; }
        *p = s + 1;
        return TOK_REDIR_OUT;
    }

    /* 단어: 따옴표를 벗기면서 모은다 */
    char   buf[MAX_LINE];
    size_t n = 0;

    while (*s && !is_space(*s) &&
           *s != '|' && *s != '<' && *s != '>' && *s != ';' && *s != '&') {
        if (*s == '"' || *s == '\'') {
            char quote = *s++;
            while (*s && *s != quote) {
                if (n < sizeof(buf)) buf[n++] = *s;
                s++;
            }
            if (*s == quote) s++;       /* 닫는 따옴표. 없으면 줄 끝까지 */
        } else {
            if (n < sizeof(buf)) buf[n++] = *s;
            s++;
        }
    }

    *p = s;
    *word_out = arena_push(buf, n);
    if (!*word_out) {
        dprintf(STDERR_FILENO, "sh: 줄이 너무 깁니다\n");
        return TOK_END;
    }
    return TOK_WORD;
}

/* 한 줄을 파이프라인 목록으로 파싱한다.
 *
 *   cmd1 | cmd2 && cmd3 || cmd4 ; cmd5
 *
 * | 는 한 파이프라인 안의 단계를 잇고, && || ; 는 파이프라인 사이를 잇는다.
 * 반환: 파이프라인 개수, 빈 줄이면 0, 오류면 -1. */
static int parse_line(char *line, pipeline_t *pipes, int max_pipes)
{
    arena_used = 0;

    int np = 1;
    pipeline_t *pl = &pipes[0];
    memset(pl, 0, sizeof(*pl));
    pl->ncmds = 1;
    pl->link  = LINK_NONE;

    cmd_t *c = &pl->cmds[0];
    memset(c, 0, sizeof(*c));

    char *p = line;
    for (;;) {
        char      *word = NULL;
        tok_type_t t    = next_token(&p, &word);

        if (t == TOK_END)
            break;

        if (t == TOK_WORD) {
            if (c->argc >= MAX_ARGS) {
                dprintf(STDERR_FILENO, "sh: 인자가 너무 많습니다\n");
                return -1;
            }
            c->argv[c->argc++] = word;
            continue;
        }

        if (t == TOK_PIPE) {
            if (c->argc == 0) {
                dprintf(STDERR_FILENO, "sh: | 앞에 명령이 없습니다\n");
                return -1;
            }
            if (pl->ncmds >= MAX_CMDS) {
                dprintf(STDERR_FILENO, "sh: 파이프가 너무 깁니다 (최대 %d)\n",
                        MAX_CMDS);
                return -1;
            }
            c = &pl->cmds[pl->ncmds++];
            memset(c, 0, sizeof(*c));
            continue;
        }

        if (t == TOK_AND || t == TOK_OR || t == TOK_SEMI) {
            if (c->argc == 0) {
                dprintf(STDERR_FILENO, "sh: 연결자 앞에 명령이 없습니다\n");
                return -1;
            }
            if (np >= max_pipes) {
                dprintf(STDERR_FILENO,
                        "sh: 이어붙인 명령이 너무 많습니다 (최대 %d)\n",
                        max_pipes);
                return -1;
            }
            pl = &pipes[np++];
            memset(pl, 0, sizeof(*pl));
            pl->ncmds = 1;
            pl->link  = (t == TOK_AND) ? LINK_AND
                      : (t == TOK_OR)  ? LINK_OR : LINK_SEQ;
            c = &pl->cmds[0];
            memset(c, 0, sizeof(*c));
            continue;
        }

        /* 리다이렉션: 바로 뒤에 파일 이름이 와야 한다 */
        char *file = NULL;
        if (next_token(&p, &file) != TOK_WORD) {
            dprintf(STDERR_FILENO, "sh: 리다이렉션 뒤에 파일이 없습니다\n");
            return -1;
        }
        if (t == TOK_REDIR_IN) {
            c->redir_in = file;
        } else {
            c->redir_out  = file;
            c->out_append = (t == TOK_REDIR_APPEND);
        }
    }

    /* argv 를 NULL 로 마감 */
    for (int i = 0; i < np; i++)
        for (int j = 0; j < pipes[i].ncmds; j++)
            pipes[i].cmds[j].argv[pipes[i].cmds[j].argc] = NULL;

    /* 마지막 파이프라인이 비어 있으면(줄이 연결자로 끝남) 버린다 */
    while (np > 0 && pipes[np - 1].cmds[0].argc == 0)
        np--;

    return np;
}

/* ── 내장 명령 ──────────────────────────────────────────────────── */

static void builtin_help(void)
{
    printf("LP-zero shell\n\n");
    printf("  cd [dir]      디렉터리 이동 (인자 없으면 /)\n");
    printf("  pwd           현재 디렉터리\n");
    printf("  echo ...      인자 출력\n");
    printf("  env           환경변수 목록\n");
    printf("  exit [n]      셸 종료\n");
    printf("  reboot        재부팅\n");
    printf("  poweroff      전원 끄기\n");
    printf("  help          이 도움말\n\n");
    printf("리다이렉션 < > >> , 파이프 | , 연결자 && || ; 를 쓸 수 있습니다.\n");
}

static const char *BUILTINS[] = {
    "exit", "cd", "pwd", "echo", "env", "help", "reboot", "poweroff", NULL
};

static bool is_builtin(const char *name)
{
    for (int i = 0; BUILTINS[i]; i++)
        if (strcmp(name, BUILTINS[i]) == 0)
            return true;
    return false;
}

/* 내장이면 처리하고 true. 종료 코드는 last_status 에 남긴다. */
static bool run_builtin(cmd_t *c)
{
    const char *cmd = c->argv[0];

    if (strcmp(cmd, "exit") == 0) {
        shell_running = false;
        last_status = (c->argc > 1) ? atoi(c->argv[1]) : 0;
        return true;
    }

    if (strcmp(cmd, "cd") == 0) {
        const char *dir = (c->argc > 1) ? c->argv[1] : "/";
        long r = lp_chdir(dir);
        if (r < 0) {
            dprintf(STDERR_FILENO, "cd: %s: 이동할 수 없습니다 (%ld)\n", dir, -r);
            last_status = 1;
        } else {
            last_status = 0;
        }
        return true;
    }

    if (strcmp(cmd, "pwd") == 0) {
        char buf[512];
        long r = lp_getcwd(buf, sizeof(buf));
        if (r < 0) { dprintf(STDERR_FILENO, "pwd: 실패 (%ld)\n", -r); last_status = 1; }
        else       { printf("%s\n", buf); last_status = 0; }
        return true;
    }

    if (strcmp(cmd, "echo") == 0) {
        for (int i = 1; i < c->argc; i++)
            printf("%s%s", c->argv[i], (i + 1 < c->argc) ? " " : "");
        printf("\n");
        last_status = 0;
        return true;
    }

    if (strcmp(cmd, "env") == 0) {
        if (environ)
            for (char **e = environ; *e; e++)
                printf("%s\n", *e);
        last_status = 0;
        return true;
    }

    if (strcmp(cmd, "help") == 0) {
        builtin_help();
        last_status = 0;
        return true;
    }

    if (strcmp(cmd, "reboot") == 0 || strcmp(cmd, "poweroff") == 0) {
        printf("%s...\n", cmd);
        lp_sync();
        int op = (strcmp(cmd, "reboot") == 0)
                 ? LINUX_REBOOT_CMD_RESTART : LINUX_REBOOT_CMD_POWER_OFF;
        long r = lp_reboot(op);
        dprintf(STDERR_FILENO, "%s: 실패 (%ld) - 권한이 있습니까?\n", cmd, -r);
        last_status = 1;
        return true;
    }

    return false;
}

/* ── 외부 명령 ──────────────────────────────────────────────────── */

/* PATH 를 뒤져 실행 파일 경로를 buf 에 채운다. 찾으면 true. */
static bool resolve_path(const char *cmd, char *buf, size_t size)
{
    if (strchr(cmd, '/')) {                 /* 경로가 주어졌으면 그대로 */
        strlcpy(buf, cmd, size);
        return lp_exists(buf);
    }

    const char *path = getenv("PATH");
    if (!path || !*path)
        path = DEFAULT_PATH;

    while (*path) {
        const char *end = strchr(path, ':');
        size_t dlen = end ? (size_t)(end - path) : strlen(path);

        if (dlen && dlen + 1 + strlen(cmd) + 1 <= size) {
            memcpy(buf, path, dlen);
            buf[dlen] = '/';
            strlcpy(buf + dlen + 1, cmd, size - dlen - 1);
            if (lp_exists(buf))
                return true;
        }

        if (!end) break;
        path = end + 1;
    }
    return false;
}

/* 자식 안에서 리다이렉션을 적용한다. 실패하면 종료. */
static void apply_redirects(cmd_t *c)
{
    if (c->redir_in) {
        long fd = lp_open(c->redir_in, O_RDONLY, 0);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "sh: %s: 열 수 없습니다\n", c->redir_in);
            lp_exit(1);
        }
        lp_dup2((int)fd, STDIN_FILENO);
        lp_close((int)fd);
    }

    if (c->redir_out) {
        int flags = O_WRONLY | O_CREAT | (c->out_append ? O_APPEND : O_TRUNC);
        long fd = lp_open(c->redir_out, flags, 0644);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "sh: %s: 만들 수 없습니다\n", c->redir_out);
            lp_exit(1);
        }
        lp_dup2((int)fd, STDOUT_FILENO);
        lp_close((int)fd);
    }
}

/* 파이프라인 실행. 각 단계를 fork 하고 파이프로 잇는다. */
static void run_pipeline(cmd_t *cmds, int n)
{
    int  prev_read = -1;      /* 이전 단계에서 넘겨받는 읽기 끝 */
    pid_t pids[MAX_CMDS];
    int  npids = 0;

    for (int i = 0; i < n; i++) {
        int pipefd[2] = { -1, -1 };
        bool last = (i == n - 1);

        if (!last && lp_pipe(pipefd) < 0) {
            dprintf(STDERR_FILENO, "sh: 파이프 생성 실패\n");
            break;
        }

        pid_t pid = lp_fork();
        if (pid < 0) {
            dprintf(STDERR_FILENO, "sh: fork 실패\n");
            if (pipefd[0] >= 0) { lp_close(pipefd[0]); lp_close(pipefd[1]); }
            break;
        }

        if (pid == 0) {
            /* ── 자식 ── */
            if (prev_read >= 0) {
                lp_dup2(prev_read, STDIN_FILENO);
                lp_close(prev_read);
            }
            if (!last) {
                lp_close(pipefd[0]);             /* 쓰기만 한다 */
                lp_dup2(pipefd[1], STDOUT_FILENO);
                lp_close(pipefd[1]);
            }

            apply_redirects(&cmds[i]);           /* 리다이렉션이 파이프보다 우선 */

            char path[512];
            if (!resolve_path(cmds[i].argv[0], path, sizeof(path))) {
                dprintf(STDERR_FILENO, "sh: %s: 명령을 찾을 수 없습니다\n",
                        cmds[i].argv[0]);
                lp_exit(127);
            }

            long e = lp_execve(path, cmds[i].argv, environ);
            dprintf(STDERR_FILENO, "sh: %s: 실행할 수 없습니다 (%ld)\n",
                    path, -e);
            lp_exit(126);
        }

        /* ── 부모 ── */
        pids[npids++] = pid;

        if (prev_read >= 0)
            lp_close(prev_read);
        if (!last) {
            lp_close(pipefd[1]);                 /* 부모는 쓰지 않는다 */
            prev_read = pipefd[0];
        }
    }

    if (prev_read >= 0)
        lp_close(prev_read);

    /* 모든 단계를 기다린다. 마지막 단계의 상태가 파이프라인 상태. */
    for (int i = 0; i < npids; i++) {
        int status = 0;
        lp_waitpid(pids[i], &status, 0);
        if (i == npids - 1)
            last_status = LP_WIFEXITED(status) ? LP_WEXITSTATUS(status)
                                               : 128 + LP_WTERMSIG(status);
    }
}

/* 내장 명령을 리다이렉션과 함께 실행한다.
 *
 * 내장은 셸 프로세스 안에서 돌아야 한다 - cd 를 자식에서 하면 부모의
 * 디렉터리가 안 바뀌어 의미가 없다. 그래서 fork 대신 셸 자신의 fd 를
 * 잠깐 바꿔치기하고 끝나면 되돌린다. */
static void run_builtin_redirected(cmd_t *c)
{
    int saved_in  = -1;
    int saved_out = -1;

    if (c->redir_in) {
        long fd = lp_open(c->redir_in, O_RDONLY, 0);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "sh: %s: 열 수 없습니다 (%ld)\n",
                    c->redir_in, -fd);
            last_status = 1;
            return;
        }
        saved_in = (int)lp_dup(STDIN_FILENO);
        lp_dup2((int)fd, STDIN_FILENO);
        lp_close((int)fd);
    }

    if (c->redir_out) {
        int flags = O_WRONLY | O_CREAT | (c->out_append ? O_APPEND : O_TRUNC);
        long fd = lp_open(c->redir_out, flags, 0644);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "sh: %s: 만들 수 없습니다 (%ld)\n",
                    c->redir_out, -fd);
            if (saved_in >= 0) {
                lp_dup2(saved_in, STDIN_FILENO);
                lp_close(saved_in);
            }
            last_status = 1;
            return;
        }
        saved_out = (int)lp_dup(STDOUT_FILENO);
        lp_dup2((int)fd, STDOUT_FILENO);
        lp_close((int)fd);
    }

    run_builtin(c);

    if (saved_in >= 0)  { lp_dup2(saved_in,  STDIN_FILENO);  lp_close(saved_in); }
    if (saved_out >= 0) { lp_dup2(saved_out, STDOUT_FILENO); lp_close(saved_out); }
}

/* ── 메인 루프 ──────────────────────────────────────────────────── */

static void print_prompt(void)
{
    char cwd[256];
    if (lp_getcwd(cwd, sizeof(cwd)) < 0)
        strlcpy(cwd, "?", sizeof(cwd));

    /* 종료 코드가 0 이 아니면 프롬프트에 표시한다 */
    if (last_status)
        printf("[%d] %s $ ", last_status, cwd);
    else
        printf("%s $ ", cwd);
}

int main(int argc, char **argv)
{
    char       line[MAX_LINE];
    pipeline_t pipes[MAX_PIPES];

    /* 인자로 파일이 주어지면 그 안의 명령을 순서대로 실행한다.
     * /etc/rc 같은 부팅 스크립트를 위한 것이다. 이때는 프롬프트를
     * 찍지 않는다. */
    int  input_fd  = STDIN_FILENO;
    bool interactive = true;

    if (argc > 1) {
        long fd = lp_open(argv[1], O_RDONLY, 0);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "sh: %s: 열 수 없습니다 (%ld)\n",
                    argv[1], -fd);
            return 127;
        }
        input_fd    = (int)fd;
        interactive = false;
    }

    while (shell_running) {
        if (interactive)
            print_prompt();

        long len = readline(input_fd, line, sizeof(line));
        if (len < 0) {              /* EOF (Ctrl-D 또는 파일 끝) */
            if (interactive)
                printf("\n");
            break;
        }
        if (len == 0)
            continue;

        /* 스크립트에서는 주석을 건너뛴다 */
        if (line[0] == '#')
            continue;

        int np = parse_line(line, pipes, MAX_PIPES);
        if (np <= 0)
            continue;

        for (int i = 0; i < np && shell_running; i++) {
            /* 단락 평가: && 는 앞이 성공했을 때만, || 는 실패했을 때만 */
            if (pipes[i].link == LINK_AND && last_status != 0) continue;
            if (pipes[i].link == LINK_OR  && last_status == 0) continue;

            cmd_t *cmds = pipes[i].cmds;
            int    n    = pipes[i].ncmds;

            /* 내장 명령은 파이프라인이 한 단계일 때만 셸 안에서 실행한다.
             * 파이프의 일부일 때는 자식에서 도는 외부 명령으로 취급한다
             * (그래야 stdin/stdout 연결이 자연스럽다). */
            if (n == 1 && cmds[0].argc > 0 && is_builtin(cmds[0].argv[0]))
                run_builtin_redirected(&cmds[0]);
            else
                run_pipeline(cmds, n);
        }
    }

    if (input_fd != STDIN_FILENO)
        lp_close(input_fd);

    return last_status;
}
