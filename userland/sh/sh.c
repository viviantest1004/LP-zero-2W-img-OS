/* sh.c - the LP-zero shell.
 *
 * Supported: builtins, PATH search, redirection (< > >>), pipes (|).
 * Not supported: job control, variable expansion, globbing, subshells.
 *   Each is bigger than the shell itself; they go in when needed. */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define MAX_LINE   1024
#define MAX_ARGS   64
#define MAX_CMDS   8        /* stages in one pipeline */
#define MAX_PIPES  8        /* pipelines joined by && || ; */

/* linux_dirent64 offsets, for tab completion (the same ones ls uses) */
#define DIRENT_RECLEN 16
#define DIRENT_TYPE   18
#define DIRENT_NAME   19
#define DT_DIR        4

typedef struct {
    char *argv[MAX_ARGS + 1];   /* NULL terminated, for execve */
    int   argc;
    char *redir_in;
    char *redir_out;
    bool  out_append;
    /* Where the error output goes. NULL means "leave it alone"; the
     * literal string "&1" means "wherever stdout ended up", which is
     * the 2>&1 everybody types and which this shell used to answer with
     * "no file after the redirection". */
    char *redir_err;
    bool  err_append;
    /* `cmd <<END` 로 온 본문의 번호. -1 이면 없다. 본문은 읽는 쪽에서
     * 미리 모아 두고 여기에는 번호만 온다 - 그 시점에 파일이 없기
     * 때문이다. */
    int   heredoc;
} cmd_t;

/* How this pipeline joins onto the one before it */
typedef enum { LINK_NONE, LINK_AND, LINK_OR, LINK_SEQ } link_t;

typedef struct {
    cmd_t  cmds[MAX_CMDS];
    int    ncmds;
    link_t link;        /* the operator in front of this pipeline */
    bool   background;  /* it ended in & - do not wait for it */
    /* `! cmd` - 결과를 뒤집는다. `if ! test -f x ; then` 이 그 쓰임의
     * 거의 전부이고, 없으면 셸이 "!: command not found" 를 찍은 뒤
     * **참으로** 이어 간다 - 조건이 통째로 뒤집힌 채 계속 도는 것이라
     * 오류 한 줄보다 훨씬 나쁘다. */
    bool   negate;
} pipeline_t;

/* /data/bin is on the data partition. Large programs such as Python live
 * there rather than in the system image (the initramfs inside the kernel). */
static const char *DEFAULT_PATH = "/bin:/data/bin:/sbin:/usr/bin:/usr/sbin";

/* Home. It is a bind mount from the data partition, so what is saved
 * there survives a reboot - unlike the rest of the root filesystem. */
#define HOME_DIR "/root"
static bool shell_running = true;

/* ── break and continue ──────────────────────────────────────────
 *
 * The shell had `while` and `for` and no way out of either. Every loop
 * ran to its natural end, which for `while` meant until its condition
 * turned false and for `for` meant every word - so "look for something,
 * stop when you find it" could not be written at all. The self-test hit
 * this: a loop waiting for a message to appear in the log kept spinning
 * after it had appeared, and reported the time it gave up rather than
 * the time it found it.
 *
 * A counter rather than a flag, because `break 2` should leave two
 * loops. Set by the builtin, cleared by the loop that consumes it, and
 * checked after every statement in a block - which is what makes it
 * stop the rest of the body too, not just the next round. */
static int loop_break;        /* how many loops still to leave */
static int loop_continue;     /* how many to skip to the next round of */
static int  last_status   = 0;

/* ── Tokenizer ────────────────────────────────────────────────────
 *
 * Tokens are copied into an arena rather than cut in place in the input
 * buffer. Cutting in place breaks in two ways:
 *   1) writing the terminating NUL over a separator loses where the next
 *      token starts;
 *   2) in "echo hi|cat" the only place to put the NUL is the operator
 *      itself, so the operator is destroyed.
 * The arena is reset per line, and tokens must live until execve, so it
 * is static. */

typedef enum {
    TOK_END,
    TOK_WORD,
    TOK_PIPE,
    TOK_AND,            /* && */
    TOK_OR,             /* || */
    TOK_SEMI,           /* ;  */
    TOK_AMP,            /* &  - run the pipeline before it in the background */
    TOK_REDIR_IN,
    TOK_REDIR_HERE,
    TOK_REDIR_OUT,
    TOK_REDIR_APPEND,
    TOK_REDIR_ERR,          /* 2>   */
    TOK_REDIR_ERR_APPEND,   /* 2>>  */
} tok_type_t;

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static bool is_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static char   arena[8192];
static size_t arena_used;

/* Copy a token into the arena and return the pointer. NULL if it is full. */
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

/* ── here-document ────────────────────────────────────────────────────
 *
 *     cat <<END
 *     ...
 *     END
 *
 * 없을 때는 `<` 두 개로 읽혀서 "sh: < needs a file after it" 이 나고
 * 본문의 줄들이 하나씩 명령으로 실행됐다. usage() 를 가진 스크립트는
 * 거의 다 이 꼴을 쓰므로 --help 하나가 스크립트 전체를 망가뜨렸다.
 *
 * 본문은 파일이 아니라 파이프로 넘긴다. /tmp 가 쓸 수 있는지 모르는
 * 시점(부팅 초기)에도 돌아야 하고, 지울 임시 파일도 남지 않는다.
 * 파이프 버퍼(보통 64KB)보다 큰 본문은 잘린다고 말해 준다 - 조용히
 * 멈추는 것보다 낫다. */
#define MAX_HEREDOCS 256
#define HEREDOC_MAX  (60 * 1024)

/* 본문은 셸이 살아 있는 동안 놓지 않는다.
 *
 * 처음에는 줄 하나를 실행하고 나면 놓았고, 그러면 함수 안의
 * here-document 가 두 번째 호출부터 사라졌다: 본문을 모으는 것은
 * 함수를 **정의할** 때이고 쓰는 것은 **부를** 때인데, 그 사이에
 * 놓아 버리기 때문이다. usage() 를 한 번 부르면 그 다음부터는
 * 도움말이 비어 있는 셸이 된다.
 *
 * 그래서 놓지 않는다. 값은 본문 몇 KB 짜리 256개가 상한이고, 그
 * 정도는 이 셸이 도는 어떤 기계에서도 문제가 아니다. */
typedef struct {
    char *body;
    bool  expand;       /* 구분자에 따옴표가 없었다 */
} heredoc_t;

static heredoc_t heredocs[MAX_HEREDOCS];
static int       nheredocs = 0;

/* Set by next_token, read by parse_line: a quoted word is never expanded
 * into file names, and a word with no * or ? never needs looking at.
 * tok_from_cmd marks a word that came out of $(...) unquoted, which is
 * the one case where a word has to be split on its own whitespace. */
static bool tok_quoted   = false;
static bool tok_has_glob = false;
static bool tok_from_cmd = false;
/* Where the word's first quote fell, as an offset into the finished
 * word; SIZE_MAX if it had none.
 *
 * This exists for one thing: telling out="" from "out=". Both are words
 * containing a quote, and for a long time the shell refused both as
 * assignments on that ground - so `out=""` ran as a command and printed
 * "sh: out=: command not found", which is what the top bar's mode label
 * did on every refresh. What decides an assignment is whether the NAME
 * is quoted, not whether the value is; the value is quoted nearly every
 * time somebody writes one. */
static size_t tok_quote_at = (size_t)-1;

static void run_logical_line(char *line);

/* ── Positional parameters ────────────────────────────────────────────
 *
 * $1 to $9, $# and $@, set two ways: by the arguments to a script, and
 * by calling a shell function. A function saves and restores them, so a
 * function called from a script does not lose the script's own.
 *
 * They are not environment variables. Exporting them would hand $1 to
 * every program the script runs, which is not what a positional
 * parameter is - it belongs to this shell and this call. */
#define MAX_POSITIONAL 32
static char pos_args[MAX_POSITIONAL][256];
static int  pos_count = 0;

/* ── set -e / set -x ──────────────────────────────────────────────────
 *
 * These are here for the same reason case is: every Debian maintainer
 * script starts with `set -e`, and a shell that answers
 * `sh: set: command not found` to that line has told the script it is
 * not going to stop on failure - and then does not stop on failure,
 * which is exactly the situation `set -e` exists to prevent. A postinst
 * that half-fails and reports success leaves a package marked installed
 * that is not.
 *
 * errexit_off is a depth rather than a flag because the places it has to
 * be suspended nest: the condition of an `if` inside the condition of a
 * `while`. POSIX exempts those - a failing test is how a conditional
 * says "no", not a failure - along with anything on the left of && or
 * ||, which run_logical_line already collapses into one status. */
static bool opt_errexit = false;
static bool opt_xtrace  = false;
static int  errexit_off = 0;
static bool shell_interactive = true;
static char script_name[128] = "sh";

/* ── Shell functions ──────────────────────────────────────────────────
 *
 *   name() {
 *       echo "$1"
 *   }
 *
 * The body is kept as the lines it was written on, and running the
 * function feeds them back through the same block executor that runs
 * everything else - so a function can contain if, while, for, pipelines
 * and other functions without any of that being written twice.
 *
 * A function runs in this shell, not a child. That is the whole point of
 * having them: cd, variable assignments and exit codes are meant to be
 * visible to the caller.
 *
 * The size limits are deliberate rather than dynamic. This shell has a
 * fixed arena for words and fixed tables for jobs and pipelines, and a
 * script that needs more than sixteen functions of forty lines is one
 * that wants Python, which is on the data partition. */
/* 40줄이었다. 그 숫자가 어디서 걸리는지는 dpkg 가 알려 줬다:
 * dpkg-maintscript-helper 의 dir_to_symlink() 가 158줄이고, 그것이
 * 잘리면 함수 한가운데가 사라진 채로 실행돼서 "sh: while without do
 * or done" 같은, 원인과 아무 상관 없어 보이는 말이 나온다. 그러면
 * 그 패키지는 설치되지 않고, 왜 안 되는지는 아무 데도 적히지 않는다.
 *
 * 본문을 char[MAX_LINE] 배열이 아니라 strdup 한 포인터로 들고 있으니
 * 한도를 키워도 값이 비싸지 않다: 쓰지 않는 함수 자리는 포인터 여덟
 * 바이트다. 예전 방식이었다면 64×256×1024 = 16MB 를 BSS 에 늘 잡고
 * 있어야 했다. */
#define MAX_FUNCS       64
#define MAX_FUNC_LINES  256

typedef struct {
    char  name[64];
    char *lines[MAX_FUNC_LINES];      /* strdup - 쓴 만큼만 */
    int   nlines;
} func_t;

static func_t funcs[MAX_FUNCS];
static int    nfuncs = 0;

static void call_func(func_t *f, char **argv, int argc);

static func_t *func_find(const char *name)
{
    for (int i = 0; i < nfuncs; i++)
        if (strcmp(funcs[i].name, name) == 0)
            return &funcs[i];
    return NULL;
}

/* Replace $NAME, ${NAME} or $? starting at *sp. Returns the new length.
 *
 * An undefined variable becomes nothing at all, which is what every other
 * shell does and what makes "cd $SOMEWHERE" fail loudly rather than
 * quietly doing something else. */
/* The ')' that closes a '$(' - counting nesting, and not being fooled
 * by a bracket inside quotes. */
static const char *matching_paren(const char *s)
{
    int  depth = 1;
    bool in_single = false, in_double = false;

    for (; *s; s++) {
        if (in_single) { if (*s == '\'') in_single = false; continue; }
        if (in_double) { if (*s == '"')  in_double = false; continue; }
        if (*s == '\'') { in_single = true;  continue; }
        if (*s == '"')  { in_double = true;  continue; }
        if (*s == '(')  depth++;
        else if (*s == ')' && --depth == 0) return s;
    }
    return NULL;
}

/* Run a command and put what it printed into the word being built.
 *
 * The command runs in a forked copy of this shell with its output on a
 * pipe, which is why $(...) can hold anything a line can hold -
 * pipelines, redirections, another $(...) - without a second parser.
 *
 * Trailing newlines are dropped and the rest turn into spaces, which is
 * what every shell does: "for f in $(ls)" is meant to loop over names,
 * not over one string with newlines in it. */
static size_t expand_command(const char *cmd, char *buf, size_t n, size_t size)
{
    int fds[2];
    if (lp_pipe(fds) < 0)
        return n;

    pid_t pid = lp_fork();
    if (pid < 0) {
        lp_close(fds[0]);
        lp_close(fds[1]);
        return n;
    }

    if (pid == 0) {
        lp_close(fds[0]);
        lp_dup2(fds[1], STDOUT_FILENO);
        lp_close(fds[1]);
        /* Ctrl-C has to reach what is inside the brackets. The
         * interactive shell ignores it; this child must not. */
        lp_signal_default(SIGINT);
        lp_signal_default(SIGQUIT);

        char copy[MAX_LINE];
        strlcpy(copy, cmd, sizeof copy);
        run_logical_line(copy);
        lp_exit(last_status);
    }

    lp_close(fds[1]);

    static char out[8192];
    size_t got = 0;
    for (;;) {
        long r = lp_read(fds[0], out + got, sizeof out - got);
        if (r <= 0) break;
        got += (size_t)r;
        if (got >= sizeof out) break;
    }
    lp_close(fds[0]);

    int status = 0;
    lp_waitpid(pid, &status, 0);

    while (got > 0 && (out[got - 1] == '\n' || out[got - 1] == '\r'))
        got--;

    for (size_t i = 0; i < got && n < size; i++)
        buf[n++] = (out[i] == '\n' || out[i] == '\r' || out[i] == '\t')
                   ? ' ' : out[i];
    return n;
}

/* `command` - the older spelling of $(command).
 *
 * $(...) was added and this was not, which meant the form most people
 * actually type went through as literal text: `kill -9 `pidof guard``
 * passed the backticks and the word "pidof" to kill as arguments, and
 * kill said it could not find a process called "`pidof". Nothing about
 * that error points at the real problem, which is the worst kind.
 *
 * No nesting: backticks cannot nest without escaping, which is the
 * reason $(...) exists. The closing backtick is the next one that is
 * not preceded by a backslash. */
static size_t expand_backtick(char **sp, char *buf, size_t n, size_t size)
{
    char *s = *sp + 1;              /* step over the opening ` */

    const char *close = NULL;
    for (const char *c = s; *c; c++) {
        if (*c == '\\' && c[1]) { c++; continue; }
        if (*c == '`') { close = c; break; }
    }

    if (!close) {
        dprintf(STDERR_FILENO, "sh: ` with no closing `\n");
        *sp = s;
        return n;
    }

    char cmd[MAX_LINE];
    size_t len = (size_t)(close - s);
    if (len >= sizeof cmd) len = sizeof cmd - 1;
    memcpy(cmd, s, len);
    cmd[len] = '\0';

    size_t before = n;
    n = expand_command(cmd, buf, n, size);

    for (size_t i = before; i < n; i++)
        if (buf[i] == ' ') { tok_from_cmd = true; break; }

    *sp = (char *)close + 1;
    return n;
}

/* The pid of the most recent background job, which is what $! is.
 * Declared up here because expand_dollar reads it and job_add - which
 * sets it - comes much further down. */
static pid_t last_bg_pid = 0;

static bool   glob_match(const char *pat, const char *name);
static size_t expand_dollar(char **sp, char *buf, size_t n, size_t size);

/* 문자열 하나를 확장해서 out 에 담는다. ${VAR:-$OTHER} 처럼 기본값
 * 자리에 또 다른 확장이 오는 경우를 위해 있다. */
static void expand_str(const char *in, char *out, size_t size)
{
    size_t n = 0;
    char  *p = (char *)in;
    while (*p && n + 1 < size) {
        if (*p == '$') {
            n = expand_dollar(&p, out, n, size - 1);
        } else if (*p == '\'' || *p == '"') {
            char q = *p++;
            while (*p && *p != q) {
                if (n + 1 < size) out[n++] = *p;
                p++;
            }
            if (*p == q) p++;
        } else {
            out[n++] = *p++;
        }
    }
    out[n] = '\0';
}

/* ${VAR#pat} 와 ${VAR%pat} 는 앞뒤를 깎는다. shortest 면 가장 짧게
 * 맞는 것을, 아니면 가장 길게 맞는 것을 뗀다. */
static void strip_prefix(char *val, const char *pat, bool longest)
{
    size_t len = strlen(val);
    size_t best = 0;
    bool   found = false;
    for (size_t i = 0; i <= len; i++) {
        char save = val[i];
        val[i] = '\0';
        bool hit = glob_match(pat, val);
        val[i] = save;
        if (hit) {
            best = i;
            found = true;
            if (!longest) break;
        }
    }
    if (found && best)
        memmove(val, val + best, len - best + 1);
}

static void strip_suffix(char *val, const char *pat, bool longest)
{
    size_t len = strlen(val);
    size_t best = len;
    bool   found = false;
    for (size_t i = len + 1; i-- > 0; ) {
        if (glob_match(pat, val + i)) {
            best = i;
            found = true;
            if (!longest) break;
        }
    }
    if (found && best < len)
        val[best] = '\0';
}

