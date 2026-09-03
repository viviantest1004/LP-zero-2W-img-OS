/* id - which user this is, and which groups.
 *
 *   id            this process
 *   id <user>     somebody in /etc/passwd
 *   groups        just the group names
 *
 * There is normally one user here and it is root. That is not a reason
 * to leave this out: a program dropped to another user with dropprivs,
 * or a script checking whether it is root before writing somewhere,
 * needs an answer, and "run whoami and hope" is not one.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

static void show(const lp_user_t *u)
{
    char g[32];
    lp_group_name(u->gid, g, sizeof g);
    printf("uid=%d(%s) gid=%d(%s) groups=%d(%s)\n",
           (int)u->uid, u->name, (int)u->gid, g, (int)u->gid, g);
}

int main(int argc, char **argv)
{
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    bool as_groups = (strcmp(base, "groups") == 0);

    if (argc > 1 && strcmp(argv[1], "-h") == 0) {
        printf("usage: %s [user]\n", base);
        printf("  with no user, this process\n");
        return 0;
    }

    lp_user_t u;

    if (argc > 1) {
        if (!lp_user_by_name(argv[1], &u)) {
            dprintf(STDERR_FILENO,
                    "%s: there is no user called \"%s\"\n", base, argv[1]);
            return 1;
        }
    } else {
        uid_t me = (uid_t)lp_getuid();
        if (!lp_user_by_uid(me, &u)) {
            /* Running as a uid with no line in /etc/passwd. Say the
             * number rather than nothing - that is the situation after
             * dropprivs to an id nobody created. */
            char g[32];
            gid_t mygid = (gid_t)lp_getgid();
            lp_group_name(mygid, g, sizeof g);
            if (as_groups)
                printf("%s\n", g);
            else
                printf("uid=%d gid=%d(%s)  - no /etc/passwd entry\n",
                       (int)me, (int)mygid, g);
            return 0;
        }
    }

    if (as_groups) {
        char g[32];
        lp_group_name(u.gid, g, sizeof g);
        printf("%s\n", g);
        return 0;
    }

    show(&u);
    return 0;
}
