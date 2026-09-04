# LP-zero / linux-LP

**직접 만든 초경량 리눅스 배포판.** 커널 설정부터 libc, init, 셸,
103개 명령어까지 전부 새로 썼습니다. 커널 이미지 하나가 곧 시스템
전체이고, 부팅하면 램으로 풀립니다.

원래는 **라즈베리파이 제로 2 W** 한 대에 올리려고 만들었습니다. 512MB
램에 우분투를 올리면 아무것도 하기 전에 램의 절반이 사라지는데, 그게
아까워서 시작한 일입니다. 만들다 보니 특별히 그 보드에만 매인 구석이
없어서, 지금은 **arm64 가상머신과 amd64 PC에서도** 그대로 돕니다.

| | |
|---|---|
| 시스템 전체 | 커널 + 유저랜드 파일 **하나**, 11~23MB |
| 부팅 후 남는 램 | 512MB 보드에서 **480MB** |
| 부팅 시간 | 전원 인가 후 프롬프트까지 10초 남짓 |
| 명령어 | 103개, 전부 자체 구현 |
| SSH | **기본 내장**, 공개키 전용 (비밀번호 인증은 컴파일 자체가 안 됨) |
| 파이썬 | CPython 3.12 + pip. manylinux 휠 설치됨 (numpy 확인) |
| 방화벽 | 기본 켜짐. nftables 를 netlink 로 직접 |
| 라이선스 | MIT |

외부에서 가져온 것은 네 개뿐입니다 — 라즈베리파이 GPU 부트롬이
요구하는 Broadcom 블롭, 그리고 암호 구현 세 개(dropbear, wpa_supplicant,
OpenSSL). **암호는 직접 만들면 안 되는 물건**이라 일부러 가져다 썼습니다.

---

## 어느 이미지를 받아야 하나

| 이미지 | 어디서 도나 |
|---|---|
| `dist/arm64/...img.xz` | **라즈베리파이 제로 2 W 실기 + arm64 가상머신 둘 다.** SD카드에 굽거나 UTM/QEMU에 붙이면 됩니다 |
| `dist/amd64/linux-LP_amd64.img.xz` | **일반 PC와 데스크톱 가상머신.** VMware, VirtualBox, QEMU/KVM, Hyper-V |

arm64 이미지 하나가 실기와 가상머신을 모두 지원합니다. 라즈베리파이 GPU가
읽는 압축 안 된 커널과, UEFI가 읽는 EFI 실행 파일을 **둘 다** 담았기
때문입니다.

amd64 이미지는 시스템 안에서 스스로를 **`linux-LP`** 이라고 부릅니다.
라즈베리파이가 아닌데 그렇다고 말하면 안 되니까요.

---

## 최소 사양

| | 최소 | 권장 |
|---|---|---|
| RAM | **64MB** | 256MB 이상 |
| 저장장치 | **256MB** | 4GB 이상 (파이썬 쓰면 1GB는 필요) |
| CPU | arm64(ARMv8) 또는 x86-64 | 코어 수 무관 |
| 화면 | 없어도 됨 (시리얼/SSH만으로 전부 가능) | |

64MB는 과장이 아닙니다 — 부팅 직후 커널과 유저랜드가 쓰는 램이
30MB 남짓입니다. 파이썬을 안 쓴다면 저장장치도 256MB로 충분합니다.

---

## SSH 접속 (기본 내장)

SSH 서버(dropbear)가 **처음부터 들어 있고 자동으로 뜹니다.** 따로 설치할
것도, 켤 것도 없습니다. 다만 **비밀번호 로그인은 아예 컴파일하지
않았습니다** — 인터넷에 물린 보드에서 비밀번호는 시간 문제라서, 선택지로
두지 않았습니다. 공개키만 씁니다.

### 1. 열쇠를 넣는다

카드(또는 이미지)의 **첫 번째 파티션은 FAT32** 라서 윈도우·맥·리눅스
어디서나 그냥 열립니다. 거기 `authorized_keys` 파일에 공개키를 넣으세요.

