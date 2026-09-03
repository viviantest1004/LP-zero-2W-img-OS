/* su - run something as another user.
 *
 *   su <user>              a shell as them
 *   su <user> -c <command> one command as them
 *   sudo <command>         the same as root, for scripts that expect it
 *
 * ── Why there is no password ──
 * Nothing on this machine checks one. SSH is public-key only with
 * password authentication compiled out of the server, and there is no
 * login program on the console. Asking for a password here would be
 * theatre: there would be nothing to compare it against.
 *
 * The enforcement is the kernel's and it is real. setuid() from root to
 * anybody succeeds; from anybody to anybody else it fails with EPERM.
 * So root can become a user without a password - as it can everywhere -
 * and a user cannot become root at all, with or without one.
 *
 * dropprivs does the same job for a program that should never have been
 * root in the first place. su is for a person at a prompt.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static int become(const lp_user_t *u, char **argv, int argc, int cmd_at)
{
    /* Order matters and getting it wrong is silent. setgid after setuid
     * fails - the process is no longer privileged enough to change its
     * group - and leaves something running with a user's uid and root's
     * group, which is not what anybody asked for. */
    gid_t only = u->gid;
    if (lp_setgroups(1, &only) < 0 && u->uid != 0)
        dprintf(STDERR_FILENO, "su: could not set the group list\n");

    if (lp_setgid(u->gid) < 0) {
        dprintf(STDERR_FILENO, "su: cannot become group %d\n", (int)u->gid);
        return 1;
    }
    if (lp_setuid(u->uid) < 0) {
        dprintf(STDERR_FILENO,
                "su: cannot become %s.\n"
                "su:   Only root can do this, and this is uid %d. That is\n"
                "su:   the kernel's rule, not ours - there is no password\n"
                "su:   that would change it.\n", u->name, lp_getuid());
        return 1;
    }

    /* Check rather than trust. A setuid that silently did nothing would
     * leave a "user" shell running as root. */
    if (lp_getuid() != (int)u->uid) {
        dprintf(STDERR_FILENO,
                "su: ** still uid %d after asking for %d. Not continuing.\n",
                lp_getuid(), (int)u->uid);
        return 1;
    }

    setenv("USER", u->name, 1);
    setenv("HOME", u->home, 1);
    setenv("SHELL", u->shell, 1);
    if (u->home[0] && lp_is_dir(u->home))
        lp_chdir(u->home);

    char *shell_argv[4];
    char *const *run;

    if (cmd_at > 0) {
        shell_argv[0] = (char *)u->shell;
        shell_argv[1] = (char *)"-c";
        shell_argv[2] = argv[cmd_at];
        shell_argv[3] = NULL;
        /* Our shell has no -c. Run the command through it as a file of
         * one line would be wrong too, so it is exec'd directly when it
         * is a single word and handed to the shell otherwise. */
        if (!strchr(argv[cmd_at], ' ') && !strchr(argv[cmd_at], '|') &&
            !strchr(argv[cmd_at], '>')) {
            char path[256];
            snprintf(path, sizeof path, "/bin/%s", argv[cmd_at]);
            if (lp_exists(path)) {
                char *one[2] = { path, NULL };
                lp_execve(path, one, environ);
            }
            if (lp_exists(argv[cmd_at])) {
                char *one[2] = { argv[cmd_at], NULL };
                lp_execve(argv[cmd_at], one, environ);
            }
        }
        dprintf(STDERR_FILENO,
                "su: this shell has no -c, so only a single program can be\n"
                "su:   run this way. Put the pipeline in a file and run\n"
                "su:   that: su %s -c /data/bin/yourscript\n", u->name);
        return 1;
    }

    shell_argv[0] = (char *)u->shell;
    shell_argv[1] = NULL;
    run = shell_argv;
    lp_execve(u->shell, (char *const *)run, environ);

    dprintf(STDERR_FILENO, "su: cannot run %s\n", u->shell);
    (void)argc;
    return 127;
}

int main(int argc, char **argv)
{
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];

    if (argc > 1 && (strcmp(argv[1], "-h") == 0 ||
                     strcmp(argv[1], "--help") == 0)) {
        printf("su - run something as another user\n\n");
        printf("  su <user>                 a shell as them\n");
        printf("  su <user> -c <program>    one program as them\n");
        printf("  sudo <program>            as root\n\n");
        printf("There is no password, because nothing on this machine\n");
        printf("checks one - SSH is public-key only and there is no login\n");
        printf("program. The kernel is the enforcement: root can become\n");
        printf("anybody, and anybody else can become nobody.\n\n");
        printf("  useradd <name>   make a user\n");
        printf("  dropprivs        the same for a program, not a person\n");
        return 0;
    }

    /* sudo <command> is su root -c <command>, which is what scripts
     * written elsewhere expect to be able to say. */
    if (strcmp(base, "sudo") == 0) {
        if (argc < 2) {
            dprintf(STDERR_FILENO, "usage: sudo <program> [args...]\n");
            return 2;
        }
        if (lp_getuid() == 0) {
            /* Already root: run it, which is what sudo would do after
             * deciding it was allowed. */
            char path[256];
            const char *cmd = argv[1];
            if (cmd[0] != '/') {
                snprintf(path, sizeof path, "/bin/%s", cmd);
                if (!lp_exists(path))
                    snprintf(path, sizeof path, "/data/bin/%s", cmd);
                cmd = path;
            }
            lp_execve(cmd, argv + 1, environ);
            dprintf(STDERR_FILENO, "sudo: cannot run %s\n", argv[1]);
            return 127;
        }
        dprintf(STDERR_FILENO,
                "sudo: this is uid %d, and there is no way up from here.\n"
                "sudo:   Nothing on this machine grants root to a user -\n"
                "sudo:   no password to check and no rules file. Log in as\n"
                "sudo:   root over SSH instead.\n", lp_getuid());
        return 1;
    }

    const char *who = "root";
    int cmd_at = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) { cmd_at = ++i; }
        else who = argv[i];
    }

    lp_user_t u;
    if (!lp_user_by_name(who, &u)) {
        dprintf(STDERR_FILENO,
                "su: there is no user called \"%s\".\n"
                "su:   'useradd -l' lists the ones there are.\n", who);
        return 1;
    }

    return become(&u, argv, argc, cmd_at);
}
