# desktop/session — from a booted kernel to a window on the screen

Three files, and between them they are the only place the whole chain is
written down. Every piece of it can be read from somewhere else — the
kernel config, `preinit`, `init.c`, `/etc/rc`, `/etc/services`, wayfire's
own metadata — but the order, and why each step is where it is, cannot be
put back together from the parts afterwards. That is what this file is
for.

| file | what it decides |
|---|---|
| `start-desktop` | everything a normal Linux gets from systemd and logind and this machine does not have |
| `wayfire.ini` | what the desktop looks like and which keys do what |
| `README.md` | this: what starts what, and in which order |

Only the disk-rooted amd64 image runs any of it. The Raspberry Pi Zero 2 W
and the RAM-live images have no display server and are not meant to.

---

## The chain

```
firmware / UEFI
  └─ bzImage                    kernel + a one-program initramfs
      └─ preinit                finds the ext4 partition labelled LPROOT,
         │                      mounts it read-only, switch_root into it
         └─ /bin/init  (pid 1)  ours. No systemd, no logind.
             ├─ mounts proc, sysfs, devtmpfs, devpts
             │                  ← /dev/dri and /dev/input appear here
             ├─ /etc/rc         runs once, and nothing revives what it starts
             │    remount / rw, /tmp, /dev/shm, /data, network, firewall
             └─ /etc/services   supervised. Dies → init starts it again.
                  guard, dropbear, dhcp, ntp, logd, …
                  └─ start-desktop
                       ├─ checks: wayfire, its config, /dev/dri, /dev/input
                       ├─ makes /run/user/0, 0700, on a tmpfs
                       ├─ names the shortcuts' missing programs, once
                       ├─ sets the environment (seat, software rendering)
                       └─ dbus-run-session wayfire -c ~/.config/wayfire.ini
                            └─ wayfire reads wayfire.ini
                                 ├─ #0e0e0e desktop, no decorations
                                 ├─ Super+T, Super+Q, Alt+Tab, …
                                 └─ [autostart] foot
                                      └─ a window on the screen
```

Alongside all of that, and not shown because it never stops: init keeps a
login shell on `/dev/tty1`, respawning it forever. It is the reason a
failed desktop is not a dead machine.

### Why the desktop is in `/etc/services` and not in `/etc/rc`

`/etc/rc` says it in its own first paragraph: *anything that has to stay
alive belongs in `/etc/services`, not here. Nothing started from this
file gets restarted when it dies.*

The project has paid for that lesson twice — `guard` and
`wpa_supplicant` were both started from `rc`, and killing either one
disabled it permanently on a machine nobody was watching. A compositor
started from `rc` with `&` has exactly the same shape: it crashes once,
the screen goes black, and nothing on the machine notices or cares.

The line to add to `boot/rootfs-overlay/etc/services` is:

```
?/usr/bin/wayfire start-desktop
```

The `?` is init's "only if that path exists" prefix. It is what keeps
this one line out of the way of the arm64 and RAM-live images, which
share the same `/etc/services` and have no compositor: init prints one
sentence saying it is not starting it, instead of failing to exec
something twelve times.

To try it without rebuilding an image, `service add start-desktop` writes
`/data/services`, which init reads too and re-reads every few seconds.

---

## What this machine does not have

### No seat manager

`/dev/dri` is the GPU, `/dev/input` is the keyboard and mouse, and on a
normal Linux the compositor opens neither. logind owns the seat: it opens
the devices, hands the file descriptors over D-Bus, and takes them back
when the session switches away. That is the thing that lets a desktop run
as an ordinary user.

There is no logind here — systemd is not pid 1 on this machine, our init
is — and no seatd either. So `LIBSEAT_BACKEND=builtin` tells libseat to
open the devices itself, and libseat's builtin backend only works as
root.

**How it is arranged:** devtmpfs creates the nodes `0660 root:video` and
`0660 root:input`, the session runs as root, root passes the owner check.
Nothing is chmodded and no group is added.

