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

if command -v xz >/dev/null 2>&1; then
    echo "==> unpacking (about 13GB, mostly empty space)"
    xz -d -k -f "$NAME"
    echo
    echo "Done: $(pwd)/linux-LP_desktop.img"
else
    echo
    echo "Done: $(pwd)/$NAME"
    echo "To unpack it: install xz (macOS: brew install xz), then"
    echo "    xz -d -k $NAME"
fi
echo
echo "USB stick:  sudo dd if=linux-LP_desktop.img of=/dev/<stick> bs=4M conv=fsync status=progress"
echo "UTM/QEMU:   use linux-LP_desktop.img as a disk (UEFI, x86_64, Q35, 4GB+ RAM)."
echo "            Display: a GPU-accelerated one (UTM: virtio-gpu-gl / \"GPU Supported\";"
echo "            QEMU: -device virtio-vga-gl -display gtk,gl=on) gives window buttons with"
echo "            minimise and animations; without 3D, LP runs its CPU-drawn session."
