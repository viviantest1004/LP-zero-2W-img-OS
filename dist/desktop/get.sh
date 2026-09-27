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
if [ -f "$IMG" ] && [ "$(wc -c < "$IMG" | tr -d ' ')" -gt 5000000000 ] && [ "$IMG" -nt "$NAME" ]; then
    echo "==> $IMG is already unpacked"
elif command -v xz >/dev/null 2>&1; then
    echo "==> unpacking with xz (about 8GB, mostly empty space)"
    xz -d -k -f "$NAME"
elif command -v python3 >/dev/null 2>&1 && python3 -c "import lzma" 2>/dev/null; then
    echo "==> unpacking with python3 (about 8GB; a few minutes)"
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

# ── On a Mac: a ready UTM virtual machine ────────────────────────────
#
# Setting UTM up by hand is where it went wrong: the image picked as a
# "boot ISO" (it is a disk, and the firmware finds nothing on a CD),
# the .xz given instead of the .img, IDE, no 3D. So on macOS this also
# writes LP.utm, which UTM opens as it is: x86_64 emulated on a q35 PC,
# UEFI, 4 cores running in parallel, 4GB, the 3D display, and two disks:
#
#   NVMe  an empty 16GB disk (qcow2, grows as it fills) - LP installs here
#   USB   the image, as the stick it would be on a real PC
#
# The empty disk is first in the boot order: until something is
# installed on it the firmware passes it by and starts the stick, and
# after that it starts LP from the disk without anybody changing a thing.
if [ "$(uname -s)" = Darwin ]; then
    VM=LP.utm
    if [ -e "$VM" ]; then
        echo "==> $VM is already here - left as it is (delete it for a fresh one)"
    else
        echo "==> making $VM for UTM"
        mkdir -p "$VM/Data"
        # A clone on APFS: instant, and no second 8GB until one changes.
        cp -c "$IMG" "$VM/Data/$IMG" 2>/dev/null || cp "$IMG" "$VM/Data/$IMG"
        # The empty 16GB qcow2 disk (qemu-img create -f qcow2 ... 16G),
        # gzip'd: 196KB of header and zeros.
        printf '%s' 'H4sIAAAAAAACA+3QwUoDMRAA0GzrB/gJ+QY/QhC8ePK8dlNcaHeXbIquJz9b8GIrFtpLW899LyQQmBlm5un+4TuEMA/Hbrf35uAff999VHXwnrerM+w/r/Ov523qZwhNm8sUX9pyWZVq0ee8GcqlGbP0XlLu6lVs6lLHZbtKJ+Pni3495DSObd/FMg3p3FS7+l2Tmvh4F1NXcpvGU+2HVf0xxZyWi37TlfFs+2E757oexguXPKty/RaPZg4AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAD/NrMCAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACAq1f9HQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAALhiP9LWuLgAAQMA' | base64 --decode | gunzip > "$VM/Data/lp-disk.qcow2"
        uuid() { uuidgen 2>/dev/null || cat /proc/sys/kernel/random/uuid; }
        u() { uuid | tr 'a-f' 'A-F'; }
        mac=$(uuid | tr -d '-' | tr 'A-F' 'a-f' | cut -c1-10 | sed 's/\(..\)/\1:/g; s/:$//')
        cat > "$VM/config.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Backend</key>
	<string>QEMU</string>
	<key>ConfigurationVersion</key>
	<integer>4</integer>
	<key>Display</key>
	<array>
		<dict>
			<key>DownscalingFilter</key>
			<string>Linear</string>
			<key>DynamicResolution</key>
			<true/>
			<key>Hardware</key>
			<string>virtio-vga-gl</string>
			<key>NativeResolution</key>
			<false/>
			<key>UpscalingFilter</key>
			<string>Nearest</string>
		</dict>
	</array>
	<key>Drive</key>
	<array>
		<dict>
			<key>Identifier</key>
			<string>$(u)</string>
			<key>ImageName</key>
			<string>lp-disk.qcow2</string>
			<key>ImageType</key>
			<string>Disk</string>
			<key>Interface</key>
			<string>NVMe</string>
			<key>InterfaceVersion</key>
			<integer>1</integer>
			<key>ReadOnly</key>
			<false/>
		</dict>
		<dict>
			<key>Identifier</key>
			<string>$(u)</string>
			<key>ImageName</key>
			<string>$IMG</string>
			<key>ImageType</key>
			<string>Disk</string>
			<key>Interface</key>
			<string>USB</string>
			<key>InterfaceVersion</key>
			<integer>1</integer>
			<key>ReadOnly</key>
			<false/>
		</dict>
	</array>
	<key>Information</key>
	<dict>
		<key>Icon</key>
		<string>linux</string>
		<key>IconCustom</key>
		<false/>
		<key>Name</key>
		<string>LP</string>
		<key>UUID</key>
		<string>$(u)</string>
	</dict>
	<key>Input</key>
	<dict>
		<key>MaximumUsbShare</key>
		<integer>3</integer>
		<key>UsbBusSupport</key>
		<string>3.0</string>
		<key>UsbSharing</key>
		<false/>
	</dict>
	<key>Network</key>
	<array>
		<dict>
			<key>Hardware</key>
			<string>virtio-net-pci</string>
			<key>IsolateFromHost</key>
			<false/>
			<key>MacAddress</key>
			<string>02:$mac</string>
			<key>Mode</key>
			<string>Shared</string>
			<key>PortForward</key>
			<array/>
		</dict>
	</array>
	<key>QEMU</key>
	<dict>
		<key>AdditionalArguments</key>
		<array/>
		<key>BalloonDevice</key>
		<false/>
		<key>DebugLog</key>
		<false/>
		<key>Hypervisor</key>
		<false/>
		<key>PS2Controller</key>
		<false/>
		<key>RNGDevice</key>
		<true/>
		<key>RTCLocalTime</key>
		<false/>
		<key>TPMDevice</key>
		<false/>
		<key>TSO</key>
		<false/>
		<key>UEFIBoot</key>
		<true/>
	</dict>
	<key>Serial</key>
	<array/>
	<key>Sharing</key>
	<dict>
		<key>ClipboardSharing</key>
		<true/>
		<key>DirectoryShareMode</key>
		<string>None</string>
		<key>DirectoryShareReadOnly</key>
		<false/>
	</dict>
	<key>Sound</key>
	<array>
		<dict>
			<key>Hardware</key>
			<string>intel-hda</string>
		</dict>
	</array>
	<key>System</key>
	<dict>
		<key>Architecture</key>
		<string>x86_64</string>
		<key>CPU</key>
		<string>default</string>
		<key>CPUCount</key>
		<integer>4</integer>
		<key>CPUFlagsAdd</key>
		<array/>
		<key>CPUFlagsRemove</key>
		<array/>
		<key>ForceMulticore</key>
		<true/>
		<key>JITCacheSize</key>
		<integer>0</integer>
		<key>MemorySize</key>
		<integer>4096</integer>
		<key>Target</key>
		<string>q35</string>
	</dict>
</dict>
</plist>
PLIST
    fi
    echo
    echo "UTM: double-click $(pwd)/$VM (or: open \"$(pwd)/$VM\"), then press play."
    echo "     The installer starts from the USB stick; install onto the 16GB NVMe"
    echo "     disk, and the next start boots LP from it by itself."
    open "$VM" 2>/dev/null || true
    exit 0
fi

echo
echo "USB stick:  sudo dd if=linux-LP_desktop.img of=/dev/<stick> bs=4M conv=fsync status=progress"
echo "QEMU:       UEFI, x86_64, q35, 2+ cores, 4GB+ RAM; the image as a USB disk and an"
echo "            empty disk of 16GB or more (NVMe) to install LP onto."
echo "            -device virtio-vga-gl -display gtk,gl=on gives window buttons with"
echo "            minimise and animations; without 3D, LP runs its CPU-drawn session."
