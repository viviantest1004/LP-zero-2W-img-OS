# LP-zero / linux-LP

**한국어 → [README.md](README.md)**

### ⬇ [Download an image](https://viviantest1004.github.io/LP-zero-2W-img-OS/)

One page: which image you want, and what to do with it once you have it.
If that page does not open, the same files are in [`dist/`](dist/).

---

**A small Linux distribution written from scratch.** Kernel configuration,
C library, init, shell, 108 commands — all of it new. A single kernel image
file is the whole system, and it unpacks into RAM at boot.

It started with one **Raspberry Pi Zero 2 W**. That board has 512MB of RAM,
and Ubuntu spends half of it before you have done anything, which seemed
like a waste. Nothing in it turned out to be specific to that board, so it
now runs on **arm64 virtual machines and amd64 PCs** as well.

| | |
|---|---|
| Whole system | **one** kernel + userland file, 13–22MB |
| RAM left after boot | about **480MB** on a 512MB board |
| Boot time | roughly 10 seconds from power to a prompt |
| Commands | **108**, every one written here |
| SSH | **built in**, public key only (password auth is not compiled) |
| Python | CPython 3.12 + pip. manylinux wheels install (numpy confirmed) |
| Packages | its own `pkg`, and `apt` — real Debian packages |
| External drives | mounted on plug-in, and pinnable across reboots |
| Firewall | on by default. nftables driven straight over netlink |
| Licence | MIT |

Four things come from elsewhere — the Broadcom blobs the Pi's GPU boot ROM
insists on, and three cryptographic implementations (dropbear,
wpa_supplicant, OpenSSL). **Cryptography is the one thing you do not write
yourself**, so those were taken rather than written.

---

## Who wrote this

