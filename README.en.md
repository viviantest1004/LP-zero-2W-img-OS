# LP-zero / linux-LP

**한국어 문서: [README.md](README.md)** · Start here: **[`GUIDE/`](GUIDE/)**

**A Linux distribution written from scratch.** The kernel configuration,
the C library, init, the shell and all 107 commands are original code.
One kernel image is the entire system: the userland is packed inside it
and unpacks into RAM at boot.

It began as an attempt to get something useful out of a single
**Raspberry Pi Zero 2 W**. Put Ubuntu on 512MB of RAM and half of it is
gone before you have done anything. Nothing about the result turned out
to be tied to that board, so it now runs just as well on **arm64 virtual
machines and amd64 PCs**.

| | |
|---|---|
| Whole system | kernel + userland in **one file**, 11-23MB |
| RAM left after boot | **480MB** free on a 512MB board |
| Boot time | about 10 seconds from power to a prompt |
| Commands | 107, every one of them written here |
| SSH | **built in**, public key only (password auth is not compiled in) |
| Python | CPython 3.12 + pip, manylinux wheels install (numpy confirmed) |
| Firewall | on by default, nftables driven straight over netlink |
| Licence | MIT |

Four things come from outside: the Broadcom blob the Raspberry Pi GPU
boot ROM insists on, and three cryptography implementations (dropbear,
wpa_supplicant, OpenSSL). **Cryptography is the one thing you should not
write yourself**, so it was deliberately borrowed.

---

## Who wrote this

