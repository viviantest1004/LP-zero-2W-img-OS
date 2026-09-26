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
# script builds the other kind, on a GPT disk because that is what a
# UEFI PC boots:
#
#   p1  FAT32, 256MB, the EFI system partition - the kernel as
#       \EFI\BOOT\BOOTX64.EFI, and the files a person edits from
#       another computer
#   p2  ext4, everything else, labelled LPROOT and typed "Linux root
#       (x86-64)" - the actual root
#
# Both partitions get fresh random GUIDs on every build (write-gpt.py),
# and lp-install gives the disk it installs to fresh ones again. That is
# what lets preinit tell this disk's root from another LP disk's when
# both are plugged in - the USB stick an installation was made from is
# usually still in the laptop at the first reboot.
#
# The kernel it builds carries a tiny initramfs holding one program,
# preinit, whose only job is to find p2, mount it and switch_root into
# it. Everything after that runs from disk: /etc survives, packages
# install into /usr rather than an overlay, and the root can be bigger
# than the RAM.
#
#   ./tools/mkdisk.sh                 the console image, 1GB
#   SIZE_GB=16 ./tools/mkdisk.sh      bigger
#   tools/mkdesktop.sh                the desktop image (it calls this)
#
# The result is written with dd, the same as the other images:
#   xz -d < dist/linux-LP_desktop.img.xz | sudo dd of=/dev/sdX bs=4M
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && cd .. && pwd)"
source "${REPO_ROOT}/tools/common.sh"

# 1GB, not 4.
#
# The console root filesystem that goes in it is nine megabytes, so the
# image only has to be big enough to hold what it ships with. (expandfs,
# which grows the root to the end of the disk on the first boot, reads
# an MBR; on this GPT disk it stops without writing anything. The
# desktop image is sized with room to spare for that reason, and an
# installed system gets a root partition the size of its whole disk
# from lp-install.)
#
# Getting this wrong is paid for twice at build time: the intermediate
# filesystem is built beside the image at full size, so a 4GB image
# means writing eight gigabytes to package nine megabytes, and it is
# also four gigabytes for somebody to download.
#
#   SIZE_GB=8 ./tools/mkdisk.sh    if a big one is wanted anyway
SIZE_GB="${SIZE_GB:-1}"
# The merged desktop root is about 3GB, so the console default of 1GB
# does not fit it. mkdesktop.sh sets this.
SECTOR=512
ESP_MB=256
ESP_START=8192                          # 4MiB in, as the other images do
ESP_SECTORS=$(( ESP_MB * 1024 * 1024 / SECTOR ))
ROOT_START=$(( ESP_START + ESP_SECTORS ))

ROOT_LABEL="LPROOT"

# The kernel command line, in one place: it is compiled into the kernel
# and a copy goes onto the FAT partition, and two spellings of it were
# two chances for them to disagree. (The kernel track keeps this line;
# every option on it was checked against the 6.12 source it builds.)
#
#   console=tty0 console=ttyS0   printk goes to both. /dev/console is the
#                                last one, ttyS0: the serial port in a VM.
#                                A PC registers the legacy port at 0x3f8
#                                whether or not a UART answers there, so
#                                on the XPS too /dev/console is ttyS0 and
#                                what is written to it goes nowhere -
#                                anything meant for a person on the
#                                screen has to name tty1 (init does)
#   quiet loglevel=3             the boot is meant to show the firmware's
#                                logo and then the desktop and nothing in
#                                between (kernel/lp-zero-amd64.fragment,
#                                "booting without a flicker"). The kernel
#                                writes to the screen only below level 3
#                                - critical, alert, panic - and so fbcon
#                                never takes the screen over on a good
#                                boot; preinit reads "quiet" too and says
#                                only failures
#   vt.global_cursor_default=0   no blinking text cursor in the corner if
#                                something does bring the console up
#                                mid-boot. A program that wants a person
#                                to type on a VT turns it back on with
#                                ESC [ ? 25 h
#   fbcon=font:TER16x32          the XPS panel is 3840x2160 at 15.6", and
#                                the default 8x16 font there is too small
#                                to read; boot messages, the rescue shell
#                                on tty1 and any text fallback all use it
#
# Not here, on purpose: i915.fastboot (6.12 has no such parameter - it
# always reads back the firmware's mode), preempt=full (the kernel is
# built with PREEMPT and boots in full mode already), i915.enable_guc
# (Skylake loads neither GuC nor HuC unless told to, and nothing here
# needs them).
KERNEL_CMDLINE="root=LABEL=${ROOT_LABEL} rw console=tty0 console=ttyS0,115200 quiet loglevel=3 vt.global_cursor_default=0 fbcon=font:TER16x32"

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
# Skipped when the bzImage already there was built from the same inputs.
# Rebuilding it to package a different root filesystem is eight minutes
# for a byte-identical result.
step "커널 (디스크 루트용)"
BZ_EXISTING="${KERNEL_OUT}/bzImage"
# 커널 안에 든 프로그램은 preinit 하나다. 그래서 다시 지어야 하는지는
# preinit 의 **내용**이 바뀌었는지로 정한다.
#
# 전에는 시각(-nt)으로 정했고, 그것은 틀린 질문이었다: userland 를
# 한 번 make 하면 libc 를 건드리지 않은 프로그램까지 전부 다시
# 링크되어 preinit 의 시각이 앞선다. 내용은 한 바이트도 다르지
# 않은데 커널을 다시 지으라고 하고, 커널 소스가 없는 기계에서는
# 거기서 빌드가 멈춘다.
#
# The inputs are more than preinit, though. The config was once fixed;
# now the XPS support lives in it, and a stamp that looked only at
# preinit kept shipping a kernel built from the old config after the
# config had changed - and the command line and the firmware pin are
# compiled in too, and the firmware sits in the initramfs beside
# preinit. So the stamp covers all of them, and the kernel source commit
# and the scripts that merge the config and build it. (The file keeps
# its old name so an existing stamp is simply a mismatch, not an error.)
PREINIT="${REPO_ROOT}/userland/bin-amd64/preinit"
STAMP="${KERNEL_OUT}/preinit.sha256"
NOW_SUM=""
if [[ -f "$PREINIT" ]]; then
    NOW_SUM="$( { cat "$PREINIT" \
                    "${REPO_ROOT}/kernel/lp-zero.config" \
                    "${REPO_ROOT}/kernel/lp-zero-amd64.fragment" \
                    "${REPO_ROOT}/tools/mkamd64config.sh" \
                    "${REPO_ROOT}/kernel/linux.commit" \
                    "${REPO_ROOT}/kernel/build.sh" \
                    "${REPO_ROOT}/tools/fetch-pc-fw.sh"
                  printf '%s' "$KERNEL_CMDLINE"; } | sha256sum | cut -d' ' -f1)"