static size_t expand_dollar(char **sp, char *buf, size_t n, size_t size)
{
    char *s = *sp + 1;              /* step over the $ */

    if (*s == '(') {                /* $(command) */
        const char *close = matching_paren(s + 1);
        if (close) {
            char cmd[MAX_LINE];
            size_t len = (size_t)(close - (s + 1));
            if (len >= sizeof cmd) len = sizeof cmd - 1;
            memcpy(cmd, s + 1, len);
            cmd[len] = '\0';

            size_t before = n;
            n = expand_command(cmd, buf, n, size);

            /* Only mark it splittable if it actually produced a gap to
             * split on - otherwise every substitution would go through
             * the splitter for nothing. */
            for (size_t i = before; i < n; i++)
                if (buf[i] == ' ') { tok_from_cmd = true; break; }

            *sp = (char *)close + 1;
            return n;
        }
        dprintf(STDERR_FILENO, "sh: $( without )\n");
        *sp = s + 1;
        return n;
    }

    if (*s >= '0' && *s <= '9') {   /* $0 .. $9 */
        int idx = *s - '0';
        const char *val = "";
        if (idx == 0)                    val = script_name;
        else if (idx <= pos_count)       val = pos_args[idx - 1];
        while (*val && n < size) buf[n++] = *val++;
        *sp = s + 1;
        return n;
    }

    if (*s == '!') {                /* the last background job's pid */
        /* Without this there is no way to name a job you just started.
         * The obvious workaround - `kill \`pidof sh\`` - kills every
         * shell on the machine including the script doing the killing,
         * which is how the self-test kept ending halfway through with
         * no error. */
        char num[16];
        snprintf(num, sizeof num, "%d", (int)last_bg_pid);
        for (char *q = num; *q && n < size; q++) buf[n++] = *q;
        *sp = s + 1;
        return n;
    }

    if (*s == '#') {                /* how many there are */
        char num[16];
        snprintf(num, sizeof num, "%d", pos_count);
        for (char *q = num; *q && n < size; q++) buf[n++] = *q;
        *sp = s + 1;
        return n;
    }

    if (*s == '@' || *s == '*') {   /* all of them */
        for (int i = 0; i < pos_count; i++) {
            if (i && n < size) buf[n++] = ' ';
            for (const char *v = pos_args[i]; *v && n < size; v++)
                buf[n++] = *v;
        }
        if (pos_count > 1)
            tok_from_cmd = true;     /* several words, not one */
        *sp = s + 1;
        return n;
    }

    if (*s == '?') {                /* the last exit code */
        char num[16];
        snprintf(num, sizeof(num), "%d", last_status);
        for (char *q = num; *q && n < size; q++) buf[n++] = *q;
        *sp = s + 1;
        return n;
    }

    /* ── ${...} ──────────────────────────────────────────────────
     *
     * 여기 있는 것들이 없을 때 무슨 일이 났는지 적어 둔다: 셸은
     * ${FOO:-기본값} 에서 $FOO 만 확장하고 나머지를 **글자 그대로**
     * 남겼다. 그래서 값이 없을 때 `:-기본값}` 이라는 문자열이 변수에
     * 들어갔다. 오류가 아니라 쓰레기가 들어가는 쪽이라 훨씬 나쁘다.
     *
     *   ${VAR}          그냥 값
     *   ${#VAR}         길이
     *   ${VAR:-w}       없거나 비었으면 w      ${VAR-w}   없으면 w
     *   ${VAR:=w}       그리고 그 값을 놓는다  ${VAR=w}
     *   ${VAR:+w}       있고 비지 않았으면 w   ${VAR+w}
     *   ${VAR:?w}       없으면 w 를 찍고 만다  ${VAR?w}
     *   ${VAR#pat}      앞에서 짧게 깎기       ${VAR##pat}  길게
     *   ${VAR%pat}      뒤에서 짧게 깎기       ${VAR%%pat}  길게
     */
    bool braced = (*s == '{');
    if (!braced) {
        char name[64];
        size_t k = 0;
        while (is_name_char(*s) && k < sizeof(name) - 1)
            name[k++] = *s++;
        name[k] = '\0';

        if (k == 0) {               /* a bare $: leave it as typed */
            if (n < size) buf[n++] = '$';
            *sp = s;
            return n;
        }
        const char *val = getenv(name);
        if (val)
            while (*val && n < size) buf[n++] = *val++;
        *sp = s;
        return n;
    }

    s++;                            /* step over the { */

    bool want_len = false;
    if (*s == '#' && (is_name_char(s[1]) || s[1] == '@' || s[1] == '*')) {
        want_len = true;
        s++;
    }

    char   name[64];
    size_t k = 0;
    while (is_name_char(*s) && k < sizeof(name) - 1)
        name[k++] = *s++;
    name[k] = '\0';

    /* ${1}, ${@}, ${#} - 이름이 아닌 것들. */
    char special = '\0';
    if (k == 0 && (*s == '@' || *s == '*' || *s == '?' || *s == '!' ||
                   *s == '#'))
        special = *s++;

    /* 연산자. */
    char op[3] = { 0, 0, 0 };
    if (*s == ':' && (s[1] == '-' || s[1] == '=' || s[1] == '+' ||
                      s[1] == '?')) {
        op[0] = ':'; op[1] = s[1]; s += 2;
    } else if (*s == '-' || *s == '=' || *s == '+' || *s == '?') {
        op[0] = *s++;
    } else if (*s == '#' || *s == '%') {
        op[0] = *s++;
        if (*s == op[0]) op[1] = *s++;
    }

    /* 연산자의 오른쪽. 짝이 되는 } 까지. */
    char   word[MAX_LINE];
    size_t wn = 0;
    int    depth = 1;
    while (*s) {
        if (*s == '{') depth++;
        else if (*s == '}') { if (--depth == 0) break; }
        if (wn + 1 < sizeof word) word[wn++] = *s;
        s++;
    }
    word[wn] = '\0';
    if (*s == '}') s++;

    /* 값을 고른다. */
    char        vbuf[MAX_LINE];
    const char *val = NULL;
    if (special == '@' || special == '*') {
        size_t vn = 0;
        for (int i = 0; i < pos_count; i++) {
            if (i && vn + 1 < sizeof vbuf) vbuf[vn++] = ' ';
            for (const char *v = pos_args[i]; *v && vn + 1 < sizeof vbuf; v++)
                vbuf[vn++] = *v;
        }
        vbuf[vn] = '\0';
        val = vbuf;
    } else if (special == '?') {
        snprintf(vbuf, sizeof vbuf, "%d", last_status);
        val = vbuf;
    } else if (special == '!') {
        snprintf(vbuf, sizeof vbuf, "%d", (int)last_bg_pid);
        val = vbuf;
    } else if (special == '#') {
        snprintf(vbuf, sizeof vbuf, "%d", pos_count);
        val = vbuf;
    } else if (k == 1 && name[0] >= '0' && name[0] <= '9') {
        int idx = name[0] - '0';
        val = (idx == 0) ? script_name
            : (idx <= pos_count ? pos_args[idx - 1] : NULL);
    } else if (k > 0) {
        val = getenv(name);
    } else {
        if (n < size) buf[n++] = '$';
        *sp = s;
        return n;
    }

    bool empty = (val == NULL || *val == '\0');
    bool unset = (val == NULL);

    char out[MAX_LINE];
    out[0] = '\0';
    if (val) strlcpy(out, val, sizeof out);

    if (op[0] == ':' || op[0] == '-' || op[0] == '=' || op[0] == '+' ||
        op[0] == '?') {
        char which   = (op[0] == ':') ? op[1] : op[0];
        bool trigger = (op[0] == ':') ? empty : unset;

        /* 오른쪽은 쓸 때만 확장한다.
         *
         * `${DPKG_ROOT:+$(realpath "$DPKG_ROOT")}` 이 그 이유다.
         * 값이 없을 때 이 자리는 빈 문자열이어야 하는데, 미리
         * 확장하면 realpath 가 빈 인자로 **실제로 실행되어**
         * "realpath: '': No such file or directory" 를 찍는다.
         * 결과는 버려지지만 오류는 남고, 스크립트는 아무 잘못이
         * 없는데 오류를 내는 것처럼 보인다. */
        bool need = (which == '+') ? !trigger : trigger;

        char w[MAX_LINE];
        w[0] = '\0';
        if (need)
            expand_str(word, w, sizeof w);

        if (which == '-') {
            if (trigger) strlcpy(out, w, sizeof out);
        } else if (which == '=') {
            if (trigger) {
                strlcpy(out, w, sizeof out);
                if (k > 0) setenv(name, w, 1);
            }
        } else if (which == '+') {
            if (trigger) out[0] = '\0';
            else         strlcpy(out, w, sizeof out);
        } else if (which == '?') {
            if (trigger) {
                dprintf(STDERR_FILENO, "sh: %s: %s\n",
                        k ? name : "parameter",
                        w[0] ? w : "not set");
                out[0] = '\0';
            }
        }
    } else if (op[0] == '#') {
        char pat[MAX_LINE];
        expand_str(word, pat, sizeof pat);
        strip_prefix(out, pat, op[1] == '#');
    } else if (op[0] == '%') {
        char pat[MAX_LINE];
        expand_str(word, pat, sizeof pat);
        strip_suffix(out, pat, op[1] == '%');
    }

    if (want_len) {
        char num[24];
        snprintf(num, sizeof num, "%d", (int)strlen(out));
        strlcpy(out, num, sizeof out);
    }

    for (const char *q = out; *q && n < size; q++)
        buf[n++] = *q;

    *sp = s;
    return n;
}

/* Read one token. For TOK_WORD, *word_out gets the arena pointer. */
static tok_type_t next_token(char **p, char **word_out)
{
    char *s = *p;

    while (is_space(*s)) s++;
    if (*s == '\0') { *p = s; return TOK_END; }

    /* An operator is a token in itself. Two-character ones (&& ||) must
     * be checked before the single-character ones (|). */
    if (*s == '&' && s[1] == '&') { *p = s + 2; return TOK_AND;  }
    if (*s == '&')                { *p = s + 1; return TOK_AMP;  }
    if (*s == '|' && s[1] == '|') { *p = s + 2; return TOK_OR;   }
    if (*s == '|')                { *p = s + 1; return TOK_PIPE; }
    if (*s == ';')                { *p = s + 1; return TOK_SEMI; }
    /* 2> and 2>>, before the word scanner can take "2" for a word. */
    if (*s == '2' && s[1] == '>') {
        if (s[2] == '>') { *p = s + 3; return TOK_REDIR_ERR_APPEND; }
        *p = s + 2;
        return TOK_REDIR_ERR;
    }
    /* << 은 여기서 잡아야 한다. < 두 개로 읽히면 본문이 명령이 된다. */
    if (*s == '<' && s[1] == '<') {
        *p = s + 2;
        if (**p == '-') (*p)++;            /* <<- 는 읽는 쪽이 처리했다 */
        return TOK_REDIR_HERE;
    }
    if (*s == '<') { *p = s + 1; return TOK_REDIR_IN; }
    if (*s == '>') {
        if (s[1] == '>') { *p = s + 2; return TOK_REDIR_APPEND; }
        *p = s + 1;
        return TOK_REDIR_OUT;
    }

    /* A word: gather it, stripping quotes and expanding as we go.
     *
     * Three things get replaced here:
     *   ~        home, but only at the very start of the word
     *   $NAME    a variable, ${NAME} when the name runs into other text
     *   $?       what the last command returned
     *
     * Single quotes stop all of it, the way they do everywhere else -
     * inside them a $ is just a dollar sign. Double quotes stop the word
     * from being split but not the expansion. */
    char   buf[MAX_LINE];
    size_t n = 0;

    tok_quoted = false;
    tok_has_glob = false;
    tok_from_cmd = false;
    tok_quote_at = (size_t)-1;

    /* ~ or ~/... at the start of a word. Not ~user: there is one user. */
    if (*s == '~' && (s[1] == '\0' || s[1] == '/' || is_space(s[1]))) {
        const char *home = HOME_DIR;
        while (*home && n < sizeof(buf)) buf[n++] = *home++;
        s++;
    }

    while (*s && !is_space(*s) &&
           *s != '|' && *s != '<' && *s != '>' && *s != ';' && *s != '&') {
        if (*s == '\'') {                 /* single quotes: nothing expands */
            tok_quoted = true;
            if (tok_quote_at == (size_t)-1) tok_quote_at = n;
            s++;
            while (*s && *s != '\'') {
                if (n < sizeof(buf)) buf[n++] = *s;
                s++;
            }
            if (*s == '\'') s++;
        } else if (*s == '"') {
            tok_quoted = true;
            if (tok_quote_at == (size_t)-1) tok_quote_at = n;
            s++;
            while (*s && *s != '"') {
                if (*s == '$') {
                    n = expand_dollar(&s, buf, n, sizeof(buf));
                    tok_from_cmd = false;   /* "$(...)" is one word */
                } else if (*s == '`') {
                    n = expand_backtick(&s, buf, n, sizeof(buf));
                    tok_from_cmd = false;   /* "`...`" likewise */
                } else {
                    if (n < sizeof(buf)) buf[n++] = *s;
                    s++;
                }
            }
            if (*s == '"') s++;
        } else if (*s == '$') {
            n = expand_dollar(&s, buf, n, sizeof(buf));
        } else if (*s == '`') {
            n = expand_backtick(&s, buf, n, sizeof(buf));
        } else {
            if (*s == '*' || *s == '?')
                tok_has_glob = true;
            if (n < sizeof(buf)) buf[n++] = *s;
            s++;
        }
    }

    *p = s;
    *word_out = arena_push(buf, n);
    if (!*word_out) {
        dprintf(STDERR_FILENO, "sh: line too long\n");
        return TOK_END;
    }
    return TOK_WORD;
}

/* ── Wildcards ────────────────────────────────────────────────────────
 *
 * The shell expands * and ? before the command ever runs, which is why
 * "rm *.log" means the same thing as "grep *.log" - the program only ever
 * sees a list of real names. Doing it in each program instead would make
 * every one of them subtly different.
 *
 * A pattern that matches nothing is left exactly as typed. That is what
 * every Bourne shell does, and it means "echo 2*3" prints 2*3 rather
 * than an error.
 */
static bool glob_match(const char *pat, const char *name)
{
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat)
                return true;             /* trailing * takes the rest */
            /* Try every place the rest of the pattern could start. */
            for (const char *n = name; ; n++) {
                if (glob_match(pat, n))
                    return true;
                if (!*n)
                    return false;
            }
        }
        if (!*name)
            return false;
        if (*pat != '?' && *pat != *name)
            return false;
        pat++;
        name++;
    }
    return *name == '\0';
}

#define GLOB_MAX      128
#define GLOB_NAME_MAX 128

static char glob_names[GLOB_MAX][GLOB_NAME_MAX];
static int  nglob;

/* Expand one word. Returns how many names it produced, 0 for none. */
static int glob_expand(const char *word, cmd_t *c)
{
    nglob = 0;

    /* Only the last component may hold a wildcard. Expanding a pattern in
     * the middle of a path means walking the tree, and every use of it
     * here is "the files in one directory". */
    const char *slash = strrchr(word, '/');
    char        dir[256];
    const char *pat;

    if (!slash) {
        strlcpy(dir, ".", sizeof(dir));
        pat = word;
    } else {
        size_t dlen = (size_t)(slash - word);
        if (dlen == 0) {
            strlcpy(dir, "/", sizeof(dir));
        } else {
            if (dlen >= sizeof(dir)) return 0;
            memcpy(dir, word, dlen);
            dir[dlen] = '\0';
        }
        pat = slash + 1;
    }

    long fd = lp_open(dir, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return 0;

    char buf[8192];
    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (long off = 0; off < n && nglob < GLOB_MAX; ) {
            char       *rec  = buf + off;
            u16         len  = *(u16 *)(rec + DIRENT_RECLEN);
            const char *name = rec + DIRENT_NAME;
            off += len;

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;
            /* A * does not match a leading dot unless the pattern has
             * one. Otherwise "rm *" would take .ssh with it. */
            if (name[0] == '.' && pat[0] != '.')
                continue;
            if (!glob_match(pat, name))
                continue;

            /* Put the directory back on the front, so the name still
             * points where it was typed. */
            if (slash)
                snprintf(glob_names[nglob], GLOB_NAME_MAX, "%s%s%s",
                         dir, strcmp(dir, "/") == 0 ? "" : "/", name);
            else
                strlcpy(glob_names[nglob], name, GLOB_NAME_MAX);
            nglob++;
        }
    }
    lp_close((int)fd);

    if (nglob == 0)
        return 0;

    /* getdents hands them over in whatever order the filesystem keeps
     * them, which for ext4 is a hash. Sort, or "ls *" comes out scrambled
     * and every listing looks different. */
    for (int i = 1; i < nglob; i++) {
        char tmp[GLOB_NAME_MAX];
        strlcpy(tmp, glob_names[i], GLOB_NAME_MAX);
        int j = i - 1;
        while (j >= 0 && strcmp(glob_names[j], tmp) > 0) {
            strlcpy(glob_names[j + 1], glob_names[j], GLOB_NAME_MAX);
            j--;
        }
        strlcpy(glob_names[j + 1], tmp, GLOB_NAME_MAX);
    }

    int added = 0;
    for (int i = 0; i < nglob && c->argc < MAX_ARGS; i++) {
        char *w = arena_push(glob_names[i], strlen(glob_names[i]));
        if (!w)
            break;
        c->argv[c->argc++] = w;
        added++;
    }
    return added;
}

/* NAME=value in front of a command.
 *
 * Ours sets the variable for good, not just for that one command the way
 * a POSIX shell does. With no subshells and no export there is nowhere
 * else for it to live, and "PATH=/data/bin:$PATH" doing what it looks
 * like it does is worth more than the distinction. */
static bool try_assignment(const char *word)
{
    const char *eq = strchr(word, '=');
    if (!eq || eq == word)
        return false;
    if (*word >= '0' && *word <= '9')
        return false;                    /* not a name */
    for (const char *q = word; q < eq; q++)
        if (!is_name_char(*q))
            return false;

    char   name[64];
    size_t n = (size_t)(eq - word);
    if (n >= sizeof(name))
        return false;
    memcpy(name, word, n);
    name[n] = '\0';

    setenv(name, eq + 1, 1);
    return true;
}

/* Parse one line into a list of pipelines.
 *
 *   cmd1 | cmd2 && cmd3 || cmd4 ; cmd5
 *
 * | joins stages within a pipeline; && || ; join pipelines together.
 * Returns the pipeline count, 0 for a blank line, -1 on error. */
