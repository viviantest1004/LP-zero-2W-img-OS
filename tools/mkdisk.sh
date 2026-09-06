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

# 1GB, not 4.
#
# The root filesystem that goes in it is nine megabytes. expandfs grows
# the partition and the filesystem to the end of whatever disk the image
# is written to, on the first boot - that is the entire reason it
# exists - so the image only has to be big enough to hold what it ships
# with.
#
# Getting this wrong is paid for twice at build time: the intermediate
# filesystem is built beside the image at full size, so a 4GB image
# means writing eight gigabytes to package nine megabytes, and it is
# also four gigabytes for somebody to download.
#
#   SIZE_GB=8 ./tools/mkdisk.sh    if a big one is wanted anyway
SIZE_GB="${SIZE_GB:-1}"
# The merged desktop root is about 700MB, so the console default of 1GB
# does not fit it. mkdesktop.sh sets this.
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
# LP_ROOTFS_OVERRIDE points at a root built somewhere else - the merged
# Debian-plus-ours tree that mkdesktop.sh makes. Without it this builds
# the console image from our userland alone, which is the same image and
# a smaller one.
ROOTFS="${LP_ROOTFS_OVERRIDE:-${REPO_ROOT}/userland/rootfs-amd64}"
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
#
# Skipped when there is already a bzImage that is newer than the preinit
# inside it. The kernel here changes only when preinit does - the config
# is fixed and the initramfs holds one program - so rebuilding it to
# package a different root filesystem is eight minutes for a byte-
# identical result.
step "커널 (디스크 루트용)"
BZ_EXISTING="${KERNEL_OUT}/bzImage"
if [[ -f "$BZ_EXISTING" && \
      ! "${REPO_ROOT}/userland/bin-amd64/preinit" -nt "$BZ_EXISTING" ]]; then
    log "이미 있는 것을 씁니다 ($(stat -c%s "$BZ_EXISTING") bytes)"
    log "다시 빌드하려면 지우십시오: rm ${BZ_EXISTING}"
else
LP_ARCH=amd64 \
LP_ROOTFS_DIR="$(python3 -c "import os,sys;print(os.path.relpath(sys.argv[1], os.path.join(sys.argv[2],'userland')))" "$TINY" "$REPO_ROOT")" \
LP_BUILD_DIR="${LPZERO_WORK}/build-amd64-disk" \
LP_OUT_SUBDIR=out-amd64-disk \
LP_CMDLINE="root=LABEL=${ROOT_LABEL} rw console=tty0 console=ttyS0,115200 loglevel=4" \
    "${REPO_ROOT}/kernel/build.sh"
fi

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

# 들어갈 것이 자리보다 크면 여기서 멈춘다.
#
# mke2fs 는 이 경우 "Could not allocate block in ext2 filesystem while
# writing file <아무 파일 이름>" 이라고만 말한다. 그 이름은 마침 마지막
# 으로 쓰려던 파일일 뿐이라, 읽는 사람은 그 파일이 잘못된 줄 알고 한참
# 을 엉뚱한 데서 찾는다. 무엇이 부족한지는 여기서 이미 알 수 있다.
NEED_KB=$(du -sk "$ROOTFS" | cut -f1)
HAVE_KB=$(( ROOT_SECTORS * SECTOR / 1024 ))
# ext4 자체의 메타데이터에 5% 쯤. 여유가 없으면 마지막에 가서 터진다.
if (( NEED_KB * 105 / 100 > HAVE_KB )); then
    want=$(( (NEED_KB * 105 / 100 + ESP_MB * 1024) / 1024 / 1024 + 1 ))
    die "루트가 파티션보다 큽니다: $(( NEED_KB / 1024 ))MiB 를 $(( HAVE_KB / 1024 ))MiB 에 넣을 수 없습니다.
       SIZE_GB=${want} ./tools/mkdisk.sh"
fi

mkdir -p "$OUT_DIR"
rm -f "$IMAGE"
truncate -s $(( TOTAL_SECTORS * SECTOR )) "$IMAGE"

# Built straight into the image at the partition's offset, with no
# intermediate file.
#
# The obvious way is to mkfs a separate file and dd it in, and it costs
# twice the disk and twice the time: a 1GB image needs a 1GB filesystem
# beside it, and then every block is read and written again. mke2fs
# takes -E offset= precisely so that this is unnecessary. On a machine
# with room to spare that is merely wasteful; on one without, it is the
# difference between a build that finishes and one that fills the disk
# at 90% and leaves a corrupt image behind.
#
# -d takes the directory straight in, which keeps ownership and modes
# without a loop mount - and a loop mount needs privileges a build
# should not assume it has.
mkfs.ext4 -q -F -L "$ROOT_LABEL" -m 1 \
    -E offset=$(( ROOT_START * SECTOR )) \
    -d "$ROOTFS" \
    "$IMAGE" $(( ROOT_SECTORS * SECTOR / 1024 ))k
log "$(( ROOT_SECTORS * SECTOR / 1024 / 1024 ))MiB  (이미지 안에 직접)"

# ── 4. the EFI system partition ──────────────────────────────────
step "EFI 시스템 파티션 (FAT32, ${ESP_LABEL})"
ESP_IMG="${OUT_DIR}/.esp.img"
rm -f "$ESP_IMG"
truncate -s $(( ESP_SECTORS * SECTOR )) "$ESP_IMG"
mkfs.vfat -F 32 -n "$ESP_LABEL" "$ESP_IMG" >/dev/null

mmd -i "$ESP_IMG" ::EFI ::EFI/BOOT
mcopy -o -i "$ESP_IMG" "$BZIMAGE" ::EFI/BOOT/BOOTX64.EFI
log "EFI/BOOT/BOOTX64.EFI"

# startup.nsh, and it is not belt and braces.
#
# EFI/BOOT/BOOTX64.EFI is the removable-media path and every firmware is
# supposed to try it. Not all of them do on the first boot of a blank
# NVRAM: OVMF with fresh variables walks its own boot list, finds
# nothing it put there itself, and falls through to the EFI shell -
# which leaves a machine sitting at a Shell> prompt that most people
# will read as "it does not boot".
#
# The shell runs startup.nsh without being asked. So the one firmware
# path that looks like a dead end becomes the one that boots.
printf 'fs0:\r\nEFI\\BOOT\\BOOTX64.EFI\r\n' > "${OUT_DIR}/.startup.nsh"
mcopy -o -i "$ESP_IMG" "${OUT_DIR}/.startup.nsh" ::startup.nsh
rm -f "${OUT_DIR}/.startup.nsh"
log "startup.nsh (NVRAM 이 비어 있을 때의 길)"

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
dd if="$ESP_IMG"  of="$IMAGE" bs=1M seek=$(( ESP_START / 2048 )) conv=notrunc,sparse status=none

python3 "${REPO_ROOT}/tools/write-mbr.py" "$IMAGE" \
    "$ESP_START" "$ESP_SECTORS" "$ROOT_START" "$ROOT_SECTORS"

rm -f "$ESP_IMG"

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
