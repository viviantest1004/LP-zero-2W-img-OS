# What to do with it

Four things this board is actually good at, written as `/data/rc.local`
scripts you can copy. `rc.local` runs at every boot, after the network
is up and behind the firewall, and `bootcount` skips it after five short
boots — so a mistake in here cannot lock you out.

    scp examples/temperature-log.sh root@<board>:/data/rc.local

Each one is a working script, not a sketch. They use the shell this
system has: `$(...)`, functions, `if`, `while`, `for`.

| | What it does | Why this board |
|---|---|---|
| `temperature-log.sh` | Records temperature, memory and load to a CSV forever | Runs for months on 0.5W with nothing to maintain |
| `webhook-notify.sh` | Posts to a URL when something changes | No cloud account, no agent, no runtime |
| `http-server.sh` | Serves a directory over HTTP | 22MB of system, so the RAM is yours |
| `usb-backup.sh` | Copies a directory to a USB stick when one is plugged in | The disk tools are built in |

## Reading the log afterwards

Every example writes to `/data/log/`, which survives a reboot and which
`guard` protects: when the data partition fills, it drops the oldest
rotated log rather than letting the machine seize up.

    tail -f /data/log/temperature.csv
    scp root@<board>:/data/log/temperature.csv .