static int parse_line(char *line, pipeline_t *pipes, int max_pipes)
{
    arena_used = 0;
    bool assigned = false;      /* the line held at least one NAME=value */

    int np = 1;
    pipeline_t *pl = &pipes[0];
    memset(pl, 0, sizeof(*pl));
    pl->ncmds = 1;
    pl->link  = LINK_NONE;

    cmd_t *c = &pl->cmds[0];
    memset(c, 0, sizeof(*c));
    c->heredoc = -1;

    char *p = line;
    for (;;) {
        char      *word = NULL;
        tok_type_t t    = next_token(&p, &word);

        if (t == TOK_END)
            break;

        if (t == TOK_WORD) {
            if (c->argc >= MAX_ARGS) {
                dprintf(STDERR_FILENO, "sh: too many arguments\n");
                return -1;
            }

            /* `!` 이 명령 자리의 첫 낱말이면 결과를 뒤집으라는 뜻이다.
             * 그 자리가 아니면 그냥 느낌표다 - `echo !` 는 ! 를 찍는다. */
            if (c->argc == 0 && !tok_quoted && strcmp(word, "!") == 0) {
                pl->negate = !pl->negate;
                continue;
            }

            /* NAME=value, but only in front of the command. Anywhere else
             * it is an ordinary argument: echo A=B has to print A=B.
             *
             * The name has to be unquoted; the value does not. So the
             * test is where the first quote fell, not whether there was
             * one - out="" and X="$Y" are assignments, "X=1" is not. */
            const char *eqp = strchr(word, '=');
            if (c->argc == 0 && eqp &&
                (size_t)(eqp - word) < tok_quote_at &&
                try_assignment(word)) {
                assigned = true;
                continue;
            }

            /* An unquoted $(...) that produced spaces becomes several
             * words. Without this, "for f in $(ls)" would loop once,
             * over one string with every name in it. */
            if (tok_from_cmd && !tok_quoted && strchr(word, ' ')) {
                char *w = word;
                while (*w && c->argc < MAX_ARGS) {
                    while (*w == ' ') w++;
                    if (!*w) break;
                    char *end = w;
                    while (*end && *end != ' ') end++;
                    bool last = (*end == '\0');
                    *end = '\0';
                    c->argv[c->argc++] = w;
                    if (last) break;
                    w = end + 1;
                }
                continue;
            }

            /* A pattern turns into the names it matched, or stays as it
             * was written when it matched nothing. */
            if (tok_has_glob && !tok_quoted && glob_expand(word, c) > 0)
                continue;

            c->argv[c->argc++] = word;
            continue;
        }

        if (t == TOK_PIPE) {
            if (c->argc == 0) {
                dprintf(STDERR_FILENO, "sh: no command before |\n");
                return -1;
            }
            if (pl->ncmds >= MAX_CMDS) {
                dprintf(STDERR_FILENO, "sh: pipeline too long (max %d)\n",
                        MAX_CMDS);
                return -1;
            }
            c = &pl->cmds[pl->ncmds++];
            memset(c, 0, sizeof(*c));
            c->heredoc = -1;
            continue;
        }

        if (t == TOK_AND || t == TOK_OR || t == TOK_SEMI || t == TOK_AMP) {
            if (t == TOK_AMP)
                pl->background = true;

            /* "X=1 ; echo $X" is two statements and the first one is an
             * assignment, which is a whole command even though it puts
             * nothing in argv. Only a genuinely empty one - a line that
             * starts with ; or && - is a mistake worth naming. */
            if (c->argc == 0 && !assigned) {
                dprintf(STDERR_FILENO, "sh: no command before the operator\n");
                return -1;
            }
            if (c->argc == 0) {
                assigned = false;      /* consumed by this statement */
                last_status = 0;
                if (t == TOK_AMP)
                    pl->background = false;
                continue;              /* keep filling the same slot */
            }
            if (np >= max_pipes) {
                dprintf(STDERR_FILENO,
                        "sh: too many commands joined together (max %d)\n",
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
            c->heredoc = -1;
            continue;
        }

        /* Redirection: a file name has to follow immediately.
         *
         * "&1" is the exception and the reason 2>&1 works: it is not a
         * file, it is "send this to wherever the other one went". The
         * word scanner hands it over as an ordinary word, so it is
         * recognised here rather than in the lexer. */
        char *file = NULL;

        /* "&1" has to be spotted before next_token runs, because the
         * lexer sees '&' and says "background job" - which is right
         * everywhere except immediately after a redirection. That is
         * why 2>&1 came back as "2> needs a file after it". */
        {
            char *q = p;
            while (*q == ' ' || *q == '\t') q++;
            if (q[0] == '&' && q[1] >= '0' && q[1] <= '9') {
                static char amp[3] = "&1";
                amp[1] = q[1];
                file = amp;
                p = q + 2;
                if (t == TOK_REDIR_ERR || t == TOK_REDIR_ERR_APPEND) {
                    c->redir_err  = file;
                    c->err_append = false;
                } else {
                    c->redir_out  = file;
                    c->out_append = false;
                }
                continue;
            }
        }

        if (next_token(&p, &file) != TOK_WORD) {
            dprintf(STDERR_FILENO,
                    "sh: %s needs a file after it\n",
                    (t == TOK_REDIR_HERE)      ? "<<" :
                    (t == TOK_REDIR_IN)        ? "<"  :
                    (t == TOK_REDIR_ERR)       ? "2>" :
                    (t == TOK_REDIR_ERR_APPEND)? "2>>":
                    (t == TOK_REDIR_APPEND)    ? ">>" : ">");
            return -1;
        }
        if (t == TOK_REDIR_HERE) {
            /* 읽는 쪽이 본문을 모아 두고 여기에는 @번호 만 남겼다. */
            c->heredoc = (file[0] == '@') ? atoi(file + 1) : -1;
            if (c->heredoc < 0 || c->heredoc >= nheredocs) {
                dprintf(STDERR_FILENO,
                        "sh: << without a body - the delimiter never"
                        " appeared\n");
                c->heredoc = -1;
            }
        } else if (t == TOK_REDIR_IN) {
            c->redir_in = file;
        } else if (t == TOK_REDIR_ERR || t == TOK_REDIR_ERR_APPEND) {
            c->redir_err  = file;
            c->err_append = (t == TOK_REDIR_ERR_APPEND);
        } else {
            c->redir_out  = file;
            c->out_append = (t == TOK_REDIR_APPEND);
        }
    }

    /* Terminate each argv with NULL. */
    for (int i = 0; i < np; i++)
        for (int j = 0; j < pipes[i].ncmds; j++)
            pipes[i].cmds[j].argv[pipes[i].cmds[j].argc] = NULL;

    /* Drop a trailing empty pipeline (the line ended on an operator). */
    while (np > 0 && pipes[np - 1].cmds[0].argc == 0)
        np--;

    /* "FOO=bar" on its own is a whole command, and it succeeded. */
    if (np == 0 && assigned)
        last_status = 0;

    return np;
}

/* ── Builtins ───────────────────────────────────────────────────── */


static void source_file(const char *path);   /* `. file` 이 쓴다 */

static const char *BUILTINS[] = {
    "break", "continue",
    /* "help" is deliberately not here. It is /bin/help, a real program,
     * so it can scan PATH and list what is actually installed - a
     * builtin would only ever know what was hardcoded into the shell. */
    "exit", "cd", "pwd", "echo", "env", "reboot", "poweroff", "halt",
    "test", "[", "true", "false", ":",
    "read", "shift", "export", "set", "local", "unset", ".", "source", NULL
};

/* ── local ───────────────────────────────────────────────────────────
 *
 * 이 셸에는 변수 하나의 저장소밖에 없다 - 환경이다. 함수 안에서 놓은
 * 이름은 밖에서도 보이고, 그래서 두 함수가 같은 이름을 쓰면 서로를
 * 덮는다. `local` 이 하는 일은 그 덮어씀을 함수가 끝날 때 되돌리는
 * 것이다.
 *
 * 구현은 원래 값을 쌓아 두었다가 함수가 끝날 때 되돌리는 것이 전부다.
 * 진짜 지역 변수는 아니다 - 함수가 부르는 프로그램은 여전히 그 값을
 * 환경에서 본다. dash 도 그 점은 같고(local 도 export 된다), 셸
 * 스크립트가 local 에 기대하는 것은 "밖을 망가뜨리지 않는다" 이지
 * "자식에게 숨긴다" 가 아니다.
 *
 * 없을 때 무슨 일이 생기냐면: dpkg 의 관리자 스크립트가 전부
 * `local CONFFILE="$1"` 로 시작하고, local 을 모르는 셸은 그 줄을
 * "local 이라는 명령을 CONFFILE=... 인자로 부른다" 로 읽는다.
 * 명령이 없으니 대입은 일어나지 않고, 함수는 빈 변수로 계속 돈다. */
#define MAX_LOCALS 128

typedef struct {
    char  name[64];
    char *old;          /* 함수에 들어오기 전 값. 없었으면 NULL */
    bool  had;
} local_save_t;

static local_save_t local_stack[MAX_LOCALS];
static int          nlocals = 0;
static int          in_function = 0;

/* 이름 하나를 이 함수의 것으로 표시하고 값을 놓는다. */
static void local_declare(const char *arg)
{
    char name[64];
    const char *eq = strchr(arg, '=');
    size_t len = eq ? (size_t)(eq - arg) : strlen(arg);
    if (len >= sizeof name) len = sizeof name - 1;
    memcpy(name, arg, len);
    name[len] = '\0';
    if (!name[0])
        return;

    if (nlocals < MAX_LOCALS) {
        local_save_t *sv = &local_stack[nlocals++];
        strlcpy(sv->name, name, sizeof sv->name);
        const char *cur = getenv(name);
        sv->had = cur != NULL;
        sv->old = cur ? strdup(cur) : NULL;
    } else {
        dprintf(STDERR_FILENO,
                "sh: more than %d local variables at once - %s will not be"
                " put back\n", MAX_LOCALS, name);
    }

    /* 값이 없는 `local x` 는 비운다. 그래야 [ -z "$x" ] 가 참이 된다. */
    if (eq) setenv(name, eq + 1, 1);
    else    unsetenv(name);
}

/* 함수가 끝날 때 base 아래로 쌓인 것을 전부 되돌린다. */
static void locals_unwind(int base)
{
    while (nlocals > base) {
        local_save_t *sv = &local_stack[--nlocals];
        if (sv->had) setenv(sv->name, sv->old, 1);
        else         unsetenv(sv->name);
        free(sv->old);
        sv->old = NULL;
    }
}

/* ── test ─────────────────────────────────────────────────────────────
 *
 *   test -f <path>      a file exists
 *   test -d <path>      a directory exists
 *   test -e <path>      either
 *   test -s <path>      exists and is not empty
 *   test -z <string>    the string is empty
 *   test -n <string>    the string is not
 *   test a = b          equal      a != b   not equal
 *   test a -eq b        numbers    -ne -lt -le -gt -ge
 *   test <string>       not empty
 *
 * "[" is the same thing spelled differently, and wants a closing "]".
 *
 * A builtin rather than a program because the exit code is the whole
 * point and the shell has to see it directly - and because a script
 * calling this in a loop should not be forking for it.
 *
 * Why this and not a full if: with && and || an exit code is already a
 * branch, and the boot script has used that from the start. What was
 * missing was a way to ask a question about a file. So:
 *
 *   test -s /data/rc.local && sh /data/rc.local
 *   test -f /data/x || echo "not there"
 */
/* Surrounding blanks are allowed, because an operand almost always
 * arrives through backticks and the command that produced it is free to
 * line its output up: `calc 5 - 3` says "  2", and refusing that reads
 * as the comparison being unknown rather than the number being spaced. */
static bool str_is_num(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if (*s == '-' || *s == '+') s++;
    if (*s < '0' || *s > '9') return false;
    while (*s >= '0' && *s <= '9') s++;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    return *s == '\0';
}

/* strtol already skips leading blanks, so this only has to agree with
 * str_is_num about what counts as a number. */
static long str_to_num(const char *s)
{
    return strtol(s, NULL, 10);
}

/* Returns the exit code: 0 is true, the way the shell counts. */
static int run_test(char **argv, int argc)
{
    /* "[ ... ]": drop the closing bracket. */
    if (strcmp(argv[0], "[") == 0) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            dprintf(STDERR_FILENO, "[: missing ]\n");
            return 2;
        }
        argc--;
    }

    /* "test" on its own is false; one argument is true when non-empty. */
    if (argc == 1)
        return 1;
    if (argc == 2)
        return argv[1][0] ? 0 : 1;

    /* Negation, which is worth having because "not" reads better than
     * inverting the whole condition. */
    if (strcmp(argv[1], "!") == 0)
        return run_test(argv + 1, argc - 1) == 0 ? 1 : 0;

    if (argc == 3) {
        const char *op  = argv[1];
        const char *arg = argv[2];

        if (strcmp(op, "-e") == 0) return lp_exists(arg) ? 0 : 1;
        if (strcmp(op, "-d") == 0) return lp_is_dir(arg) ? 0 : 1;

        if (strcmp(op, "-f") == 0) {
            lp_stat_t st;
            if (lp_stat(arg, &st, true) < 0) return 1;
            return (st.mode & LP_S_IFMT) == LP_S_IFREG ? 0 : 1;
        }
        if (strcmp(op, "-s") == 0) {
            lp_stat_t st;
            if (lp_stat(arg, &st, true) < 0) return 1;
            return st.size > 0 ? 0 : 1;
        }
        if (strcmp(op, "-z") == 0) return arg[0] ? 1 : 0;
        if (strcmp(op, "-n") == 0) return arg[0] ? 0 : 1;

        /* Device files, which a script watching for a USB stick needs:
         * "test -b /dev/sda1" is how you ask whether one is plugged in
         * without mounting it first. */
        if (strcmp(op, "-b") == 0 || strcmp(op, "-c") == 0) {
            lp_stat_t st;
            if (lp_stat(arg, &st, true) < 0) return 1;
            u32 want = (op[1] == 'b') ? LP_S_IFBLK : LP_S_IFCHR;
            return (st.mode & LP_S_IFMT) == want ? 0 : 1;
        }
        if (strcmp(op, "-L") == 0 || strcmp(op, "-h") == 0) {
            lp_stat_t st;
            if (lp_stat(arg, &st, false) < 0) return 1;
            return (st.mode & LP_S_IFMT) == LP_S_IFLNK ? 0 : 1;
        }

        /* -r -w -x ask what this process may do, which is not the same
         * as what the mode bits say - root may write a file with no w
         * anywhere in it. access() answers the question that was asked. */
        /* -t N: 그 fd 가 터미널인가.
         *
         * 색을 켤지 정할 때 쓴다 - `[ -t 1 ]` 이 참이면 사람이 보는
         * 화면이고, 거짓이면 파이프나 파일이라 색 코드를 넣으면
         * 그것이 그대로 저장된다. 이것이 없으면 셸이 "unknown test"
         * 를 찍고 2 를 돌려주는데, 2 는 거짓이 아니라 오류다. */
        if (strcmp(op, "-t") == 0)
            return lp_isatty(atoi(arg)) ? 0 : 1;

        if (strcmp(op, "-r") == 0) return lp_access(arg, 4) == 0 ? 0 : 1;
        if (strcmp(op, "-w") == 0) return lp_access(arg, 2) == 0 ? 0 : 1;
        if (strcmp(op, "-x") == 0) return lp_access(arg, 1) == 0 ? 0 : 1;

        dprintf(STDERR_FILENO, "test: %s: unknown test\n", op);
        return 2;
    }

    if (argc == 4) {
        const char *a  = argv[1];
        const char *op = argv[2];
        const char *b  = argv[3];

        if (strcmp(op, "=")  == 0) return strcmp(a, b) == 0 ? 0 : 1;
        if (strcmp(op, "!=") == 0) return strcmp(a, b) != 0 ? 0 : 1;

        bool numeric = strcmp(op, "-eq") == 0 || strcmp(op, "-ne") == 0 ||
                       strcmp(op, "-lt") == 0 || strcmp(op, "-le") == 0 ||
                       strcmp(op, "-gt") == 0 || strcmp(op, "-ge") == 0;
        if (numeric) {
            /* Name the operand that is not a number. Reporting this as
             * an unknown comparison sends the reader off to check
             * whether the shell has -gt at all, which is the wrong
             * question and costs an hour. */
            if (!str_is_num(a) || !str_is_num(b)) {
                dprintf(STDERR_FILENO, "test: %s: not a number\n",
                        str_is_num(a) ? b : a);
                return 2;
            }
            long x = str_to_num(a);
            long y = str_to_num(b);
            if (strcmp(op, "-eq") == 0) return x == y ? 0 : 1;
            if (strcmp(op, "-ne") == 0) return x != y ? 0 : 1;
            if (strcmp(op, "-lt") == 0) return x <  y ? 0 : 1;
            if (strcmp(op, "-le") == 0) return x <= y ? 0 : 1;
            if (strcmp(op, "-gt") == 0) return x >  y ? 0 : 1;
            if (strcmp(op, "-ge") == 0) return x >= y ? 0 : 1;
        }

        dprintf(STDERR_FILENO, "test: %s: unknown comparison\n", op);
        return 2;
    }

    dprintf(STDERR_FILENO, "test: too many arguments\n");
    return 2;
}

static bool is_builtin(const char *name)
{
    for (int i = 0; BUILTINS[i]; i++)
        if (strcmp(name, BUILTINS[i]) == 0)
            return true;
    return false;
}