**What it costs:** the compositor is root. Every window it opens is root.
Anything installed from `apt` and started from the desktop is root. There
is no seat to revoke from, so a program that gets hold of the DRM device
keeps it until it exits. This is not effort saved — root is the only
account this system has, so a video group and a session and a seat would
be three mechanisms protecting nobody from nobody. It becomes wrong on
the day there is a second account, and that is the day `seatd` belongs in
`/etc/services`.

### No `XDG_RUNTIME_DIR`

Wayland puts its socket there and refuses to start without one. On a
normal Linux `pam_systemd` creates `/run/user/<uid>` at login and removes
it at logout; nothing here does either. `start-desktop` makes
`/run/user/0` at 0700, on a tmpfs mounted over `/run/user` so that a
socket left by a killed session is gone by definition rather than by
somebody remembering to delete it.

Only `/run/user` gets the tmpfs, not `/run` — `/run` came with the Debian
base with directories in it that packages made, and covering those with
an empty filesystem would hide them for nothing. And the mount is guarded
by a `/proc/mounts` check, because init restarts this script: without the
guard, twelve restarts would stack twelve filesystems, each hiding the
one beneath it, and the empty one on top is the one in use.

### No `/dev/shm`

`/etc/rc` mounts it. `start-desktop` only checks and warns, because the
mount is a system filesystem like `/tmp` and should not be arranged by
the thing that needs it — but the desktop is the only thing on this
machine that ever cared, which is why it went unmounted for so long.

### D-Bus, and whether it is worth starting one

Nothing running today needs it. GTK 4 draws perfectly well with no bus,
complains about the accessibility bus, and carries on.

What needs it is the next thing installed. A libadwaita application asks
the desktop portal for the colour scheme instead of reading the GTK
setting — `desktop/theme/settings.ini` says so in its own comments — and
with nobody answering it comes up **light on a dark desktop**, with no
error anywhere. Notifications and the portal file chooser fail the same
silent way.

So yes, it is worth it, and it is cheap: `dbus-run-session` makes a bus,
puts its address in the environment, and execs the compositor. When it is
not installed the launcher is `/usr/bin/env` instead, which runs the
compositor and nothing else.

> The full path is not decoration. `env` is one of our shell's builtins,
> and a builtin cannot be handed a command to run — written as a bare
> name it printed the environment, returned 0, and never started the
> compositor. The console said the session had started, init was content
> because nothing had died, and the log held a tidy list of environment
> variables. A name with a slash in it is not a builtin name.

### No GPU

QEMU has no graphics card, so Mesa falls back to llvmpipe and every pixel
of every frame is drawn by the CPU.

The one line that makes that work is `WLR_RENDERER_ALLOW_SOFTWARE=1`.
wlroots refuses a software renderer unless it is told otherwise, and it
is right to refuse — on real hardware a software renderer means a driver
failed to load and the desktop is about to be unusable. Without that
line the session exits at startup with one line in the log and a black
screen on the monitor.

`LIBGL_ALWAYS_SOFTWARE` and `GALLIUM_DRIVER=llvmpipe` are deliberately
**not** set. They would work, and they would also throw away the Intel
driver that is in this kernel on purpose, on every machine that has one.
Mesa already falls back to llvmpipe when it finds no driver for the
device in front of it; what it cannot do by itself is talk wlroots into
accepting the result.

`WLR_NO_HARDWARE_CURSORS=1` and `GSK_RENDERER=cairo` are the opposite
decision, and unconditional. A virtual GPU has no cursor plane, so with
hardware cursors on, the pointer is composited into a plane nothing scans
out and the mouse is invisible while everything else works. GTK 4 draws
through GL by default, which on llvmpipe is software emulating a GPU
emulating software. Both cost a little on a machine with a real graphics
card — and detecting which machine this is cannot be done reliably with
the tools here, because the drivers are built into the kernel rather than
loaded, so `/sys/module` does not answer. This system has only ever been
verified under QEMU, so these are set for QEMU. They are the two lines to
delete on the day it is verified on hardware.

