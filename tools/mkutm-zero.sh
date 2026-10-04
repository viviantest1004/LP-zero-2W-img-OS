#!/usr/bin/env bash
# mkutm-zero.sh - LP-zero (arm64) as a UTM virtual machine, ready to open:
# UTM on a Mac, and UTM SE on an iPhone or iPad (no JIT there - every
# instruction is interpreted, which is what most of the choices below are
# about).
#
#   tools/mksdcard.sh --linux --uefi-only --no-micropython   the disk
#   tools/mkutm-zero.sh [OUT.zip]                            -> dist/linux-LP_arm64_UTM.zip
#
# The zip holds one bundle, LP-zero.utm:
#
#   config.plist          what UTM reads (ConfigurationVersion 4, the
#                         format of Configuration/UTMQemuConfiguration*.swift
#                         in UTM's source)
#   Data/vmlinuz.efi      the system: kernel and the whole userland, one file
#   Data/lp-zero.qcow2    the disk - the FAT boot partition and /data; 8GB
#                         that takes only what is written (about 45MB)
#
# ── What the machine is, and why ──
#
#   aarch64 "virt", 512MB, one CPU   what the system was made for (a Pi Zero
#                         2 W has 512MB). One CPU because without JIT
#                         QEMU runs every CPU on one host thread in turn:
#                         a second only adds switching.
#   the kernel started by UTM itself (a "Linux kernel" drive), no UEFI
#                         Firmware is the slowest thing to interpret: on an
#                         iPhone it is most of a boot. The disk still has
#                         the kernel as EFI/BOOT/BOOTAA64.EFI, so turning
#                         "UEFI Boot" on and removing the kernel drive is a
#                         machine that boots from its disk instead.
#   no display, a serial "Terminal"   this is a shell; a framebuffer
#                         console would be drawn by the CPU being
#                         interpreted. The kernel's console is ttyAMA0
#                         (CONFIG_CMDLINE). UTM's "fit the window" sends
#                         `stty cols N rows N`, which LP has for this.
#   no USB bus            nothing to plug in; fewer devices to emulate
#   virtio disk, network, and random numbers
#   network "Emulated VLAN" (the only kind UTM has on iOS), with the
#                         host's port 2222 forwarded to SSH on 22
#
# The time comes from virt's PL031 clock (kernel/lp-zero.config), and
# UTM's stop button is the power button, which lp-tune turns into a
# shutdown.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="${IMG:-${REPO_ROOT}/sdcard/lp-zero.img}"
KERNEL="${KERNEL:-${REPO_ROOT}/kernel/out/vmlinuz.efi}"
OUT="${1:-${REPO_ROOT}/dist/linux-LP_arm64_UTM.zip}"
DISK_SIZE="${DISK_SIZE:-8G}"
MEM_MB="${MEM_MB:-512}"
NAME="${VM_NAME:-LP-zero}"
WORK="${WORK:-$(mktemp -d)}"

die() { printf 'mkutm-zero.sh: %s\n' "$*" >&2; exit 1; }
for t in qemu-img zip python3; do
    command -v "$t" >/dev/null || die "$t is needed"
done
[[ -f "$IMG" ]]    || die "no disk image at $IMG (tools/mksdcard.sh --linux --uefi-only --no-micropython)"
[[ -f "$KERNEL" ]] || die "no kernel at $KERNEL (kernel/build.sh)"
python3 - "$KERNEL" <<'PY' || die "$KERNEL is not an arm64 EFI kernel"
import struct, sys
d = open(sys.argv[1], 'rb').read(0x200)
pe = struct.unpack_from('<I', d, 0x3c)[0]
sys.exit(0 if d[:2] == b'MZ' and d[pe:pe+4] == b'PE\0\0' and struct.unpack_from('<H', d, pe + 4)[0] == 0xaa64 else 1)
PY

B="${WORK}/${NAME}.utm"
rm -rf "$B"
mkdir -p "$B/Data"
cp "$KERNEL" "$B/Data/vmlinuz.efi"
# Compressed clusters: the image is mostly a FAT partition and an empty
# ext4, and what the VM writes later is stored plainly as it always is.
qemu-img convert -q -c -f raw -O qcow2 "$IMG" "$B/Data/lp-zero.qcow2"
qemu-img resize -q "$B/Data/lp-zero.qcow2" "$DISK_SIZE"

