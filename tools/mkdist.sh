#!/usr/bin/env bash
#
# mkdist.sh - the three things somebody else gets.
#
#   dist/test_a_123_LPzero2W_linux-utm.zip     arm64, virtual machines.
#         QEMU, UTM and UTM SE. UEFI only, so it carries one compressed
#         kernel (vmlinuz.efi, 11MB) and no GPU firmware.
#
#   dist/test_a_123_LPzero2W_linux.img.xz      arm64, a real Pi Zero 2 W.
#         Also boots in a VM. It has to carry the uncompressed 22MB
#         kernel and start.elf, because the Broadcom GPU firmware loads
#         the kernel itself and cannot decompress one.
#
#   dist/linux-LP_amd64.img.xz                 amd64, a PC or a desktop VM.
#         Called linux-LP inside, because it is not a Raspberry Pi.
#         UEFI only - which is how a PC boots anyway - so one bzImage,
#         no firmware blobs, no device tree, no config.txt.
#
# Why the arm64 pair and not one image: the universal one has to hold a
# kernel the GPU can read, and a VM has no use for either that or
# start.elf, since UEFI looks only at EFI/BOOT/BOOTAA64.EFI.
#
# Usage:
#   ./tools/mkdist.sh          all three
#   ./tools/mkdist.sh utm      arm64 VM only
#   ./tools/mkdist.sh sd       arm64 Pi image only
#   ./tools/mkdist.sh amd64    amd64 only
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && cd .. && pwd)"
DIST="${REPO_ROOT}/dist"
IMG="${REPO_ROOT}/sdcard/lp-zero.img"

die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }
log()  { printf '  %s\n' "$*"; }

WHAT="${1:-all}"
case "$WHAT" in all|utm|sd|amd64) ;; *) die "알 수 없는 인자: $WHAT" ;; esac

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

# ── amd64 ────────────────────────────────────────────────────────
if [[ "$WHAT" == "all" || "$WHAT" == "amd64" ]]; then
    step "amd64 이미지 (PC / 데스크톱 가상머신)"

    # Its own userland, rootfs and kernel: different instruction set,
    # different name inside, different dropbear.
    make -C "${REPO_ROOT}/userland" ARCH=amd64 >/dev/null
    ( cd "${REPO_ROOT}/userland" \
      && LP_ARCH=amd64 LP_BINDIR=bin-amd64 LP_ROOTFS_DIR=rootfs-amd64 \
         LP_CPIO_NAME=initramfs-amd64.cpio.gz \
         LP_HOSTNAME=linux-lp LP_OS_NAME=linux-LP ./mkrootfs.sh >/dev/null )
    LP_ARCH=amd64 LP_ROOTFS_DIR=rootfs-amd64 "${REPO_ROOT}/kernel/build.sh" >/dev/null
    LP_ARCH=amd64 LP_ROOTFS_DIR=rootfs-amd64 \
        "${REPO_ROOT}/tools/mksdcard.sh" --linux --uefi-only >/dev/null
    [[ -f "$IMG" ]] || die "amd64 이미지가 만들어지지 않았습니다"

    rm -f "${DIST}/linux-LP_amd64.img.xz"
    xz -9 -T0 -c "$IMG" > "${DIST}/linux-LP_amd64.img.xz"
    log "linux-LP_amd64.img.xz  $(stat -c%s "${DIST}/linux-LP_amd64.img.xz") bytes"

    # And rebuild the arm64 kernel, because the two share kernel/out
    # only through this script's own ordering - leaving the tree holding
    # an amd64 rootfs would make the next `make` quietly wrong.
    make -C "${REPO_ROOT}/userland" >/dev/null
    ( cd "${REPO_ROOT}/userland" && ./mkrootfs.sh >/dev/null )
fi

# 체크섬은 여기서 만든다.
#
# 손으로 적어두었더니 이미지를 다시 빌드할 때마다 조용히 어긋났다.
# 받는 사람 입장에서 맞지 않는 sha256 은 없느니만 못하다 - 파일이
# 깨진 것인지 목록이 낡은 것인지 구별할 방법이 없기 때문이다.
# 이미지를 만든 자리에서 같이 만들어야 어긋날 수가 없다.
step "체크섬"
( cd "$DIST" && rm -f SHA256SUMS.txt \
  && sha256sum *.img.xz *.zip > SHA256SUMS.txt 2>/dev/null || true )
if [[ -f "${DIST}/test_a_123_LPzero2W_linux.img.xz" ]]; then
    ( cd "$DIST" && sha256sum test_a_123_LPzero2W_linux.img.xz \
        > PI_IMAGE_SHA256.txt )
fi
while read -r _ name; do log "$name"; done < "${DIST}/SHA256SUMS.txt"

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