fi
OLD_SUM="$(cat "$STAMP" 2>/dev/null || true)"

if [[ -f "$BZ_EXISTING" && -n "$NOW_SUM" && "$NOW_SUM" == "$OLD_SUM" ]]; then
    log "이미 있는 것을 씁니다 ($(stat -c%s "$BZ_EXISTING") bytes)"
    log "다시 빌드하려면 지우십시오: rm ${BZ_EXISTING}"
elif [[ -f "$BZ_EXISTING" && -z "$OLD_SUM" && \
        ! "$PREINIT" -nt "$BZ_EXISTING" ]]; then
    # 도장이 아직 없는 예전 빌드. 시각으로 판단하고 도장을 남긴다.
    log "이미 있는 것을 씁니다 ($(stat -c%s "$BZ_EXISTING") bytes)"
    printf '%s\n' "$NOW_SUM" > "$STAMP"
else
LP_ARCH=amd64 \
LP_ROOTFS_DIR="$(python3 -c "import os,sys;print(os.path.relpath(sys.argv[1], os.path.join(sys.argv[2],'userland')))" "$TINY" "$REPO_ROOT")" \
LP_BUILD_DIR="${LPZERO_WORK}/build-amd64-disk" \
LP_OUT_SUBDIR=out-amd64-disk \
LP_CMDLINE="$KERNEL_CMDLINE" \
    "${REPO_ROOT}/kernel/build.sh"
fi

BZIMAGE="${KERNEL_OUT}/bzImage"
[[ -f "$BZIMAGE" ]] || die "${BZIMAGE} 가 만들어지지 않았습니다"
[[ -n "$NOW_SUM" ]] && printf '%s\n' "$NOW_SUM" > "${KERNEL_OUT}/preinit.sha256"
log "$(stat -c%s "$BZIMAGE") bytes"

# ── 3. the root filesystem ───────────────────────────────────────
#
# Built from the same rootfs directory the RAM-live image uses, so there
# is one userland and not two. What differs is where it ends up and what
# /etc/rc does when it finds itself on a writable root.
step "루트 파일시스템 (ext4, ${ROOT_LABEL})"
[[ -d "$ROOTFS" ]] || die "${ROOTFS} 가 없습니다. mkrootfs.sh 를 먼저."

TOTAL_SECTORS=$(( SIZE_GB * 1024 * 1024 * 1024 / SECTOR ))
# GPT keeps a backup of its table in the last 33 sectors of the disk, so
# the root stops short of them; rounded down to a whole MiB, which is
# what every partitioning tool aligns to and costs under a megabyte.
ROOT_SECTORS=$(( (TOTAL_SECTORS - 34 - ROOT_START + 1) / 2048 * 2048 ))

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

