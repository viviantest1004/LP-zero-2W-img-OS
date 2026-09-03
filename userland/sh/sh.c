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
} cmd_t;

/* How this pipeline joins onto the one before it */
typedef enum { LINK_NONE, LINK_AND, LINK_OR, LINK_SEQ } link_t;

typedef struct {
    cmd_t  cmds[MAX_CMDS];
    int    ncmds;
    link_t link;        /* the operator in front of this pipeline */
    bool   background;  /* it ended in & - do not wait for it */
} pipeline_t;

/* /data/bin is on the data partition. Large programs such as Python live
 * there rather than in the system image (the initramfs inside the kernel). */
static const char *DEFAULT_PATH = "/bin:/data/bin:/sbin:/usr/bin:/usr/sbin";

/* Home. It is a bind mount from the data partition, so what is saved
 * there survives a reboot - unlike the rest of the root filesystem. */
#define HOME_DIR "/root"
static bool shell_running = true;
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
    TOK_REDIR_OUT,
    TOK_REDIR_APPEND,
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

/* Set by next_token, read by parse_line: a quoted word is never expanded
 * into file names, and a word with no * or ? never needs looking at.
 * tok_from_cmd marks a word that came out of $(...) unquoted, which is
 * the one case where a word has to be split on its own whitespace. */
static bool tok_quoted   = false;
static bool tok_has_glob = false;
static bool tok_from_cmd = false;

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
#define MAX_FUNCS       16
#define MAX_FUNC_LINES  40