```bash
# 카드를 PC에 꽂으면 보이는 부트 파티션에
cat ~/.ssh/id_ed25519.pub >> /media/BOOT/authorized_keys
```

키가 없으면 먼저 만드세요: `ssh-keygen -t ed25519`

> `/boot/authorized_keys` 가 있는 동안은 **그쪽이 원본**입니다. 기기
> 안에서 고쳐도 부팅할 때 덮어씁니다. 기기에서 관리하고 싶으면
> `/boot` 에서 그 파일을 지우세요. 이렇게 만든 이유는, `/data` 가
> 망가져도 카드만 뽑아서 열쇠를 다시 넣으면 들어갈 수 있어야 하기
> 때문입니다.

### 2. 주소를 알아낸다

```
# 기기 화면이나 시리얼 콘솔에서
ifconfig
```
DHCP로 자동으로 주소를 받습니다. 고정 주소를 쓰려면 부트 파티션의
`network.conf` 를 쓰세요.

### 3. 붙는다

```bash
ssh root@192.168.0.42
```

사용자는 **root 하나**입니다. 포트는 22이고 방화벽이 기본으로 열어둡니다.

### 가상머신에서

QEMU라면 포트를 넘겨주세요:
```bash
-netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0
```
```bash
ssh -p 2222 root@localhost
```

### 안 될 때

```
authkey -l           # 지금 어떤 키가 허용돼 있나
service status dropbear
firewall status
logd                 # /data/log/auth 에 로그인 기록
```

---

## 쓰는 법

### SD카드에 굽기 (라즈베리파이)

```bash
xz -d < LP-zero_arm64.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
```
`/dev/sdX` 를 **꼭 확인하세요.** 틀리면 그 디스크가 사라집니다.

첫 부팅에서 `/data` 파티션이 카드 전체로 자동으로 늘어납니다.

### 가상머신 (UTM / QEMU / VMware / VirtualBox)

압축을 풀고 디스크로 붙이면 끝입니다. **UEFI 부팅**으로 설정하세요 —
MBR 부트스트랩은 비어 있어서 legacy BIOS로는 안 뜹니다.

```bash
# QEMU, arm64
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 4096 -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=vars.fd \
  -drive file=LP-zero_arm64.img,format=raw,if=virtio \
  -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0

# QEMU, amd64
qemu-system-x86_64 -m 4096 -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=vars.fd \
  -drive file=linux-LP_amd64.img,format=raw,if=virtio
```

디스크를 키우려면 이미지 파일을 `truncate -s 16G` 하면 됩니다. 파일은
sparse 라서 실제로 쓰는 만큼만 차지하고, 첫 부팅에 `/data` 가 알아서
늘어납니다.

### WiFi (라즈베리파이)

부트 파티션의 `wpa_supplicant.conf` 에 적으면 됩니다.

```
network={
    ssid="우리집"
    psk="비밀번호"
}
```

---

## 왜 안 죽나

이 시스템의 제일 큰 특징입니다. 세 가지 설계가 겹쳐 있습니다.

### 1. 루트 파일시스템이 램에 있다

시스템 전체가 커널 이미지 안의 cpio 이고, 부팅할 때 램으로 풀립니다.
**디스크 위의 어떤 것도 돌아가는 시스템의 일부가 아닙니다.**

- 전원을 갑자기 뽑아도 시스템이 깨질 데가 없습니다. 다음 부팅은 항상
  공장 초기 상태입니다.
- 뭘 잘못 지워도 재부팅하면 원래대로입니다.
- 업데이트가 파일 하나 교체로 끝나고, 실패하면 이전 파일로 돌아갑니다.

대신 `/bin` 에 설치한 게 날아가면 곤란하니, `/bin`·`/lib`·`/usr`·`/opt`
등은 **오버레이**로 만들어 두었습니다 — 읽으면 이미지 것이 보이고,
쓰면 `/data` 에 남습니다. `cp mytool /bin/` 이 재부팅을 견딥니다.

### 2. `guard` — 감시 데몬

메모리, 온도, 전압, CPU, 디스크를 계속 봅니다.

