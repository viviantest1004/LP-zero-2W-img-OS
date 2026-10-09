#!/bin/sh
# nouveau-gate.sh - desktop/graphics/rootfs/usr/lib/lp/nouveau-gate against
# fake /sys trees: who gets nouveau, and that the GPU left without it is
# set to switch itself off.
#
#   sh tests/nouveau-gate.sh        (needs dash; prints PASS/FAIL lines)
set -u
GATE=$(cd "$(dirname "$0")/.." && pwd)/desktop/graphics/rootfs/usr/lib/lp/nouveau-gate
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fails=0

dev() {     # dev ROOT ADDR VENDOR CLASS [BOOT_VGA]
    d=$1/bus/pci/devices/$2
    mkdir -p "$d/power"
    echo "$3" > "$d/vendor"
    echo "$4" > "$d/class"
    echo on > "$d/power/control"
    [ -n "${5:-}" ] && echo "$5" > "$d/boot_vga"
    return 0
}

run() {     # run ROOT [CONF] -> prints "LOADED" if modprobe was called
    echo "${2:-}" > "$T/conf"
    [ -n "${2:-}" ] || rm -f "$T/conf"
    : > "$T/kmsg"
    LP_SYSFS=$1 LP_NOUVEAU_CONF=$T/conf LP_MODPROBE="echo LOADED" LP_KMSG=$T/kmsg \
        dash "$GATE"
}

check() {   # check NAME GOT WANT
    if [ "$2" = "$3" ]; then echo "PASS  $1"; else echo "FAIL  $1: got '$2', wanted '$3'"; fails=$((fails + 1)); fi
}

# The XPS 15 9550: Intel draws the screen, the GTX 960M is a 3D controller.
X=$T/xps
dev "$X" 0000:00:02.0 0x8086 0x030000 1
dev "$X" 0000:01:00.0 0x10de 0x030200
dev "$X" 0000:00:1f.3 0x8086 0x040300
check "Optimus laptop: no nouveau" "$(run "$X")" ""
check "  and the GPU may switch off" "$(cat "$X/bus/pci/devices/0000:01:00.0/power/control")" auto
check "  and nothing else is touched" "$(cat "$X/bus/pci/devices/0000:00:1f.3/power/control")" on
check "  and the log says why" "$(grep -c 'nouveau not loaded for 0000:01:00.0' "$T/kmsg")" 1
check "Optimus laptop, /etc/lp/nouveau on: nouveau" "$(run "$X" on)" "LOADED --ignore-install nouveau"

# A desktop whose only card is NVIDIA, the boot display.
D=$T/desk
dev "$D" 0000:01:00.0 0x10de 0x030000 1
check "NVIDIA draws the screen: nouveau" "$(run "$D")" "LOADED --ignore-install nouveau"
check "  unless /etc/lp/nouveau says off" "$(run "$D" off)" ""

# No boot_vga anywhere (firmware that does not say): the only GPU loads.
N=$T/none
dev "$N" 0000:01:00.0 0x10de 0x030000
check "only GPU, no boot_vga: nouveau" "$(run "$N")" "LOADED --ignore-install nouveau"

# Options from the kernel command line reach the real modprobe.
check "options passed through" "$(echo on > "$T/conf"; LP_SYSFS=$X LP_NOUVEAU_CONF=$T/conf LP_MODPROBE='echo LOADED' LP_KMSG=$T/kmsg dash "$GATE" runpm=0)" \
      "LOADED --ignore-install nouveau runpm=0"

[ "$fails" = 0 ]
