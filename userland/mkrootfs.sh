#!/usr/bin/env bash
#
# mkrootfs.sh - 루트 파일시스템을 조립하고 initramfs(cpio)를 만든다.
#
# 리눅스는 부팅 시 initramfs 를 램디스크로 풀고 그 안의 /init 을 PID 1 로
# 실행한다. 형식은 newc cpio 아카이브다.
#
# 우리 프로그램은 전부 정적 링크라 라이브러리도 동적 링커도 필요 없다.
# 외부에서 가져오는 것은 셋뿐이다:
#   dropbear         SSH 서버 (직접 만든 암호 구현은 위험하다)
#   wpa_supplicant   WPA2 인증 (같은 이유)
#   brcm 펌웨어      무선칩 내부로 올라가는 블롭
#
# 사용법:
#   ./mkrootfs.sh              rootfs/ 조립
#   ./mkrootfs.sh --cpio       initramfs.cpio.gz 까지
#   ./mkrootfs.sh --test       chroot 로 실행 (root 필요)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/.." && pwd)"
# shellcheck source=tools/common.sh
source "${REPO_ROOT}/tools/common.sh"


BIN_DIR="${HERE}/bin"
ROOT_DIR="${HERE}/rootfs"
CPIO_OUT="${HERE}/initramfs.cpio.gz"
OVERLAY="${REPO_ROOT}/boot/rootfs-overlay"
FW_DIR="${REPO_ROOT}/blobs/brcm"
THIRD="${THIRDPARTY_DIR:-${LPZERO_WORK}/thirdparty}"

