#!/bin/sh
# get.sh - download the LP desktop image and put it back together.
#
#   curl -fsSL https://raw.githubusercontent.com/viviantest1004/LP-zero-2W-img-OS/refs/heads/claude/hohho-xvzof5/dist/desktop/get.sh | sh
#
# The image is about 2GB compressed, over GitHub's 100MB limit for one file,
# so it is kept here as 45MB pieces. This fetches every piece (resuming
# any that were cut off), checks each against SHA256SUMS, joins them into
# linux-LP_desktop.img.xz, checks that too, and unpacks it to
# linux-LP_desktop.img when xz is installed. Run it again after an
# interruption and it carries on where it stopped.
#
# Works on macOS and Linux: curl, and shasum or sha256sum.
set -eu

BASE="${LP_BASE:-https://raw.githubusercontent.com/viviantest1004/LP-zero-2W-img-OS/refs/heads/claude/hohho-xvzof5/dist/desktop}"
NAME="linux-LP_desktop.img.xz"
DIR="${LP_DIR:-LP-desktop-image}"

if command -v sha256sum >/dev/null 2>&1; then
    sha() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
    sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
    echo "get.sh: need sha256sum or shasum" >&2
    exit 1
fi

mkdir -p "$DIR"
cd "$DIR"
echo "==> into $(pwd)"

curl -fsSL --retry 5 -o SHA256SUMS "$BASE/SHA256SUMS"
PARTS=$(grep -c "\.part[0-9][0-9]\$" SHA256SUMS)
want=$(grep " $NAME\$" SHA256SUMS | cut -d' ' -f1)

if [ -f "$NAME" ] && [ "$(sha "$NAME")" = "$want" ]; then
    echo "==> $NAME is already here and complete"
else
echo "==> $PARTS pieces"

grep "\.part[0-9][0-9]\$" SHA256SUMS | while read -r want part; do
    if [ -f "$part" ] && [ "$(sha "$part")" = "$want" ]; then
        echo "    $part  ok (already here)"
        continue
    fi
    tries=0
    while :; do
        tries=$((tries + 1))
        # -C - resumes a piece that was cut off half way.
        curl -fL --retry 5 --retry-delay 2 -C - -s -o "$part" "$BASE/$part" || true
        if [ "$(sha "$part")" = "$want" ]; then
            echo "    $part  ok"
            break
        fi
        rm -f "$part"
        if [ "$tries" -ge 3 ]; then
            echo "get.sh: $part does not match SHA256SUMS after 3 tries" >&2
            exit 1
        fi
        echo "    $part  damaged, fetching again"
    done
done

echo "==> joining"
cat $(grep "\.part[0-9][0-9]\$" SHA256SUMS | awk '{print $2}') > "$NAME"
if [ "$(sha "$NAME")" != "$want" ]; then
    echo "get.sh: the joined $NAME does not match SHA256SUMS" >&2
    exit 1
fi
echo "    $NAME  ok ($want)"
rm -f $(grep "\.part[0-9][0-9]\$" SHA256SUMS | awk '{print $2}')
fi

# Unpacked, because a VM (UTM) or dd needs the raw disk, not the .xz: given
# the .xz the firmware finds no partitions and drops to the UEFI shell.
# macOS has no xz; its python3 has lzma, so that is the second way.
IMG=linux-LP_desktop.img
if [ -f "$IMG" ] && [ "$(wc -c < "$IMG" | tr -d ' ')" -gt 10000000000 ] && [ "$IMG" -nt "$NAME" ]; then
    echo "==> $IMG is already unpacked"
elif command -v xz >/dev/null 2>&1; then
    echo "==> unpacking with xz (about 13GB, mostly empty space)"
    xz -d -k -f "$NAME"
elif command -v python3 >/dev/null 2>&1 && python3 -c "import lzma" 2>/dev/null; then
    echo "==> unpacking with python3 (about 13GB, mostly empty space; a few minutes)"
    python3 - "$NAME" "$IMG" <<'PY'
import lzma, shutil, sys
with lzma.open(sys.argv[1]) as src, open(sys.argv[2] + ".part", "wb") as dst:
    shutil.copyfileobj(src, dst, 16 << 20)
PY
    mv -f "$IMG.part" "$IMG"
else
    echo
    echo "Done: $(pwd)/$NAME  (still compressed)"
    echo "Unpack it before using it: brew install xz && xz -d -k $NAME"
    exit 0
fi
echo
echo "Done: $(pwd)/$IMG  - use THIS file as the VM's disk, not the .xz"
echo
echo "USB stick:  sudo dd if=linux-LP_desktop.img of=/dev/<stick> bs=4M conv=fsync status=progress"
echo "UTM/QEMU:   use linux-LP_desktop.img as a disk (UEFI, x86_64, Q35, 4GB+ RAM)."
echo "            Display: a GPU-accelerated one (UTM: virtio-gpu-gl / \"GPU Supported\";"
echo "            QEMU: -device virtio-vga-gl -display gtk,gl=on) gives window buttons with"
echo "            minimise and animations; without 3D, LP runs its CPU-drawn session."