- **메모리 고갈**: 예비 용량 아래로 떨어지면 제일 큰 프로세스부터
  정리합니다. init·셸·SSH·워치독은 절대 안 건드립니다.
- **포크 폭탄**: 프로세스 그룹째 한 번에 죽입니다. 실측 **2937개를 한
  번의 패스로** 정리했습니다. 로그인한 셸은 살립니다.
- **CPU 폭주**: 10초에 경고, 30초에 우선순위 강등. 죽이지는 않습니다 —
  시킨 일일 수도 있으니까.
- **과열/저전압**: 라즈베리파이의 스로틀 비트를 읽어 알려줍니다.
- **디스크 가득 참**: 로그부터 정리합니다.

guard 자체도 init이 감시합니다. 죽이면 **1초 안에 되살아납니다.**

### 3. 되돌아올 길을 항상 남긴다

- **워치독**: 하드웨어 타이머. 커널이 멈추면 보드가 스스로 재시작합니다.
- **`bootcount`**: 5분을 못 버틴 부팅이 5번 연속이면 `/data/rc.local`
  을 건너뜁니다. 사용자 스크립트가 부팅을 막는 상황에서 빠져나옵니다.
- **업데이트 롤백**: 새 시스템이 5분을 못 버티면 이전 이미지로 자동
  복귀합니다.
- **SSH 키 3중화**: 이미지 안 / 부트 파티션(FAT, 어디서나 편집 가능) /
  `/data`. `/data` 가 통째로 날아가도 카드만 뽑으면 들어갈 수 있습니다.
- **`fsck`**: 기기 자체에서 `/data` 를 검사·복구합니다.
- **정상 종료**: `poweroff` 는 서비스를 멈추고, 디스크에 다 쓰고,
  `/data` 를 **언마운트한 뒤** 전원을 내립니다. 다음 부팅에 저널 복구가
  필요 없습니다.

---

## 보안

- **비밀번호 인증 없음.** dropbear를 그 기능 없이 컴파일했습니다.
- **업데이트 서명 검증.** RSA-2048/PKCS#1 v1.5/SHA-256. 공개키는 커널
  이미지 안에 있어서 카드를 뽑아 고칠 수 없습니다. 서명이 안 맞으면
  설치를 거부하고, **우회 옵션은 없습니다.**
- **방화벽 기본 켜짐.** nftables 규칙을 netlink로 직접 넣습니다.
  트랜잭션 하나로 들어가므로 "규칙 절반만 적용된" 순간이 없습니다.
- **DNS 위조 방지.** 트랜잭션 ID는 `getrandom`, 소켓은 `connect`,
  질문 섹션까지 대조합니다.
- **NTP 위조 방지.** 논스·모드·stratum을 검증합니다.
- **`/data` 는 nosuid,nodev** 로 마운트합니다. 카드를 다른 PC에 꽂아
  setuid 파일을 심어도 소용없습니다.
- **`integrity`**: 재부팅을 넘겨 살아남는 파일들의 해시를 확인합니다.
- **로그인 기록**: `/data/log/auth`.

---

## 누구에게 맞나

### 이런 분께 권합니다

- **라즈베리파이 제로/제로 2 W로 뭔가 24시간 돌리려는 분.** 램 480MB가
  통째로 남습니다. 온도 로거, 홈 자동화, 센서 수집 같은 일에
  우분투를 올릴 이유가 없습니다.
- **손 안 대고 오래 돌아가야 하는 기기.** 전원이 불안정하거나 물리적으로
  접근하기 어려운 곳 — 위의 "왜 안 죽나"가 전부 그 상황을 위한 것입니다.
- **리눅스가 실제로 어떻게 돌아가는지 보고 싶은 분.** 커널 설정부터
  셸까지 전부 읽을 수 있는 크기이고, 주석이 "왜 이렇게 했는지"를
  설명합니다.
- **최소한의 것만 있는 환경이 필요한 분.** 공격 표면이 작습니다.

### 이런 분께는 권하지 않습니다

