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

BIN_DIR="${HERE}/bin"
ROOT_DIR="${HERE}/rootfs"
CPIO_OUT="${HERE}/initramfs.cpio.gz"
OVERLAY="${REPO_ROOT}/boot/rootfs-overlay"
FW_DIR="${REPO_ROOT}/blobs/brcm"
THIRD="${THIRDPARTY_DIR:-/home/user/kernel-work/thirdparty}"

# 프로그램 목록은 Makefile 의 PROGS 에서 읽는다.
# 두 곳에 나눠 적으면 새 도구를 추가할 때 한쪽을 빠뜨린다
# (실제로 zram/memwatch/expandfs/mkdir 를 빌드해놓고 rootfs 에 넣지 않아
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
mkdir -p "$ROOT_DIR"/{bin,dev/pts,proc,sys,tmp,etc,root/.ssh,data,var/run,lib/firmware/brcm}

# ── 우리 프로그램 ────────────────────────────────────────────────
for p in "${PROGRAMS[@]}"; do
    cp "${BIN_DIR}/${p}" "${ROOT_DIR}/bin/${p}"
done
# mount 는 argv[0] 로 동작을 가른다. 같은 파일을 umount 이름으로도 둔다.
ln -sf mount "${ROOT_DIR}/bin/umount"
log "bin/: ${PROGRAMS[*]} umount"

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
PASSWD
cat > "${ROOT_DIR}/etc/group" <<'GROUP'
root:x:0:
GROUP
printf 'lp-zero\n' > "${ROOT_DIR}/etc/hostname"

# 공개키는 ~/.ssh/authorized_keys 에 있어야 dropbear 가 읽는다.
if [[ -f "${ROOT_DIR}/etc/authorized_keys" ]]; then
    cp "${ROOT_DIR}/etc/authorized_keys" "${ROOT_DIR}/root/.ssh/authorized_keys"
    chmod 600 "${ROOT_DIR}/root/.ssh/authorized_keys"
fi
chmod 700 "${ROOT_DIR}/root/.ssh" "${ROOT_DIR}/root"

cat > "${ROOT_DIR}/etc/motd" <<'MOTD'
LP-zero OS

  펌웨어(베어메탈) -> 리눅스 커널 -> 자체 libc -> 자체 init -> 자체 셸

★ 저장 위치에 주의하세요

  /data     SD 카드. 재부팅해도 남습니다.
  그 밖     RAM. 재부팅하면 사라집니다 (/root, /tmp 포함).

  파일을 남기려면 /data 안에 두세요.

명령: help
상태: zram status / memwatch / ls /data
MOTD

echo ""
log "etc/: rc, wpa_supplicant.conf, authorized_keys, passwd, group, motd"

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