**Every line of it was written by [Claude Code](https://claude.com/claude-code),
Anthropic's coding agent**, working from the repository owner's
direction. That includes the kernel configuration, the C library, init,
the shell, all 107 commands, the build system, the self-test, and this
document.

It is worth being plain about what that means. The code was designed,
debugged and revised in the open — the commit log reads as an
explanation of why each change was made, and the comments say what was
tried and what broke. It was also not reviewed by an independent
engineer, has never run in production, and carries no track record.
Read [Before you rely on this](#before-you-rely-on-this) before you put
it anywhere that matters.

---

## What is on which branch

| Branch | What it holds |
|---|---|
| **`main`** | Everything. Full source, `GUIDE/`, both READMEs, and prebuilt images in `dist/`. This is the branch you want. |
| **`dev`** | The development branch. The same files as `main` right now — it exists to carry the commit history, where each change is explained at length. |

New work goes on a branch, gets built, passes `tests/selftest.sh`, and
is then merged into `main`.

`GUIDE/BRANCHES.txt` says the same in more detail, in English and
Korean.

---

## Before you rely on this

An honest list. None of it is a reason not to try the system; all of it
is a reason not to be surprised.

- **It is a hobby operating system, not a supported product.** There is
  no security team, no CVE process and nobody on call. For anything
  that has to keep working, use Raspberry Pi OS Lite or Debian.
- **Real hardware is not verified.** Everything here was tested in
  QEMU. On a physical Pi Zero 2 W, the WiFi association, the thermal
  sensors and the hardware watchdog have not yet been confirmed. Treat
  the arm64 image as untested on metal.
- **The security work has not been audited.** Update signature
  checking, the firewall, the DNS and NTP hardening are all implemented
  and all reasoned about in the comments, and none of it has been
  reviewed by anybody else. Do not put this straight onto a hostile
  network on the strength of that section alone.
- **The C library is ours, and it is not complete.** It covers what the
  107 commands need. Anything else you compile against it may hit a
  function that is not there. Ordinary Linux binaries run through
  `run`, against the glibc that ships alongside Python.
- **`/data` is the only place that survives a reboot,** and it is one
  partition on one card. Cards die. Back up anything you care about.
- **`dd` to the wrong device destroys that device.** Check `/dev/sdX`
  twice. This is the one mistake here that cannot be undone by
  rebooting.
- **No GUI, a small package set, no containers.** The DRM drivers are
  in the kernel, but X11 or Wayland is yours to install; packages are
  what `pkg` and pip can reach; cgroups and namespaces are not
  configured for container runtimes.
- **The bootloader does not apply device tree overlays.** On real
  hardware this is invisible: start.elf hands us a tree with them
  already applied and we prefer that one. Where there is no start.elf,
  `config.txt` overlays are skipped and the loader says which.
- **Things will be rough in places.** A recent example: every message
  logged through our own logger was invisible to `dmesg` for months,
  because a kmsg record without a trailing newline is a continuation
  the kernel never finalises. It was found by running the self-test end
  to end and asking why one check failed. Expect more of that kind.

What *is* verified: the amd64 image passes all 51 checks in
`tests/selftest.sh` on real QEMU boots — boot, storage, SSH, Python,
graphics, security, error paths, redirection, logging, timing, and the
watchdog noticing a process pinning a core.

---

## Which image do I want

| Image | Where it runs |
|---|---|
| `dist/test_a_123_LPzero2W_linux.img.xz` | **Raspberry Pi Zero 2 W hardware and arm64 virtual machines.** Burn it to a card, or attach it in UTM/QEMU |
| `dist/linux-LP_amd64.img.xz` | **Ordinary PCs and desktop virtual machines.** VMware, VirtualBox, QEMU/KVM, Hyper-V |
| `dist/test_a_123_LPzero2W_linux-utm.zip` | arm64 virtual machines only. Smaller, because a VM has no use for the GPU firmware |

One arm64 image covers both the board and a VM, because it carries the
uncompressed kernel the Raspberry Pi GPU reads **and** the EFI
executable UEFI reads.

The amd64 image calls itself **`linux-LP`** on the inside. It is not a
Raspberry Pi and should not claim to be.

Verify what you downloaded:

```bash
cd dist && sha256sum -c SHA256SUMS.txt
```

---

## Minimum requirements

| | Minimum | Comfortable |
|---|---|---|
| RAM | **64MB** | 256MB or more |
| Storage | **256MB** | 4GB or more (1GB if you want Python) |
| CPU | arm64 (ARMv8) or x86-64 | any number of cores |
| Display | not needed — serial and SSH do everything | |

64MB is not an exaggeration: the kernel and userland together use about
30MB just after boot. Without Python, 256MB of storage is genuinely
enough.

---

## SSH, which is already running

The SSH server (dropbear) is **built in and starts on its own.** There
is nothing to install and nothing to enable. What there is not, is
password login — **it is not compiled in at all.** On a board facing the
internet a password is only a matter of time, so it was never offered as
an option. Public keys only.

### 1. Put your key in

The **first partition of the card (or image) is FAT32**, so it opens on
Windows, macOS and Linux without ceremony. Put your public key in a file
called `authorized_keys` there.

```bash
# on the boot partition that appears when you insert the card
cat ~/.ssh/id_ed25519.pub >> /media/BOOT/authorized_keys
```

No key yet? `ssh-keygen -t ed25519`

> While `/boot/authorized_keys` exists, **it is the master copy** and it
> overwrites the device's own at every boot. Edit it on the device and
> the change is gone next time. Delete it from `/boot` if you would
> rather manage keys on the machine. The reason it works this way: if
> `/data` is destroyed, you can still get in by pulling the card and
> editing one file.

### 2. Find the address

```
# on the device's screen or serial console
ifconfig
```

DHCP is automatic. For a fixed address, write `network.conf` on the boot
partition.

### 3. Connect

```bash
ssh root@192.168.0.42
```

There is **one user, root.** Port 22, which the firewall opens by
default.

### From a virtual machine

Forward the port in QEMU:

```bash
-netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0
```
```bash
ssh -p 2222 root@localhost
```

### When it will not connect

```
authkey -l           # which keys are accepted right now
service status dropbear
firewall status
cat /data/log/auth   # login records
```

---

## Using it

### Burning a card (Raspberry Pi)

```bash
xz -d < dist/test_a_123_LPzero2W_linux.img.xz \
  | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
```

**Check `/dev/sdX`.** Get it wrong and that disk is gone.

On the first boot, the `/data` partition grows to fill the card by
itself.

### Virtual machines (UTM / QEMU / VMware / VirtualBox)

Decompress it, attach it as a disk, and set the machine to **UEFI
boot** — the MBR bootstrap is empty on purpose, so legacy BIOS will not
start it.

```bash
# QEMU, arm64
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 4096 -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=vars.fd \
  -drive file=lp-zero.img,format=raw,if=virtio \
  -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0

# QEMU, amd64
qemu-system-x86_64 -m 4096 -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=vars.fd \
  -drive file=linux-LP_amd64.img,format=raw,if=virtio
```

To make the disk bigger, `truncate -s 16G` the image file. It is sparse,
so it only takes up what is written, and `/data` expands on the first
boot.

### WiFi (Raspberry Pi)

Write it on the boot partition, in `wpa_supplicant.conf`:

```
network={
    ssid="my network"
    psk="the password"
}
```

---

## Why it does not fall over

This is the part the system was actually built around. Three designs
stack on top of each other.

### 1. The root filesystem lives in RAM

The whole system is a cpio inside the kernel image, unpacked into RAM at
boot. **Nothing on disk is part of the running system.**

- Pull the power at any moment and there is nothing to corrupt. The next
  boot is always the factory state.
- Delete something you should not have, and a reboot puts it back.
- An update replaces one file, and a failed one goes back to the
  previous file.

Losing whatever you installed into `/bin` would be no fun, so `/bin`,
`/lib`, `/usr`, `/opt` and friends are **overlays**: reads see the
image, writes land on `/data`. `cp mytool /bin/` survives a reboot.

### 2. `guard`, the watchdog daemon

It keeps an eye on memory, temperature, voltage, CPU and disk.

- **Out of memory**: below the reserve, it clears the largest processes
  first. init, the shell, SSH and the watchdog are never touched.
- **Fork bomb**: killed by process group, all at once. A measured
  **2937 processes cleared in a single pass**. Your logged-in shell
  survives.
- **CPU runaway**: a warning at 10 seconds, a priority demotion at 30.
  It does not kill — you may have meant it.
- **Overheating and undervoltage**: it reads the Raspberry Pi throttle
  bits and says so.
- **Disk full**: logs go first.

init watches guard in turn. Kill it and it is **back within a second.**

### 3. There is always a way back

- **Watchdog**: a hardware timer. If the kernel stops, the board
  restarts itself.
- **`bootcount`**: five boots in a row that fail to last five minutes,
  and `/data/rc.local` is skipped. That is the way out when a user
  script is what stops the machine booting.
- **Update rollback**: a new system that cannot survive five minutes
  reverts to the previous image automatically.
- **SSH keys in three places**: inside the image, on the FAT boot
  partition (editable anywhere), and on `/data`. Lose `/data` entirely
  and the card still lets you in.
- **`fsck`**: check and repair `/data` from the machine itself.
- **A real shutdown**: `poweroff` stops the services, flushes to disk
  and **unmounts `/data`** before cutting power, so the next boot has no
  journal to replay.

---

## Plugging a drive in

A USB stick or an external disk mounts itself. automount listens to the
kernel's uevent messages, so a drive appears under `/media/<name>`
within a moment of being plugged in and is released when it is pulled.
It also scans once at startup for drives that were already there.

```
automount -l            what is mounted right now
automount -u <name>     release one
```

It never touches the disk this system booted from, and it mounts
nosuid,nodev for the same reason /data does: that filesystem was written
by somebody else's machine.

### Drives you want to keep

`/media` is a temporary place. For a drive that should be at the same
path after every reboot, adopt it:

```
storage                        what survives a reboot, and how full
storage adopt /dev/sdb photos  keep it at /mnt/photos
storage format /dev/sdb backup erase it, make ext4, then keep it
storage forget photos          stop keeping it
```

An adopted drive is matched by **filesystem label, not device name**, so
it works in any port. sdb1 becomes sdc1 the moment somebody plugs in a
second drive first, and a system that loses its storage over the order
things were plugged in is not one you can leave alone for months.

```
storage
  what survives a reboot
    boot       /boot            110M free of 126M     12% used   /dev/sda1
    data       /data             63M free of 112M     43% used   /dev/sda2
    photos     /mnt/photos       51M free of 55M       8% used   /dev/sdb
```

`poweroff` releases these before it unmounts /data, detaching anything
busy - a drive nobody can unmount must not become a machine nobody can
switch off.

---

## How much of the boot chain is ours

```
boot ROM -> bootcode.bin -> start.elf -> our firmware -> Linux
              (Broadcom blobs)           (firmware/)     (kernel/)
```

The bare-metal firmware in `firmware/` reads the SD card itself and
loads Linux: our own EMMC driver (Arasan SDHCI), MBR parsing, FAT32
with long filenames, device tree rewriting, and the handover per the
arm64 boot protocol.

start.elf can load a kernel by itself, so this step does not add a
capability - it replaces a link in the chain with our own. When
something goes wrong, the exception vectors catch it and say what and
where: the exception class, the address being touched, whether it was
being read or written, and the call path that led there.

The two Broadcom blobs cannot be replaced: the GPU boot ROM demands
them before any ARM core runs at all.

---

## Security

- **No password authentication.** dropbear is compiled without it.
- **Signed updates.** RSA-2048 / PKCS#1 v1.5 / SHA-256. The public key
  lives inside the kernel image, so pulling the card does not let you
  change it. A bad signature refuses the install, and **there is no
  override.**
- **Firewall on by default.** nftables rules go in over netlink
  directly, in a single transaction, so there is no moment where half
  the rules are live.
- **DNS spoofing defences.** Transaction IDs from `getrandom`, a
  connected socket, and the question section checked on the way back.
- **NTP spoofing defences.** Nonce, mode and stratum are all verified.
- **`/data` is mounted nosuid,nodev.** Planting a setuid file by putting
  the card in another PC achieves nothing.
- **`integrity`**: hashes the files that survive a reboot and tells you
  if one changed.
- **Login records** in `/data/log/auth`.

None of this has been independently audited. See
[Before you rely on this](#before-you-rely-on-this).

---

## Who it suits

### A good fit

- **Anyone running something around the clock on a Pi Zero or Zero 2 W.**
  480MB of RAM stays free. A temperature logger, home automation or
  sensor collection does not need Ubuntu under it.
- **Devices that have to run untouched for a long time.** Unreliable
  power, or somewhere physically awkward to reach — everything in "why
  it does not fall over" exists for exactly that.
- **Anyone who wants to see how Linux actually works.** From kernel
  configuration to the shell, it is small enough to read, and the
  comments explain why things are the way they are.
- **Anyone who needs a minimal environment.** The attack surface is
  small.

### A bad fit

- **A desktop.** There is no GUI. The DRM drivers are there, but X11 and
  Wayland are yours to install.
- **Anyone who needs a lot of packages.** There is no apt or dnf behind
  it. You live within what `pkg` and pip can do.
- **Multi-user setups.** You can create users, but the design assumes
  one root.
- **Work that needs a proven distribution.** This has not been tested
  the way Debian or Ubuntu has. Use Raspberry Pi OS Lite there.
- **Containers.** cgroups and namespaces are not enabled for that.

---

## Every command

`help` prints this list on the machine. `help <command>` explains one.

### Shell (16)

| | |
|---|---|
| `cd` | change directory |
| `pwd` | print the current directory |
| `echo` | print the arguments |
| `env` | list environment variables |
| `exit` | leave the shell |
| `reboot` | restart the machine |
| `poweroff` | shut the machine down |
| `help` | this list |
| `test` | ask about a file or a string |
| `true` | succeed |
| `false` | fail |
| `if` | branch on a command's result |
| `while` | repeat while a command works |
| `for` | repeat over a list |
| `break` | leave a loop |
| `continue` | go to the next round of a loop |

### Files and storage (25)

| | |
|---|---|
| `ls` | list a directory |
| `cp` | copy files |
| `mv` | move or rename |
| `rm` | delete files |
| `mkdir` | create directories |
| `touch` | create an empty file |
| `mount` | mount a filesystem |
| `umount` | unmount a filesystem |
| `expandfs` | grow /data to fill the card |
| `disk` | what storage is attached |
| `part` | change the partition table |
| `datadisk` | choose which partition is /data |
| `storage` | what survives a reboot, and adding to it |
| `automount` | mount drives as they are plugged in |
| `fsck` | check and repair /data |
| `tar` | make and open archives |
| `find` | walk a directory tree |
| `du` | how much space it takes |
| `chmod` | change what may be done |
| `chown` | change who owns a file |
| `chgrp` | change the group |
| `ln` | another name for a file |
| `stat` | what a file is |
| `chattr` | flags root has to undo first |
| `lsattr` | show those flags |

### Text (11)

| | |
|---|---|
| `cat` | print a file |
| `edit` | edit a file on screen |
| `more` | read it a screen at a time |
| `grep` | print the lines that match |
| `head` | the first lines |
| `tail` | the last lines |
| `wc` | count lines, words, characters |
| `sort` | put lines in order |
| `uniq` | collapse repeated lines |
| `cut` | take columns out of lines |
| `tee` | write to a file and pass on |

### System (36)

| | |
|---|---|
| `top` | what is running, and stop it |
| `ps` | what is running, once |
| `df` | how full each filesystem is |
| `free` | how much memory is left |
| `usage` | memory and disk at a glance |
| `clear` | wipe the screen |
| `reset` | put the terminal back together |
| `run` | run an ordinary Linux binary |
| `dropprivs` | run something as not-root |
| `uname` | what this system is |
| `hostname` | what this machine calls itself |
| `uptime` | how long it has run, and load |
| `whoami` | which user this is |
| `id` | user and group, by number and name |
| `groups` | which group |
| `useradd` | make a user |
| `userdel` | remove one |
| `su` | run something as another user |
| `sudo` | run something as root |
| `service` | what init keeps alive |
| `sha256sum` | the checksum of a file |
| `integrity` | has anything persistent changed |
| `kill` | stop a process, by pid or name |
| `sleep` | wait |
| `watchdog` | reboot the board if it hangs |
| `logd` | collect logs to /data/log |
| `dmesg` | the kernel's own log |
| `sysinfo` | memory, CPU, disks, network |
| `zram` | compressed swap in RAM |
| `guard` | the safety net (memory, heat, power, CPU) |
| `bootcount` | detect a reboot loop |
| `beacon` | report how the board is doing |
| `calc` | integer calculator |
| `pkg` | install and remove packages |
| `update` | replace the system, reversibly |
| `splash` | draw the boot screen |

### Network (14)

| | |
|---|---|
| `dhcp` | get an address, and keep it |
| `ipconfig` | a fixed address from a file |
| `net` | set it up, and say where it broke |
| `ping` | is it there, and how far |
| `ifconfig` | look at or set an interface |
| `route` | where packets go |
| `nslookup` | what address a name has |
| `wget` | download a file |
| `wpa_supplicant` | join a WiFi network |
| `wpa_cli` | talk to wpa_supplicant |
| `dropbear` | the SSH server |
| `dropbearkey` | make an SSH host key |
| `authkey` | keep a way in over SSH |
| `firewall` | which ports are open |

### Python (3)

| | |
|---|---|
| `python` | CPython 3.12 |
| `python3` | the same as python |
| `micropython` | MicroPython - small, fast |

### Time (2)

| | |
|---|---|
| `date` | show or set the clock |
| `ntp` | set the clock from the net |

---

## Building it yourself

### What you need

```bash
sudo apt install clang lld llvm gcc-aarch64-linux-gnu \
     mtools dosfstools e2fsprogs xz-utils zip cpio bc \
     bsdextrautils qemu-system-arm qemu-system-x86 python3
```

arm64 cross-builds from an x86 host. amd64 is native, so no cross
compiler is needed.

### arm64 (Raspberry Pi and arm64 VMs)

```bash
./tools/fetch-kernel.sh      # Linux source
./tools/fetch-blobs.sh       # Raspberry Pi GPU firmware
make                         # userland
make kernel                  # kernel + initramfs
make sdcard-linux            # sdcard/lp-zero.img
```

### amd64 (PC)

```bash
make ARCH=amd64
( cd userland && LP_ARCH=amd64 LP_BINDIR=bin-amd64 \
    LP_ROOTFS_DIR=rootfs-amd64 LP_CPIO_NAME=initramfs-amd64.cpio.gz \
    LP_HOSTNAME=linux-lp LP_OS_NAME=linux-LP ./mkrootfs.sh )
LP_ARCH=amd64 LP_ROOTFS_DIR=rootfs-amd64 ./kernel/build.sh
LP_ARCH=amd64 LP_ROOTFS_DIR=rootfs-amd64 ./tools/mksdcard.sh --linux --uefi-only
```

### All three distribution images at once

```bash
./tools/mkdist.sh            # everything, into dist/
```

Build products land in `.build/` beside the repository. Move them with
`LPZERO_WORK=/mnt/big/lpzero ./tools/mkdist.sh` if the disk is tight.

### The update signing key

```bash
./tools/sign-release.sh --new-key    # creates a key in keys/, once
make                                  # the public key goes into the image
./tools/sign-release.sh kernel/out/Image
```

**Never put the private key on a device.** `keys/` is in `.gitignore`.

### Checking a build

`tests/selftest.sh` runs on the machine and prints one PASS or FAIL per
check.

```bash
sh /path/to/selftest.sh
```

---

## Why the source is not split by architecture

There are no `arm64/` and `amd64/` folders, because both architectures
build from **the same source**. What actually differs:

| | |
|---|---|
| `userland/libc/include/syscall-arm64.h` / `-x86_64.h` | syscall numbers and calling convention |
| `userland/libc/src/crt0.S` | the entry point, split by `#if` |
| `kernel/lp-zero.config` / `lp-zero-amd64.config` | kernel configuration |

The 107 commands and the body of libc are **identical, character for
character.** Split them into copied folders and sooner or later only one
copy gets fixed — which is not hypothetical: an amd64 image once shipped
with 43MB of arm64 binaries in it. `check_tree_arch` now compares the
architecture of every ELF at build time and **stops the build** on a
mismatch.

```
userland/          one set of sources; make ARCH=amd64 picks the target
  libc/            our own libc, straight on top of the syscalls
  init/ sh/ ...    107 commands, one directory each
kernel/            kernel configuration and build script
boot/              what goes on the boot partition (config.txt, /etc/rc)
tools/             image building, signing, distribution
tests/             the self-test
GUIDE/             usage and branch map, in English and Korean
dist/              finished images
```

---

## Details

### Partitions

| | |
|---|---|
| p1 | FAT32, 128MB. Kernel, `config.txt`, `authorized_keys`, WiFi and firewall settings. **Editable on any PC** |
| p2 | ext4, all the rest, mounted as `/data`. Expands to fill the card on the first boot |

### What survives a reboot

| Path | After a reboot |
|---|---|
| `/data` | kept |
| `/root` | kept (bound to `/data/root`) |
| `/bin` `/lib` `/usr` `/opt` `/sbin` `/srv` | kept (overlay on `/data`) |
| `/etc` `/tmp` `/var` | **gone**, deliberately |

`/etc` is left out on purpose: `/etc/rc` and `/etc/services` are read
**before** `/data` is mounted. A stale copy shadowing a system update is
a board that does not come up.

### Python

`/data/python/bin/python3.12`, reachable as `python`.

It is dynamically linked, with glibc alongside it in `/data/glibc`.
Build it statically and every wheel with a C extension — numpy, pillow,
cryptography — stops working. The operating system itself uses no glibc:
init, the shell and every command run on `userland/libc`.

```bash
pip install requests
python -m pip install numpy    # manylinux wheels work
```

### Logs

`/data/log/messages` and `/data/log/auth`. Kernel messages and our own
programs' messages go into the same file. They rotate, so they do not
grow without limit.

### Services

init keeps them alive; `service` shows you.

```
service                    the state of all of them
service restart dropbear
service stop beacon
```

`guard`, `dropbear` and `watchdog` **cannot be stopped.** Without those
three there is no way left to get the device back.

---

## Licence

MIT; see `LICENSE`.

Bundled third-party software keeps its own licence — dropbear (MIT),
wpa_supplicant (BSD), OpenSSL (Apache 2.0), the Broadcom GPU firmware
(proprietary, redistribution permitted), the Linux kernel (GPL-2.0).