- **데스크톱으로 쓰려는 분.** GUI가 들어 있지 않습니다. DRM 드라이버는
  넣어뒀지만 X11/Wayland는 직접 설치해야 합니다.
- **패키지가 많이 필요한 분.** apt/dnf 같은 큰 저장소가 없습니다.
  `pkg` 와 pip로 되는 범위 안에서 써야 합니다.
- **여러 사용자를 두려는 분.** 만들 수는 있지만, 기본은 root 한 명을
  전제로 설계돼 있습니다.
- **검증된 배포판이 필요한 업무.** 이건 한 사람이 만든 것이고,
  Debian이나 Ubuntu만큼 시험받지 않았습니다. 그럴 자리에는
  Raspberry Pi OS Lite를 쓰세요.
- **컨테이너를 돌리려는 분.** cgroup/네임스페이스를 그 용도로 켜두지
  않았습니다.

---

## 전부 어떤 명령이 있나

`help` 를 치면 기기에서도 볼 수 있습니다. `help <명령>` 은 그 명령만
설명합니다.

### 셸 (14개)

| 명령 | 하는 일 |
|---|---|
| `cd` | change directory |
| `pwd` | print the current directory |
| `echo` | print the arguments |
| `env` | list environment variables |
| `exit` | leave the shell |
| `reboot` | restart the machine |
| `poweroff` | shut the machine down |
| `help` | this list |
| `test` | ask about a file or a string |
| `true` | succeed |
| `false` | fail |
| `if` | branch on a command's result |
| `while` | repeat while a command works |
| `for` | repeat over a list |

### 파일과 저장장치 (23개)

| 명령 | 하는 일 |
|---|---|
| `ls` | list a directory |
| `cp` | copy files |
| `mv` | move or rename |
| `rm` | delete files |
| `mkdir` | create directories |
| `touch` | create an empty file |
| `mount` | mount a filesystem |
| `umount` | unmount a filesystem |
| `expandfs` | grow /data to fill the card |
| `disk` | what storage is attached |
| `part` | change the partition table |
| `datadisk` | choose which partition is /data |
| `fsck` | check and repair /data |
| `tar` | make and open archives |
| `find` | walk a directory tree |
| `du` | how much space it takes |
| `chmod` | change what may be done |
| `chown` | change who owns a file |
| `chgrp` | change the group |
| `ln` | another name for a file |
| `stat` | what a file is |
| `chattr` | flags root has to undo first |
| `lsattr` | show those flags |

### 텍스트 (11개)

| 명령 | 하는 일 |
|---|---|
| `cat` | print a file |
| `edit` | edit a file on screen |
| `more` | read it a screen at a time |
| `grep` | print the lines that match |
| `head` | the first lines |
| `tail` | the last lines |
| `wc` | count lines, words, characters |
| `sort` | put lines in order |
| `uniq` | collapse repeated lines |
| `cut` | take columns out of lines |
| `tee` | write to a file and pass on |

### 시스템 (36개)

| 명령 | 하는 일 |
|---|---|
| `top` | what is running, and stop it |
| `ps` | what is running, once |
| `df` | how full each filesystem is |
| `free` | how much memory is left |
| `usage` | memory and disk at a glance |
| `clear` | wipe the screen |
| `reset` | put the terminal back together |
| `run` | run an ordinary Linux binary |
| `dropprivs` | run something as not-root |
| `uname` | what this system is |
| `hostname` | what this machine calls itself |
| `uptime` | how long it has run, and load |
| `whoami` | which user this is |
| `id` | user and group, by number and name |
| `groups` | which group |
| `useradd` | make a user |
| `userdel` | remove one |
| `su` | run something as another user |
| `sudo` | run something as root |
| `service` | what init keeps alive |
| `sha256sum` | the checksum of a file |
| `integrity` | has anything persistent changed |
| `kill` | stop a process, by pid or name |
| `sleep` | wait |
| `watchdog` | reboot the board if it hangs |
| `logd` | collect logs to /data/log |
| `dmesg` | the kernel's own log |
| `sysinfo` | memory, CPU, disks, network |
| `zram` | compressed swap in RAM |
| `guard` | the safety net (memory, heat, power, CPU) |
| `bootcount` | detect a reboot loop |
| `beacon` | report how the board is doing |
| `calc` | integer calculator |
| `pkg` | install and remove packages |
| `update` | replace the system, reversibly |
| `splash` | draw the boot screen |

