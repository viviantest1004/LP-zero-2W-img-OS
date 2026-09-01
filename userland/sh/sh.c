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
} pipeline_t;

/* /data/bin is on the data partition. Large programs such as Python live
 * there rather than in the system image (the initramfs inside the kernel). */
static const char *DEFAULT_PATH = "/bin:/data/bin:/sbin:/usr/bin:/usr/sbin";
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
    TOK_REDIR_IN,
    TOK_REDIR_OUT,
    TOK_REDIR_APPEND,
} tok_type_t;

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static char   arena[MAX_LINE * 2];
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

/* Read one token. For TOK_WORD, *word_out gets the arena pointer. */
static tok_type_t next_token(char **p, char **word_out)
{
    char *s = *p;

    while (is_space(*s)) s++;
    if (*s == '\0') { *p = s; return TOK_END; }

    /* An operator is a token in itself. Two-character ones (&& ||) must
     * be checked before the single-character ones (|). */
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

    /* A word: gather it, stripping quotes. */
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
            if (*s == quote) s++;       /* closing quote; without one, to end of line */
        } else {
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

/* Parse one line into a list of pipelines.
 *
 *   cmd1 | cmd2 && cmd3 || cmd4 ; cmd5
 *
 * | joins stages within a pipeline; && || ; join pipelines together.
 * Returns the pipeline count, 0 for a blank line, -1 on error. */
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
                dprintf(STDERR_FILENO, "sh: too many arguments\n");
                return -1;
            }
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

        if (t == TOK_AND || t == TOK_OR || t == TOK_SEMI) {
            if (c->argc == 0) {
                dprintf(STDERR_FILENO, "sh: no command before the operator\n");
                return -1;
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

    return np;
}

/* ── Builtins ───────────────────────────────────────────────────── */

static void builtin_help(void)
{
    printf("LP-zero shell\n\n");
    printf("  cd [dir]      change directory (to / with no argument)\n");
    printf("  pwd           where you are\n");
    printf("  echo ...      print the arguments\n");
    printf("  env           list the environment\n");
    printf("  exit [n]      leave the shell\n");
    printf("  reboot        restart the machine\n");
    printf("  poweroff      shut down\n");
    printf("  help          this help\n\n");
    printf("Redirection < > >> , pipes | , and && || ; all work.\n\n");
    printf("files:  ls  cat  cp  mv  rm  mkdir  touch  edit\n");
    printf("time:   date   date -z list   date -s \"2026-09-01 12:00:00\"   ntp\n");
    printf("system: sysinfo  zram  memwatch  mount  expandfs  dhcp\n");
    printf("other:  calc \"2+3*4\"      python (on /data)\n");
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

/* Handle a builtin and return true. The exit code goes in last_status. */
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

    if (strcmp(cmd, "help") == 0) {
        builtin_help();
        last_status = 0;
        return true;
    }

    if (strcmp(cmd, "reboot") == 0 || strcmp(cmd, "poweroff") == 0) {
        printf("%s...\n", cmd);

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

/* Run a pipeline: fork each stage and join them with pipes. */
static void run_pipeline(cmd_t *cmds, int n)
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

/* ── Main loop ─────────────────────────────────────────────────── */

static void print_prompt(void)
{
    char cwd[256];
    if (lp_getcwd(cwd, sizeof(cwd)) < 0)
        strlcpy(cwd, "?", sizeof(cwd));

    /* Show a non-zero exit code in the prompt. */
    if (last_status)
        printf("[%d] %s $ ", last_status, cwd);
    else
        printf("%s $ ", cwd);
}

int main(int argc, char **argv)
{
    char       line[MAX_LINE];
    pipeline_t pipes[MAX_PIPES];

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
    }

    while (shell_running) {
        if (interactive)
            print_prompt();

        long len = readline(input_fd, line, sizeof(line));
        if (len < 0) {              /* EOF: Ctrl-D, or the end of the file */
            if (interactive)
                printf("\n");
            break;
        }
        if (len == 0)
            continue;

        /* Skip comments when running a script. */
        if (line[0] == '#')
            continue;

        int np = parse_line(line, pipes, MAX_PIPES);
        if (np <= 0)
            continue;

        for (int i = 0; i < np && shell_running; i++) {
            /* Short circuit: && only after success, || only after failure. */
            if (pipes[i].link == LINK_AND && last_status != 0) continue;
            if (pipes[i].link == LINK_OR  && last_status == 0) continue;

            cmd_t *cmds = pipes[i].cmds;
            int    n    = pipes[i].ncmds;

            /* Builtins run inside the shell only when the pipeline has one
             * stage. As part of a pipe they are treated like external commands
             * in a child, so stdin/stdout hook up naturally. */
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
