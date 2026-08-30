#!/usr/bin/env bash
#
# mkrootfs.sh - 루트 파일시스템을 조립하고 initramfs(cpio)를 만든다.
#
# 리눅스는 부팅 시 initramfs 를 램디스크로 풀고 그 안의 /init 을 PID 1 로
# 실행한다. 형식은 newc cpio 아카이브다 (gzip 압축 가능).
#
# 우리 시스템은 전부 정적 링크라 라이브러리도 동적 링커도 넣을 필요가 없다.
# 바이너리 네 개와 빈 디렉터리 몇 개가 전부다.
#
# 사용법:
#   ./mkrootfs.sh              rootfs/ 디렉터리 조립
#   ./mkrootfs.sh --cpio       initramfs.cpio.gz 까지 생성
#   ./mkrootfs.sh --test       조립 후 chroot 로 실행해본다 (root 필요)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_DIR="${HERE}/bin"
ROOT_DIR="${HERE}/rootfs"
CPIO_OUT="${HERE}/initramfs.cpio.gz"

PROGRAMS=(init sh cat ls)

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
log() { printf '  %s\n' "$*"; }

for p in "${PROGRAMS[@]}"; do
    [[ -f "${BIN_DIR}/${p}" ]] || die "bin/${p} 가 없습니다. 'make' 를 먼저 실행하세요."
done

# ── 디렉터리 구조 ────────────────────────────────────────────────
echo "rootfs 조립 중..."
rm -rf "$ROOT_DIR"
mkdir -p "$ROOT_DIR"/{bin,dev,proc,sys,tmp,etc,root}

for p in "${PROGRAMS[@]}"; do
    cp "${BIN_DIR}/${p}" "${ROOT_DIR}/bin/${p}"
done
log "bin/: ${PROGRAMS[*]}"

# 커널은 initramfs 의 /init 을 PID 1 로 실행한다.
# /bin/init 을 가리키게 두면 한 벌만 유지하면 된다.
ln -sf bin/init "${ROOT_DIR}/init"
log "/init -> bin/init"

cat > "${ROOT_DIR}/etc/motd" <<'MOTD'
LP-zero OS

전부 직접 만든 시스템입니다:
  펌웨어(베어메탈) -> 리눅스 커널 -> 자체 libc -> 자체 init -> 자체 셸

'help' 를 입력하면 명령 목록이 나옵니다.
MOTD

printf 'lp-zero\n' > "${ROOT_DIR}/etc/hostname"

TOTAL=$(du -sb "$ROOT_DIR" | cut -f1)
echo ""
echo "rootfs: ${ROOT_DIR}  (${TOTAL} bytes)"

# ── initramfs cpio ──────────────────────────────────────────────
if [[ "${1:-}" == "--cpio" ]]; then
    command -v cpio >/dev/null 2>&1 || die "cpio 가 필요합니다 (apt install cpio)"

    # newc 형식이 리눅스 initramfs 의 표준이다.
    # --reproducible 로 타임스탬프/inode 를 고정해 빌드를 재현 가능하게 한다.
    ( cd "$ROOT_DIR" && find . -print0 \
        | LC_ALL=C sort -z \
        | cpio --null --create --format=newc --reproducible --quiet ) \
        | gzip -9 -n > "$CPIO_OUT"

    echo "initramfs: ${CPIO_OUT}  ($(stat -c%s "$CPIO_OUT") bytes)"
fi

# ── chroot 실행 테스트 ──────────────────────────────────────────
if [[ "${1:-}" == "--test" ]]; then
    [[ "$(id -u)" == "0" ]] || die "chroot 테스트는 root 권한이 필요합니다"
    echo ""
    echo "chroot 로 실행합니다. 'exit' 로 빠져나옵니다."
    echo ""
    # binfmt_misc 에 F 플래그로 등록되어 있으면 chroot 안에서도
    # 인터프리터를 찾을 수 있다 (등록 시점에 열어두기 때문).
    chroot "$ROOT_DIR" /bin/init
fi
