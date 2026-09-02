/* whoami - which user this is.
 *
 * There is one, and it is root. This exists because scripts and habits
 * expect it to, and because "am I root here" is worth being able to ask
 * without thinking about it.
 */
#include "types.h"
#include "stdio.h"
#include "unistd.h"

int main(void)
{
    int uid = lp_getuid();
    if (uid == 0)
        printf("root\n");
    else
        printf("uid %d\n", (int)uid);
    return 0;
}
