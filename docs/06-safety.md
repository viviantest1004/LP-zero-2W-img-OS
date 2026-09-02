# Staying up

This board runs headless on a shelf. Anything that needs a person
standing next to it is, in practice, a board that stays broken until
someone notices - so every failure here has to have an answer that does
not involve a keyboard.

What follows is every layer, what it actually does, and how to turn it
off if it is in the way.

## The layers, from the outside in

| Failure | What answers it | Where |
|---|---|---|
| The kernel hangs | hardware watchdog resets the board | `watchdog`, `/etc/services` |
| The boot hangs | the watchdog is armed before the boot script runs | `watchdog -1 -t 120` in `/etc/rc` |
| The kernel panics | reboots after 10s instead of waiting forever | `CONFIG_PANIC_TIMEOUT=10` |
| A task wedges in the kernel | printed with a backtrace after 120s | `CONFIG_DETECT_HUNG_TASK` |
| The boot keeps failing | `/data/rc.local` is skipped after 5 tries | `bootcount` |
| Out of memory | the biggest process is killed, never the ones you need | `guard` |
| The chip overheats | held at the lowest frequency until it cools | `guard` |
| The power supply sags | same lever - less current drawn | `guard` |
| A process eats the CPU | pushed to the back of the run queue | `guard` |
| `/data` fills up | warned early, old log dropped when critical | `guard` |
| The RAM filesystem fills up | warned - this is memory no process owns | `guard` |
| `/tmp` runs away | bounded at 64MB, so the program gets ENOSPC | `/etc/rc` |
| ext4 corruption | the filesystem goes read-only rather than writing more | `errors=remount-ro` |
| Power cut while writing `/boot` | mounted read-only; FAT has no journal | `mount -o ro` |

## guard

One daemon, one loop. Memory is checked every second, everything else
every five - a temperature does not change in a second, and walking
`/proc` for nothing is exactly the idle cost this system is trying not to
pay.

```
guard [-d] [-r reserveMB] [-w warnMB]
```

**Protected processes** - `init`, `guard`, `watchdog`, `sh`, `dropbear`,
`wpa_supplicant`, `logd`. They are never killed for memory, and they are
kept at the front of the run queue (`nice -5`, and `-10` for the watchdog,
which must get a turn or the board resets). The point is that the machine
stays reachable while it is in trouble - a safety net you cannot log in to
observe is not one.

Protection is written into `/proc/<pid>/oom_score_adj` as well, so it
holds even if the kernel's own OOM killer runs first. The value is
inherited across `fork`, so the shell dropbear starts per connection is
protected without anyone tracking it.

**Memory.** Below 64MB free it warns; below 32MB it kills the largest
unprotected process - `SIGTERM`, half a second, then `SIGKILL`. One at a
time, looking again in between, so it never takes more than it needs.

**Heat.** Above 80C the CPU is pinned to its lowest frequency, and let go
again below 65C. The gap is what stops it flapping between the two
governors every few seconds. The chip throttles itself at 80C anyway -
that is the hardware protecting itself - but it will not tell anyone, and
it will not stop accepting work.

**Power.** The GPU firmware keeps a word recording every time it has had
to step in, and undervoltage leaves no other trace: the board does not
crash, it corrupts the card quietly, weeks later. `guard` reads it, says
so, and caps the frequency - which cuts the current draw that caused it.
`sysinfo` shows the same word under `[health]`.

**CPU.** A process holding 90% of a core for a full minute goes to
`nice 10`. A minute is deliberate: anything shorter punishes an ordinary
build or a Python script for doing its job. And the punishment is only a
priority - nothing is ever killed for using the CPU, since that may well
be exactly what you asked it to do. It just no longer gets to decide
whether SSH answers.

**Disk.** `/data` under 32MB warns; under 8MB the previous rotated log
(`/data/log/messages.1`) is deleted, being the one thing that can be freed
without asking. The root filesystem is watched too, and gets its own
message, because space used there is RAM that no process owns - the memory
killer would work through every process on the machine without freeing a
single page of it.

None of this needs the hardware to exist. On a virtual machine there is no
sensor, no firmware word and no cpufreq; those reads fail and are skipped.

## bootcount

The watchdog reboots a board that has stopped answering, which is right
exactly once. If what wedged it is in the startup script, the watchdog
does it again, and again, and the board spends the rest of its life
rebooting - with no window to log in and fix the file causing it.

So every boot bumps a number on the data partition, and `guard` clears it
once the system has stayed up for five minutes, which is what a boot that
worked looks like. The number only grows when boots keep failing. At five,
`/etc/rc` stops running `/data/rc.local` - the one part of the boot you can
change, and so the most likely thing to be at fault - and the machine comes
up plain and reachable.

There is deliberately no clock anywhere in this. At that point in the boot
the time has been restored from a file at best, and a board that has never
reached the network does not know what year it is.

```sh
bootcount -c        # clear it by hand, after fixing rc.local
sysinfo             # [health] shows the count when it is above 1
```

## Turning it off

Nothing here is mandatory. `/etc/rc` and `/etc/services` are plain text
files on the boot partition's image, and the failure of any one line never
stops the boot.

```sh
kill <guard pid>                      # no more memory, heat or CPU policy
watchdog -x                           # disarm the watchdog
guard -d -r 16 -w 32                  # or just move the thresholds
```

The thresholds that are compile-time (80C, 90% for 60s, 64MB of `/tmp`)
are at the top of `userland/guard/guard.c` and in `/etc/rc`, one named
constant each.

## What has been tested, and what has not

Verified under QEMU, booting the real image from USB, VirtIO and NVMe:
the watchdog resetting the board when petting stops, the early arm during
boot handing over to the daemon, `guard` demoting a Python process pinning
a core after exactly 60s, `nice -5` landing on `init`, `/tmp` capped at
64MB, `/data` mounted `errors=remount-ro`, `/boot` read-only, the boot
counter surviving power cycles and tripping on the fifth failed boot.

Not verified, because it needs the real board: the temperature sensor, the
firmware's undervoltage word, and the frequency governor actually moving.
The code paths for all three are skipped cleanly where the files do not
exist, which is what a virtual machine looks like - but "it does not crash
without the hardware" is not the same as "it works with it".