typedef struct {
    char name[64];
    char lines[MAX_FUNC_LINES][MAX_LINE];
    int  nlines;
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

    bool braced = (*s == '{');
    if (braced) s++;

    char name[64];
    size_t k = 0;
    while ((is_name_char(*s)) && k < sizeof(name) - 1)
        name[k++] = *s++;
    name[k] = '\0';

    if (braced && *s == '}') s++;

    if (k == 0) {                   /* a bare $: leave it as typed */
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
            s++;
            while (*s && *s != '\'') {
                if (n < sizeof(buf)) buf[n++] = *s;
                s++;
            }
            if (*s == '\'') s++;
        } else if (*s == '"') {
            tok_quoted = true;
            s++;
            while (*s && *s != '"') {
                if (*s == '$') {
                    n = expand_dollar(&s, buf, n, sizeof(buf));
                    tok_from_cmd = false;   /* "$(...)" is one word */
                } else {
                    if (n < sizeof(buf)) buf[n++] = *s;
                    s++;
                }
            }
            if (*s == '"') s++;
        } else if (*s == '$') {
            n = expand_dollar(&s, buf, n, sizeof(buf));
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

            /* NAME=value, but only in front of the command. Anywhere else
             * it is an ordinary argument: echo A=B has to print A=B. */
            if (c->argc == 0 && !tok_quoted && try_assignment(word)) {
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
            continue;
        }

        /* Redirection: a file name has to follow immediately. */
        char *file = NULL;
        if (next_token(&p, &file) != TOK_WORD) {
            dprintf(STDERR_FILENO, "sh: no file after the redirection\n");
            return -1;
        }
        if (t == TOK_REDIR_IN) {
            c->redir_in = file;
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


static const char *BUILTINS[] = {
    /* "help" is deliberately not here. It is /bin/help, a real program,
     * so it can scan PATH and list what is actually installed - a
     * builtin would only ever know what was hardcoded into the shell. */
    "exit", "cd", "pwd", "echo", "env", "reboot", "poweroff",
    "test", "[", "true", "false", NULL
};

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
static bool str_is_num(const char *s)
{
    if (*s == '-' || *s == '+') s++;
    if (!*s) return false;
    for (; *s; s++)
        if (*s < '0' || *s > '9')
            return false;
    return true;
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

        if (op[0] == '-' && str_is_num(a) && str_is_num(b)) {
            long x = strtol(a, NULL, 10);
            long y = strtol(b, NULL, 10);
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

    if (strcmp(cmd, "true") == 0)  { last_status = 0; return true; }
    if (strcmp(cmd, "false") == 0) { last_status = 1; return true; }

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

    if (strcmp(cmd, "reboot") == 0 || strcmp(cmd, "poweroff") == 0) {
        printf("%s...\n", cmd);

        /* ── Shutting down properly ──
         *
         * This used to ask the kernel to reset immediately, which makes
         * every reboot indistinguishable from pulling the plug. ext4's
         * journal protects the filesystem's own structure, but not the
         * contents of files being written - and logd is writing all the
         * time. The result was a machine with no clean shutdown path at
         * all: the watchdog's reset, a power cut and a deliberate
         * reboot were the same event.
         *
         * So: ask everything to stop, give it a moment, flush, and put
         * the data partition into a state where nothing is in flight.
         *
         * SIGTERM to everything except ourselves and init. Nothing here
         * needs a graceful shutdown of its own; what matters is that
         * they stop writing before the sync. */
        printf("  stopping services\n");
        /* Ourselves first: kill(-1) reaches every process this one may
         * signal, and that includes this shell. Without this the shell
         * dies here and the machine never actually reboots. init is
         * excluded by the kernel, so it survives to see us go. */
        lp_signal_ignore(SIGTERM);
        lp_kill(-1, SIGTERM);
        lp_sleep_ms(400);

        printf("  flushing to disk\n");
        lp_sync();

        /* Read-only, so anything still holding a file cannot write
         * during the reset. It fails harmlessly when /data is not
         * mounted, which is the case on a board running from RAM. */
        lp_mount(NULL, "/data", NULL, MS_REMOUNT | MS_RDONLY, NULL);
        lp_sync();

        /* Write the time down just before we go. This board has no
         * battery-backed clock, so without this the next boot starts at 1970.
         * The ntp daemon saves every 30s; this fills in that last window.
         * Anything before 2020 means the clock was never set, and saving it
         * would be worse than saving nothing. */
        s64 now = lp_time();
        if (now >= 1577836800LL) {
            char buf[32];
            int  n = snprintf(buf, sizeof(buf), "%lld\n", (long long)now);
            long fd = lp_open("/data/.clock", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                lp_write((int)fd, buf, (size_t)n);
                lp_close((int)fd);
            }
        }

        lp_sync();
        int op = (strcmp(cmd, "reboot") == 0)
                 ? LINUX_REBOOT_CMD_RESTART : LINUX_REBOOT_CMD_POWER_OFF;
        long r = lp_reboot(op);
        dprintf(STDERR_FILENO, "%s: failed (%ld) - do you have permission?\n", cmd, -r);
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
static void apply_redirects(cmd_t *c)
{
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

    run_builtin(c);

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

/* The first word of a line, for deciding whether it is a keyword. */
static void first_word(const char *line, char *out, size_t size)
{
    while (*line == ' ' || *line == '\t') line++;

    size_t n = 0;
    while (*line && *line != ' ' && *line != '\t' && n < size - 1)
        out[n++] = *line++;
    out[n] = '\0';
}

static bool is_keyword(const char *w)
{
    static const char *kw[] = {
        "if", "then", "elif", "else", "fi",
        "while", "do", "done", "for", NULL
    };
    for (int i = 0; kw[i]; i++)
        if (strcmp(w, kw[i]) == 0)
            return true;
    return false;
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

    /* The brace has to be there. Without it we cannot tell where the
     * function ends, and a shell that guesses at that is worse than one
     * that says so. */
    return line[0] == '{';
}

/* Does this line open a block that needs a matching fi, done or }? */
static bool opens_block(const char *w)
{
    return strcmp(w, "if") == 0 || strcmp(w, "while") == 0 ||
           strcmp(w, "for") == 0;
}

static bool closes_block(const char *w)
{
    return strcmp(w, "fi") == 0 || strcmp(w, "done") == 0;
}

/* Split a line on ';' into logical lines, but only where a keyword is
 * involved - otherwise "a ; b" would stop being one thing the existing
 * parser handles, and it handles it correctly already. */
static int split_statements(const char *line, block_line_t *out, int max)
{
    char w[32];
    first_word(line, w, sizeof(w));

    /* No keyword anywhere: leave it exactly as typed. */
    if (!is_keyword(w) && !strstr(line, "; then") && !strstr(line, "; do") &&
        !strstr(line, "; fi") && !strstr(line, "; done") &&
        !strstr(line, ";then") && !strstr(line, ";do") &&
        !strstr(line, ";fi") && !strstr(line, ";done")) {
        strlcpy(out[0], line, MAX_LINE);
        return 1;
    }

    int  n = 0;
    const char *p = line;

    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        const char *semi = strchr(p, ';');
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
                               strcmp(kw, "else") == 0);

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

        if (opens_block(w))  depth++;
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

        if (cond[0])
            run_logical_line(cond);

        /* Any further lines before "then" are part of the condition too,
         * and the last one decides. */
        for (int i = cursor + 1; i < then_at; i++)
            run_logical_line(lines[i]);

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

        if (cond[0])
            run_logical_line(cond);
        for (int i = start + 1; i < do_at; i++)
            run_logical_line(lines[i]);

        if (last_status != 0)
            break;

        exec_block(lines + do_at + 1, end - do_at - 1);

        if (last_status >= 128)      /* killed by a signal */
            break;
        if (rounds > 1000000) {
            dprintf(STDERR_FILENO, "sh: while: a million rounds - stopping\n");
            break;
        }
    }

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
        if (last_status >= 128)
            break;
    }

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
    int depth = 1;
    int end = -1;
    for (int i = start + 1; i < n; i++) {
        char w[32];
        first_word(lines[i], w, sizeof w);
        char inner[64];
        if (is_func_def(lines[i], inner, sizeof inner)) depth++;
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
    f->nlines = 0;

    for (int i = start + 1; i < end && f->nlines < MAX_FUNC_LINES; i++)
        strlcpy(f->lines[f->nlines++], lines[i], MAX_LINE);

    if (end - start - 1 > MAX_FUNC_LINES)
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
     * being called twice. */
    static block_line_t body[MAX_FUNC_LINES];
    int nb = f->nlines;
    for (int i = 0; i < nb; i++)
        strlcpy(body[i], f->lines[i], MAX_LINE);

    depth++;
    exec_block(body, nb);
    depth--;

    pos_count = saved_count;
    for (int i = 0; i < saved_count; i++)
        strlcpy(pos_args[i], saved[i], 256);
}

/* Run a run of logical lines, handling any control structures in them. */
static int exec_block(block_line_t *lines, int n)
{
    int i = 0;
    while (i < n && shell_running) {
        char w[32];
        first_word(lines[i], w, sizeof(w));

        char fname[64];
        if (is_func_def(lines[i], fname, sizeof fname))
            i = define_func(lines, n, i, fname);
        else if (strcmp(w, "if") == 0)    i = exec_if(lines, n, i);
        else if (strcmp(w, "while") == 0) i = exec_while(lines, n, i);
        else if (strcmp(w, "for") == 0)   i = exec_for(lines, n, i);
        else {
            run_logical_line(lines[i]);
            i++;
        }
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

    /* A failed command leaves its exit code in front of the prompt. It is
     * the one piece of state that is easy to miss and annoying to ask
     * for after the fact. */
    if (last_status)
        snprintf(out, size, "[%d] root@%s:%s# ", last_status, hostname, shown);
    else
        snprintf(out, size, "root@%s:%s# ", hostname, shown);

    return (int)utf8_str_width(out, strlen(out));
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

        read_hostname();
        hist_load();
        if (lp_is_dir(HOME_DIR))
            lp_chdir(HOME_DIR);
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
            len = readline(input_fd, line, sizeof(line));
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
        int depth = 0;
        for (int i = 0; i < nblock; i++) {
            char w[32], fname[64];
            first_word(block[i], w, sizeof(w));
            if (opens_block(w))  depth++;
            if (closes_block(w)) depth--;
            /* A function body is a block too, opened by "name() {" and
             * closed by a brace on its own line. Without counting it,
             * typing a function at the prompt would try to run the
             * first line of it as soon as Enter was pressed. */
            if (is_func_def(block[i], fname, sizeof fname)) depth++;
            if (strcmp(w, "}") == 0) depth--;
        }

        while (depth > 0 && nblock < MAX_BLOCK - 8 && shell_running) {
            long more;
            if (interactive)
                more = edit_line("> ", 2, line, sizeof(line));
            else
                more = readline(input_fd, line, sizeof(line));

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
                if (opens_block(w))  depth++;
                if (closes_block(w)) depth--;
                if (is_func_def(block[nblock + i], fname, sizeof fname))
                    depth++;
                if (strcmp(w, "}") == 0) depth--;
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
