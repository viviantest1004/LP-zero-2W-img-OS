/* init.c - PID 1.
 *
 * 커널이 마지막으로 하는 일이 이 프로그램을 띄우는 것이다.
 * 여기서부터는 우리 책임이다.
 *
 * PID 1 의 특별한 규칙:
 *   · 절대 종료하면 안 된다. 종료하면 커널이 패닉한다.
 *   · 고아가 된 프로세스가 전부 이쪽으로 넘어온다. 거둬주지 않으면
 *     좀비가 쌓여 프로세스 테이블이 찬다.
 *   · 파일시스템 마운트, 콘솔 준비 등 초기 설정을 직접 해야 한다.
 *     systemd 가 하던 일을 우리가 한다. */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define SHELL_PATH   "/bin/sh"
#define RC_SCRIPT    "/etc/rc"
#define RESPAWN_MS   1000   /* 셸이 즉시 죽을 때 폭주 방지 대기 */

/* 마운트할 가상 파일시스템 목록.
 * 이것들이 없으면 ps 도 안 되고 /dev/null 도 없다. */
static const struct {
    const char *source;
    const char *target;
    const char *fstype;
    unsigned long flags;
} MOUNTS[] = {
    { "proc",     "/proc", "proc",     MS_NOSUID | MS_NODEV | MS_NOEXEC },
    { "sysfs",    "/sys",  "sysfs",    MS_NOSUID | MS_NODEV | MS_NOEXEC },
    /* devtmpfs 는 커널이 장치 노드를 자동으로 만들어준다.
     * CONFIG_DEVTMPFS 가 켜져 있어야 한다. */
    { "devtmpfs", "/dev",  "devtmpfs", MS_NOSUID },
    /* SSH 로 들어온 세션은 의사 터미널(PTY)을 쓴다. devpts 가 없으면
     * dropbear 가 셸을 띄우지 못한다. /dev 마운트 뒤에 와야 한다. */
    { "devpts",   "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC },
};

static void mount_filesystems(void)
{
    for (unsigned i = 0; i < sizeof(MOUNTS) / sizeof(MOUNTS[0]); i++) {
        /* 마운트 지점이 없으면 만든다 (initramfs 에 없을 수 있다) */
        if (!lp_is_dir(MOUNTS[i].target))
            lp_mkdir(MOUNTS[i].target, 0755);

        long r = lp_mount(MOUNTS[i].source, MOUNTS[i].target,
                          MOUNTS[i].fstype, MOUNTS[i].flags, NULL);
        if (r < 0)
            dprintf(STDERR_FILENO, "init: %s 마운트 실패 (%ld)\n",
                    MOUNTS[i].target, -r);
    }
}

/* 콘솔을 stdin/stdout/stderr 에 붙인다.
 *
 * 커널은 init 을 실행하기 전에 /dev/console 을 열어 fd 0,1,2 로 준다.
 * 그런데 initramfs 안에 /dev/console 장치 노드가 없으면 그 시도가 실패하고
 * ("unable to open an initial console") init 은 fd 하나 없이 시작한다.
 * 그 상태에서는 오류 메시지조차 어디로도 나가지 않아 원인 파악이 불가능하다.
 *
 * devtmpfs 를 마운트하고 나면 /dev/console 이 생기므로 직접 열어 붙인다. */
static void setup_console(void)
{
    long fd = lp_open("/dev/console", O_RDWR, 0);
    if (fd < 0)
        return;             /* 여기서 실패하면 알릴 방법도 없다 */

    lp_dup2((int)fd, STDIN_FILENO);
    lp_dup2((int)fd, STDOUT_FILENO);
    lp_dup2((int)fd, STDERR_FILENO);

    if (fd > STDERR_FILENO)
        lp_close((int)fd);
}

static void banner(void)
{
    printf("\n");
    printf("  LP-zero OS\n");
    printf("  init (pid %d)\n", lp_getpid());
    printf("\n");
}

/* 부팅 스크립트를 실행하고 끝날 때까지 기다린다.
 *
 * init 이 직접 하지 않고 스크립트로 뺀 이유: 무선 연결이나 SSH 시작 같은
 * 것은 설정에 따라 자주 바뀐다. 그때마다 init 을 다시 빌드하는 것보다
 * /etc/rc 를 고치는 편이 낫다. */
static void run_rc(void)
{
    if (!lp_exists(RC_SCRIPT))
        return;

    pid_t pid = lp_fork();
    if (pid < 0) {
        dprintf(STDERR_FILENO, "init: rc 를 위한 fork 실패\n");
        return;
    }

    if (pid == 0) {
        char *argv[] = { (char *)SHELL_PATH, (char *)RC_SCRIPT, NULL };
        char *envp[] = {
            (char *)"PATH=/bin:/sbin:/usr/bin:/usr/sbin",
            (char *)"HOME=/",
            NULL
        };
        lp_execve(SHELL_PATH, argv, envp);
        lp_exit(127);
    }

    /* rc 가 끝날 때까지 기다린다. 그 사이 다른 자식이 죽으면 함께 거둔다. */
    for (;;) {
        int status = 0;
        pid_t done = lp_wait(&status);
        if (done < 0)
            break;
        if (done == pid) {
            if (!LP_WIFEXITED(status) || LP_WEXITSTATUS(status) != 0)
                dprintf(STDERR_FILENO, "init: %s 가 오류로 끝났습니다\n",
                        RC_SCRIPT);
            break;
        }
    }
}

/* 셸을 하나 띄운다. 자식 pid 를 돌려준다. */
static pid_t spawn_shell(void)
{
    pid_t pid = lp_fork();
    if (pid < 0) {
        dprintf(STDERR_FILENO, "init: fork 실패 (%d)\n", (int)pid);
        return -1;
    }

    if (pid == 0) {
        /* 새 세션의 리더가 되어 제어 터미널을 갖는다.
         * 이게 없으면 Ctrl-C 같은 것이 동작하지 않는다. */
        lp_setsid();

        char *argv[] = { (char *)SHELL_PATH, NULL };
        char *envp[] = {
            (char *)"PATH=/bin:/sbin:/usr/bin:/usr/sbin",
            (char *)"HOME=/",
            (char *)"TERM=linux",
            NULL
        };

        lp_execve(SHELL_PATH, argv, envp);

        dprintf(STDERR_FILENO, "init: %s 를 실행할 수 없습니다\n", SHELL_PATH);
        lp_exit(127);
    }

    return pid;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    bool is_pid1 = (lp_getpid() == 1);

    /* PID 1 이 아니면(개발 중 직접 실행) 마운트를 건너뛴다.
     * 이미 마운트된 시스템에 다시 마운트하려 들면 곤란하다. */
    if (is_pid1) {
        mount_filesystems();
        setup_console();
    } else
        printf("init: pid 1 이 아니므로 마운트를 건너뜁니다 (테스트 모드)\n");

    banner();

    if (is_pid1)
        run_rc();

    pid_t shell_pid = spawn_shell();

    /* 메인 루프: 죽은 자식을 거두고, 셸이 죽으면 다시 띄운다. */
    for (;;) {
        int status = 0;
        pid_t pid = lp_wait(&status);

        if (pid < 0) {
            /* 기다릴 자식이 없다 (ECHILD = 10).
             * PID 1 은 종료하면 안 되므로 잠깐 쉬고 다시 본다. */
            if (!is_pid1)
                break;
            lp_sleep_ms(200);
            continue;
        }

        if (pid != shell_pid)
            continue;           /* 고아 프로세스를 거둔 것뿐 */

        int code = LP_WIFEXITED(status) ? LP_WEXITSTATUS(status) : -1;
        printf("\ninit: 셸이 종료되었습니다 (코드 %d). 다시 시작합니다.\n", code);

        if (!is_pid1)
            break;              /* 테스트 모드에서는 한 번만 */

        lp_sleep_ms(RESPAWN_MS);
        shell_pid = spawn_shell();
    }

    printf("init: 종료\n");
    return 0;
}