---

## When the compositor stops

There are three ways back in, and it is worth knowing which one applies.

| what happened | the way in |
|---|---|
| the session is running and you want a console | **Ctrl+Alt+F1**. wlroots switches virtual terminals itself, and init's shell is waiting on tty1. |
| the compositor exited, cleanly or not | the tty1 shell, already there. wayfire puts the terminal back into text mode on the way out. |
| the compositor was killed outright | **SSH**. See below. |

`start-desktop` returns when the compositor does, and init starts it
again a second later. If it keeps failing within a minute of starting,
init doubles the wait each time and gives up after twelve tries, saying
so and naming `service start start-desktop` as the way to try again. That
give-up is the safety property: a machine that cannot start a desktop
ends up at a console prompt rather than looping on a black screen
forever. `start-desktop` is deliberately *not* on init's critical list
— `guard`, `dropbear` and `watchdog` are, and those are retried forever
because the machine is unreachable without them. A desktop is not.

A machine with no display device at all exits **78**
(`LP_EXIT_NO_HARDWARE`), which init treats as "nothing to do here" and
never retries — the same contract the `watchdog` service uses in a
virtual machine with no watchdog timer.

### The one failure this cannot repair

A compositor that is **killed outright** — `SIGKILL`, or a segfault that
takes its cleanup with it — never hands the virtual terminal back. The
screen stays in graphics mode with the keyboard switched off: black, and
not answering. Nothing in this userland can put a VT into text mode
again; there is no `chvt` and no `kbd_mode`, and `reset` only writes
escape sequences to a terminal that is no longer listening.

SSH is the way in, and it is always there — `dropbear` is a supervised
service and is on init's critical list. From there, `reboot`.

This is why `Ctrl+Alt+Backspace` sends `SIGTERM` and not `SIGKILL`, and
why the `idle` plugin's DPMS is switched off: turning a virtual display
off and back on is the other operation that can return a dark screen with
everything still running.

---

## The shortcuts

`설정 › 키 지정` is the list of what this desktop promises. What wayfire
0.7.5 can actually answer:

| 설정 says | keys | how |
|---|---|---|
| 터미널 열기 | Super+T | `[command]` → `foot` |
| 파일 관리자 열기 | Super+E | `[command]` → `lp-files` |
| 화면 잠금 | Super+L | `[command]` → `swaylock`. **Read the warning below.** |
| 창 닫기 | Super+Q | `[core] close_top_view` |
| 창 전환 | Alt+Tab | `[switcher]` |
| 최대화 | Super+Up | `[wm-actions] toggle_maximize` |
| 왼쪽/오른쪽 반쪽 | Super+Left/Right | `[grid] slot_l` / `slot_r` |
| 작업공간 개요 | Super+Tab | `[expo] toggle` |
| 전체 스크린샷 | Print | `[command]` → `grim` |
| 영역 스크린샷 | Shift+Print | `[command]` → `grim -g "$(slurp)"` |
| 셸 강제 재시작 | Ctrl+Alt+Backspace | `[command]` → `pkill -TERM wayfire` |
| 가상 터미널 1·2 | Ctrl+Alt+F1/F2 | wlroots, with no configuration |

### What does not map