uuid() { python3 -c 'import uuid; print(str(uuid.uuid4()).upper())'; }
MAC=$(python3 -c 'import os; print("02:" + ":".join("%02x" % b for b in os.urandom(5)))')
cat > "$B/config.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Backend</key>
	<string>QEMU</string>
	<key>ConfigurationVersion</key>
	<integer>4</integer>
	<key>Display</key>
	<array/>
	<key>Drive</key>
	<array>
		<dict>
			<key>Identifier</key>
			<string>$(uuid)</string>
			<key>ImageName</key>
			<string>vmlinuz.efi</string>
			<key>ImageType</key>
			<string>LinuxKernel</string>
			<key>Interface</key>
			<string>None</string>
			<key>InterfaceVersion</key>
			<integer>1</integer>
			<key>ReadOnly</key>
			<true/>
		</dict>
		<dict>
			<key>Identifier</key>
			<string>$(uuid)</string>
			<key>ImageName</key>
			<string>lp-zero.qcow2</string>
			<key>ImageType</key>
			<string>Disk</string>
			<key>Interface</key>
			<string>VirtIO</string>
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
		<string>${NAME}</string>
		<key>Notes</key>
		<string>LP-zero OS (arm64), a shell system in one 13MB file: 512MB of RAM is plenty. apt installs real Debian packages (trixie). Log in is automatic on the serial terminal; SSH is key-only (authkey new), on the host's port 2222.</string>
		<key>UUID</key>
		<string>$(uuid)</string>
	</dict>
	<key>Input</key>
	<dict>
		<key>MaximumUsbShare</key>
		<integer>3</integer>
		<key>UsbBusSupport</key>
		<string>Disabled</string>
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
			<string>${MAC}</string>
			<key>Mode</key>
			<string>Emulated</string>
			<key>PortForward</key>
			<array>
				<dict>
					<key>GuestPort</key>
					<integer>22</integer>
					<key>HostPort</key>
					<integer>2222</integer>
					<key>Protocol</key>
					<string>TCP</string>
				</dict>
			</array>
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
		<false/>
	</dict>
	<key>Serial</key>
	<array>
		<dict>
			<key>Mode</key>
			<string>Terminal</string>
			<key>Target</key>
			<string>Auto</string>
			<key>Terminal</key>
			<dict>
				<key>CursorBlink</key>
				<true/>
				<key>Font</key>
				<string>Menlo</string>
				<key>FontSize</key>
				<integer>12</integer>
				<key>Theme</key>
				<string>Default</string>
			</dict>
		</dict>
	</array>
	<key>Sharing</key>
	<dict>
		<key>ClipboardSharing</key>
		<false/>
		<key>DirectoryShareMode</key>
		<string>None</string>
		<key>DirectoryShareReadOnly</key>
		<false/>
	</dict>
	<key>Sound</key>
	<array/>
	<key>System</key>
	<dict>
		<key>Architecture</key>
		<string>aarch64</string>
		<key>CPU</key>
		<string>default</string>
		<key>CPUCount</key>
		<integer>1</integer>
		<key>CPUFlagsAdd</key>
		<array/>
		<key>CPUFlagsRemove</key>
		<array/>
		<key>ForceMulticore</key>
		<false/>
		<key>JITCacheSize</key>
		<integer>0</integer>
		<key>MemorySize</key>
		<integer>${MEM_MB}</integer>
		<key>Target</key>
		<string>virt</string>
	</dict>
</dict>
</plist>
PLIST
python3 -c "import plistlib,sys; plistlib.load(open(sys.argv[1],'rb'))" "$B/config.plist" \
    || die "config.plist does not parse"

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
( cd "$WORK" && zip -q -r -X "$OUT" "${NAME}.utm" )
echo "mkutm-zero.sh: $OUT ($(du -h "$OUT" | cut -f1))"
sha256sum "$OUT"
