#!/usr/bin/env bash
#
# mkdisk.sh - build the disk-rooted amd64 image.
#
# The other images this project makes carry their whole system inside
# the kernel: a cpio unpacked into RAM at boot, nothing on disk that the
# running system depends on. That is what makes a board survive having
# its power pulled, and it is the right answer for a Raspberry Pi in a
# cupboard.
#
# It is the wrong answer for a machine somebody sits in front of. This
# script builds the other kind:
#
#   p1  FAT32, 256MB, the EFI system partition - the kernel and the
#       files a person edits from another computer
#   p2  ext4, everything else, labelled LPROOT - the actual root
#
# The kernel it builds carries a tiny initramfs holding one program,
# preinit, whose only job is to find p2, mount it and switch_root into
# it. Everything after that runs from disk: /etc survives, packages
# install into /usr rather than an overlay, and the root can be bigger
# than the RAM.
#
#   ./tools/mkdisk.sh                 a 4GB image
#   SIZE_GB=16 ./tools/mkdisk.sh      bigger
#
# The result is written with dd, the same as the other images:
#   xz -d < dist/linux-LP_desktop.img.xz | sudo dd of=/dev/sdX bs=4M
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && cd .. && pwd)"
source "${REPO_ROOT}/tools/common.sh"

SIZE_GB="${SIZE_GB:-4}"
SECTOR=512
ESP_MB=256
ESP_START=8192                          # 4MiB in, as the other images do
ESP_SECTORS=$(( ESP_MB * 1024 * 1024 / SECTOR ))
ROOT_START=$(( ESP_START + ESP_SECTORS ))

ROOT_LABEL="LPROOT"
# The same label the RAM-live images use, because /etc/rc mounts /boot by
# label and there is no reason for this image to be the exception - the
# boot partition holds the same authorized_keys, firewall.conf and
# wpa_supplicant.conf on both.
ESP_LABEL="LPZERO"

OUT_DIR="${REPO_ROOT}/sdcard"
IMAGE="${OUT_DIR}/linux-LP_desktop.img"
ROOTFS="${REPO_ROOT}/userland/rootfs-amd64"
TINY="${LPZERO_WORK}/initramfs-preinit"
KERNEL_OUT="${REPO_ROOT}/kernel/out-amd64-disk"