| 설정 says | why not |
|---|---|
| 작업공간 전환 — Super+1–9 | **This wayfire has no direct workspace-by-number binding at all.** `vswitch` owns workspace movement and has only `binding_left/right/up/down`; there is no IPC to ask from outside either. What `wayfire.ini` does instead is bind `[expo] select_workspace_1..9` to the same keys, so Super+Tab then Super+1 goes where Super+1 alone was meant to. Making it whole needs wayfire 0.8, which has an IPC plugin, or a plugin of our own. |
| 검색 열기 — Ctrl+Space | there is no search. It is part of the top bar, which is not written. |
| 퀵메뉴 열기 — Super+A | the same. |
| 전원 메뉴 — Super+Escape | the same. |
| 설정 열기 — Super+I | the settings app exists as a mockup on the `gui` branch and not as a program. |
| 창 스크린샷 — Alt+Print | needs the focused window's geometry, and wayfire 0.7 has no way to ask for it from outside. |
| 화면 녹화 — Ctrl+Shift+R | `wf-recorder` would do it. Not bound, because starting and stopping on one key needs a toggle whose failure mode is a recording nobody knows is running. |
| 입력 소스 전환 — 한/영 | an input method's job. The compositor sees a keycode; what turns a sequence of them into 한글 is a program this session does not start. |
| 볼륨 올리기/내리기 | there is no sound daemon. The kernel has the drivers, nothing drives them. |

There is also one binding this desktop has that `설정` does not list, and
that is a gap in the screen rather than in the config: **Super+Alt+arrows**
moves between workspaces. It is wayfire's own default and it is the only
direct workspace switch that exists here.

> **화면 잠금.** `swaylock` is not in the base. Until it is installed, or
> until the design's own lock screen exists, Super+L locks nothing — and
> a screen that was not locked looks exactly like one that was, until
> somebody touches it. `start-desktop` prints
> `swaylock is NOT installed - Super+L DOES NOT LOCK THE SCREEN` at every
> start for that reason.

---

## Installing it

| file | where it goes |
|---|---|
| `wayfire.ini` | `/root/.config/wayfire.ini` |
| `start-desktop` | `/bin/start-desktop`, mode 755 |
| the line above | `boot/rootfs-overlay/etc/services` |

`start-desktop` is `#!/bin/sh`, and `/bin/sh` on this machine is our own
shell, not dash — `/usr/bin/sh` is Debian's. init execs the path
directly, so the kernel's `CONFIG_BINFMT_SCRIPT` is what reads the
`#!` line. It is on by default and nothing in this tree turns it off.

What the base needs, beyond `wayfire` and `foot` which are already in it:

```
apt install grim slurp swaylock
```

`fonts-nanum` is already there, which matters: `desktop/theme/settings.ini`
asks for `Nanum Gothic 9`, and a font name that does not resolve gives a
fallback that is not the mockup's.

The theme is installed separately and this session does not touch it —
`~/.config/gtk-3.0/` and `~/.config/gtk-4.0/`, from `desktop/theme/`. A
session that comes up in the wrong greys is a theme that did not get
installed, not a compositor that misread this file.

---

## Not verified

Everything here was written against the real tree: every wayfire option
name and default quoted in `wayfire.ini` was read out of
`/usr/share/wayfire/metadata/*.xml` on the built image, and `start-desktop`
was run end to end under the actual `/bin/sh` binary with the external
commands stubbed — which is how the `env` builtin trap above was found.

What has **not** happened is a boot. Nobody has watched this draw a
window. In particular:

- `background_color` is written as `#0E0E0EFF`. Wayfire's own default for
  it is four floats. If the hex form turns out not to parse, the same
  colour is `0.055 0.055 0.055 1.0`.
- Print Screen is bound as `KEY_SYSRQ | KEY_PRINT` because a PC keyboard
  reports keycode 99 (`KEY_SYSRQ`) and wayfire's shipped example binds
  `KEY_PRINT` (210). Both are accepted rather than guessing which of us
  is wrong about somebody else's keyboard.
- `LANG=C.UTF-8`, which is always present in Debian and always right
  about the encoding. It is not right about collation. Precomposed
  Hangul sorts correctly by code point anyway, so this is only wrong for
  mixed scripts; changing it means generating a `ko_KR.UTF-8` locale
  first, and a `LANG` naming a locale that was never generated produces
  a warning from every program that starts.
- The `[autostart] terminal = foot` line is temporary and says so. Until
  the top bar exists, a working desktop is a flat `#0e0e0e` rectangle,
  which is pixel for pixel what a compositor that started and then died
  looks like. The terminal is the difference between those two.