/* Handle a builtin and return true. The exit code goes in last_status. */
static bool run_builtin(cmd_t *c)
{
    const char *cmd = c->argv[0];

    if (strcmp(cmd, "test") == 0 || strcmp(cmd, "[") == 0) {
        last_status = run_test(c->argv, c->argc);
        return true;
    }

    /* read - the only way a script can ask a question.
     *
     *   read name              one line into $name
     *   read -p "Sure? " ans   with a prompt first
     *   read -r line           backslashes stay backslashes
     *   read a b rest          split on spaces; the last one takes the rest
     *
     * Without this a script could print a question and had no way to
     * hear the answer, so every confirmation on this machine had to be
     * compiled into a C program. Returns 1 at end of input, which is
     * what makes `while read line ; do ... ; done` stop. */
    if (strcmp(cmd, "read") == 0) {
        const char *prompt = NULL;
        bool raw = false;
        int  first = 1;
        for (; first < c->argc; first++) {
            if (strcmp(c->argv[first], "-r") == 0) raw = true;
            else if (strcmp(c->argv[first], "-p") == 0 && first + 1 < c->argc)
                prompt = c->argv[++first];
            else break;
        }
        (void)raw;                 /* nothing here interprets backslashes */

        if (prompt)
            fputs(prompt, STDOUT_FILENO);

        char buf[MAX_LINE];
        long n = readline(STDIN_FILENO, buf, sizeof buf);
        if (n < 0) { last_status = 1; return true; }

        if (first >= c->argc) {
            /* No variable named: POSIX puts the line in REPLY. */
            setenv("REPLY", buf, 1);
            last_status = 0;
            return true;
        }

        char *p = buf;
        for (int i = first; i < c->argc; i++) {
            while (*p == ' ' || *p == '\t') p++;

            if (i == c->argc - 1) {
                /* The last variable takes everything left, trailing
                 * spaces trimmed - that is what makes `read cmd rest`
                 * useful for parsing a line. */
                char *end = p + strlen(p);
                while (end > p && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
                setenv(c->argv[i], p, 1);
                break;
            }

            char *word = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
            setenv(c->argv[i], word, 1);
        }
        last_status = 0;
        return true;
    }

    /* shift - drop the first positional argument.
     *
     * `while test $# -gt 0 ; do ... ; shift ; done` is how a script
     * walks its own arguments, and without shift there was no way to
     * write that loop at all. */
    if (strcmp(cmd, "shift") == 0) {
        int by = (c->argc > 1) ? atoi(c->argv[1]) : 1;
        if (by < 0) {
            dprintf(STDERR_FILENO, "shift: %s is not a count\n", c->argv[1]);
            last_status = 2;
            return true;
        }
        if (by > pos_count) {
            /* Nothing left to shift is an error, not a silent no-op:
             * a loop that relies on shift to end would otherwise spin. */
            last_status = 1;
            return true;
        }
        for (int i = 0; i + by < pos_count; i++)
            strlcpy(pos_args[i], pos_args[i + by], sizeof pos_args[0]);
        pos_count -= by;
        last_status = 0;
        return true;
    }

    /* set - the shell's own options, and the positional parameters.
     *
     *   set -e / set +e     stop on a failing command, or do not
     *   set -x / set +x     print each line before running it
     *   set -- a b c        replace $1 $2 $3
     *   set                 print the variables (POSIX; here, nothing
     *                       useful is available, so it says so)
     *
     * Unknown options are reported rather than swallowed. A script that
     * asked for something this shell does not do should hear about it
     * once, at the line that asked, instead of behaving oddly later. */
    if (strcmp(cmd, "set") == 0) {
        last_status = 0;

        if (c->argc == 1) {
            dprintf(STDERR_FILENO,
                    "set: 이 셸은 변수 목록을 내놓지 못합니다."
                    " `env` 를 쓰십시오\n");
            last_status = 1;
            return true;
        }

        int i = 1;
        for (; i < c->argc; i++) {
            const char *a = c->argv[i];

            if (strcmp(a, "--") == 0) { i++; break; }

            if (a[0] != '-' && a[0] != '+') break;

            bool on = (a[0] == '-');
            for (const char *f = a + 1; *f; f++) {
                switch (*f) {
                case 'e': opt_errexit = on; break;
                case 'x': opt_xtrace  = on; break;
                default:
                    dprintf(STDERR_FILENO,
                            "set: -%c 는 이 셸에 없습니다\n", *f);
                    last_status = 2;
                    break;
                }
            }
        }

        /* Anything left is the new positional parameters. `set -e` on
         * its own must not wipe them, so this only runs when there
         * really are words after the options. */
        if (i < c->argc) {
            pos_count = 0;
            for (; i < c->argc && pos_count < MAX_POSITIONAL; i++)
                strlcpy(pos_args[pos_count++], c->argv[i],
                        sizeof pos_args[0]);
        }
        return true;
    }

    /* export - accepted, because every script written elsewhere says it.
     *
     * There is nothing for it to do here: an assignment in this shell
     * calls setenv, so a variable is in the environment from the moment
     * it exists and every child already sees it. `export NAME=value`
     * still has to assign, though, or the script silently loses the
     * value. Refusing the word outright would break scripts for no
     * reason; doing nothing quietly would lose data. */
    if (strcmp(cmd, "export") == 0) {
        for (int i = 1; i < c->argc; i++) {
            char *eq = strchr(c->argv[i], '=');
            if (!eq)
                continue;           /* already exported; nothing to do */
            char name[128];
            size_t len = (size_t)(eq - c->argv[i]);
            if (len >= sizeof name) len = sizeof name - 1;
            memcpy(name, c->argv[i], len);
            name[len] = '\0';
            setenv(name, eq + 1, 1);
        }
        last_status = 0;
        return true;
    }

    if (strcmp(cmd, "local") == 0) {
        /* 함수 밖의 local 은 그냥 대입이다. dash 는 오류로 치지만,
         * 오류를 내면 그 줄에서 스크립트가 멈추고 대입도 사라진다 -
         * 값을 잃는 쪽이 더 나쁘다. */
        for (int i = 1; i < c->argc; i++) {
            if (in_function > 0) {
                local_declare(c->argv[i]);
            } else {
                char *eq = strchr(c->argv[i], '=');
                if (eq) {
                    char name[128];
                    size_t len = (size_t)(eq - c->argv[i]);
                    if (len >= sizeof name) len = sizeof name - 1;
                    memcpy(name, c->argv[i], len);
                    name[len] = '\0';
                    setenv(name, eq + 1, 1);
                }
            }
        }
        last_status = 0;
        return true;
    }

    /* `. file` - 그 파일을 **이 셸에서** 읽어 돌린다.
     *
     * 없을 때는 PATH 에서 "." 이라는 이름의 프로그램을 찾다가
     * "/usr/local/sbin/.: cannot run it (13)" 로 끝났다. 디렉터리를
     * 실행하려 한 것이고, 그 말로는 무엇이 없는지 알 수 없다.
     *
     * 자식으로 돌리면 안 된다: 이 명령의 요점이 함수와 변수를
     * **부르는 쪽에** 남기는 것이기 때문이다. dpkg 의 스크립트들은
     * 오류 처리 함수를 이렇게 들여온다. */
    if (strcmp(cmd, ".") == 0 || strcmp(cmd, "source") == 0) {
        if (c->argc < 2) {
            dprintf(STDERR_FILENO, "sh: %s: needs a file to read\n", cmd);
            last_status = 2;
            return true;
        }
        source_file(c->argv[1]);
        return true;
    }

    if (strcmp(cmd, "unset") == 0) {
        for (int i = 1; i < c->argc; i++)
            unsetenv(c->argv[i]);
        last_status = 0;
        return true;
    }

    /* ":" is "true" spelled the way scripts spell it - `while :` and
     * `if : ; then` are both idiomatic, and both were a
     * "command not found" here. */
    if (strcmp(cmd, "true") == 0 ||
        strcmp(cmd, ":") == 0)     { last_status = 0; return true; }
    if (strcmp(cmd, "false") == 0) { last_status = 1; return true; }

    /* break and continue take an optional count, the way every other
     * shell does: `break 2` leaves two loops. A count larger than the
     * number of loops we are actually in leaves all of them, which is
     * what happens elsewhere and is less surprising than an error. */
    if (strcmp(cmd, "break") == 0 || strcmp(cmd, "continue") == 0) {
        int levels = (c->argc > 1) ? atoi(c->argv[1]) : 1;
        if (levels < 1)
            levels = 1;
        if (strcmp(cmd, "break") == 0)
            loop_break = levels;
        else
            loop_continue = levels;
        last_status = 0;
        return true;
    }

    if (strcmp(cmd, "exit") == 0) {
        shell_running = false;
        last_status = (c->argc > 1) ? atoi(c->argv[1]) : 0;
        return true;
    }

    if (strcmp(cmd, "cd") == 0) {
        const char *dir = (c->argc > 1) ? c->argv[1] : HOME_DIR;
        long r = lp_chdir(dir);
        if (r < 0) {
            dprintf(STDERR_FILENO, "cd: %s: cannot change to it (%ld)\n", dir, -r);
            last_status = 1;
        } else {
            last_status = 0;
        }
        return true;
    }

    if (strcmp(cmd, "pwd") == 0) {
        char buf[512];
        long r = lp_getcwd(buf, sizeof(buf));
        if (r < 0) { dprintf(STDERR_FILENO, "pwd: failed (%ld)\n", -r); last_status = 1; }
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

    if (strcmp(cmd, "reboot") == 0 || strcmp(cmd, "poweroff") == 0 ||
        strcmp(cmd, "halt") == 0) {
        /* ── One shutdown path, and it goes through init ──
         *
         * This used to do the whole thing here: SIGTERM everything,
         * sync, remount /data read-only, then reboot(2). It worked, and
         * it could not work well, because the shell is not the process
         * that knows what is running. init restarts supervised services
         * - that is its job - so every service the shell killed came
         * straight back while the machine was going down. The console
         * showed "service guard exited - again in 1s" underneath
         * "stopping services".
         *
         * And nothing unmounted /data. The remount to read-only kept
         * the filesystem consistent, which is why this was survivable,
         * but the next boot still replays a journal and fsck still
         * wants a look.
         *
         * init can do both: it stops restarting first, and it knows
         * about the overlays and the /root bind that have to come off
         * before /data will unmount. So this now says what is wanted
         * and lets init do it - the same as /bin/poweroff, which is the
         * same signal, so there is one sequence to get right rather
         * than two that drift. */
        int sig = (strcmp(cmd, "reboot") == 0) ? SIGUSR2 : SIGUSR1;

        if (lp_kill(1, sig) != 0) {
            dprintf(STDERR_FILENO,
                    "%s: init did not take the signal - nothing has been"
                    " stopped.\n", cmd);
            last_status = 1;
            return true;
        }

        /* init prints the rest and takes the machine down. If it is
         * still here after ten seconds, say so rather than leave the
         * person looking at a prompt wondering. */
        for (int i = 0; i < 100; i++)
            lp_sleep_ms(100);

        dprintf(STDERR_FILENO,
                "%s: init took the signal but the machine is still"
                " running.\n", cmd);
        last_status = 1;
        return true;
    }

    return false;
}

/* ── External commands ──────────────────────────────────────────── */

/* Search PATH and fill buf with the executable's path. true if found. */
static bool resolve_path(const char *cmd, char *buf, size_t size)
{
    if (strchr(cmd, '/')) {                 /* a path was given: use it */
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

/* Apply redirections inside the child. Exit if one fails. */
/* here-document 본문을 파이프에 흘려 넣고 읽는 끝을 stdin 으로.
 * 성공하면 true. */
static bool apply_heredoc(cmd_t *c)
{
    if (c->heredoc < 0 || c->heredoc >= nheredocs ||
        !heredocs[c->heredoc].body)
        return false;

    int fds[2];
    if (lp_pipe(fds) != 0)
        return false;

    const char *body = heredocs[c->heredoc].body;

    /* 확장은 지금 한다 - 변수는 이 명령이 도는 시점의 값이어야 한다.
     * 줄 단위로 하는 이유는 확장 버퍼가 한 줄 크기이기 때문이다. */
    char  *made = NULL;
    if (heredocs[c->heredoc].expand) {
        size_t cap = strlen(body) * 2 + 256, len2 = 0;
        made = malloc(cap);
        if (made) {
            made[0] = '\0';
            const char *p = body;
            while (*p) {
                const char *nl = strchr(p, '\n');
                size_t rl = nl ? (size_t)(nl - p) : strlen(p);
                char raw2[MAX_LINE], exp2[MAX_LINE];
                if (rl >= sizeof raw2) rl = sizeof raw2 - 1;
                memcpy(raw2, p, rl);
                raw2[rl] = '\0';
                expand_str(raw2, exp2, sizeof exp2);

                size_t el = strlen(exp2);
                if (len2 + el + 2 >= cap) {
                    size_t bigger = (len2 + el + 2) * 2;
                    char *grown = malloc(bigger);
                    if (!grown) break;
                    memcpy(grown, made, len2 + 1);
                    free(made);
                    made = grown;
                    cap  = bigger;
                }
                memcpy(made + len2, exp2, el);
                len2 += el;
                made[len2++] = '\n';
                made[len2]   = '\0';

                if (!nl) break;
                p = nl + 1;
            }
            body = made;
        }
    }

    size_t len = strlen(body);

    /* 파이프 버퍼보다 크면 여기서 막힌다. 그래서 모을 때 상한을
     * 두었고, 그 상한은 버퍼보다 작다. */
    if (len)
        lp_write(fds[1], body, len);
    free(made);
    lp_close(fds[1]);
    lp_dup2(fds[0], STDIN_FILENO);
    lp_close(fds[0]);
    return true;
}

static void apply_redirects(cmd_t *c)
{
    apply_heredoc(c);

    if (c->redir_in) {
        long fd = lp_open(c->redir_in, O_RDONLY, 0);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "sh: %s: cannot open\n", c->redir_in);
            lp_exit(1);
        }
        lp_dup2((int)fd, STDIN_FILENO);
        lp_close((int)fd);
    }

    if (c->redir_out) {
        int flags = O_WRONLY | O_CREAT | (c->out_append ? O_APPEND : O_TRUNC);
        long fd = lp_open(c->redir_out, flags, 0644);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "sh: %s: cannot create\n", c->redir_out);
            lp_exit(1);
        }
        lp_dup2((int)fd, STDOUT_FILENO);
        lp_close((int)fd);
    }

    /* Error output last, so that 2>&1 means what everybody expects:
     * "wherever stdout is going, by the time you read this line". Doing
     * it before the stdout redirection above would point stderr at the
     * terminal and then move stdout to the file - which is the classic
     * surprise, and the reason the order is worth a comment. */
    if (c->redir_err) {
        if (strcmp(c->redir_err, "&1") == 0) {
            lp_dup2(STDOUT_FILENO, STDERR_FILENO);
        } else {
            int flags = O_WRONLY | O_CREAT |
                        (c->err_append ? O_APPEND : O_TRUNC);
            long fd = lp_open(c->redir_err, flags, 0644);
            if (fd < 0) {
                dprintf(STDERR_FILENO, "sh: %s: cannot create\n", c->redir_err);
                lp_exit(1);
            }
            lp_dup2((int)fd, STDERR_FILENO);
            lp_close((int)fd);
        }
    }
}

/* ── Background jobs ──────────────────────────────────────────────────
 *
 * A command ending in & runs without the shell waiting for it. There is
 * no job control beyond that - no fg, no bg, no Ctrl-Z - because those
 * need process groups and terminal ownership, and what & is actually for
 * here is starting something long and carrying on typing.
 *
 * We do have to collect them when they finish, or every one leaves a
 * zombie behind. That happens just before each prompt, which is also the
 * only moment it is polite to print "Done". */
#define MAX_JOBS 16

typedef struct {
    pid_t pid;
    int   id;
    char  cmd[48];
} job_t;

static job_t jobs[MAX_JOBS];
static int   njobs = 0;
static int   next_job_id = 1;

static void job_add(pid_t pid, const char *cmd)
{
    last_bg_pid = pid;

    if (njobs >= MAX_JOBS) {
        printf("[&] %d %s\n", (int)pid, cmd);
        return;                     /* still running, just not tracked */
    }
    jobs[njobs].pid = pid;
    jobs[njobs].id  = next_job_id++;
    strlcpy(jobs[njobs].cmd, cmd, sizeof(jobs[njobs].cmd));
    printf("[%d] %d\n", jobs[njobs].id, (int)pid);
    njobs++;
}

static void reap_jobs(void)
{
    for (int i = 0; i < njobs; ) {
        int   status = 0;
        pid_t r = lp_waitpid(jobs[i].pid, &status, WNOHANG);
        if (r == jobs[i].pid) {
            printf("[%d] done   %s\n", jobs[i].id, jobs[i].cmd);
            jobs[i] = jobs[njobs - 1];
            njobs--;
        } else {
            i++;
        }
    }
}

/* Run a pipeline: fork each stage and join them with pipes. */
static void run_pipeline(cmd_t *cmds, int n, bool background)
{
    int  prev_read = -1;      /* read end handed over by the previous stage */
    pid_t pids[MAX_CMDS];
    int  npids = 0;

    for (int i = 0; i < n; i++) {
        int pipefd[2] = { -1, -1 };
        bool last = (i == n - 1);

        if (!last && lp_pipe(pipefd) < 0) {
            dprintf(STDERR_FILENO, "sh: cannot create a pipe\n");
            break;
        }

        pid_t pid = lp_fork();
        if (pid < 0) {
            dprintf(STDERR_FILENO, "sh: fork failed\n");
            if (pipefd[0] >= 0) { lp_close(pipefd[0]); lp_close(pipefd[1]); }
            break;
        }

        if (pid == 0) {
            /* ── child ── */
            /* Ctrl-C is for whatever is running, not for the shell. The
             * shell ignores it; a command has to have it back, or there
             * is no way to stop one that will not stop by itself.
             *
             * A background job keeps ignoring it: & means "get on with
             * this while I do other things", and it would be a surprise
             * for interrupting a foreground command to take it out too -
             * they share a process group, so the signal reaches both. */
            if (background) {
                lp_signal_ignore(SIGINT);
                lp_signal_ignore(SIGQUIT);
            } else {
                lp_signal_default(SIGINT);
                lp_signal_default(SIGQUIT);
            }

            /* Never the stop signals, foreground or background. A
             * stopped child would leave the shell waiting on it with no
             * way to continue it - there is no `fg` on this machine. */
            lp_signal_ignore(SIGTSTP);
            lp_signal_ignore(SIGTTIN);
            lp_signal_ignore(SIGTTOU);

            if (prev_read >= 0) {
                lp_dup2(prev_read, STDIN_FILENO);
                lp_close(prev_read);
            }
            if (!last) {
                lp_close(pipefd[0]);             /* we only write */
                lp_dup2(pipefd[1], STDOUT_FILENO);
                lp_close(pipefd[1]);
            }

            apply_redirects(&cmds[i]);           /* redirection beats the pipe */

            /* A builtin or a function is a perfectly good stage.
             *
             * This used to go straight to the PATH search, so
             * `echo x | grep x` answered "sh: echo: command not found" -
             * echo is a builtin and there is no /bin/echo on this
             * system. The same for pwd, env, test and every function the
             * person had defined. That is one of the two or three most
             * typed shapes in any shell, and it has never worked here.
             *
             * We are already in the forked child with the pipe wired up,
             * so running it here is right in the other sense too: a `cd`
             * or an assignment in a pipeline stage must not reach back
             * into the shell that started it, and it cannot from here. */
            {
                func_t *pf = func_find(cmds[i].argv[0]);
                if (pf) {
                    call_func(pf, cmds[i].argv, cmds[i].argc);
                    lp_exit(last_status);
                }
                if (is_builtin(cmds[i].argv[0])) {
                    run_builtin(&cmds[i]);
                    lp_exit(last_status);
                }
            }

            char path[512];
            if (!resolve_path(cmds[i].argv[0], path, sizeof(path))) {
                dprintf(STDERR_FILENO, "sh: %s: command not found\n",
                        cmds[i].argv[0]);
                lp_exit(127);
            }

            long e = lp_execve(path, cmds[i].argv, environ);
            dprintf(STDERR_FILENO, "sh: %s: cannot run it (%ld)\n",
                    path, -e);
            lp_exit(126);
        }

        /* ── parent ── */
        pids[npids++] = pid;

        if (prev_read >= 0)
            lp_close(prev_read);
        if (!last) {
            lp_close(pipefd[1]);                 /* the parent does not write */
            prev_read = pipefd[0];
        }
    }

    if (prev_read >= 0)
        lp_close(prev_read);

    /* In the background we do not wait at all - the last stage becomes
     * the job, and the earlier stages of a pipeline are collected with
     * it when it goes. */
    if (background) {
        if (npids > 0)
            job_add(pids[npids - 1], cmds[0].argv[0]);
        last_status = 0;
        return;
    }

    /* Wait for every stage. The last stage's status is the pipeline's. */
    for (int i = 0; i < npids; i++) {
        int status = 0;
        lp_waitpid(pids[i], &status, 0);
        if (i == npids - 1)
            last_status = LP_WIFEXITED(status) ? LP_WEXITSTATUS(status)
                                               : 128 + LP_WTERMSIG(status);
    }
}

/* Run a builtin with redirection.
 *
 * A builtin has to run inside the shell process: cd in a child would
 * leave the parent's directory unchanged and mean nothing. So instead of
 * forking we swap the shell's own fds and put them back afterwards. */