log()  { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

for t in mkfs.vfat mkfs.ext4 mcopy dd truncate python3; do
    command -v "$t" >/dev/null 2>&1 || die "$t 가 없습니다"
done

# ── 1. the tiny initramfs ────────────────────────────────────────
#
# One program and one device node. /dev/console has to be in the cpio
# rather than made at run time: the kernel opens it to give init its
# stdin, stdout and stderr, and without it every message preinit prints
# about what went wrong goes nowhere - which is the one situation those
# messages exist for.
step "preinit initramfs"
[[ -x "${REPO_ROOT}/userland/bin-amd64/preinit" ]] \
    || die "bin-amd64/preinit 가 없습니다. 'make ARCH=amd64' 를 먼저."

rm -rf "$TINY"
mkdir -p "$TINY/dev"
cp "${REPO_ROOT}/userland/bin-amd64/preinit" "$TINY/init"
mknod "$TINY/dev/console" c 5 1 2>/dev/null || true
mknod "$TINY/dev/null"    c 1 3 2>/dev/null || true
log "$(du -sh "$TINY" | cut -f1)  ($TINY)"

# ── 2. the kernel that boots from disk ───────────────────────────
step "커널 (디스크 루트용)"
LP_ARCH=amd64 \
LP_ROOTFS_DIR="$(python3 -c "import os,sys;print(os.path.relpath(sys.argv[1], os.path.join(sys.argv[2],'userland')))" "$TINY" "$REPO_ROOT")" \
LP_BUILD_DIR="${LPZERO_WORK}/build-amd64-disk" \
LP_OUT_SUBDIR=out-amd64-disk \
LP_CMDLINE="root=LABEL=${ROOT_LABEL} rw console=tty0 console=ttyS0,115200 loglevel=4" \
    "${REPO_ROOT}/kernel/build.sh"

BZIMAGE="${KERNEL_OUT}/bzImage"
[[ -f "$BZIMAGE" ]] || die "${BZIMAGE} 가 만들어지지 않았습니다"
log "$(stat -c%s "$BZIMAGE") bytes"

# ── 3. the root filesystem ───────────────────────────────────────
#
# Built from the same rootfs directory the RAM-live image uses, so there
# is one userland and not two. What differs is where it ends up and what
# /etc/rc does when it finds itself on a writable root.
step "루트 파일시스템 (ext4, ${ROOT_LABEL})"
[[ -d "$ROOTFS" ]] || die "${ROOTFS} 가 없습니다. mkrootfs.sh 를 먼저."

TOTAL_SECTORS=$(( SIZE_GB * 1024 * 1024 * 1024 / SECTOR ))
ROOT_SECTORS=$(( TOTAL_SECTORS - ROOT_START ))
ROOT_IMG="${OUT_DIR}/.root.img"

mkdir -p "$OUT_DIR"
rm -f "$ROOT_IMG"
truncate -s $(( ROOT_SECTORS * SECTOR )) "$ROOT_IMG"

# -d takes a directory straight in, which keeps ownership and modes
# without a loop mount - and a loop mount needs privileges a build
# should not assume it has.
mkfs.ext4 -q -F -L "$ROOT_LABEL" -m 1 -d "$ROOTFS" "$ROOT_IMG"
log "$(( ROOT_SECTORS * SECTOR / 1024 / 1024 ))MiB"

# ── 4. the EFI system partition ──────────────────────────────────
step "EFI 시스템 파티션 (FAT32, ${ESP_LABEL})"
ESP_IMG="${OUT_DIR}/.esp.img"
rm -f "$ESP_IMG"
truncate -s $(( ESP_SECTORS * SECTOR )) "$ESP_IMG"
mkfs.vfat -F 32 -n "$ESP_LABEL" "$ESP_IMG" >/dev/null

mmd -i "$ESP_IMG" ::EFI ::EFI/BOOT
mcopy -o -i "$ESP_IMG" "$BZIMAGE" ::EFI/BOOT/BOOTX64.EFI
log "EFI/BOOT/BOOTX64.EFI"

for f in authorized_keys wpa_supplicant.conf firewall.conf beacon.conf; do
    src="${REPO_ROOT}/boot/rootfs-overlay/etc/${f}"
    [[ -f "$src" ]] && mcopy -o -i "$ESP_IMG" "$src" "::${f}" && log "$f"
done

# The command line is compiled into the kernel, but a copy here means a
# person with a card reader can see what it is, and a real bootloader
# would read it.
printf 'root=LABEL=%s rw console=tty0 console=ttyS0,115200 loglevel=4\n' \
    "$ROOT_LABEL" > "${OUT_DIR}/.cmdline.txt"
mcopy -o -i "$ESP_IMG" "${OUT_DIR}/.cmdline.txt" ::cmdline.txt
rm -f "${OUT_DIR}/.cmdline.txt"

# ── 5. assemble ──────────────────────────────────────────────────
step "이미지 조립"
rm -f "$IMAGE"
truncate -s $(( TOTAL_SECTORS * SECTOR )) "$IMAGE"
dd if="$ESP_IMG"  of="$IMAGE" bs=$SECTOR seek=$ESP_START  conv=notrunc status=none
dd if="$ROOT_IMG" of="$IMAGE" bs=$SECTOR seek=$ROOT_START conv=notrunc status=none

python3 "${REPO_ROOT}/tools/write-mbr.py" "$IMAGE" \
    "$ESP_START" "$ESP_SECTORS" "$ROOT_START" "$ROOT_SECTORS"

rm -f "$ESP_IMG" "$ROOT_IMG"

step "결과"
log "$(stat -c%s "$IMAGE") bytes  ${IMAGE}"
log ""
log "  p1  ${ESP_MB}MiB  FAT32  ${ESP_LABEL}   커널과 설정"
log "  p2  나머지  ext4   ${ROOT_LABEL}    루트 파일시스템"
log ""
log "굽기:  sudo dd if=${IMAGE} of=/dev/sdX bs=4M conv=fsync status=progress"
log "QEMU:  qemu-system-x86_64 -m 4096 -smp 4 \\"
log "         -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \\"
log "         -drive if=pflash,format=raw,file=vars.fd \\"
log "         -drive file=${IMAGE},format=raw,if=virtio"