**All of it was written by [Claude Code](https://claude.com/claude-code)**,
Anthropic's coding agent, working under the direction of the repository's
owner. That covers the kernel configuration, the C library, init, the shell,
all 108 commands, the bare-metal bootloader, the build system, the self-test,
and this document.

It is worth being clear about what that means.

The design, the debugging and the corrections are **all on the record.**
Commit messages explain why each change was made; comments say what was
tried and what broke. You can follow how a decision was reached.

At the same time, **no human wrote or reviewed this code.** It has had no
independent engineering review, no production use, and no track record.
Read the next section before you put it anywhere that matters.

---

## Before you rely on this

Honestly stated. None of this is "so do not use it" — it is all closer to
"so do not be surprised."

- **This is a hobby operating system, not a supported product.** No security
  team, no CVE process, nobody on call. For anything that has to keep
  working, use Raspberry Pi OS Lite or Debian.
- **Real hardware is not verified.** Everything was tested in QEMU. WiFi on
  a real Pi, the thermal sensors, the hardware watchdog, and the EMMC driver
  against an actual SD card are all unconfirmed.
- **The security work has not been audited.** Signature checking on updates,
  the firewall, and the DNS and NTP spoofing defences are all implemented and
  the reasoning is in the comments, but nobody else has reviewed them. Do not
  put this straight onto a hostile network on the strength of the
  [Security](#security) section alone.
- **The network path in `apt` is unverified.** The code that fetches the
  Debian base and the chroot it runs in were each confirmed separately, but
  the two have never been run end to end over a real internet connection.
  The first `apt setup` may go wrong.
- **The C library is ours, and it is not complete.** It has what the 108
  commands need and no more. Compiling something else against it can run into
  a function that is not there. Ordinary Linux binaries run under `run`, on
  the glibc that ships alongside Python.
- **`/data` is the only thing that survives a reboot,** and it is one
  partition on one card. Cards die. Back up anything you care about.
- **`dd` to the wrong device destroys that device.** Check `/dev/sdX` twice.
  It is the one mistake here that a reboot cannot undo.
- **No GUI, and it does not run containers.** The DRM drivers are in the
  kernel but X11/Wayland is yours to install, and cgroups and namespaces are
  not enabled for container use.
- **There are rough edges.** Two recent ones. Every message our own logger
  wrote was invisible to `dmesg` for months — a kmsg record that does not end
  in a newline is a continuation the kernel never finalises. And on the amd64
  image no external binary would start at all: the dynamic linker's name was
  hardcoded to the arm64 one on both architectures. Both are fixed and the
  self-test now guards them, but expect more of the same.

**What is verified:** both images pass **all 53 checks** in
`tests/selftest.sh` on real QEMU boots — booting, storage, SSH, Python,
external glibc binaries, graphics, security, error paths, redirection,
logging, time, and whether the watchdog notices a process pinning a core.

---

## What is on which branch

| Branch | What is in it |
|---|---|
| **`main`** | Everything. Full source, `GUIDE/`, both READMEs, the pre-built images in `dist/`, the download page. **This is the branch you want.** |
| **`dev`** | The development branch. Currently identical in content to `main`. This is where the commit history, with its long explanations of each change, accumulates. |

New work happens on `dev`, gets built, has to pass `tests/selftest.sh`, and
then goes up to `main`. So the images on `main` are always the images the
source next to them actually produces.

There is more detail in [`GUIDE/BRANCHES.txt`](GUIDE/BRANCHES.txt), in
English and Korean.

---

## Which image do I want

| Image | Where it runs |
|---|---|
| `dist/test_a_123_LPzero2W_linux.img.xz` | **Raspberry Pi Zero 2 W hardware and arm64 VMs alike.** Burn it to a card, or attach it in UTM/QEMU |
| `dist/linux-LP_amd64.img.xz` | **Ordinary PCs and desktop VMs.** VMware, VirtualBox, QEMU/KVM, Hyper-V |
| `dist/test_a_123_LPzero2W_linux-utm.zip` | **arm64 VMs only.** Unzip and double-click; UTM opens it. No GPU firmware, so it will not boot on the board |

One arm64 image covers both hardware and virtual machines because it carries
**both** an uncompressed kernel for the Pi's GPU and an EFI executable for
UEFI.

Check what you downloaded. A truncated image fails in confusing ways
halfway through boot.

```bash
sha256sum -c dist/SHA256SUMS.txt
```

The amd64 image calls itself **`linux-LP`** from the inside. It is not a
Raspberry Pi and should not claim to be one.

---

## Minimum requirements

| | Minimum | Comfortable |
|---|---|---|
| RAM | **64MB** | 256MB or more |
| Storage | **256MB** | 4GB or more (1GB for Python, 2GB with `apt`) |
| CPU | arm64 (ARMv8) or x86-64 | any number of cores |
| Display | not required (serial/SSH is enough for everything) | |

64MB is not an exaggeration — the kernel and userland use around 30MB just
after boot. Without Python, 256MB of storage really is enough.

---

## Using it

### Burning a card (Raspberry Pi)

```bash
xz -d < test_a_123_LPzero2W_linux.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
```

**Check `/dev/sdX`.** Get it wrong and that disk is gone.

On first boot the `/data` partition grows to fill the card by itself.

### In a virtual machine (UTM / QEMU / VMware / VirtualBox)

Decompress and attach it as a disk. Set **UEFI boot** — the MBR bootstrap is
deliberately empty, so legacy BIOS will not start it.

```bash
# QEMU, arm64
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 4096 -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=vars.fd \
  -drive file=test_a_123_LPzero2W_linux.img,format=raw,if=virtio \
  -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0

# QEMU, amd64
qemu-system-x86_64 -m 4096 -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=vars.fd \
  -drive file=linux-LP_amd64.img,format=raw,if=virtio
```

To get a bigger disk, `truncate -s 16G` the image file. It is sparse, so it
only takes the space it actually uses, and `/data` grows into it on the next
boot.

### WiFi (Raspberry Pi)

Write it into `wpa_supplicant.conf` on the boot partition.

```
network={
    ssid="home"
    psk="password"
}
```

---

## Getting in over SSH

The SSH server (dropbear) is **there from the start and comes up on its
own.** Nothing to install, nothing to enable.

**Password login is not compiled in at all.** On a board facing the internet
a password is a matter of time, so it was not offered as an option. Public
keys only.

### 1. Put a key in

The **first partition of the card (or image) is FAT32**, so it opens on
Windows, macOS and Linux alike. Put your public key in `authorized_keys`
there.

```bash
cat ~/.ssh/id_ed25519.pub >> /media/BOOT/authorized_keys
```

No key yet? `ssh-keygen -t ed25519`.

> While `/boot/authorized_keys` exists, **it is the source of truth.**
> Editing the copy on the machine is overwritten at boot. To manage keys on
> the machine instead, delete that file from `/boot`. It works this way so
> that even if `/data` is destroyed you can pull the card, put a key back,
> and get in.

### 2. Find the address

`ifconfig`, on the screen or the serial console. DHCP is automatic. For a
fixed address, write `network.conf` on the boot partition.

### 3. Connect

```bash
ssh root@192.168.0.42
```

There is **one user, root**. Port 22, and the firewall opens it by default.

In a virtual machine, forward the port:

```bash
-netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0
ssh -p 2222 root@localhost
```

### When it does not work

```
authkey -l              which keys are authorized right now
service status dropbear
firewall status
logd                    login records in /data/log/auth
```

---

## Why it does not fall over

This is the point of the whole thing. Three designs stacked on each other.

### 1. The root filesystem is in RAM

The entire system is a cpio inside the kernel image, unpacked into RAM at
boot. **Nothing on disk is part of the running system.**

- Pulling the power leaves nothing to corrupt. The next boot is always the
  factory state.
- Delete the wrong thing and a reboot puts it back.
- An update is one file replaced, and a failed one is the previous file
  restored.

Losing what you installed into `/bin` would be no fun, though, so `/bin`,
`/lib`, `/usr`, `/opt` and friends are **overlays** — reads see the image,
writes land on `/data`. `cp mytool /bin/` survives a reboot.

### 2. `guard` — the watching daemon

It keeps an eye on memory, temperature, voltage, CPU and disk.

- **Memory exhaustion**: below the reserve, it clears the largest processes
  first. It will never touch init, the shell, SSH or the watchdog.
- **Fork bombs**: killed a whole process group at a time. Measured: **2937
  processes cleared in a single pass.** Logged-in shells survive.
- **A runaway CPU**: a warning at 10 seconds, a priority drop at 30. It does
  not kill — you may have meant it.
- **Overheating / undervoltage**: it reads the Pi's throttle bits and says
  so.
- **A full disk**: logs go first.

init watches guard in turn. Kill it and it is **back within a second.**

### 3. There is always a way back

- **Watchdog**: a hardware timer. If the kernel stops, the board restarts
  itself.
- **`bootcount`**: five consecutive boots that fail to last five minutes and
  `/data/rc.local` is skipped. That is the way out when a user script is what
  is preventing boot.
- **Update rollback**: a new system that cannot survive five minutes is
  replaced by the previous image automatically.
- **Three copies of the SSH keys**: in the image, on the boot partition (FAT,
  editable anywhere), and on `/data`. Even if `/data` is lost entirely, the
  card gets you back in.
- **`fsck`**: check and repair `/data` on the machine itself.
- **A real shutdown**: `poweroff` stops services, flushes to disk, and
  **unmounts `/data`** before cutting power. The next boot needs no journal
  recovery.

---

## Plugging a drive in

A USB stick or an external disk mounts itself. A listener on the kernel's
uevent socket puts it on `/media/<name>` within a moment and cleans up when
it is pulled. Drives already attached at boot get picked up in a sweep.

```
automount -l                   what is mounted right now
automount -u <name>            detach one
```

The disk the system booted from is never touched. The mount options are the
same `nosuid,nodev` as `/data` — it is a filesystem written on somebody
else's machine.

### For a drive you want to keep

`/media` is a temporary place. For a drive that should be in the same place
after a reboot, adopt it.

```
storage                          what survives a reboot, and how full it is
storage adopt /dev/sdb photos    pinned at /mnt/photos
storage format /dev/sdb backup   wipe it, make it ext4, pin it
storage forget photos            stop
```

Adopted drives are found **by filesystem label**, not by device name, so
they come back in the same place through a different port. `sdb1` becomes
`sdc1` the moment you plug in another drive first.

```
storage
  what survives a reboot
    boot       /boot            110M free of 126M     12% used   /dev/sda1
    data       /data             63M free of 112M     43% used   /dev/sda2
    photos     /mnt/photos       51M free of 55M       8% used   /dev/sdb
```

`poweroff` clears these drives before it unmounts `/data`, detaching them if
somebody is still using one — one drive should not be able to stop you
turning the machine off.

### Disks and partitions

```
disk                           every block device the kernel found
part /dev/sdb                  look at and change the partition table
datadisk                       choose which partition /data comes from
expandfs                       grow /data to the end of the card
fsck                           check and repair /data
```

---

## Debian packages (apt)

`apt install` works. It is real Debian apt.

```
apt install ripgrep htop           install
apt run htop                       run what you installed
apt search sqlite                  what is available
apt shell                          a shell inside it
apt status                         where things stand
apt purge-all                      remove the whole thing
```

### How it works

A whole Debian userland goes into `/data/debian`, and apt runs with that
directory as its root. dpkg has absolute paths like `/var/lib/dpkg`
compiled into it, so showing it that tree as a real root is the only way.

Which means:

- **The system image is untouched.** Its size does not change.
- **`apt remove` cannot break this machine's commands.** They are not in
  there.
- **`rm -rf /data/debian` undoes all of it.**

### The first time takes a while

The Debian base is **95MB to download, 440MB unpacked**. Given that this
entire system is 13–22MB, the base alone is twenty times larger. So it is
not in the image; it is fetched the first time you use `apt`. You are told
how long it will take before it starts, and it will not begin if `/data` has
no room.

It needs at least 900MB free on `/data`. On a small card, grow it with
`expandfs` or attach an external drive with `storage`.

### Things to know

What you install runs **inside the Debian tree** — hence `apt run htop`. And
this system's init does not start services in there; to have one come up at
boot, put it in `/data/rc.local`.

`pkg` is still here. The small packages built for this system still install
with `pkg`, straight onto `/data`. `apt` is for when you want something
Debian already has.

And, as noted above, **`apt setup` over a real internet connection is still
unverified.**

---

## Python and ordinary Linux binaries

Python is `/data/python/bin/python3.12`, and `python` finds it.

It is dynamically linked, with glibc alongside it in `/data/glibc`. Building
it statically would rule out every wheel with a C extension in it — numpy,
pillow, cryptography. The operating system itself does not use glibc: init,
the shell and every command run on `userland/libc`.

```bash
pip install requests
python -m pip install numpy    # manylinux wheels work
```

**Ordinary Linux binaries** run on that same glibc. Call them through
`run ./whatever`, or just run them directly — the loader symlinks are set up
at boot. The right dynamic linker is chosen per architecture, so this works
on arm64 and amd64 alike; that it did not work on amd64 was a bug fixed
recently.

---

## How much of the boot chain is ours

```
boot ROM → bootcode.bin → start.elf → our firmware → Linux
              (Broadcom blobs)         (firmware/)    (kernel/)
```

The bare-metal firmware in `firmware/` reads the SD card itself and loads
Linux. The EMMC driver (Arasan SDHCI), MBR parsing, FAT32 reading (long
names included), device-tree rewriting, and the handover under the arm64
boot protocol are all our own code.

start.elf can load a kernel directly too, so this stage is not a new
capability — it is one link of the chain replaced with ours. When it goes
wrong the exception vectors catch it and say what broke and where: the kind
of exception, the address touched, whether it was a read or a write, and the
call path that got there.

The two Broadcom blobs (bootcode.bin, start.elf) are what the GPU boot ROM
demands, and cannot be replaced.

---

## Security

- **No password authentication.** dropbear is compiled without it.
- **Signature checking on updates.** RSA-2048 / PKCS#1 v1.5 / SHA-256. The
  public key lives inside the kernel image, so it cannot be changed by
  pulling the card. A bad signature refuses the install, and **there is no
  override.**
- **Firewall on by default.** nftables rules are pushed straight over
  netlink, in one transaction, so there is no moment when half the rules are
  in place.
- **DNS spoofing defences.** Transaction IDs from `getrandom`, a connected
  socket, and the question section checked against what was asked.
- **NTP spoofing defences.** Nonce, mode and stratum all verified.
- **`/data` is mounted nosuid,nodev.** Putting the card in another PC and
  planting a setuid file achieves nothing. External drives get the same
  options.
- **`integrity`**: checks hashes of the files that survive a reboot.
- **Login records**: `/data/log/auth`.

None of this has been audited by anyone. See
[Before you rely on this](#before-you-rely-on-this).

---

## Who this suits

### A good fit

- **Anyone running something around the clock on a Pi Zero / Zero 2 W.** You
  keep 480MB of RAM. A temperature logger, home automation or sensor
  collection does not need Ubuntu under it.
- **Devices that have to run untouched for a long time.** Unreliable power,
  or physically hard to reach — everything in
  [Why it does not fall over](#why-it-does-not-fall-over) exists for that.
- **Anyone who wants to see how Linux actually works.** From kernel
  configuration to the shell it is small enough to read, and the comments
  explain why things were done the way they were.
- **Anyone who needs a minimal environment.** The attack surface is small.

### Not a good fit

- **A desktop.** There is no GUI. The DRM drivers are in the kernel, but
  X11/Wayland is yours to install.
- **Several users.** You can create them, but the design assumes one root.
- **Work that needs a proven distribution.** This has not been tested the way
  Debian or Ubuntu has. Use Raspberry Pi OS Lite there.
- **Containers.** cgroups and namespaces are not enabled for that.

`apt` has removed a lot of the "not enough packages" objection. But it runs
inside the Debian tree, and this system's init does not manage services in
there.

---

## Every command

`help` prints this on the machine too. `help <command>` explains just one.

### Shell (16)

| Command | What it does |
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

| Command | What it does |
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

| Command | What it does |
|---|---|
| `cat` | print a file |
| `edit` | edit a file on screen |
| `more` | read it a screen at a time |
| `grep` | print the lines that match, with context |
| `head` | the first lines |
| `tail` | the last lines |
| `wc` | count lines, words, characters |
| `sort` | put lines in order |
| `uniq` | collapse repeated lines |
| `cut` | take columns out of lines |
| `tee` | write to a file and pass on |

### System (37)

| Command | What it does |
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
| `apt` | install packages from Debian |
| `pkg` | install and remove packages |
| `update` | replace the system, reversibly |
| `splash` | draw the boot screen |

### Network (14)

| Command | What it does |
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

| Command | What it does |
|---|---|
| `python` | CPython 3.12 |
| `python3` | the same as python |
| `micropython` | MicroPython — small, fast |

### Time (2)

| Command | What it does |
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

arm64 is cross-compiled from an x86 host. amd64 is native, so no
cross-compiler is needed for it.

### arm64 (Raspberry Pi + arm64 VMs)

```bash
./tools/fetch-kernel.sh      # the Linux source
./tools/fetch-blobs.sh       # the Pi's GPU firmware
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

### All three releases at once

```bash
./tools/mkdist.sh            # three images, checksums and the download page into dist/
```

The checksums and `index.html` are generated from the actual files by that
script. Written by hand they go stale on the next rebuild, and a checksum
that does not match is worse than none — the person downloading cannot tell
a corrupt transfer from a stale page. That has already happened here once.

Intermediate build output goes into `.build/` next to the repository. If
disk is tight, `LPZERO_WORK=/mnt/big/lpzero ./tools/mkdist.sh` moves it.

### Checking a build

```bash
./tests/selftest.sh          # on the machine. 53 checks
```

### The update signing key

```bash
./tools/sign-release.sh --new-key    # makes a key in keys/ (once)
make                                  # the public key goes into the image
./tools/sign-release.sh kernel/out/Image
```

**Never put the private key on a device.** `keys/` is in `.gitignore`.

---

## How the source is laid out

```
userland/          one set of sources. make ARCH=amd64 switches
  libc/            our libc (straight on top of syscalls)
  init/ sh/ ...    108 commands, one directory each
firmware/          the bare-metal bootloader (EMMC, FAT32, DTB, vectors)
kernel/            kernel configuration and build scripts
boot/              what goes on the boot partition (config.txt, /etc/rc)
tools/             image building, signing, download-page generation
tests/             the self-test
GUIDE/             usage and branch notes (English/Korean)
web/               the download page template
dist/              the finished images
```

### Why there are no per-architecture directories

There is no `arm64/` and no `amd64/`, because both architectures use **the
same sources.** Exactly three things actually differ:

| | |
|---|---|
| `userland/libc/include/syscall-arm64.h` / `-x86_64.h` | syscall numbers and calling convention |
| `userland/libc/src/crt0.S` | the entry point (branched with `#if`) |
| `kernel/lp-zero.config` / `lp-zero-amd64.fragment` | kernel configuration |

The 108 commands and the body of the libc are **identical, character for
character.** Splitting them into copied directories invites fixing one side
and not the other, and that has happened here twice — 43MB of arm64 binaries
once shipped inside an amd64 image, and the dynamic linker's name was
hardcoded to the arm64 one on both. The first is now caught by
`check_tree_arch`, which compares the architecture of every ELF at build time
and **stops the build**; the second is guarded by two self-test checks.

---

## Details

### Partition layout

| | |
|---|---|
| p1 | FAT32, 128MB. Kernel, `config.txt`, `authorized_keys`, WiFi settings, firewall settings. **Editable on any PC** |
| p2 | ext4, all the rest → `/data`. Grows to fill the card on first boot |

### What survives

| Path | After a reboot |
|---|---|
| `/data` | kept |
| `/root` | kept (bind of `/data/root`) |
| `/bin` `/lib` `/usr` `/opt` `/sbin` `/srv` | kept (overlay onto `/data`) |
| `/mnt/<adopted drive>` | kept (found again by label) |
| `/media/<automounted>` | **temporary** (gone when unplugged) |
| `/etc` `/tmp` `/var` | **gone** (deliberately) |

`/etc` is left out on purpose: `/etc/rc` and `/etc/services` are read
**before** `/data` is mounted. A stale copy shadowing a system update means
a board that does not come up.

### Logs

`/data/log/messages` and `/data/log/auth`. Kernel messages and our
programs' messages go into the same place. They rotate, so they cannot grow
without bound.

### Services

init keeps them alive; `service` shows them.

```
service                    the state of everything
service restart dropbear
service stop beacon
```

`guard`, `dropbear` and `watchdog` **cannot be turned off.** Without those
three there is no way left to get the machine back.

---

## Known limits

They are all in [Before you rely on this](#before-you-rely-on-this). One
thing that is not there: our bootloader **does not apply device-tree
overlays.** On real hardware this does not matter, because it takes the tree
start.elf already applied them to; anywhere else, `dtoverlay` lines in
`config.txt` are ignored and the loader names what it skipped.

---

## Licence

MIT — see [`LICENSE`](LICENSE).

The bundled third-party software keeps its own licences: dropbear (MIT),
wpa_supplicant (BSD), OpenSSL (Apache 2.0), the Broadcom GPU firmware
(proprietary, redistribution permitted), and the Linux kernel (GPL-2.0).