# README.txt: what a person looking at this partition from Windows or a
# Mac needs to know, and the two firmware settings that stop the XPS
# from booting this at all. Both are things nothing on the stick can
# fix or even detect before the kernel is running - with Secure Boot on
# the firmware refuses the unsigned kernel before it starts, and with
# the SATA controller in RAID mode the NVMe disk is invisible to Linux.
# lp-install says the same on screen when it detects either.
cat > "${OUT_DIR}/.readme.txt" <<'README'
linux-LP - EFI system partition
===============================

EFI/BOOT/BOOTX64.EFI   the kernel (it is its own UEFI boot loader)
cmdline.txt            the kernel command line compiled into it, for reference
startup.nsh            makes the UEFI shell boot the kernel too

Before booting this stick on a Dell XPS 15 9550 (F2 opens the BIOS setup):

 1. Secure Boot -> Secure Boot Enable: OFF
    The kernel is not signed by Microsoft; with Secure Boot on, the
    firmware refuses to start it and nothing on this stick can say why.

 2. System Configuration -> SATA Operation: AHCI
    Dell ships these machines set to "RAID On". In that mode the NVMe
    disk sits behind Intel RST and Linux cannot see it, so there is
    nowhere to install to. If Windows is still on the disk, switch it to
    safe mode once before changing this, or it will not boot afterwards.

Then F12 at power-on -> the USB stick.

-----------------------------------------------------------------------

부팅 전에 (XPS 15 9550, 전원을 켤 때 F2 로 BIOS 설정):

 1. Secure Boot -> Secure Boot Enable: 끔
    커널이 Microsoft 서명을 받지 않았습니다. 켜 두면 펌웨어가 커널을
    거부하고, 이 USB 안의 어떤 것도 그 이유를 화면에 보여 줄 수 없습니다.

 2. System Configuration -> SATA Operation: AHCI 로 바꾸세요
    "RAID On" 으로 출고됩니다. 그 상태에서는 NVMe 디스크가 Intel RST 뒤에
    숨어 리눅스가 볼 수 없고, 설치할 곳이 없습니다. 디스크에 Windows 가
    남아 있다면 바꾸기 전에 한 번 안전 모드로 부팅해 두어야 합니다.

그 다음 전원을 켤 때 F12 -> USB.
README
sed -i 's/$/\r/' "${OUT_DIR}/.readme.txt"
mcopy -o -i "$ESP_IMG" "${OUT_DIR}/.readme.txt" ::README.txt
rm -f "${OUT_DIR}/.readme.txt"
log "README.txt (Secure Boot, AHCI)"

for f in authorized_keys wpa_supplicant.conf firewall.conf beacon.conf; do
    src="${REPO_ROOT}/boot/rootfs-overlay/etc/${f}"
    [[ -f "$src" ]] && mcopy -o -i "$ESP_IMG" "$src" "::${f}" && log "$f"
done

# The command line is compiled into the kernel, but a copy here means a
# person with a card reader can see what it is, and a real bootloader
# would read it.
printf '%s\n' "$KERNEL_CMDLINE" > "${OUT_DIR}/.cmdline.txt"
mcopy -o -i "$ESP_IMG" "${OUT_DIR}/.cmdline.txt" ::cmdline.txt
rm -f "${OUT_DIR}/.cmdline.txt"

# ── 5. assemble ──────────────────────────────────────────────────
step "이미지 조립"
dd if="$ESP_IMG"  of="$IMAGE" bs=1M seek=$(( ESP_START / 2048 )) conv=notrunc,sparse status=none

# GPT, not MBR: the partition types say what each one is ("EFI system",
# "Linux root (x86-64)"), and each partition gets a GUID of its own that
# preinit and the installed system's boot entry can name.
PARTUUIDS="$(python3 "${REPO_ROOT}/tools/write-gpt.py" "$IMAGE" \
    "${ESP_START}:${ESP_SECTORS}:esp:EFI system" \
    "${ROOT_START}:${ROOT_SECTORS}:root:${ROOT_LABEL}")"
while read -r n u; do log "p${n} PARTUUID=${u}"; done <<< "$PARTUUIDS"

rm -f "$ESP_IMG"

step "결과"
log "$(stat -c%s "$IMAGE") bytes  ${IMAGE}"
log ""
log "  GPT"
log "  p1  ${ESP_MB}MiB  FAT32  ${ESP_LABEL}   커널과 설정 (EFI 시스템 파티션)"
log "  p2  나머지  ext4   ${ROOT_LABEL}    루트 파일시스템 (Linux root x86-64)"
log ""
log "굽기:  sudo dd if=${IMAGE} of=/dev/sdX bs=4M conv=fsync status=progress"
log "QEMU:  qemu-system-x86_64 -m 4096 -smp 4 \\"
log "         -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \\"
log "         -drive if=pflash,format=raw,file=vars.fd \\"
log "         -drive file=${IMAGE},format=raw,if=virtio"
