# 유저랜드 — 자체 libc · init · 셸

외부 libc 를 전혀 링크하지 않는다. `_start` 부터 `printf` 까지 전부 우리 코드다.

```
userland/
├── libc/
│   ├── include/  types.h syscall.h unistd.h string.h stdio.h stdlib.h
│   └── src/      crt0.S unistd.c string.c stdio.c malloc.c
├── init/         PID 1
├── sh/           셸
├── cat/  ls/     유틸리티
└── mkrootfs.sh   rootfs 조립 + initramfs(cpio) 생성
```

현재 크기:

| | 바이트 |
|---|---|
| `init` | 4,352 |
| `sh` | 8,576 |
| `cat` | 3,512 |
| `ls` | 3,864 |
| **initramfs.cpio.gz** | **7,693** |

## AArch64 시스템콜의 함정

x86 을 기준으로 알던 시스템콜이 AArch64 에는 **아예 없다**. asm-generic
표를 쓰기 때문이다:

| 익숙한 것 | AArch64 에서는 |
|---|---|
| `open` | `openat(AT_FDCWD, ...)` |
| `stat` | `newfstatat` |
| `fork` | `clone(SIGCHLD, 0, 0, 0, 0)` |
| `pipe` | `pipe2(fds, 0)` |
| `dup2` | `dup3(old, new, 0)` — 단 old==new 면 EINVAL 이라 따로 처리 |

호출 규약은 번호를 `x8`, 인자를 `x0`~`x5`, `svc #0`. 반환은 `x0` 이고
**오류는 음수 `-errno`** 로 온다 (glibc 처럼 -1 + errno 전역이 아니다).
그래서 우리 래퍼는 errno 전역 없이 음수를 그대로 노출한다.

## 빌드와 테스트

```bash
cd userland
make                    # bin/{init,sh,cat,ls}
./mkrootfs.sh --cpio    # rootfs/ 와 initramfs.cpio.gz
```

x86 호스트에서 aarch64 바이너리를 돌리려면 QEMU 유저모드가 필요하다:

```bash
apt install qemu-user qemu-user-static
qemu-aarch64 bin/sh                 # 단독 실행
```

`init` 이 셸을 `execve` 하는 것까지 검증하려면 binfmt_misc 등록이 필요하다.
QEMU 유저모드는 `execve` 를 호스트 커널로 넘기는데, 호스트가 aarch64 를
읽을 줄 알아야 하기 때문이다.

```bash
mount -t binfmt_misc none /proc/sys/fs/binfmt_misc
cat > /tmp/reg <<'EOF'
:qemu-a64:M::\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00:\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:/usr/bin/qemu-aarch64-static:OCF
EOF
cat /tmp/reg > /proc/sys/fs/binfmt_misc/register

chroot userland/rootfs /bin/init      # 전체 통합 테스트
```

### ⚠️ 등록 문자열의 백슬래시를 셸이 해석하게 두면 안 된다

커널은 `\x7f` 같은 **리터럴 텍스트**를 받아 자기가 unquote 한다.
`printf ':qemu:M::\x7fELF\x02...'` 처럼 셸이 미리 실제 바이트로 바꾸면
문자열이 **NUL 바이트에서 잘린다.** 그러면 매직이

```
magic 7f454c46020101        <- 7바이트만 남음
mask  ffffffffffffff
```

가 되어 **모든 64비트 리틀엔디언 ELF 와 일치**한다. x86 바이너리까지
aarch64 에뮬레이터로 넘어가면서 시스템에서 아무것도 실행할 수 없게 된다
(`/bin/bash` 조차 `ENOEXEC`).

그래서 위처럼 **따옴표 친 heredoc**(`<<'EOF'`)을 써서 백슬래시를 보존한다.
등록 후에는 반드시 확인한다:

```bash
cat /proc/sys/fs/binfmt_misc/qemu-a64 | grep magic
# 7f454c460201010000000000000000000200b700  (40자여야 정상)
```

짧으면 즉시 해제한다. 이때 `echo` 와 리다이렉션은 bash 내장이라
`exec` 이 깨진 상태에서도 동작한다:

```bash
echo -1 > /proc/sys/fs/binfmt_misc/qemu-a64
```

또 인터프리터는 **정적 링크판**(`qemu-aarch64-static`)이어야 한다.
동적 링크판은 chroot 안에서 자기 공유 라이브러리를 찾지 못한다.

## 아직 없는 것

셸: 잡 컨트롤, 변수 확장(`$VAR`), 와일드카드(`*`), 서브셸, `&&`/`||`.
libc: 부동소수점 printf, 시그널 핸들러, 스레드.

필요해질 때 붙인다. 지금은 셸이 도는 데 필요한 만큼만 있다.