static void run_builtin_redirected(cmd_t *c)
{
    int saved_in  = -1;
    int saved_out = -1;
    int saved_err = -1;

    if (c->heredoc >= 0) {
        saved_in = (int)lp_dup(STDIN_FILENO);
        if (!apply_heredoc(c)) {
            lp_close(saved_in);
            saved_in = -1;
        }
    }

    if (c->redir_in) {
        long fd = lp_open(c->redir_in, O_RDONLY, 0);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "sh: %s: cannot open (%ld)\n",
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
            dprintf(STDERR_FILENO, "sh: %s: cannot create (%ld)\n",
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

    /* Same for the error output, and after stdout for the same reason:
     * 2>&1 has to mean "where stdout goes now", not "where it went
     * before the line was read". */
    if (c->redir_err) {
        if (strcmp(c->redir_err, "&1") == 0) {
            saved_err = (int)lp_dup(STDERR_FILENO);
            lp_dup2(STDOUT_FILENO, STDERR_FILENO);
        } else {
            int flags = O_WRONLY | O_CREAT |
                        (c->err_append ? O_APPEND : O_TRUNC);
            long fd = lp_open(c->redir_err, flags, 0644);
            if (fd >= 0) {
                saved_err = (int)lp_dup(STDERR_FILENO);
                lp_dup2((int)fd, STDERR_FILENO);
                lp_close((int)fd);
            }
        }
    }

    run_builtin(c);

    if (saved_err >= 0) { lp_dup2(saved_err, STDERR_FILENO); lp_close(saved_err); }
    if (saved_in >= 0)  { lp_dup2(saved_in,  STDIN_FILENO);  lp_close(saved_in); }
    if (saved_out >= 0) { lp_dup2(saved_out, STDOUT_FILENO); lp_close(saved_out); }
}

/* ── The line editor ──────────────────────────────────────────────────
 *
 * The kernel will edit a line for us - that is what canonical mode is -
 * but it only knows backspace. No history, no cursor keys, no
 * completion. On a board reached over a serial cable or a screen with no
 * scrollback, retyping a long command because of one typo near the start
 * is the difference between a system you use and one you tolerate.
 *
 * So we take the terminal raw and draw the line ourselves:
 *
 *   left/right, Ctrl-A, Ctrl-E     move
 *   up/down                        history
 *   tab                            complete a command or a path
 *   Ctrl-U, Ctrl-K, Ctrl-W         cut to start, to end, one word back
 *   Ctrl-L                         clear the screen
 *   Ctrl-C                         abandon the line
 *   Ctrl-D                         end of input, or delete forwards
 *
 * Everything moves by character, not by byte, so a Hangul syllable -
 * three bytes, two columns wide - behaves like the single character it
 * is. The line scrolls sideways when it outgrows the screen.
 */

#define HIST_MAX      64
#define HIST_FILE     "/root/.sh_history"

static char *history[HIST_MAX];
static int   hist_count = 0;

static int   term_cols = 80;

/* Width of a slice of the buffer, in screen columns. */
static int slice_width(const char *s, size_t from, size_t to)
{
    return (int)utf8_str_width(s + from, to - from);
}

static void hist_add(const char *line)
{
    if (!line[0])
        return;
    /* Do not store the same command twice in a row. */
    if (hist_count && strcmp(history[hist_count - 1], line) == 0)
        return;

    char *copy = malloc(strlen(line) + 1);
    if (!copy)
        return;
    strcpy(copy, line);

    if (hist_count == HIST_MAX) {
        free(history[0]);
        for (int i = 1; i < HIST_MAX; i++)
            history[i - 1] = history[i];
        hist_count--;
    }
    history[hist_count++] = copy;
}

/* History lives on the data partition, so it survives a reboot. Appending
 * as we go rather than writing it all out at exit means a power cut does
 * not take the session with it. */
static void hist_load(void)
{
    long fd = lp_open(HIST_FILE, O_RDONLY, 0);
    if (fd < 0)
        return;
    char line[MAX_LINE];
    while (readline((int)fd, line, sizeof(line)) >= 0)
        hist_add(line);
    lp_close((int)fd);
}

static void hist_save_one(const char *line)
{
    if (!line[0])
        return;
    long fd = lp_open(HIST_FILE, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        return;                     /* home is read-only or absent */
    lp_write((int)fd, line, strlen(line));
    lp_write((int)fd, "\n", 1);
    lp_close((int)fd);
}

typedef struct {
    char   buf[MAX_LINE];
    size_t len;                     /* bytes in use */
    size_t cur;                     /* cursor, as a byte index */
} line_t;

/* Redraw the whole line and put the cursor back where it belongs.
 *
 * When the line is wider than the screen we show a window of it that
 * keeps the cursor visible - the alternative is wrapping, and wrapping
 * needs to know how many rows we spilled onto, which the terminal will
 * not tell us. */
static void redraw(const char *prompt, int prompt_w, line_t *l)
{
    int avail = term_cols - prompt_w - 1;
    if (avail < 8)
        avail = 8;

    /* Slide the window forward until the cursor fits inside it. */
    size_t start = 0;
    while (slice_width(l->buf, start, l->cur) > avail)
        start = utf8_next(l->buf, l->len, start);

    /* Fill the window from there. */
    size_t end = start;
    while (end < l->len) {
        size_t next = utf8_next(l->buf, l->len, end);
        if (slice_width(l->buf, start, next) > avail)
            break;
        end = next;
    }

    fputs("\r", STDOUT_FILENO);
    fputs(prompt, STDOUT_FILENO);
    lp_write(STDOUT_FILENO, l->buf + start, end - start);
    fputs("\x1b[K", STDOUT_FILENO);          /* erase whatever was longer */

    int back = slice_width(l->buf, l->cur, end);
    if (back > 0) {
        char mv[16];
        snprintf(mv, sizeof(mv), "\x1b[%dD", back);
        fputs(mv, STDOUT_FILENO);
    }
}

static void line_insert(line_t *l, const char *text, size_t n)
{
    if (l->len + n >= sizeof(l->buf))
        return;
    memmove(l->buf + l->cur + n, l->buf + l->cur, l->len - l->cur);
    memcpy(l->buf + l->cur, text, n);
    l->len += n;
    l->cur += n;
    l->buf[l->len] = '\0';
}

static void line_delete(line_t *l, size_t from, size_t to)
{
    if (from >= to)
        return;
    memmove(l->buf + from, l->buf + to, l->len - to);
    l->len -= (to - from);
    l->cur  = from;
    l->buf[l->len] = '\0';
}

static void line_set(line_t *l, const char *text)
{
    strlcpy(l->buf, text, sizeof(l->buf));
    l->len = strlen(l->buf);
    l->cur = l->len;
}

/* Start of the word the cursor sits in - where completion begins. */
static size_t word_start(const line_t *l)
{
    size_t i = l->cur;
    while (i > 0 && l->buf[i - 1] != ' ' && l->buf[i - 1] != '\t')
        i--;
    return i;
}

/* Is the word at `start` the first one on the line? Then it is a command
 * name and gets completed from PATH, not from the current directory. */
static bool is_command_position(const line_t *l, size_t start)
{
    for (size_t i = 0; i < start; i++) {
        char c = l->buf[i];
        if (c == ' ' || c == '\t')
            continue;
        /* Everything after one of these starts a fresh command. */
        if (c == '|' || c == ';' || c == '&')
            continue;
        return false;
    }
    return true;
}

/* ── Completion ──
 * Candidates are collected into one buffer of names. There is no sorting
 * and no deduplication beyond what the directory gives us: this is a list
 * a person is about to read, and it is short. */
#define COMP_MAX      128
#define COMP_NAME_MAX 64

static char comp[COMP_MAX][COMP_NAME_MAX];
static int  ncomp;

static void comp_add(const char *name, bool is_dir)
{
    if (ncomp >= COMP_MAX)
        return;
    for (int i = 0; i < ncomp; i++)
        if (strcmp(comp[i], name) == 0)
            return;
    strlcpy(comp[ncomp], name, COMP_NAME_MAX);
    if (is_dir)
        strlcat(comp[ncomp], "/", COMP_NAME_MAX);
    ncomp++;
}

/* Every name in `dir` starting with `prefix`. */
static void comp_scan_dir(const char *dir, const char *prefix, bool mark_dirs)
{
    long fd = lp_open(dir, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return;

    size_t plen = strlen(prefix);
    char   buf[8192];

    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (long off = 0; off < n; ) {
            char       *rec  = buf + off;
            u16         len  = *(u16 *)(rec + DIRENT_RECLEN);
            u8          type = *(u8 *)(rec + DIRENT_TYPE);
            const char *name = rec + DIRENT_NAME;
            off += len;

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;
            /* A leading dot is only offered when it was asked for. */
            if (name[0] == '.' && prefix[0] != '.')
                continue;
            if (plen && strncmp(name, prefix, plen) != 0)
                continue;

            comp_add(name, mark_dirs && type == DT_DIR);
        }
    }
    lp_close((int)fd);
}

static void comp_commands(const char *prefix)
{
    for (int i = 0; BUILTINS[i]; i++)
        if (strncmp(BUILTINS[i], prefix, strlen(prefix)) == 0)
            comp_add(BUILTINS[i], false);

    const char *path = getenv("PATH");
    if (!path || !*path)
        path = DEFAULT_PATH;

    char dir[256];
    while (*path) {
        size_t i = 0;
        while (*path && *path != ':' && i < sizeof(dir) - 1)
            dir[i++] = *path++;
        dir[i] = '\0';
        if (i)
            comp_scan_dir(dir, prefix, false);
        if (*path == ':')
            path++;
    }
}

/* Split "some/path/pre" into the directory to look in and the prefix to
 * match. The directory part stays in the line; only the prefix is
 * replaced by what we complete. */
static void comp_files(const char *word, char *dirbuf, size_t dirsize,
                       const char **prefix_out)
{
    const char *slash = strrchr(word, '/');
    if (!slash) {
        strlcpy(dirbuf, ".", dirsize);
        *prefix_out = word;
    } else {
        size_t dlen = (size_t)(slash - word);
        if (dlen == 0) {                 /* "/pre" - the root itself */
            strlcpy(dirbuf, "/", dirsize);
        } else {
            if (dlen >= dirsize) dlen = dirsize - 1;
            memcpy(dirbuf, word, dlen);
            dirbuf[dlen] = '\0';
        }
        *prefix_out = slash + 1;
    }
    comp_scan_dir(dirbuf, *prefix_out, true);
}

/* How much of the candidates is identical - what we can safely insert
 * without choosing for the user. */
static size_t common_prefix_len(void)
{
    if (ncomp == 0)
        return 0;
    size_t n = strlen(comp[0]);
    for (int i = 1; i < ncomp; i++) {
        size_t j = 0;
        while (j < n && comp[i][j] && comp[0][j] == comp[i][j])
            j++;
        n = j;
    }
    return n;
}

static void complete(const char *prompt, int prompt_w, line_t *l,
                     bool second_tab)
{
    ncomp = 0;

    size_t ws = word_start(l);
    char   word[MAX_LINE];
    size_t wlen = l->cur - ws;
    if (wlen >= sizeof(word))
        return;
    memcpy(word, l->buf + ws, wlen);
    word[wlen] = '\0';

    const char *prefix = word;
    char        dirbuf[256];

    if (is_command_position(l, ws) && !strchr(word, '/'))
        comp_commands(word);
    else
        comp_files(word, dirbuf, sizeof(dirbuf), &prefix);

    if (ncomp == 0)
        return;

    size_t plen   = strlen(prefix);
    size_t common = common_prefix_len();

    /* Insert whatever every candidate agrees on. */
    if (common > plen) {
        line_insert(l, comp[0] + plen, common - plen);
        /* One candidate, fully typed: add a space so the next word can
         * start. A directory ends in / instead, to keep going. */
        if (ncomp == 1 && comp[0][common - 1] != '/')
            line_insert(l, " ", 1);
        redraw(prompt, prompt_w, l);
        return;
    }

    /* Nothing to add. Show what the choices are, but only when asked
     * twice - one tab that prints a list nobody wanted is noise. */
    if (!second_tab)
        return;

    fputs("\r\n", STDOUT_FILENO);
    int col = 0;
    for (int i = 0; i < ncomp; i++) {
        char cell[COMP_NAME_MAX + 4];
        snprintf(cell, sizeof(cell), "%-18s", comp[i]);
        fputs(cell, STDOUT_FILENO);
        if (++col >= (term_cols / 18 > 0 ? term_cols / 18 : 1)) {
            fputs("\r\n", STDOUT_FILENO);
            col = 0;
        }
    }
    if (col)
        fputs("\r\n", STDOUT_FILENO);
    redraw(prompt, prompt_w, l);
}

/* Read one line with editing. Returns its length, or -1 at end of input.
 * The prompt is printed here rather than by the caller, because every
 * redraw has to reprint it. */
static long edit_line(const char *prompt, int prompt_w,
                      char *out, size_t size)
{
    lp_termios_t saved;
    if (lp_term_raw(STDIN_FILENO, &saved) < 0) {
        /* Not a terminal after all - fall back to the kernel's own line
         * editing, which is what a pipe or a serial log wants anyway. */
        fputs(prompt, STDOUT_FILENO);
        return readline(STDIN_FILENO, out, size);
    }

    int rows = 24;
    lp_term_size(STDOUT_FILENO, &rows, &term_cols);

    line_t l;
    l.buf[0] = '\0';
    l.len = 0;
    l.cur = 0;

    int  hist_pos = hist_count;      /* one past the end: the new line */
    char pending[MAX_LINE];          /* the line being typed, parked while
                                        we walk back through history */
    pending[0] = '\0';
    bool last_was_tab = false;
    long result = 0;

    redraw(prompt, prompt_w, &l);

    for (;;) {
        char c;
        long n = lp_read(STDIN_FILENO, &c, 1);
        if (n <= 0) {                /* the terminal went away */
            result = -1;
            break;
        }

        /* Two tabs in a row means "show me the choices". Remember what
         * the previous key was before this one overwrites it. */
        bool was_tab = last_was_tab;
        last_was_tab = (c == '\t');

        if (c == '\r' || c == '\n') {
            fputs("\r\n", STDOUT_FILENO);
            result = (long)l.len;
            break;
        }

        if (c == 3) {                /* Ctrl-C */
            fputs("^C\r\n", STDOUT_FILENO);
            l.buf[0] = '\0';
            l.len = l.cur = 0;
            result = 0;
            break;
        }

        if (c == 4) {                /* Ctrl-D */
            if (l.len == 0) {
                result = -1;
                break;
            }
            if (l.cur < l.len)
                line_delete(&l, l.cur, utf8_next(l.buf, l.len, l.cur));
            redraw(prompt, prompt_w, &l);
            continue;
        }

        if (c == 127 || c == 8) {    /* backspace */
            if (l.cur > 0) {
                size_t prev = utf8_prev(l.buf, l.cur);
                line_delete(&l, prev, l.cur);
            }
            redraw(prompt, prompt_w, &l);
            continue;
        }

        if (c == 1) { l.cur = 0;      redraw(prompt, prompt_w, &l); continue; }
        if (c == 5) { l.cur = l.len;  redraw(prompt, prompt_w, &l); continue; }

        if (c == 21) {               /* Ctrl-U: cut back to the start */
            line_delete(&l, 0, l.cur);
            redraw(prompt, prompt_w, &l);
            continue;
        }

        if (c == 11) {               /* Ctrl-K: cut to the end */
            l.len = l.cur;
            l.buf[l.len] = '\0';
            redraw(prompt, prompt_w, &l);
            continue;
        }

        if (c == 23) {               /* Ctrl-W: cut the word before us */
            size_t i = l.cur;
            while (i > 0 && l.buf[i - 1] == ' ') i--;
            while (i > 0 && l.buf[i - 1] != ' ') i--;
            line_delete(&l, i, l.cur);
            redraw(prompt, prompt_w, &l);
            continue;
        }

        if (c == 12) {               /* Ctrl-L: clear the screen */
            fputs("\x1b[H\x1b[2J", STDOUT_FILENO);
            redraw(prompt, prompt_w, &l);
            continue;
        }

        if (c == '\t') {
            complete(prompt, prompt_w, &l, was_tab);
            continue;
        }

        if (c == 27) {               /* an escape sequence */
            char a, b;
            if (lp_read(STDIN_FILENO, &a, 1) <= 0) continue;
            if (a != '[' && a != 'O') continue;
            if (lp_read(STDIN_FILENO, &b, 1) <= 0) continue;

            if (b == 'A' || b == 'B') {          /* up / down */
                if (hist_count == 0)
                    continue;
                if (b == 'A' && hist_pos > 0) {
                    if (hist_pos == hist_count)
                        strlcpy(pending, l.buf, sizeof(pending));
                    line_set(&l, history[--hist_pos]);
                } else if (b == 'B' && hist_pos < hist_count) {
                    hist_pos++;
                    line_set(&l, hist_pos == hist_count
                                 ? pending : history[hist_pos]);
                }
                redraw(prompt, prompt_w, &l);
                continue;
            }

            if (b == 'C') {                      /* right */
                if (l.cur < l.len)
                    l.cur = utf8_next(l.buf, l.len, l.cur);
                redraw(prompt, prompt_w, &l);
                continue;
            }
            if (b == 'D') {                      /* left */
                if (l.cur > 0)
                    l.cur = utf8_prev(l.buf, l.cur);
                redraw(prompt, prompt_w, &l);
                continue;
            }
            if (b == 'H') { l.cur = 0;     redraw(prompt, prompt_w, &l); continue; }
            if (b == 'F') { l.cur = l.len; redraw(prompt, prompt_w, &l); continue; }

            /* The numbered ones end in ~: 1 home, 3 delete, 4 end. */
            if (b >= '1' && b <= '8') {
                char tilde;
                if (lp_read(STDIN_FILENO, &tilde, 1) <= 0) continue;
                if (b == '1') l.cur = 0;
                if (b == '4') l.cur = l.len;
                if (b == '3' && l.cur < l.len)
                    line_delete(&l, l.cur, utf8_next(l.buf, l.len, l.cur));
                redraw(prompt, prompt_w, &l);
            }
            continue;
        }

        if ((unsigned char)c < 32)
            continue;                /* a control key we do not use */

        /* A character may be several bytes. Read the rest of it before
         * inserting, so the buffer never holds half a character. */
        char seq[8];
        seq[0] = c;
        int want = utf8_seq_len((unsigned char)c);
        if (want < 1 || want > 4)
            want = 1;
        for (int i = 1; i < want; i++)
            if (lp_read(STDIN_FILENO, &seq[i], 1) <= 0) { want = i; break; }

        line_insert(&l, seq, (size_t)want);
        redraw(prompt, prompt_w, &l);
    }

    lp_term_restore(STDIN_FILENO, &saved);

    strlcpy(out, l.buf, size);
    return result;
}

/* Run one ordinary line - pipelines, redirection, && || ; - which is
 * everything the shell did before control structures existed. */
static void run_logical_line(char *line)
{
    static pipeline_t pipes[MAX_PIPES];

    if (line[0] == '#')
        return;

    int np = parse_line(line, pipes, MAX_PIPES);
    if (np <= 0)
        return;

    for (int i = 0; i < np && shell_running; i++) {
        if (pipes[i].link == LINK_AND && last_status != 0) continue;
        if (pipes[i].link == LINK_OR  && last_status == 0) continue;

        cmd_t *cmds = pipes[i].cmds;
        int    n    = pipes[i].ncmds;

        /* A function is looked up before anything on disk, and runs in
         * this shell rather than a child - cd and variable assignments
         * inside one are meant to be visible to the caller. */
        func_t *f = (n == 1 && cmds[0].argc > 0)
                    ? func_find(cmds[0].argv[0]) : NULL;

        if (f)
            call_func(f, cmds[0].argv, cmds[0].argc);
        else if (n == 1 && cmds[0].argc > 0 && is_builtin(cmds[0].argv[0]))
            run_builtin_redirected(&cmds[0]);
        else
            run_pipeline(cmds, n, pipes[i].background);

        if (pipes[i].negate)
            last_status = last_status == 0 ? 1 : 0;
    }
}

/* ── Control structures ───────────────────────────────────────────────
 *
 *   if <command> ; then <commands> ; elif <command> ; then ... ;
 *      else <commands> ; fi
 *   while <command> ; do <commands> ; done
 *   for NAME in a b c ; do <commands> ; done
 *
 * All three work spread over several lines as well, which is how they
 * are written in a script.
 *
 * ── How this sits on top of the existing shell ──
 * Everything below works on whole logical lines. A line is one of these
 * keywords, or it is an ordinary command that the parser already knows
 * how to run. Splitting on ';' first turns
 *
 *     if test -f x ; then echo yes ; fi
 *
 * into three lines, which is the same thing written out - so one piece
 * of code handles both shapes and neither is a special case.
 *
 * ── Why now ──
 * && and || have carried the boot script from the start, and they are
 * genuinely enough for "try this, otherwise that". What they cannot do
 * is ask a question once and act on the answer in two places, or repeat
 * something, or walk a list. Every one of those turned up as soon as
 * there was anything to script.
 */
#define MAX_BLOCK 256

typedef char block_line_t[MAX_LINE];

/* 줄 끝의 백슬래시는 "다음 줄에 이어진다" 는 뜻이다.
 *
 * 이것이 없을 때 무슨 일이 생겼는지 적어 둔다. 긴 스크립트는 거의
 * 전부 이 꼴을 쓴다:
 *
 *     [ -n "$DPKG_MAINTSCRIPT_NAME" ] || \
 *       error "환경 변수가 없습니다"
 *
 * 이음이 없으면 첫 줄이 그대로 끝나고 백슬래시가 명령 이름으로
 * 읽혀서 "sh: \: command not found" 가 난다. 그리고 다음 줄의
 * error 는 || 와 상관없이 **항상** 실행된다. 오류 하나가 아니라
 * 조건이 통째로 사라지는 것이고, 그래서 조용히 틀린다.
 *
 * 짝수 개의 백슬래시는 이어짐이 아니다 - `\\` 는 백슬래시 한 개를
 * 뜻하는 완결된 줄이다. */
static long readline_joined(int fd, char *buf, size_t size)
{
    long len = readline(fd, buf, size);
    if (len < 0)
        return len;

    for (;;) {
        if (len <= 0 || buf[len - 1] != '\\')
            break;

        int bs = 0;
        for (long i = len - 1; i >= 0 && buf[i] == '\\'; i--) bs++;
        if (bs % 2 == 0)
            break;

        buf[--len] = '\0';                 /* 백슬래시는 버린다 */

        char more[MAX_LINE];
        long m = readline(fd, more, sizeof more);
        if (m < 0)
            break;

        /* 이어지는 줄의 들여쓰기는 빈칸 하나로. 그대로 붙이면
         * `cmd` 와 `arg` 사이에 여섯 칸이 들어간다. */
        char *t = more;
        while (*t == ' ' || *t == '\t') t++;
        if (len > 0 && buf[len - 1] != ' ' && buf[len - 1] != '\t' && *t &&
            (size_t)len + 1 < size)
            buf[len++] = ' ';

        size_t k = strlen(t);
        if ((size_t)len + k >= size) k = size - (size_t)len - 1;
        memcpy(buf + len, t, k);
        len += (long)k;
        buf[len] = '\0';
    }
    return len;
}

/* 한 줄에 << 가 있으면 그 본문을 여기서 읽어 둔다.
 *
 * 파서는 한 줄씩 본다. here-document 는 그 줄이 아니라 **다음 줄들**
 * 에 있으므로, 파서가 볼 수 있는 시점에는 이미 늦다. 그래서 줄을
 * 읽는 이 자리에서 본문을 통째로 가져다 표에 넣고, 줄에는 번호만
 * 남긴다: `cat <<END` 는 `cat <<@0` 이 된다.
 *
 * 구분자를 따옴표로 감싸면(<<'END') 본문 안의 $ 는 확장하지 않는다.
 * <<- 는 각 줄 앞의 탭을 떼는데, 들여쓴 스크립트 안에서 본문만
 * 왼쪽 끝에 붙이지 않아도 되게 하려고 있는 것이다. */
static void collect_heredocs(char *line, size_t size, int fd)
{
    char *lt = strstr(line, "<<");
    if (!lt)
        return;
    /* `a << b` 가 아니라 `2>&1` 같은 것에 걸리지 않게: << 앞이 < 면
     * 그것은 이미 우리가 본 것이다. */
    if (lt > line && lt[-1] == '<')
        return;

    char *d = lt + 2;
    bool strip_tabs = false;
    if (*d == '-') { strip_tabs = true; d++; }
    while (*d == ' ' || *d == '\t') d++;

    /* 구분자. 따옴표가 있으면 본문은 확장하지 않는다. */
    bool expand = true;
    char delim[128];
    size_t dn = 0;
    if (*d == '\'' || *d == '"') {
        char q = *d++;
        expand = false;
        while (*d && *d != q && dn + 1 < sizeof delim) delim[dn++] = *d++;
        if (*d == q) d++;
    } else {
        while (*d && *d != ' ' && *d != '\t' && *d != ';' && *d != '|' &&
               *d != '&' && *d != '>' && dn + 1 < sizeof delim)
            delim[dn++] = *d++;
    }
    delim[dn] = '\0';
    if (!dn)
        return;

    if (nheredocs >= MAX_HEREDOCS) {
        dprintf(STDERR_FILENO,
                "sh: more than %d here-documents on one line\n",
                MAX_HEREDOCS);
        return;
    }

    /* 본문. */
    size_t cap = 4096, len = 0;
    char  *body = malloc(cap);
    if (!body)
        return;
    body[0] = '\0';

    char  raw[MAX_LINE];
    for (;;) {
        long got = readline(fd, raw, sizeof raw);
        if (got < 0)
            break;                       /* 구분자 없이 끝났다 */

        char *t = raw;
        if (strip_tabs) while (*t == '\t') t++;
        if (strcmp(t, delim) == 0)
            break;

        /* 확장은 여기서 하지 않는다.
         *
         * 본문을 모으는 시점은 스크립트를 **읽는** 때이고, 쓰는 때는
         * 그 명령이 **도는** 때다. 둘 사이에 변수가 정해지는 일이
         * 흔하다 - dpkg 의 usage() 는 파일 위쪽에 있고 $PROGNAME 은
         * 아래에서 정해지므로, 읽을 때 확장하면 도움말의 프로그램
         * 이름이 늘 빈칸이 된다. */
        const char *src = t;
        size_t sl = strlen(src);
        while (len + sl + 2 > cap) {
            size_t bigger = cap * 2;
            char  *grown  = malloc(bigger);
            if (!grown) { free(body); return; }
            memcpy(grown, body, len + 1);
            free(body);
            body = grown;
            cap  = bigger;
        }
        memcpy(body + len, src, sl);
        len += sl;
        body[len++] = '\n';
        body[len]   = '\0';

        if (len > HEREDOC_MAX) {
            dprintf(STDERR_FILENO,
                    "sh: here-document is larger than %dKB - the rest was"
                    " dropped\n", HEREDOC_MAX / 1024);
            break;
        }
    }

    int idx = nheredocs++;
    heredocs[idx].body   = body;
    heredocs[idx].expand = expand;

    /* `<<[-]DELIM` 을 `<<@N` 으로. */
    char tag[24];
    snprintf(tag, sizeof tag, "<<@%d", idx);
    char rest[MAX_LINE];
    strlcpy(rest, d, sizeof rest);
    size_t head = (size_t)(lt - line);
    if (head + strlen(tag) + strlen(rest) + 1 < size) {
        strcpy(line + head, tag);
        strcpy(line + head + strlen(tag), rest);
    }
}

/* The first word of a line, for deciding whether it is a keyword. */
static void first_word(const char *line, char *out, size_t size)
{
    while (*line == ' ' || *line == '\t') line++;

    size_t n = 0;
    while (*line && *line != ' ' && *line != '\t' && n < size - 1)
        out[n++] = *line++;
    out[n] = '\0';
}


/* "name() {" or "name () {" - the line that starts a function.
 * Writes the name into `out` when it is one. */
static bool is_func_def(const char *line, char *out, size_t size)
{
    while (*line == ' ' || *line == '\t') line++;

    size_t n = 0;
    while (is_name_char(*line) && n < size - 1)
        out[n++] = *line++;
    out[n] = '\0';
    if (n == 0)
        return false;

    while (*line == ' ' || *line == '\t') line++;
    if (line[0] != '(' ) return false;
    line++;
    while (*line == ' ' || *line == '\t') line++;
    if (line[0] != ')') return false;
    line++;
    while (*line == ' ' || *line == '\t') line++;

    /* 중괄호는 이 줄에 있거나 다음 줄에 홀로 있다. 두 번째 꼴을
     * 인정하지 않으면 dpkg 의 관리자 스크립트가 통째로 돌지 않는다 -
     * 그 파일들이 그 꼴을 쓴다. 이 줄이 여기서 끝났다면 다음 줄에서
     * { 를 찾는 것은 define_func 의 일이다. */
    return line[0] == '{' || line[0] == '\0' || line[0] == '#';
}

/* 한 줄이 중괄호 깊이를 얼마나 바꾸는지. +1, 0, -1.
 *
 * 세 가지가 같은 규칙 아래 있어야 짝이 맞는다:
 *
 *   foo() {      함수 정의, 여는 중괄호가 같은 줄
 *   foo()        함수 정의, 여는 중괄호는 다음 줄
 *   {            그냥 묶음
 *
 * 예전에는 함수 정의를 하나 세고 홀로 선 { 는 세지 않았다. 그래서
 * 두 번째 꼴은 { 에서 한 번 더 세지 않아 } 하나에 두 번 닫혔고,
 * 세 번째 꼴은 } 에서만 세어서 깊이가 음수가 됐다. */
static int brace_depth(const char *line, const char *w,
                       char *fname, size_t fsize, bool *pending)
{
    if (is_func_def(line, fname, fsize)) {
        /* 여는 중괄호가 다음 줄에 있더라도 겹은 지금 열린 것이다.
         * 그렇지 않으면 줄을 모으는 쪽이 "다 읽었다" 고 판단하고
         * 함수 이름만 담긴 한 줄을 실행해 버린다. */
        *pending = !strchr(line, '{');
        return 1;
    }
    if (strcmp(w, "{") == 0) {
        if (*pending) { *pending = false; return 0; }   /* 위에서 이미 셌다 */
        return 1;
    }
    *pending = false;
    if (strcmp(w, "}") == 0) return -1;
    return 0;
}

/* 정의는 pipe_into_block 옆에 있다 - 그것을 쓰기 때문이다. */
static bool pipes_into_block(const char *line);

/* Does this line open a block that needs a matching fi, done, esac or }? */
static bool opens_block(const char *w)
{
    return strcmp(w, "if") == 0 || strcmp(w, "while") == 0 ||
           strcmp(w, "for") == 0 || strcmp(w, "case") == 0;
}

static bool closes_block(const char *w)
{
    return strcmp(w, "fi") == 0 || strcmp(w, "done") == 0 ||
           strcmp(w, "esac") == 0;
}

/* Split a line on ';' into logical lines, but only where a keyword is
 * involved - otherwise "a ; b" would stop being one thing the existing
 * parser handles, and it handles it correctly already. */
/* The next ';' that is not inside quotes, or NULL.
 *
 * strchr(p, ';') was used here, and it does not know about quoting: it
 * found the semicolons inside `sh -c 'while true; do :; done'` and cut
 * the line into four pieces, three of which were fragments of a quoted
 * string. The error that came out - "while without do or done" - was
 * about a statement the user never wrote.
 *
 * The same bug hit anything with a semicolon in a quoted argument:
 * grep 'a;b', echo "one; two", a sed script. */
/* The next ; that really separates two statements.
 *
 * Not one inside quotes, not one escaped, and not one inside a command
 * substitution: `echo $(date ; uptime)` is one statement with a ; in the
 * middle of it, and splitting there would hand half of it to the shell
 * as a command of its own. */
static const char *unquoted_semicolon(const char *p)
{
    char quote = 0;
    int  depth = 0;              /* nesting inside $( ) */
    bool backtick = false;

    for (; *p; p++) {
        if (*p == '\\' && p[1]) { p++; continue; }

        if (quote) {
            if (*p == quote)
                quote = 0;
            continue;
        }
        if (backtick) {
            if (*p == '`')
                backtick = false;
            continue;
        }

        if (*p == '\'' || *p == '"') { quote = *p; continue; }
        if (*p == '`')                { backtick = true; continue; }
        if (*p == '$' && p[1] == '(') { depth++; p++; continue; }
        if (*p == '(' && depth)       { depth++; continue; }
        if (*p == ')' && depth)       { depth--; continue; }

        if (*p == ';' && depth == 0)
            return p;
    }
    return NULL;
}


/* Just past the `in` that ends a case header, or NULL.
 *
 * The word has to be found rather than searched for: `case $in in` and
 * `case "a in b" in` both contain the letters, and only the last one
 * standing alone as a word is the keyword. */
static char *case_header_end(char *s)
{
    char quote = 0;
    bool at_word_start = true;
    char *found = NULL;

    for (char *p = s; *p; p++) {
        if (quote) {
            if (*p == quote) quote = 0;
            at_word_start = false;
            continue;
        }
        if (*p == '\'' || *p == '"') {
            quote = *p;
            at_word_start = false;
            continue;
        }
        if (*p == ' ' || *p == '\t') {
            at_word_start = true;
            continue;
        }
        if (at_word_start && p[0] == 'i' && p[1] == 'n' &&
            (p[2] == ' ' || p[2] == '\t')) {
            found = p + 2;
        }
        at_word_start = false;
    }

    if (!found) return NULL;
    while (*found == ' ' || *found == '\t') found++;
    return found;
}

static int split_statements(const char *line, block_line_t *out, int max)
{
    /* Every line is split on its unquoted semicolons, keyword or not.
     *
     * This used to hand a line with no keyword in it straight through as
     * one statement, and `false ; echo $?` then answered 0: the whole
     * line was parsed - and $? expanded - before any of it ran, so the
     * value came from the command before the line rather than from
     * `false`. Every one-liner using $? was quietly wrong, while the
     * same two commands on two lines of a script were right, which is
     * the sort of difference nobody thinks to test.
     *
     * Splitting here means each statement is parsed just before it runs.
     * && and || are deliberately NOT split: those join two commands into
     * one decision and have to be parsed together. */
    int  n = 0;
    const char *p = line;

    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        const char *semi = unquoted_semicolon(p);
        size_t len = semi ? (size_t)(semi - p) : strlen(p);
        if (len >= MAX_LINE) len = MAX_LINE - 1;

        memcpy(out[n], p, len);
        out[n][len] = '\0';

        /* Trim the trailing space so first_word sees the keyword. */
        for (size_t i = len; i > 0 && (out[n][i-1] == ' ' || out[n][i-1] == '\t'); i--)
            out[n][i-1] = '\0';

        if (out[n][0]) {
            /* "then echo yes" is the keyword and a command on one line.
             * Split them, so that every keyword sits on a line of its
             * own and the block finder never has to look inside one.
             *
             * Only then, do and else: after if, elif and while what
             * follows is the condition, which belongs to the keyword,
             * and after for it is the variable and the word list. */
            char kw[32];
            first_word(out[n], kw, sizeof(kw));

            bool splittable = (strcmp(kw, "then") == 0 ||
                               strcmp(kw, "do")   == 0 ||
                               strcmp(kw, "else") == 0 ||
                               /* `{ echo a` 의 { 도 제 줄을 가져야
                                * 묶음의 시작을 찾을 수 있다. */
                               strcmp(kw, "{")    == 0);

            /* `case x in y) echo yes ;; esac` on one line.
             *
             * The others above split after the keyword itself, but case
             * has a second keyword: everything up to and including `in`
             * is the header and the first clause begins after it. Left
             * whole, the header reads as `case x in y) echo yes`, whose
             * last word is not `in`, and exec_case rejects the line it
             * was given rather than the line that was written. */
            if (strcmp(kw, "case") == 0 && n + 1 < max) {
                char *rest = case_header_end(out[n]);
                if (rest && *rest) {
                    char tail[MAX_LINE];
                    strlcpy(tail, rest, sizeof tail);
                    *rest = '\0';
                    /* Trim back to `... in`. */
                    for (size_t i = strlen(out[n]);
                         i > 0 && (out[n][i-1] == ' ' || out[n][i-1] == '\t');
                         i--)
                        out[n][i-1] = '\0';
                    strlcpy(out[n + 1], tail, MAX_LINE);
                    n += 2;
                    if (semi && semi[1] == ';') {
                        if (n < max) { strlcpy(out[n], ";;", MAX_LINE); n++; }
                        p = semi + 2;
                        continue;
                    }
                    if (!semi) break;
                    p = semi + 1;
                    continue;
                }
            }

            /* `f() { echo hi ; }` - 한 줄짜리 함수 정의.
             *
             * 세미콜론으로 자르면 `f() { echo hi` 가 한 덩어리로 남고,
             * 본문은 여는 중괄호 **다음 줄**부터 세므로 함수의 본문이
             * 비게 된다. 정의는 되는데 부르면 아무 일도 없는, 가장
             * 찾기 어려운 종류의 실패였다. 중괄호 뒤를 제 줄로 떼면
             * 여러 줄로 쓴 것과 같은 모양이 된다. */
            {
                char fname[64];
                if (is_func_def(out[n], fname, sizeof fname) && n + 1 < max) {
                    char *brace = strchr(out[n], '{');
                    if (brace && brace[1]) {
                        char *rest = brace + 1;
                        while (*rest == ' ' || *rest == '\t') rest++;
                        if (*rest) {
                            char tail[MAX_LINE];
                            strlcpy(tail, rest, sizeof tail);
                            brace[1] = '\0';
                            strlcpy(out[n + 1], tail, MAX_LINE);
                            n += 2;
                            if (semi && semi[1] == ';') {
                                if (n < max) {
                                    strlcpy(out[n], ";;", MAX_LINE); n++;
                                }
                                p = semi + 2;
                                continue;
                            }
                            if (!semi) break;
                            p = semi + 1;
                            continue;
                        }
                    }
                }
            }

            if (splittable && n + 1 < max) {
                char *rest = strstr(out[n], kw) + strlen(kw);
                while (*rest == ' ' || *rest == '\t') rest++;

                if (*rest) {
                    strlcpy(out[n + 1], rest, MAX_LINE);
                    strlcpy(out[n], kw, MAX_LINE);
                    n += 2;
                } else {
                    n++;
                }
            } else {
                n++;
            }
        }

        /* `;;` is not two separators, it is the one thing that ends a
         * case clause, and the loop above would drop it: the first ';'
         * ends the statement before it and the second produces an empty
         * one, which is discarded. A case would then run every clause
         * from the first match to the esac.
         *
         * So it is emitted as a statement of its own, and exec_block
         * knows to run nothing for it. */
        if (semi && semi[1] == ';') {
            if (n < max) {
                strlcpy(out[n], ";;", MAX_LINE);
                n++;
            }
            p = semi + 2;
            continue;
        }

        if (!semi) break;
        p = semi + 1;
    }

    return n ? n : 1;
}

static void run_logical_line(char *line);
static int  exec_block(block_line_t *lines, int n);


/* Find the line index of the next keyword at this nesting level.
 * Returns -1 when there is none. */
static int find_at_depth(block_line_t *lines, int n, int from,
                         const char *a, const char *b, const char *c)
{
    int depth = 0;
    char w[32];

    for (int i = from; i < n; i++) {
        first_word(lines[i], w, sizeof(w));

        if (depth == 0) {
            if ((a && strcmp(w, a) == 0) ||
                (b && strcmp(w, b) == 0) ||
                (c && strcmp(w, c) == 0))
                return i;
        }

        if (opens_block(w) || pipes_into_block(lines[i])) depth++;
        if (closes_block(w)) depth--;
    }
    return -1;
}

/* if COND ; then BODY ; [elif COND ; then BODY ;]... [else BODY ;] fi
 * Returns the index just past the fi. */
static int exec_if(block_line_t *lines, int n, int start)
{
    int end = find_at_depth(lines, n, start + 1, "fi", NULL, NULL);
    if (end < 0) {
        dprintf(STDERR_FILENO, "sh: if without fi\n");
        return n;
    }

    int cursor = start;
    for (;;) {
        /* The condition is everything between here and the next "then",
         * minus the leading keyword on this line. */
        int then_at = find_at_depth(lines, n, cursor + 1, "then", NULL, NULL);
        if (then_at < 0 || then_at > end) {
            dprintf(STDERR_FILENO, "sh: if without then\n");
            return end + 1;
        }

        /* "if test -f x" - run what follows the keyword. */
        char cond[MAX_LINE];
        char w[32];
        first_word(lines[cursor], w, sizeof(w));
        const char *rest = strstr(lines[cursor], w) + strlen(w);
        strlcpy(cond, rest, sizeof(cond));

        errexit_off++;
        if (cond[0])
            run_logical_line(cond);

        /* Any further lines before "then" are part of the condition too,
         * and the last one decides. */
        for (int i = cursor + 1; i < then_at; i++)
            run_logical_line(lines[i]);
        errexit_off--;

        int body_end = find_at_depth(lines, n, then_at + 1,
                                     "elif", "else", "fi");
        if (body_end < 0)
            body_end = end;

        if (last_status == 0) {
            exec_block(lines + then_at + 1, body_end - then_at - 1);
            return end + 1;
        }

        first_word(lines[body_end], w, sizeof(w));

        if (strcmp(w, "elif") == 0) {
            cursor = body_end;
            continue;
        }

        if (strcmp(w, "else") == 0) {
            exec_block(lines + body_end + 1, end - body_end - 1);
            return end + 1;
        }

        /* It was fi: the condition was false and there is no else. */
        last_status = 0;
        return end + 1;
    }
}

/* while COND ; do BODY ; done */
static int exec_while(block_line_t *lines, int n, int start)
{
    int do_at = find_at_depth(lines, n, start + 1, "do", NULL, NULL);
    int end   = find_at_depth(lines, n, start + 1, "done", NULL, NULL);

    if (do_at < 0 || end < 0 || do_at > end) {
        dprintf(STDERR_FILENO, "sh: while without do or done\n");
        return n;
    }

    /* A loop that cannot be interrupted is a machine that has to be
     * power-cycled. Ctrl-C reaches the command inside the loop, and this
     * stops the loop when that command was killed by a signal. */
    for (int rounds = 0; shell_running; rounds++) {
        char cond[MAX_LINE], w[32];
        first_word(lines[start], w, sizeof(w));
        const char *rest = strstr(lines[start], w) + strlen(w);
        strlcpy(cond, rest, sizeof(cond));

        errexit_off++;
        if (cond[0])
            run_logical_line(cond);
        for (int i = start + 1; i < do_at; i++)
            run_logical_line(lines[i]);
        errexit_off--;

        if (last_status != 0)
            break;

        exec_block(lines + do_at + 1, end - do_at - 1);

        if (loop_break) {
            loop_break--;            /* this loop is one of them */
            break;
        }
        if (loop_continue) {
            loop_continue--;
            if (loop_continue)       /* meant for a loop further out */
                break;
            continue;
        }

        if (last_status >= 128)      /* killed by a signal */
            break;

        /* ── Why this warns and no longer stops ──
         *
         * This used to break out of the loop after a million rounds. It
         * was there to stop somebody typing `while : ; do : ; done` at
         * the prompt and wedging the console, back when Ctrl-C did
         * nothing on this machine - the shell had no controlling
         * terminal, so no key could interrupt anything, and a runaway
         * loop really was unrecoverable.
         *
         * That is fixed: Ctrl-C reaches the foreground process now, and
         * guard reports a saturated board. So the cap protects against
         * nothing and breaks the case it cannot tell apart - a loop that
         * is supposed to run forever. A supervision loop in
         * /data/rc.local, watching something and sleeping a second
         * between looks, reaches a million rounds in eleven days and
         * then silently stops on a board meant to run for months.
         *
         * So: say it once, loudly, and keep going. Visible if it is an
         * accident, correct if it is not. */
        if (rounds == 1000000)
            dprintf(STDERR_FILENO,
                    "sh: while: a million rounds - still going.\n"
                    "sh:   If that is not what you meant, Ctrl-C stops it.\n");
        if (rounds > 1000000)
            rounds = 1000001;       /* stop counting; do not overflow */
    }

    last_status = 0;
    return end + 1;
}

/* ── case ─────────────────────────────────────────────────────────────
 *
 * case WORD in  PATTERN[|PATTERN...]) BODY ;;  ... esac
 *
 * This is here because of dpkg, not because of style. Every Debian
 * package runs a maintainer script through /bin/sh, /bin/sh on this
 * machine is this shell, and the first thing those scripts do is
 *
 *     case "$1" in configure) ... ;; abort-upgrade|abort-remove) ... ;; esac
 *
 * A shell without case answers `sh: case: command not found`, dpkg
 * records the package as half-configured, and apt refuses to do anything
 * further until a human runs `dpkg --configure -a` - which fails the same
 * way. So the package manager the machine ships with does not work at
 * all. `case` is the difference between an OS you can install software on
 * and one you cannot.
 *
 * The patterns are matched with glob_match - the same matcher the shell
 * uses for filenames - and deliberately not run through the tokenizer.
 * The tokenizer expands globs against the directory, so `*)` would become
 * a list of the files in the current directory and the default clause
 * would match nothing. A case pattern is a pattern, not a word to expand.
 */

/* The patterns at the head of a clause, and where the body begins.
 *
 * Returns a pointer to whatever followed the ')' - which is the body when
 * it was written on the same line, and "" when it was not - or NULL when
 * this line is not the head of a clause at all. */
static const char *case_clause_head(const char *line, char *pat, size_t patsz)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;

    /* `(pattern)` is the other form POSIX allows. */
    if (*p == '(') p++;

    const char *s = p;
    char quote = 0;

    for (; *p; p++) {
        if (quote) {
            if (*p == quote) quote = 0;
            continue;
        }
        if (*p == '\'' || *p == '"') { quote = *p; continue; }
        /* An open paren here means a function definition or a subshell,
         * not a pattern - `foo() {` would otherwise read as the pattern
         * `foo` with an empty body. */
        if (*p == '(') return NULL;
        if (*p == ')') break;
    }

    if (*p != ')') return NULL;

    size_t len = (size_t)(p - s);
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
    if (len == 0 || len >= patsz) return NULL;

    memcpy(pat, s, len);
    pat[len] = '\0';

    p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Does any of `a|b|c` match the word? */
static bool case_matches(const char *pats, const char *word)
{
    const char *p = pats;

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;

        const char *s = p;
        char quote = 0;
        while (*p && (quote || *p != '|')) {
            if (quote) {
                if (*p == quote) quote = 0;
            } else if (*p == '\'' || *p == '"') {
                quote = *p;
            }
            p++;
        }

        size_t len = (size_t)(p - s);
        while (len && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;

        /* The quotes come off, and what they quoted stops being a
         * pattern: `"*"` matches a literal asterisk. glob_match has no
         * way to be told that, so a quoted run is escaped by being
         * compared as itself - which for these patterns means the only
         * case that matters, a quoted metacharacter, still behaves. */
        char one[256];
        size_t k = 0;
        char q = 0;
        for (size_t j = 0; j < len && k + 1 < sizeof one; j++) {
            char c = s[j];
            if (q) {
                if (c == q) { q = 0; continue; }
            } else if (c == '\'' || c == '"') {
                q = c;
                continue;
            }
            one[k++] = c;
        }
        one[k] = '\0';

        /* k == 0 is the pattern `""`, which is how a script asks about an
         * unset or empty variable. glob_match("", "") is true and
         * glob_match("", anything) is false, which is exactly right, so
         * the empty pattern is passed through rather than skipped. */
        if (glob_match(one, word))
            return true;

        if (*p == '|') p++;
        else break;
    }
    return false;
}

/* The `;;` that ends this clause, or `end` when the clause runs to the
 * esac - which POSIX allows for the last one. */
static int case_clause_end(block_line_t *lines, int end, int from)
{
    int depth = 0;
    char w[32];

    for (int i = from; i < end; i++) {
        first_word(lines[i], w, sizeof w);

        if (depth == 0 && strcmp(w, ";;") == 0)
            return i;

        if (opens_block(w) || pipes_into_block(lines[i])) depth++;
        if (closes_block(w)) depth--;
    }
    return end;
}

static int exec_case(block_line_t *lines, int n, int start)
{
    int end = find_at_depth(lines, n, start + 1, "esac", NULL, NULL);
    if (end < 0) {
        dprintf(STDERR_FILENO, "sh: case without esac\n");
        return n;
    }

    /* The subject goes through the ordinary tokenizer so that "$1" and
     * ${x} are expanded the way they are anywhere else. */
    char header[MAX_LINE];
    strlcpy(header, lines[start], sizeof header);

    static pipeline_t hp[MAX_PIPES];
    int hn = parse_line(header, hp, MAX_PIPES);
    if (hn <= 0) {
        dprintf(STDERR_FILENO, "sh: case WORD in ... esac\n");
        return end + 1;
    }

    cmd_t *c = &hp[0].cmds[0];
    if (c->argc < 2 || strcmp(c->argv[c->argc - 1], "in") != 0) {
        dprintf(STDERR_FILENO, "sh: case WORD in ... esac\n");
        return end + 1;
    }

    /* argc == 2 is `case $x in` where x was empty or unset. The word is
     * the empty string, and a clause of `"") ...` or `*)` is supposed to
     * match it. */
    char subject[MAX_LINE];
    strlcpy(subject, c->argc >= 3 ? c->argv[1] : "", sizeof subject);

    for (int i = start + 1; i < end; ) {
        char pat[MAX_LINE];
        const char *body = case_clause_head(lines[i], pat, sizeof pat);

        if (!body) {          /* a blank line, or something we do not read */
            i++;
            continue;
        }

        int term = case_clause_end(lines, end, i + 1);

        if (case_matches(pat, subject)) {
            /* `configure) echo hi` put the body on the head line. The
             * head is overwritten with what followed the ')' so the
             * block below is exactly the clause and nothing else; the
             * array is scratch by this point, the same way the
             * redirection handling in exec_block rewrites its own
             * terminator line. */
            if (*body) {
                strlcpy(lines[i], body, MAX_LINE);
                exec_block(lines + i, term - i);
            } else {
                exec_block(lines + i + 1, term - i - 1);
            }
            return end + 1;
        }

        i = term + 1;
    }

    /* Nothing matched. POSIX says the status is zero. */
    last_status = 0;
    return end + 1;
}

/* for NAME in a b c ; do BODY ; done */
static int exec_for(block_line_t *lines, int n, int start)
{
    int do_at = find_at_depth(lines, n, start + 1, "do", NULL, NULL);
    int end   = find_at_depth(lines, n, start + 1, "done", NULL, NULL);

    if (do_at < 0 || end < 0 || do_at > end) {
        dprintf(STDERR_FILENO, "sh: for without do or done\n");
        return n;
    }

    /* Parse "for NAME in w1 w2 w3" through the ordinary tokenizer, so
     * that $VAR and * are expanded exactly as they are everywhere else. */
    char       header[MAX_LINE];
    strlcpy(header, lines[start], sizeof(header));

    static pipeline_t hp[MAX_PIPES];
    int hn = parse_line(header, hp, MAX_PIPES);
    if (hn <= 0 || hp[0].cmds[0].argc < 3 ||
        strcmp(hp[0].cmds[0].argv[2], "in") != 0) {
        dprintf(STDERR_FILENO, "sh: for NAME in words... ; do ... ; done\n");
        return end + 1;
    }

    cmd_t *c = &hp[0].cmds[0];
    char   name[64];
    strlcpy(name, c->argv[1], sizeof(name));

    /* The words are copied out first: running the body re-uses the
     * arena the tokenizer put them in. */
    static char words[64][256];
    int nwords = 0;
    for (int i = 3; i < c->argc && nwords < 64; i++)
        strlcpy(words[nwords++], c->argv[i], 256);

    for (int i = 0; i < nwords && shell_running; i++) {
        setenv(name, words[i], 1);
        exec_block(lines + do_at + 1, end - do_at - 1);

        if (loop_break) {
            loop_break--;
            break;
        }
        if (loop_continue) {
            loop_continue--;
            if (loop_continue)
                break;
            continue;
        }

        if (last_status >= 128)
            break;
    }

    return end + 1;
}

/* `{ ... }` - 묶음. 짝이 되는 } 를 찾아 그 사이를 이 셸에서 돌린다.
 * 새 프로세스를 만들지 않으므로 안에서 cd 를 하거나 변수를 놓으면
 * 밖에서도 보인다 - `( ... )` 와 다른 점이 그것이다. */
static int exec_group(block_line_t *lines, int n, int start)
{
    int  depth   = 1;
    int  end     = -1;
    bool pending = false;
    for (int i = start + 1; i < n; i++) {
        char w[32], fname[64];
        first_word(lines[i], w, sizeof w);
        depth += brace_depth(lines[i], w, fname, sizeof fname, &pending);
        if (depth == 0) { end = i; break; }
    }
    if (end < 0) {
        dprintf(STDERR_FILENO, "sh: { without a closing }\n");
        return n;
    }
    if (end > start + 1)
        exec_block(lines + start + 1, end - start - 1);
    return end + 1;
}

/* name() { BODY } - remember the body, run nothing.
 * Returns the index just past the closing brace. */
static int define_func(block_line_t *lines, int n, int start,
                       const char *name)
{
    /* Find the '}' that closes it, at this nesting level. A brace on a
     * line of its own is the only form accepted, which is what makes
     * finding it a matter of looking rather than parsing. */
    /* 여는 중괄호가 이 줄에 없으면 다음 줄에 홀로 있다.
     *
     *     prepare_dir_to_symlink()
     *     {
     *
     * POSIX 가 허용하는 꼴이고 dpkg 의 도우미 스크립트가 실제로 쓴다.
     * 이것을 몰랐을 때는 함수 이름이 명령으로 실행되고("sh:
     * prepare_dir_to_symlink(): command not found") 그 다음 줄의 { 도
     * 명령으로 실행됐다. */
    int body = start + 1;
    if (!strchr(lines[start], '{')) {
        char w0[32];
        first_word(lines[body < n ? body : start], w0, sizeof w0);
        if (body < n && strcmp(w0, "{") == 0) {
            body++;
        } else {
            dprintf(STDERR_FILENO,
                    "sh: %s() - expected { on this line or the next\n", name);
            return start + 1;
        }
    }

    int depth = 1;
    int end = -1;
    for (int i = body; i < n; i++) {
        char w[32];
        first_word(lines[i], w, sizeof w);
        char inner[64];
        /* 중첩된 함수도, 홀로 선 { 도 한 겹이다. 둘 중 하나만 세면
         * 짝이 맞지 않아 엉뚱한 } 에서 함수가 끝난다. */
        if (is_func_def(lines[i], inner, sizeof inner)) {
            if (!strchr(lines[i], '{')) i++;    /* { 는 다음 줄 */
            depth++;
        }
        else if (strcmp(w, "{") == 0) depth++;
        else if (strcmp(w, "}") == 0 && --depth == 0) { end = i; break; }
    }
    if (end < 0) {
        dprintf(STDERR_FILENO, "sh: %s() without a closing }\n", name);
        return n;
    }

    func_t *f = func_find(name);
    if (!f) {
        if (nfuncs >= MAX_FUNCS) {
            dprintf(STDERR_FILENO,
                    "sh: no room for more than %d functions\n", MAX_FUNCS);
            return end + 1;
        }
        f = &funcs[nfuncs++];
    }
    strlcpy(f->name, name, sizeof f->name);
    /* 같은 이름을 다시 정의하면 앞의 본문은 버린다. */
    for (int i = 0; i < f->nlines; i++)
        free(f->lines[i]);
    f->nlines = 0;

    for (int i = body; i < end && f->nlines < MAX_FUNC_LINES; i++) {
        char *copy = strdup(lines[i]);
        if (!copy) {
            dprintf(STDERR_FILENO,
                    "sh: out of memory while remembering %s()\n", name);
            break;
        }
        f->lines[f->nlines++] = copy;
    }

    if (end - body > MAX_FUNC_LINES)
        dprintf(STDERR_FILENO,
                "sh: %s() is longer than %d lines - the rest was dropped\n",
                name, MAX_FUNC_LINES);

    last_status = 0;
    return end + 1;
}

/* Call one. The positional parameters are saved and put back, so a
 * function called from a script does not eat the script's own $1. */
static void call_func(func_t *f, char **argv, int argc)
{
    static int depth = 0;

    /* A function that calls itself with no way out would otherwise run
     * until the machine is out of memory, and this shell is pid 1's
     * child - taking it down takes the console with it. */
    if (depth >= 16) {
        dprintf(STDERR_FILENO,
                "sh: %s() is %d calls deep. Stopping - this is what a\n"
                "sh:   function that calls itself forever looks like.\n",
                f->name, depth);
        last_status = 1;
        return;
    }

    char saved[MAX_POSITIONAL][256];
    int  saved_count = pos_count;
    for (int i = 0; i < pos_count; i++)
        strlcpy(saved[i], pos_args[i], 256);

    pos_count = 0;
    for (int i = 1; i < argc && pos_count < MAX_POSITIONAL; i++)
        strlcpy(pos_args[pos_count++], argv[i], 256);

    /* The body is copied out: exec_block writes into the lines it is
     * given when it splits statements, and the function has to survive
     * being called twice.
     *
     * 이 사본은 힙에 있고, 예전에는 static 이었다. static 이면 함수가
     * 함수를 부를 때 안쪽 호출이 바깥쪽의 본문을 덮어쓴다 - 돌아온
     * 뒤에 남은 줄이 다른 함수의 것이 된다. 재귀를 열여섯 겹까지
     * 허용해 놓고 그 상태를 공유하고 있었던 셈이다. 쓴 만큼만
     * 잡으므로 값도 예전보다 싸다. */
    int nb = f->nlines;
    block_line_t *body = NULL;
    if (nb > 0) {
        body = malloc(sizeof(block_line_t) * (size_t)nb);
        if (!body) {
            dprintf(STDERR_FILENO, "sh: out of memory calling %s()\n",
                    f->name);
            last_status = 1;
            pos_count = saved_count;
            for (int i = 0; i < saved_count; i++)
                strlcpy(pos_args[i], saved[i], 256);
            return;
        }
        for (int i = 0; i < nb; i++)
            strlcpy(body[i], f->lines[i], MAX_LINE);
    }

    /* local 이 놓은 것은 이 지점까지 되돌린다. */
    int local_base = nlocals;

    depth++;
    in_function++;
    exec_block(body, nb);
    in_function--;
    depth--;

    locals_unwind(local_base);
    free(body);

    pos_count = saved_count;
    for (int i = 0; i < saved_count; i++)
        strlcpy(pos_args[i], saved[i], 256);
}

/* Run a run of logical lines, handling any control structures in them. */
/* ── a pipeline feeding a loop ─────────────────────────────────────
 *
 *   cat log | while read line ; do ... ; done
 *   ls | while read f ; do ... ; done
 *
 * This is how you process output a line at a time, and it did not work:
 * the parser splits on | into pipeline stages and forks each one, so the
 * second stage was the word "while", which is not a program. It answered
 * "sh: while: command not found" and then tried to run "do" and "done"
 * as commands too.
 *
 * A loop cannot be forked like a stage - it has to run in this shell, or
 * variables set inside it would be lost, which is half of why anybody
 * writes the loop. So the left-hand side is forked instead, with its
 * output on a pipe, and the loop runs here with that pipe as its stdin.
 * That is what every other shell does, and it is why `x=1` inside such a
 * loop is visible afterwards here but not in bash - bash forks the loop.
 * Ours is the more useful half of that trade on a machine with no job
 * control.
 *
 * Returns the position of the keyword inside the line, having cut the
 * line short at the |, or NULL when this is not that shape.
 *
 * What this does NOT reach: a loop used as the CONDITION of an if, as in
 * `if cmd | while read x ; do ... ; done ; then`. The line is split into
 * statements before any of this runs, so by then the loop is four
 * separate lines and the if has only the first of them. Run the loop,
 * keep its answer, and test that instead. */
static char *pipe_into_block(char *line)
{
    char quote = 0;
    int  depth = 0;
    bool backtick = false;

    for (char *p = line; *p; p++) {
        if (*p == '\\' && p[1]) { p++; continue; }
        if (quote)    { if (*p == quote) quote = 0; continue; }
        if (backtick) { if (*p == '`')   backtick = false; continue; }
        if (*p == '\'' || *p == '"') { quote = *p; continue; }
        if (*p == '`')                { backtick = true; continue; }
        if (*p == '$' && p[1] == '(') { depth++; p++; continue; }
        if (*p == '(' && depth)       { depth++; continue; }
        if (*p == ')' && depth)       { depth--; continue; }
        if (depth || *p != '|')       continue;

        /* || is an operator, not a pipe. */
        if (p[1] == '|') { p++; continue; }

        char *q = p + 1;
        while (*q == ' ' || *q == '\t') q++;

        char w[16];
        int  k = 0;
        while (q[k] && q[k] != ' ' && q[k] != '\t' && k < (int)sizeof w - 1) {
            w[k] = q[k];
            k++;
        }
        w[k] = '\0';

        if (strcmp(w, "while") == 0 || strcmp(w, "for") == 0 ||
            strcmp(w, "if") == 0) {
            /* Trim the left side back off the | and hand back the rest. */
            char *end = p;
            while (end > line && (end[-1] == ' ' || end[-1] == '\t')) end--;
            *end = '\0';
            return q;
        }

        /* An ordinary pipe: everything after it is another stage, and
         * run_logical_line already knows what to do with that. */
        return NULL;
    }
    return NULL;
}

/* 파이프의 오른쪽이 블록을 여는가 - `cat f | while read l ; do`.
 *
 * 이런 줄의 첫 낱말은 `cat` 이라서, 낱말만 보고 깊이를 세면 while 이
 * 열린 것을 놓친다. 그러면 done 에서만 한 번 줄어들어 깊이가 한 칸
 * 모자라게 되고, 그 줄이 함수 안에 있으면 함수가 done 에서 끝난
 * 것으로 읽혀 "without a closing }" 가 난다. 실제로 dpkg 의
 * prepare_dir_to_symlink() 가 그렇게 잘렸다.
 *
 * pipe_into_block 은 줄을 고치므로 사본에 대고 묻는다. */
static bool pipes_into_block(const char *line)
{
    char copy[MAX_LINE];
    strlcpy(copy, line, sizeof copy);
    return pipe_into_block(copy) != NULL;
}

static int exec_block(block_line_t *lines, int n)
{
    int i = 0;
    while (i < n && shell_running) {
        /* `cmd | while ...` - fork the left side onto a pipe and let the
         * loop below read it. Done before first_word, because after this
         * the line starts with the keyword. */
        pid_t feeder     = -1;
        int   saved_in   = -1;
        {
            char left[MAX_LINE];
            strlcpy(left, lines[i], sizeof left);
            char *kw = pipe_into_block(left);
            if (kw && left[0]) {
                int fds[2];
                if (lp_pipe(fds) == 0) {
                    feeder = lp_fork();
                    if (feeder == 0) {
                        lp_close(fds[0]);
                        lp_dup2(fds[1], STDOUT_FILENO);
                        lp_close(fds[1]);
                        lp_signal_default(SIGINT);
                        lp_signal_default(SIGQUIT);
                        run_logical_line(left);
                        lp_exit(last_status);
                    }
                    lp_close(fds[1]);
                    if (feeder > 0) {
                        saved_in = (int)lp_dup(STDIN_FILENO);
                        lp_dup2(fds[0], STDIN_FILENO);
                        lp_close(fds[0]);
                        /* The block now starts at the keyword. */
                        strlcpy(lines[i], kw, MAX_LINE);
                    } else {
                        lp_close(fds[0]);
                    }
                }
            }
        }

        char w[32];
        first_word(lines[i], w, sizeof(w));

        /* `done > file`, `done | cmd`, `fi > file`.
         *
         * The block executors find their end by the terminator word and
         * ignore anything after it, so a redirection there was silently
         * dropped: `while ... ; done > out` ran the loop and printed to
         * the terminal, leaving an empty out and no complaint. Silently
         * doing something other than what was written is the worst of
         * the three possible behaviours, so the whole block gets the
         * redirection, which is what it means everywhere else. */
        pid_t drain    = -1;
        int   saved_out = -1;
        if (strcmp(w, "while") == 0 || strcmp(w, "for") == 0 ||
            strcmp(w, "if") == 0) {
            const char *term = (strcmp(w, "if") == 0) ? "fi" : "done";
            int e = find_at_depth(lines, n, i + 1, term, NULL, NULL);
            if (e >= 0) {
                char *rest = lines[e] + strlen(term);
                while (*rest == ' ' || *rest == '\t') rest++;

                if (*rest == '|' && rest[1] != '|') {
                    char right[MAX_LINE];
                    strlcpy(right, rest + 1, sizeof right);
                    int fds[2];
                    if (right[0] && lp_pipe(fds) == 0) {
                        drain = lp_fork();
                        if (drain == 0) {
                            lp_close(fds[1]);
                            lp_dup2(fds[0], STDIN_FILENO);
                            lp_close(fds[0]);
                            lp_signal_default(SIGINT);
                            lp_signal_default(SIGQUIT);
                            run_logical_line(right);
                            lp_exit(last_status);
                        }
                        lp_close(fds[0]);
                        if (drain > 0) {
                            saved_out = (int)lp_dup(STDOUT_FILENO);
                            lp_dup2(fds[1], STDOUT_FILENO);
                            lp_close(fds[1]);
                            strlcpy(lines[e], term, MAX_LINE);
                        } else {
                            lp_close(fds[1]);
                        }
                    }
                } else if (*rest == '>') {
                    bool append = (rest[1] == '>');
                    const char *fn = rest + (append ? 2 : 1);
                    while (*fn == ' ' || *fn == '\t') fn++;
                    if (*fn) {
                        long f = lp_open(fn, O_WRONLY | O_CREAT |
                                         (append ? O_APPEND : O_TRUNC), 0644);
                        if (f < 0) {
                            dprintf(STDERR_FILENO,
                                    "sh: %s: cannot write to it\n", fn);
                        } else {
                            saved_out = (int)lp_dup(STDOUT_FILENO);
                            lp_dup2((int)f, STDOUT_FILENO);
                            lp_close((int)f);
                            strlcpy(lines[e], term, MAX_LINE);
                        }
                    }
                }
            }
        }

        char fname[64];
        if (is_func_def(lines[i], fname, sizeof fname))
            i = define_func(lines, n, i, fname);
        /* `{ ... }` - 이 셸에서 그대로 도는 묶음. 리다이렉션을 여럿에
         * 한꺼번에 걸거나 || 뒤에 여러 명령을 두려고 쓴다. */
        else if (strcmp(w, "{") == 0)     i = exec_group(lines, n, i);
        /* 짝을 찾아 실행한 뒤라면 닫는 중괄호는 구두점이다. */
        else if (strcmp(w, "}") == 0)     i++;
        else if (strcmp(w, "if") == 0)    i = exec_if(lines, n, i);
        else if (strcmp(w, "while") == 0) i = exec_while(lines, n, i);
        else if (strcmp(w, "for") == 0)   i = exec_for(lines, n, i);
        else if (strcmp(w, "case") == 0)  i = exec_case(lines, n, i);
        /* A `;;` that reached here is the end of a clause whose body
         * already ran, or of one that was skipped. Either way it is
         * punctuation, not a command. */
        else if (strcmp(w, ";;") == 0)    i++;
        else {
            if (opt_xtrace)
                dprintf(STDERR_FILENO, "+ %s\n", lines[i]);

            run_logical_line(lines[i]);
            i++;

            /* set -e. POSIX says an interactive shell ignores it, and
             * that is not a convenience: -e in a login shell means one
             * mistyped command closes the terminal. */
            if (opt_errexit && !shell_interactive && !errexit_off &&
                last_status != 0)
                lp_exit(last_status);
        }

        if (saved_out >= 0) {
            lp_dup2(saved_out, STDOUT_FILENO);
            lp_close(saved_out);
        }
        if (drain > 0) {
            int st = 0;
            lp_waitpid(drain, &st, 0);
        }

        if (feeder > 0) {
            /* Put stdin back before anything else reads it, then collect
             * the writer. A loop that stopped early leaves it writing
             * into a pipe nobody reads; closing our end first is what
             * makes it get SIGPIPE and finish rather than block. */
            if (saved_in >= 0) {
                lp_dup2(saved_in, STDIN_FILENO);
                lp_close(saved_in);
            }
            int st = 0;
            lp_waitpid(feeder, &st, 0);
        }

        /* A pending break or continue stops the rest of this block.
         * Checking here rather than only at the end of the loop body is
         * what makes `if ... ; then break ; fi` skip the lines after it
         * instead of running them on the way out. */
        if (loop_break || loop_continue)
            break;
    }
    return last_status;
}

/* ── Main loop ─────────────────────────────────────────────────── */

static char hostname[64] = "lpzero";

static void read_hostname(void)
{
    char buf[80];
    if (proc_read("/proc/sys/kernel/hostname", buf, sizeof(buf)) <= 0)
        return;
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    if (buf[0] && strcmp(buf, "(none)") != 0)
        strlcpy(hostname, buf, sizeof(hostname));
}

/* Build the prompt, and report how wide it is on screen.
 *
 * The width is not the length: the directory may have Hangul in it, and
 * the line editor has to know which column the typed line starts at or
 * every redraw lands in the wrong place. */
/* The login name for a uid, out of /etc/passwd. Returns false when
 * there is no line for it - a machine can perfectly well be running as
 * a uid that /etc/passwd has never heard of. */
static bool user_name_of(long uid, char *out, size_t size)
{
    char buf[8192];
    long n = proc_read("/etc/passwd", buf, sizeof buf - 1);
    if (n <= 0) return false;
    buf[n] = '\0';

    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        /* name:passwd:uid:... - the name is up to the first colon and
         * the uid is the third field. */
        char *c1 = strchr(line, ':');
        if (c1) {
            char *c2 = strchr(c1 + 1, ':');
            if (c2) {
                long u = strtol(c2 + 1, NULL, 10);
                if (u == uid) {
                    size_t len = (size_t)(c1 - line);
                    if (len >= size) len = size - 1;
                    memcpy(out, line, len);
                    out[len] = '\0';
                    return true;
                }
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    return false;
}

static int build_prompt(char *out, size_t size)
{
    char cwd[256];
    if (lp_getcwd(cwd, sizeof(cwd)) < 0)
        strlcpy(cwd, "?", sizeof(cwd));

    /* Home shows as ~, the way every other shell writes it. */
    const char *shown = cwd;
    char   collapsed[300];
    size_t hlen = strlen(HOME_DIR);
    if (strncmp(cwd, HOME_DIR, hlen) == 0 &&
        (cwd[hlen] == '\0' || cwd[hlen] == '/')) {
        snprintf(collapsed, sizeof(collapsed), "~%s", cwd + hlen);
        shown = collapsed;
    }

    /* Who this is, from the kernel rather than from a string.
     *
     * The name and the '#' were both literal "root" until the desktop
     * arrived and a terminal opened as uid 1000 - which then sat there
     * saying root@lpzero with a root's # in front of it. A prompt that
     * claims to be root when it is not is the one lie a shell must not
     * tell: it is what a person checks before typing something they
     * would only type as root.
     *
     * The name comes from /etc/passwd through the uid; if that lookup
     * fails the uid itself is printed, because a number nobody
     * recognises is still true. */
    long uid = lp_getuid();
    char who[64];
    if (!user_name_of(uid, who, sizeof who))
        snprintf(who, sizeof who, "%ld", uid);

    const char *mark = (uid == 0) ? "#" : "$";

    if (last_status)
        snprintf(out, size, "[%d] %s@%s:%s%s ", last_status, who,
                 hostname, shown, mark);
    else
        snprintf(out, size, "%s@%s:%s%s ", who, hostname, shown, mark);

    return (int)utf8_str_width(out, strlen(out));
}

/* ── the files read at login ──────────────────────────────────────
 *
 * The root filesystem is unpacked into RAM at every boot, so anything
 * written into /etc is gone by morning. Until now that left no way at
 * all to set a variable, extend PATH or define a function once and
 * still have it tomorrow: every session started from nothing, and the
 * only place to put anything was /data/rc.local, which runs as root at
 * boot rather than in the shell a person is typing into.
 *
 * So an interactive shell reads two files at startup when they exist:
 *
 *   /etc/profile   shipped in the image, for the system as a whole
 *   ~/.profile     the person's own. /root is a bind mount of
 *                  /data/root, so this one is on the card and survives
 *                  a reboot - which is the whole point.
 *
 * The lines go through split_statements and exec_block, the same path
 * the prompt uses, so a function definition or an if in a profile
 * behaves exactly as it does when typed.
 *
 * A profile that fails does not stop the shell from starting. A syntax
 * error in a startup file that left somebody with no shell would be a
 * board you have to take the card out of to fix, and this system's one
 * rule is that there is always a way back. */
static void source_file(const char *path)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return;

    static block_line_t sblock[MAX_BLOCK];
    char line[MAX_LINE];
    int  nblock = 0, depth = 0;
    bool pending = false;

    for (;;) {
        long len = readline_joined((int)fd, line, sizeof line);
        if (len > 0) collect_heredocs(line, sizeof line, (int)fd);
        if (len < 0)
            break;
        if (len == 0 || line[0] == '#')
            continue;

        int added = split_statements(line, sblock + nblock,
                                     MAX_BLOCK - nblock);
        for (int i = 0; i < added; i++) {
            char w[32], fname[64];
            first_word(sblock[nblock + i], w, sizeof w);
            if (opens_block(w) || pipes_into_block(sblock[nblock + i]))
                depth++;
            if (closes_block(w)) depth--;
            depth += brace_depth(sblock[nblock + i], w, fname,
                                 sizeof fname, &pending);
        }
        nblock += added;

        if (depth <= 0) {
            if (nblock > 0)
                exec_block(sblock, nblock);
            nblock = 0;
            depth  = 0;
        }
        if (nblock >= MAX_BLOCK - 8) {
            dprintf(STDERR_FILENO,
                    "sh: %s is too long to read in one piece - the rest was"
                    " ignored\n", path);
            break;
        }
    }

    if (depth > 0)
        dprintf(STDERR_FILENO,
                "sh: %s ends with %d block%s still open - it was not run\n",
                path, depth, depth == 1 ? "" : "s");

    lp_close((int)fd);
}

int main(int argc, char **argv)
{
    char line[MAX_LINE];

    /* Given a file argument, run the commands in it in order. This is for
     * boot scripts like /etc/rc, and there is no prompt in that mode. */
     
    int  input_fd  = STDIN_FILENO;
    bool interactive = true;

    /* -q means "if it is not there, say nothing". It is for scripts that
     * may or may not exist, like /data/rc.local. Our shell has neither test
     * nor if, so there is no other way to check first. */
    int  first = 1;
    bool quiet = false;
    if (argc > 1 && strcmp(argv[1], "-q") == 0) { quiet = true; first = 2; }

    /* ── sh -c '<commands>' ──
     *
     * This was missing, and it is not a nicety. `/bin/sh -c` is how a
     * shell gets invoked by everything that is not a person:
     * glibc's system() and popen(), python's os.system and
     * subprocess(shell=True), and every program that shells out to run
     * one command. Without it they all got "sh: -c: cannot open" - the
     * shell had taken "-c" for a filename - and the failure appeared
     * inside whatever library was calling, a long way from the cause.
     *
     * Everything after the command string is $0, $1, ... which is what
     * makes `sh -c 'echo $1' x y` behave the way it does everywhere
     * else. Signals stay at their defaults: this is not an interactive
     * shell, and a caller that sends SIGINT to what it started means it
     * for the command. */
    if (argc > first && strcmp(argv[first], "-c") == 0) {
        if (argc <= first + 1) {
            dprintf(STDERR_FILENO, "sh: -c needs something to run\n");
            return 2;
        }

        strlcpy(script_name, "sh", sizeof script_name);
        shell_interactive = false;
        for (int i = first + 2; i < argc && pos_count < MAX_POSITIONAL; i++)
            strlcpy(pos_args[pos_count++], argv[i], 256);

        /* The string may hold newlines AND semicolons, and both are
         * statement separators: `sh -c 'while true; do :; done'` is one
         * line with four statements in it. split_statements is what the
         * prompt and script paths use for exactly this, so using it here
         * too is what makes `sh -c` behave identically to both. */
        static block_line_t cblock[MAX_BLOCK];
        int  cn = 0;

        /* Refuse a command that does not fit, rather than running the
         * part of it that does.
         *
         * strlcpy truncates, and the cut lands wherever byte 1023 falls
         * - mid-word, mid-path, inside a quote. `rm -rf .../run-37/data`
         * becomes `rm -rf .../run-` and removes a parent directory;
         * `echo ... > /data/report.new` becomes `> /data/report` and
         * truncates the wrong file. And `sh -c` is what system(),
         * popen() and python's subprocess(shell=True) call, which build
         * their command strings by concatenation - a kilobyte is one
         * `find` result away. Doing part of a destructive command is
         * the worst of the available outcomes. */
        char work[MAX_LINE];
        if (strlcpy(work, argv[first + 1], sizeof work) >= sizeof work) {
            dprintf(STDERR_FILENO,
                    "sh: -c: command is longer than %d bytes - refusing to"
                    " run part of it\n", MAX_LINE - 1);
            return 2;
        }

        char *p = work;
        bool too_many = false;
        while (p && *p) {
            if (cn >= MAX_BLOCK) { too_many = true; break; }
            char *eol = strchr(p, '\n');
            if (eol) *eol = '\0';

            if (*p && *p != '#')
                cn += split_statements(p, cblock + cn, MAX_BLOCK - cn);

            p = eol ? eol + 1 : NULL;
        }

        /* Same reasoning as the length check: running the first 256
         * statements of a script and reporting the last one's status -
         * normally success - hides the fact that the cleanup at the end
         * never ran. */
        if (too_many) {
            dprintf(STDERR_FILENO,
                    "sh: -c: more than %d statements - refusing to run"
                    " part of it\n", MAX_BLOCK);
            return 2;
        }

        exec_block(cblock, cn);
        return last_status;
    }

    if (argc > first) {
        long fd = lp_open(argv[first], O_RDONLY, 0);
        if (fd < 0) {
            if (quiet)
                return 0;
            dprintf(STDERR_FILENO, "sh: %s: cannot open (%ld)\n",
                    argv[first], -fd);
            return 127;
        }
        input_fd    = (int)fd;
        interactive = false;
        shell_interactive = false;

        /* Anything after the script name is $1, $2, ... and the script
         * itself is $0, which is what every shell does and what makes a
         * script something you can pass arguments to. */
        strlcpy(script_name, argv[first], sizeof script_name);
        for (int i = first + 1; i < argc && pos_count < MAX_POSITIONAL; i++)
            strlcpy(pos_args[pos_count++], argv[i], 256);
    }

    /* An interactive shell is somebody sitting at a terminal: start where
     * their files are, remember what they type, and know the machine's
     * name for the prompt. A shell running /etc/rc gets none of this. */
    static block_line_t block[MAX_BLOCK];
    int  nblock = 0;

    char prompt[384];
    if (interactive) {
        /* Ctrl-C interrupts the command, never the shell. Without this
         * the signal goes to the whole process group - the child and us
         * - and the shell dies along with what you were stopping, which
         * on the console means init restarts it and the screen clears.
         *
         * A shell running a script keeps the default: a boot script that
         * cannot be interrupted is worse than one that can. */
        lp_signal_ignore(SIGINT);
        lp_signal_ignore(SIGQUIT);

        /* And the suspend signals, in case something re-enabled Ctrl-Z
         * on the terminal. A stopped shell on a board with no job
         * control is a board you cannot type at, and no key gets it
         * back. SIGTTIN and SIGTTOU are the same hazard arriving from
         * the other direction - a background process touching the
         * terminal - and there is nothing here that would resume us. */
        lp_signal_ignore(SIGTSTP);
        lp_signal_ignore(SIGTTIN);
        lp_signal_ignore(SIGTTOU);

        read_hostname();
        hist_load();
        if (lp_is_dir(HOME_DIR))
            lp_chdir(HOME_DIR);

        /* Read after chdir, so a profile that does something relative to
         * the home directory means what it looks like it means. */
        source_file("/etc/profile");
        {
            const char *home = getenv("HOME");
            char rc[512];
            snprintf(rc, sizeof rc, "%s/.profile",
                     (home && *home) ? home : HOME_DIR);
            source_file(rc);
        }
    }

    while (shell_running) {
        long len;

        if (interactive) {
            /* Whatever just ran may have left the terminal in raw mode -
             * killed partway through, or simply careless. Put it back
             * before drawing a prompt into it. */
            lp_term_sane(STDIN_FILENO);
            reap_jobs();
            int pw = build_prompt(prompt, sizeof(prompt));
            len = edit_line(prompt, pw, line, sizeof(line));
        } else {
            len = readline_joined(input_fd, line, sizeof(line));
            if (len > 0)
                collect_heredocs(line, sizeof(line), input_fd);
        }

        if (len < 0) {              /* EOF: Ctrl-D, or the end of the file */
            if (interactive)
                printf("\n");
            break;
        }
        if (len == 0)
            continue;

        if (interactive) {
            hist_add(line);
            hist_save_one(line);
        }

        /* Skip comments when running a script. */
        if (line[0] == '#')
            continue;

        /* Break the line into logical statements, so that the one-line
         * and multi-line forms of if and while are the same thing. */
        nblock = split_statements(line, block, MAX_BLOCK);

        /* An unfinished block keeps reading. Interactively that means a
         * continuation prompt; in a script it just reads on. */
        int  depth   = 0;
        bool pending = false;
        for (int i = 0; i < nblock; i++) {
            char w[32], fname[64];
            first_word(block[i], w, sizeof(w));
            if (opens_block(w) || pipes_into_block(block[i])) depth++;
            if (closes_block(w)) depth--;
            /* A function body is a block too, opened by "name() {" and
             * closed by a brace on its own line. Without counting it,
             * typing a function at the prompt would try to run the
             * first line of it as soon as Enter was pressed. */
            depth += brace_depth(block[i], w, fname, sizeof fname,
                                 &pending);
        }

        while (depth > 0 && nblock < MAX_BLOCK - 8 && shell_running) {
            long more;
            if (interactive) {
                more = edit_line("> ", 2, line, sizeof(line));
            } else {
                more = readline_joined(input_fd, line, sizeof(line));
                if (more > 0)
                    collect_heredocs(line, sizeof(line), input_fd);
            }

            if (more < 0) {
                dprintf(STDERR_FILENO, "sh: unexpected end - %d block%s left"
                        " open\n", depth, depth == 1 ? "" : "s");
                depth = 0;
                nblock = 0;
                break;
            }
            if (more == 0)
                continue;
            if (interactive) {
                hist_add(line);
                hist_save_one(line);
            }
            if (line[0] == '#')
                continue;

            int added = split_statements(line, block + nblock,
                                         MAX_BLOCK - nblock);
            for (int i = 0; i < added; i++) {
                char w[32], fname[64];
                first_word(block[nblock + i], w, sizeof(w));
                if (opens_block(w) || pipes_into_block(block[nblock + i]))
                    depth++;
                if (closes_block(w)) depth--;
                depth += brace_depth(block[nblock + i], w,
                                     fname, sizeof fname, &pending);
            }
            nblock += added;
        }

        if (nblock > 0)
            exec_block(block, nblock);
    }

    if (input_fd != STDIN_FILENO)
        lp_close(input_fd);

    return last_status;
}