### 네트워크 (14개)

| 명령 | 하는 일 |
|---|---|
| `dhcp` | get an address, and keep it |
| `ipconfig` | a fixed address from a file |
| `net` | set it up, and say where it broke |
| `ping` | is it there, and how far |
| `ifconfig` | look at or set an interface |
| `route` | where packets go |
| `nslookup` | what address a name has |
| `wget` | download a file |
| `wpa_supplicant` | join a WiFi network |
| `wpa_cli` | talk to wpa_supplicant |
| `dropbear` | the SSH server |
| `dropbearkey` | make an SSH host key |
| `authkey` | keep a way in over SSH |
| `firewall` | which ports are open |

### 파이썬 (3개)

| 명령 | 하는 일 |
|---|---|
| `python` | CPython 3.12 |
| `python3` | the same as python |
| `micropython` | MicroPython - small, fast |

### 시간 (2개)

| 명령 | 하는 일 |
|---|---|
| `date` | show or set the clock |
| `ntp` | set the clock from the net |

---

## 직접 빌드하기

### 필요한 것

```bash
sudo apt install clang lld llvm gcc-aarch64-linux-gnu \
     mtools dosfstools e2fsprogs xz-utils zip cpio bc \
     bsdextrautils qemu-system-arm qemu-system-x86 python3
```

x86 호스트에서 arm64를 크로스 빌드합니다. amd64는 네이티브라 크로스
컴파일러가 필요 없습니다.

### arm64 (라즈베리파이 + arm64 VM)

```bash
./tools/fetch-kernel.sh      # 리눅스 소스
./tools/fetch-blobs.sh       # 라즈베리파이 GPU 펌웨어
make                         # 유저랜드
make kernel                  # 커널 + initramfs
make sdcard-linux            # sdcard/lp-zero.img
```

### amd64 (PC)

```bash
make ARCH=amd64
( cd userland && LP_ARCH=amd64 LP_BINDIR=bin-amd64 \
    LP_ROOTFS_DIR=rootfs-amd64 LP_CPIO_NAME=initramfs-amd64.cpio.gz \
    LP_HOSTNAME=linux-lp LP_OS_NAME=linux-LP ./mkrootfs.sh )
LP_ARCH=amd64 LP_ROOTFS_DIR=rootfs-amd64 ./kernel/build.sh
LP_ARCH=amd64 LP_ROOTFS_DIR=rootfs-amd64 ./tools/mksdcard.sh --linux --uefi-only
```

### 배포본 세 개 한 번에

```bash
./tools/mkdist.sh            # dist/ 에 전부
```

### 업데이트 서명 키

```bash
./tools/sign-release.sh --new-key    # keys/ 에 키 생성 (한 번만)
make                                  # 공개키가 이미지에 들어감
./tools/sign-release.sh kernel/out/Image
```
개인키는 **절대 기기에 넣지 마세요.** `keys/` 는 `.gitignore` 에
있습니다.

---

## 소스는 왜 아키텍처별로 안 나눴나

`arm64/` `amd64/` 폴더로 나누지 않았습니다. 두 아키텍처가 **같은 소스**를
쓰기 때문입니다. 실제로 다른 것은:

| | |
|---|---|
| `userland/libc/include/syscall-arm64.h` / `-x86_64.h` | 시스템 콜 번호와 호출 규약 |
| `userland/libc/src/crt0.S` | 진입점 (`#if` 로 갈림) |
| `kernel/lp-zero.config` / `lp-zero-amd64.fragment` | 커널 설정 |