# 프로그램 목록은 Makefile 의 PROGS 에서 읽는다.
# 두 곳에 나눠 적으면 새 도구를 추가할 때 한쪽을 빠뜨린다
# (실제로 zram/guard/expandfs/mkdir 를 빌드해놓고 rootfs 에 넣지 않아
#  부팅 후 "명령을 찾을 수 없습니다" 가 났었다).
read -r -a PROGRAMS <<< "$(sed -n 's/^PROGS[[:space:]]*:*=[[:space:]]*//p' "${HERE}/Makefile" | tail -1)"
[[ ${#PROGRAMS[@]} -gt 0 ]] || die "Makefile 에서 PROGS 를 읽지 못했습니다"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
log() { printf '  %s\n' "$*"; }

for p in "${PROGRAMS[@]}"; do
    [[ -f "${BIN_DIR}/${p}" ]] || die "bin/${p} 가 없습니다. 'make' 를 먼저 실행하세요."
done

echo "rootfs 조립 중..."
rm -rf "$ROOT_DIR"
mkdir -p "$ROOT_DIR"/{bin,sbin,opt,srv,dev/pts,proc,sys,tmp,etc,root/.ssh,data,var/run,lib/firmware/brcm,usr/local/bin,usr/bin,usr/sbin,usr/lib}

# ── 우리 프로그램 ────────────────────────────────────────────────
for p in "${PROGRAMS[@]}"; do
    cp "${BIN_DIR}/${p}" "${ROOT_DIR}/bin/${p}"
done
# mount 는 argv[0] 로 동작을 가른다. 같은 파일을 umount 이름으로도 둔다.
ln -sf mount "${ROOT_DIR}/bin/umount"
# chattr 도 argv[0] 로 갈린다. lsattr 은 같은 파일이다.
ln -sf chattr "${ROOT_DIR}/bin/lsattr"
# 나머지 argv[0] 쌍들. 하는 일이 같고 기본값만 다른 것들이라
# 파일을 두 개 만들 이유가 없다.
ln -sf chown   "${ROOT_DIR}/bin/chgrp"
ln -sf id      "${ROOT_DIR}/bin/groups"
ln -sf useradd "${ROOT_DIR}/bin/userdel"
ln -sf su      "${ROOT_DIR}/bin/sudo"
log "bin/: ${PROGRAMS[*]} umount lsattr chgrp groups userdel sudo"

# 커널은 initramfs 의 /init 을 PID 1 로 실행한다
ln -sf bin/init "${ROOT_DIR}/init"

# ── 외부 프로그램 ────────────────────────────────────────────────
copy_third() {
    local src="$1" name="$2"
    if [[ -f "$src" ]]; then
        cp "$src" "${ROOT_DIR}/bin/${name}"
        printf "  %-16s %8s bytes\n" "$name" "$(stat -c%s "$src")"
        return 0
    fi
    printf "  경고: %s 가 없습니다 (%s)\n" "$name" "$src"
    return 1
}

echo ""
echo "외부 프로그램:"
copy_third "${THIRD}/dropbear-2024.86/dropbear"    dropbear    || true
copy_third "${THIRD}/dropbear-2024.86/dropbearkey" dropbearkey || true
copy_third "${THIRD}/wpa_supplicant-2.11/wpa_supplicant/wpa_supplicant" wpa_supplicant || true
copy_third "${THIRD}/wpa_supplicant-2.11/wpa_supplicant/wpa_cli"        wpa_cli        || true

# ── 무선칩 펌웨어 ────────────────────────────────────────────────
# brcmfmac 드라이버가 /lib/firmware/brcm/ 에서 찾는다.
if [[ -d "$FW_DIR" ]] && compgen -G "${FW_DIR}/*" >/dev/null; then
    cp "${FW_DIR}"/* "${ROOT_DIR}/lib/firmware/brcm/"
    echo ""
    log "lib/firmware/brcm/: $(ls -1 "$FW_DIR" | wc -l)개 파일, $(du -sh "$FW_DIR" | cut -f1)"
else
    echo ""
    log "경고: blobs/brcm 이 비어 있습니다. './tools/fetch-wifi-fw.sh' 를 실행하세요."
    log "      (없으면 wlan0 인터페이스가 생기지 않습니다)"
fi

# ── 설정 파일 ────────────────────────────────────────────────────
if [[ -d "$OVERLAY" ]]; then
    cp -r "${OVERLAY}/." "${ROOT_DIR}/"
fi

# dropbear 는 사용자의 홈 디렉터리를 /etc/passwd 에서 찾는다.
# 이 파일이 없으면 로그인이 되지 않는다.
cat > "${ROOT_DIR}/etc/passwd" <<'PASSWD'
root:x:0:0:root:/root:/bin/sh
# 로그인용 계정이 아니다. dropprivs 가 권한을 낮춰 프로그램을 돌릴 때
# 쓰는 상대이며, 여기 있는 이유는 ls -l 이 숫자 대신 이름을 찍게
# 하기 위해서다. 셸이 nologin 인 것은 이 계정으로 들어올 수 없다는 뜻.
#
#   dropprivs 1000 python3 /data/app.py
#
# 네트워크에서 오는 것을 다루는 스크립트는 root 로 돌리지 않는 편이
# 낫다. 그 스크립트의 실수가 곧 기기 전체를 잃는 일이 되지 않는다.
user:x:1000:1000:unprivileged:/data/user:/bin/false
PASSWD
cat > "${ROOT_DIR}/etc/group" <<'GROUP'
root:x:0:
user:x:1000:
GROUP
printf 'lpzero\n' > "${ROOT_DIR}/etc/hostname"

# ── 이미지에 공개키 굽기 ────────────────────────────────────────
#
#   AUTHORIZED_KEYS=~/.ssh/id_ed25519.pub ./mkrootfs.sh
#
# 여기 넣은 키는 커널 이미지 안에 들어가 매 부팅 램에 펼쳐진다. 어떤
# 파일시스템에서도 닿을 수 없으므로, /data 가 랜섬웨어로 암호화되고
# /boot 까지 지워져도 재부팅 한 번이면 다시 SSH 로 들어올 수 있다.
# 카드를 새로 굽는 것 외에는 지울 방법이 없는 유일한 사본이다.
#
# 넣지 않으면 안내문만 들어가고, 부팅 때 authkey 가 "아무도 못 들어옴"
# 이라고 경고한다.
if [[ -n "${AUTHORIZED_KEYS:-}" ]]; then
    [[ -f "$AUTHORIZED_KEYS" ]] \
        || { echo "error: 공개키 파일이 없습니다: $AUTHORIZED_KEYS" >&2; exit 1; }
    # 개인키를 실수로 넣는 사고를 막는다. 공개키는 한 줄이고 ssh- 로 시작한다.
    if grep -q "PRIVATE KEY" "$AUTHORIZED_KEYS"; then
        echo "error: $AUTHORIZED_KEYS 는 개인키입니다. .pub 파일을 주세요." >&2
        exit 1
    fi
    cat "$AUTHORIZED_KEYS" >> "${ROOT_DIR}/etc/authorized_keys"
    log "etc/authorized_keys: 이미지에 공개키 $(grep -c "^ssh-" "$AUTHORIZED_KEYS")개 구움"
fi

# 공개키는 ~/.ssh/authorized_keys 에 있어야 dropbear 가 읽는다.
if [[ -f "${ROOT_DIR}/etc/authorized_keys" ]]; then
    cp "${ROOT_DIR}/etc/authorized_keys" "${ROOT_DIR}/root/.ssh/authorized_keys"
    chmod 600 "${ROOT_DIR}/root/.ssh/authorized_keys"
fi
chmod 700 "${ROOT_DIR}/root/.ssh" "${ROOT_DIR}/root"

cat > "${ROOT_DIR}/etc/motd" <<'MOTD'
LP-zero OS

  bare-metal firmware -> Linux kernel -> own libc -> own init -> own shell

Where files live

  /data     the SD card. Survives a reboot.
  /root     home. Bound to /data, so it survives too.
  anywhere else   RAM. Gone on reboot, including /tmp and /etc.

  To keep a file, put it under /data or in your home directory.

  help        list the built-in commands
  edit FILE   text editor        touch FILE   create an empty file
  calc 2+3*4  decimal, hex and binary at once
  date        the time. This board has no clock battery, and a wrong
              clock makes every HTTPS certificate check fail.
              'ntp' sets it from the network, 'date -z list' picks a zone.
  python      on /data
  sysinfo     how the machine is doing

  Commands in /data/rc.local run at every boot.
MOTD

# ── /boot 에서 실행할 바이너리의 해시 ────────────────────────────
#
# fsck 는 /boot/e2fsck 를, datadisk --format 은 /boot/mke2fs 를 root 로 실행한다. /boot 는 FAT 파티션이고,
# FAT 파티션은 카드를 뽑아 아무 PC 에나 꽂으면 쓸 수 있다. 그러니
# "부트 파티션에 있는 e2fsck" 는 그 자체로는 신뢰할 근거가 없다.
#
# 기대되는 해시를 시스템 이미지 안에 넣는다. 이 파일은 initramfs 안,
# 즉 커널 이미지 안에 있고 부팅 때 램으로 풀린다 - 어떤 파일시스템에서도
# 손댈 수 없다. 해시가 다르면 fsck 는 실행하지 않는다.
# 형식은 sha256sum 이 쓰는 것 그대로: "<해시>  <이름>".
: > "${ROOT_DIR}/etc/boot-tools.sha256"
for tool in e2fsck mke2fs; do
    src="${HERE}/prebuilt/${tool}"
    if [[ -f "$src" ]]; then
        printf '%s  %s\n' \
            "$(sha256sum "$src" | cut -d' ' -f1)" "$tool" \
            >> "${ROOT_DIR}/etc/boot-tools.sha256"
        log "etc/boot-tools.sha256  ${tool} $(sha256sum "$src" | cut -c1-16)..."
    else
        # 없으면 줄도 쓰지 않는다. 기대 해시가 없으면 실행을 거부하는
        # 쪽을 택한다 - 자동 복구나 --format 을 잃을 뿐이다.
        log "경고: prebuilt/${tool} 이 없습니다"
    fi
done

echo ""
log "etc/: rc, wpa_supplicant.conf, authorized_keys, passwd, group, motd, boot-tools.sha256"

# ── 장치 노드 ────────────────────────────────────────────────────
# 커널은 init 실행 전 /dev/console 을 열어 fd 0,1,2 로 준다.
# 없으면 init 이 파일 디스크립터 하나 없이 시작해 오류조차 못 낸다.
if [[ "$(id -u)" == "0" ]]; then
    mknod -m 600 "${ROOT_DIR}/dev/console" c 5 1
    mknod -m 666 "${ROOT_DIR}/dev/null"    c 1 3
    mknod -m 666 "${ROOT_DIR}/dev/zero"    c 1 5
    mknod -m 444 "${ROOT_DIR}/dev/urandom" c 1 9
    log "dev/: console, null, zero, urandom"
else
    log "경고: root 가 아니라 장치 노드를 만들지 못했습니다"
fi

TOTAL=$(du -sb "$ROOT_DIR" | cut -f1)
echo ""
printf "rootfs: %s  (%.2f MB)\n" "$ROOT_DIR" "$(echo "scale=2; $TOTAL/1048576" | bc)"

# ── initramfs cpio ──────────────────────────────────────────────
if [[ "${1:-}" == "--cpio" ]]; then
    command -v cpio >/dev/null 2>&1 || die "cpio 가 필요합니다 (apt install cpio)"

    # newc 가 리눅스 initramfs 의 표준 형식이다.
    # --reproducible 로 타임스탬프/inode 를 고정해 빌드를 재현 가능하게 한다.
    ( cd "$ROOT_DIR" && find . -print0 \
        | LC_ALL=C sort -z \
        | cpio --null --create --format=newc --reproducible --quiet ) \
        | gzip -9 -n > "$CPIO_OUT"

    printf "initramfs: %s  (%.2f MB)\n" "$CPIO_OUT" \
        "$(echo "scale=2; $(stat -c%s "$CPIO_OUT")/1048576" | bc)"
fi

if [[ "${1:-}" == "--test" ]]; then
    [[ "$(id -u)" == "0" ]] || die "chroot 테스트는 root 권한이 필요합니다"
    echo ""
    chroot "$ROOT_DIR" /bin/init
fi
