#!/usr/bin/env bash
#
# mkdist.sh - 남에게 건네줄 두 개를 만든다.
#
#   dist/test_a_123_LPzero2W_linux.img.xz   실기 Pi 와 가상머신 양쪽에서 부팅되는
#                                    SD 카드 이미지. xz 로 압축했다.
#   dist/test_a_123_LPzero2W_linux-utm.zip            UTM/QEMU 전용. GPU 펌웨어와 압축되지
#                                    않은 커널을 빼서 훨씬 작다.
#
# 왜 둘인가: universal 이미지는 라즈베리파이 GPU 펌웨어가 읽을 수 있도록
# 압축되지 않은 커널(22MB)과 start.elf 를 넣어야 한다. 가상머신에는 둘 다
# 쓸모가 없다 - UEFI 가 EFI/BOOT/BOOTAA64.EFI 하나만 보기 때문이다.
# 그래서 가상머신용은 vmlinuz.efi(11MB) 하나만 넣는다.
#
# 사용법:
#   ./tools/mkdist.sh          두 개 다
#   ./tools/mkdist.sh utm      가상머신용만
#   ./tools/mkdist.sh sd       SD 카드용만

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && cd .. && pwd)"
DIST="${REPO_ROOT}/dist"
IMG="${REPO_ROOT}/sdcard/lp-zero.img"

die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }
log()  { printf '  %s\n' "$*"; }

WHAT="${1:-all}"
case "$WHAT" in all|utm|sd) ;; *) die "알 수 없는 인자: $WHAT" ;; esac

command -v xz  >/dev/null || die "xz 가 없습니다 (apt install xz-utils)"
command -v zip >/dev/null || die "zip 이 없습니다 (apt install zip)"

mkdir -p "$DIST"

# ── 가상머신용 ───────────────────────────────────────────────────
if [[ "$WHAT" == "all" || "$WHAT" == "utm" ]]; then
    step "가상머신용 이미지 (UEFI 만)"
    "${REPO_ROOT}/tools/mksdcard.sh" --linux --uefi-only > /dev/null
    [[ -f "$IMG" ]] || die "이미지가 만들어지지 않았습니다"

    # zip 은 sparse 를 모른다. 이미지를 그대로 넣으면 256MB 를 통째로
    # 압축하게 되지만, 빈 곳은 0 이라 실제로는 잘 줄어든다.
    ( cd "${REPO_ROOT}/sdcard" \
      && rm -f "${DIST}/test_a_123_LPzero2W_linux-utm.zip" \
      && cp lp-zero.img test_a_123_LPzero2W_linux.img \
      && zip -q -9 "${DIST}/test_a_123_LPzero2W_linux-utm.zip" test_a_123_LPzero2W_linux.img \
      && rm -f test_a_123_LPzero2W_linux.img )
    log "test_a_123_LPzero2W_linux-utm.zip  $(stat -c%s "${DIST}/test_a_123_LPzero2W_linux-utm.zip") bytes"
fi

# ── SD 카드용 ────────────────────────────────────────────────────
if [[ "$WHAT" == "all" || "$WHAT" == "sd" ]]; then
    step "범용 이미지 (실기 Pi + 가상머신)"
    "${REPO_ROOT}/tools/mksdcard.sh" --linux > /dev/null
    [[ -f "$IMG" ]] || die "이미지가 만들어지지 않았습니다"

    rm -f "${DIST}/test_a_123_LPzero2W_linux.img.xz"
    # -T0: 코어 수만큼 스레드. 256MB 를 한 스레드로 짜면 오래 걸린다.
    xz -9 -T0 -c "$IMG" > "${DIST}/test_a_123_LPzero2W_linux.img.xz"
    log "test_a_123_LPzero2W_linux.img.xz  $(stat -c%s "${DIST}/test_a_123_LPzero2W_linux.img.xz") bytes"
fi

step "결과"
for f in "${DIST}"/*; do
    [[ -f "$f" ]] || continue
    printf '  %-30s %10s bytes\n' "$(basename "$f")" "$(stat -c%s "$f")"
done
echo ""
echo "  SD 카드에 굽기:"
echo "    xz -d < dist/test_a_123_LPzero2W_linux.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress"
echo "  UTM/QEMU:"
echo "    unzip dist/test_a_123_LPzero2W_linux-utm.zip   그리고 test_a_123_LPzero2W_linux.img 를 디스크로 붙인다"