103개 명령어와 libc 본체는 **한 글자도 다르지 않습니다.** 폴더를 나눠
복사해두면 한쪽만 고치는 사고가 나고, 그건 실제로 겪었습니다 — amd64
이미지에 arm64 바이너리가 43MB 섞여 나간 적이 있습니다. 지금은
`check_tree_arch` 가 빌드할 때 모든 ELF의 아키텍처를 대조하고 다르면
**빌드를 세웁니다.**

```
userland/          한 벌의 소스. make ARCH=amd64 로 갈림
  libc/            자체 libc (시스템 콜 위에 직접)
  init/ sh/ ...    103개 명령어, 하나에 디렉터리 하나
kernel/            커널 설정과 빌드 스크립트
boot/              부트 파티션에 들어가는 것들 (config.txt, /etc/rc)
tools/             이미지 생성, 서명, 배포
tests/             자체 점검 스크립트
dist/              완성된 이미지
```

---

## 세부 사항

### 파티션 구조

| | |
|---|---|
| p1 | FAT32 128MB. 커널, `config.txt`, `authorized_keys`, WiFi 설정, 방화벽 설정. **어느 PC에서나 편집 가능** |
| p2 | ext4 나머지 전부 → `/data`. 첫 부팅에 카드 전체로 확장 |

### 어디에 뭐가 남나

| 경로 | 재부팅 후 |
|---|---|
| `/data` | 남습니다 |
| `/root` | 남습니다 (`/data/root` 바인드) |
| `/bin` `/lib` `/usr` `/opt` `/sbin` `/srv` | 남습니다 (`/data` 오버레이) |
| `/etc` `/tmp` `/var` | **사라집니다** (일부러) |

`/etc` 를 일부러 뺀 이유: `/etc/rc` 와 `/etc/services` 는 `/data` 를
마운트하기 **전에** 읽힙니다. 낡은 사본이 시스템 업데이트를 가리면
보드가 안 뜹니다.

### 파이썬

`/data/python/bin/python3.12` 입니다. `python` 으로 부르면 됩니다.

동적 링크이고 glibc가 `/data/glibc` 에 같이 들어 있습니다. 정적으로
만들면 C 확장이 들어간 휠(numpy, pillow, cryptography...)을 하나도
못 씁니다. 운영체제 자체는 glibc를 안 씁니다 — init도 셸도 명령어도
전부 `userland/libc` 위에서 돕니다.

```bash
pip install requests
python -m pip install numpy    # manylinux 휠도 됩니다
```

### 로그

`/data/log/messages` 와 `/data/log/auth`. 커널 메시지와 우리
프로그램의 메시지가 같이 들어갑니다. 자동으로 회전하므로 무한정
커지지 않습니다.

### 서비스

`init` 이 지키고, `service` 로 봅니다.

```
service                    전부 어떤 상태인가
service restart dropbear
service stop beacon
```
`guard`·`dropbear`·`watchdog` 은 **끌 수 없습니다.** 그 셋이 없으면
기기를 되찾을 방법이 사라지기 때문입니다.

---

## 알려진 한계

정직하게 적습니다.

- **실기기 검증이 아직입니다.** 전부 QEMU에서 시험했습니다. 라즈베리파이
  실물에서의 WiFi 연결, 온도 센서, 하드웨어 워치독은 아직 확인하지
  못했습니다.
- **GUI가 없습니다.** DRM 드라이버는 들어 있어서 X11/Wayland를 올릴
  수는 있지만, 저장소에 준비된 패키지는 없습니다.
- **패키지 저장소가 작습니다.** `pkg` 와 pip 범위입니다.
- **컨테이너를 못 돌립니다.** cgroup/네임스페이스를 그 용도로 안 켰습니다.
- **한 사람이 만들었습니다.** 그만큼 시험받은 코드가 아닙니다.

---

## 라이선스

MIT. 자세한 것은 `LICENSE`.

포함된 외부 소프트웨어는 각자의 라이선스를 따릅니다 — dropbear(MIT),
wpa_supplicant(BSD), OpenSSL(Apache 2.0), Broadcom GPU 펌웨어(재배포
허용 독점 라이선스), 리눅스 커널(GPL-2.0).
