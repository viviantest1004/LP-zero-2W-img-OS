# selftest.sh - run on the machine itself and say what is broken.
#
# Every check prints one line beginning PASS, FAIL or SKIP, so the
# result can be read out of a serial log by something that knows nothing
# else about this system. SKIP means the check could not run here and
# says why - "no watchdog in a virtual machine" is a fact about the test
# machine, not a defect. A test that quietly passes because it did not
# run is worse than no test at all.
#
# Written in the subset this shell actually has: if/elif/else/fi, while,
# for, test, and backticks. No functions and no case - this shell has
# neither, which is worth knowing before writing anything longer.

echo "=== BOOT AND IDENTITY ==="
if uname -o > /tmp/t1 ; then echo "PASS  uname works: `cat /tmp/t1`" ; else echo "FAIL  uname" ; fi
if test -f /etc/osname ; then echo "PASS  /etc/osname: `cat /etc/osname`" ; else echo "FAIL  /etc/osname missing" ; fi
if pidof init  > /dev/null ; then echo "PASS  init running"  ; else echo "FAIL  init not running"  ; fi
if pidof guard > /dev/null ; then echo "PASS  guard running" ; else echo "FAIL  guard NOT running - machine undefended" ; fi
if pidof logd  > /dev/null ; then echo "PASS  logd running"  ; else echo "FAIL  logd not running"  ; fi

echo "=== STORAGE AND PERSISTENCE ==="
if mount | grep -q " /data " ; then echo "PASS  /data mounted" ; else echo "FAIL  /data NOT mounted" ; fi
if touch /data/.st ; then echo "PASS  /data writable" ; else echo "FAIL  /data read-only" ; fi
rm -f /data/.st
if persist | grep -q kept ; then echo "PASS  install dirs are kept" ; else echo "FAIL  persist holds nothing" ; fi
if echo x > /bin/.st ; then echo "PASS  /bin accepts a write" ; else echo "FAIL  /bin not writable" ; fi
if test -f /data/persist/bin/.st ; then echo "PASS  and it landed on /data" ; else echo "FAIL  write to /bin did not reach the card" ; fi
rm -f /bin/.st

echo "=== SSH ==="
if test -f /data/dropbear_ed25519_host_key ; then echo "PASS  host key is on /data" ; else echo "FAIL  no host key" ; fi
if mount | grep -q " /root " ; then echo "PASS  /root comes from /data" ; else echo "FAIL  /root is RAM - keys lost on reboot" ; fi
if pidof dropbear > /dev/null ; then echo "PASS  dropbear running" ; else echo "FAIL  dropbear not running" ; fi
if authkey -l | grep -q "no keys" ; then echo "SKIP  no SSH key authorized (a setup choice, not a fault)" ; else echo "PASS  an SSH key is authorized" ; fi

echo "=== PYTHON ==="
if test -x /data/python/bin/python3.12 ; then
  if python -c 'import sys,ssl,sqlite3,zlib,bz2,lzma,ctypes,hashlib;print("pyok",sys.version.split()[0])' > /tmp/t2 2>&1 ; then
    echo "PASS  python + ssl sqlite3 zlib bz2 lzma ctypes: `cat /tmp/t2`"
  else
    echo "FAIL  python broken: `head -1 /tmp/t2`"
  fi
else
  echo "SKIP  no CPython in this image"
fi
if test -x /data/bin/micropython ; then
  if /data/bin/micropython -c 'print("mpyok")' > /tmp/t3 2>&1 ; then echo "PASS  micropython runs" ; else echo "FAIL  micropython: `head -1 /tmp/t3`" ; fi
else
  echo "SKIP  no micropython"
fi

echo "=== ORDINARY LINUX BINARIES ==="
# The loader is named differently on every architecture, and /etc/rc
# used to link only the arm64 one. On amd64 that produced a broken
# symlink, no working loader, and a cheerful "ordinary Linux binaries
# will run" message - so nothing external ran on half the images this
# project ships and no test noticed. CPython is itself a dynamically
# linked glibc binary, which makes it the obvious thing to check with.
if test -d /data/glibc ; then
  if run /data/python/bin/python3.12 -c "print('ran')" > /tmp/t20 2>&1 ; then echo "PASS  run starts a glibc binary" ; else echo "FAIL  run cannot start one: `head -1 /tmp/t20`" ; fi
  if /data/python/bin/python3.12 -c "print('ran')" > /tmp/t21 2>&1 ; then echo "PASS  and the loader symlinks work without run" ; else echo "FAIL  /lib loader symlinks are wrong: `head -1 /tmp/t21`" ; fi
else
  echo "SKIP  no glibc on /data (an image built without python)"
fi

echo "=== GRAPHICS AND INPUT (what a UI package needs) ==="
if test -d /sys/class/drm ; then echo "PASS  DRM present: `ls /sys/class/drm | wc -l` nodes" ; else echo "FAIL  no DRM - no UI package can work" ; fi
# No card0 can mean two very different things: the driver is missing, or
# there is no display device on this machine to drive. A VM started
# without a GPU is the second, and calling that a failure trains people
# to ignore the result.
if test -e /dev/dri/card0 ; then
  echo "PASS  /dev/dri/card0 exists"
elif test -d /sys/class/drm ; then
  echo "SKIP  no /dev/dri/card0 - DRM is built in but this machine has no display device attached"
else
  echo "FAIL  no DRM and no /dev/dri/card0"
fi
if ls /sys/class/input | grep -q event ; then
  echo "PASS  input event devices exist"
else
  echo "SKIP  no input devices - none attached to this machine (serial console only)"
fi
if test -e /dev/fb0 ; then echo "PASS  /dev/fb0 exists" ; else echo "SKIP  no fbdev (DRM may still be fine)" ; fi

echo "=== SECURITY ==="
if test -f /etc/update-key.pub ; then echo "PASS  update key is inside the system image" ; else echo "SKIP  no update key in this build" ; fi
# An unsigned image must be refused.
#
# The image has to be convincing enough to reach the signature check.
# update looks at the size and the magic first, and rejects anything
# that fails those as "not a kernel" - which is correct, and means a
# file full of nonsense never tests the signature at all. A first
# attempt at this test used exactly such a file and reported the
# signature check as broken when it had simply never run.
#
# So we hand it the kernel this machine is running, copied without its
# signature. Genuine image, right size, right magic, no .sig beside it:
# the one case the check exists for.
KIMG=""
if test -f /boot/EFI/BOOT/BOOTX64.EFI ; then KIMG=/boot/EFI/BOOT/BOOTX64.EFI ; fi
if test -f /boot/EFI/BOOT/BOOTAA64.EFI ; then KIMG=/boot/EFI/BOOT/BOOTAA64.EFI ; fi

if test -z "$KIMG" ; then
  echo "SKIP  no kernel image on /boot to test the signature check with"
else
  rm -f /data/fake-kernel.img /data/fake-kernel.img.sig
  cp $KIMG /data/fake-kernel.img
  update /data/fake-kernel.img > /tmp/t4 2>&1
  if grep -q signed /tmp/t4 ; then echo "PASS  update refuses an unsigned image" ; else echo "FAIL  unsigned image was NOT refused: `head -1 /tmp/t4`" ; fi
  rm -f /data/fake-kernel.img
fi
rm -f /data/.update.sig
echo guard > /data/services.disabled
sleep 3
if pidof guard > /dev/null ; then echo "PASS  guard ignores services.disabled" ; else echo "FAIL  guard was disabled by an editable file" ; fi
rm -f /data/services.disabled
service stop guard > /tmp/t5 2>&1
if grep -q force /tmp/t5 ; then echo "PASS  service stop guard is refused" ; else echo "FAIL  service stop guard was allowed" ; fi
if firewall status > /tmp/t6 2>&1 ; then echo "PASS  firewall reports its state" ; else echo "FAIL  firewall status failed" ; fi

echo "=== ERROR PATHS ==="
if cat /nope/nope > /tmp/t8 2>&1 ; then echo "FAIL  cat on a missing file succeeded" ; else echo "PASS  cat on a missing file fails" ; fi
if cd /nope/nope ; then echo "FAIL  cd to nowhere succeeded" ; else echo "PASS  cd to a missing directory fails" ; fi
if rm /nope/nope ; then echo "FAIL  rm of nothing succeeded" ; else echo "PASS  rm on a missing file fails" ; fi
if mkdir /proc/nope ; then echo "FAIL  mkdir in /proc succeeded" ; else echo "PASS  mkdir where it cannot fails" ; fi
if df / > /dev/null      ; then echo "PASS  df"      ; else echo "FAIL  df"      ; fi
if free > /dev/null      ; then echo "PASS  free"    ; else echo "FAIL  free"    ; fi
if top -n 1 > /dev/null  ; then echo "PASS  top"     ; else echo "FAIL  top"     ; fi
if ps > /dev/null        ; then echo "PASS  ps"      ; else echo "FAIL  ps"      ; fi
if disk > /dev/null      ; then echo "PASS  disk"    ; else echo "FAIL  disk"    ; fi
if sysinfo > /dev/null   ; then echo "PASS  sysinfo" ; else echo "FAIL  sysinfo" ; fi
if dmesg > /dev/null     ; then echo "PASS  dmesg"   ; else echo "FAIL  dmesg"   ; fi
if tar -c /tmp/t.tar /etc/osname > /dev/null ; then echo "PASS  tar creates" ; else echo "FAIL  tar create" ; fi
if sha256sum /etc/osname > /dev/null ; then echo "PASS  sha256sum" ; else echo "FAIL  sha256sum" ; fi

echo "=== SHELL REDIRECTION ==="
if definitely-not-a-command 2> /tmp/r1 ; then echo "FAIL  unknown command succeeded" ; else echo "PASS  2> captures stderr to a file" ; fi
if grep -q "not found" /tmp/r1 ; then echo "PASS  and the message really landed there" ; else echo "FAIL  2> wrote nothing" ; fi
definitely-not-a-command > /tmp/r2 2>&1
if grep -q "not found" /tmp/r2 ; then echo "PASS  2>&1 merges stderr into stdout" ; else echo "FAIL  2>&1 did not merge" ; fi
echo first > /tmp/r3
definitely-not-a-command 2>> /tmp/r3
if grep -q first /tmp/r3 ; then echo "PASS  2>> appends rather than truncating" ; else echo "FAIL  2>> truncated the file" ; fi

echo "=== LOGGING ==="
if test -f /data/log/messages ; then echo "PASS  the log exists: `wc -l < /data/log/messages` lines" ; else echo "FAIL  no log file" ; fi
if grep -q "init:" /data/log/messages ; then echo "PASS  our own programs reach the log" ; else echo "FAIL  the log has kernel messages only" ; fi

echo "=== TIME AND SLEEP ==="
# sleep 1 through sleep 9 were right and everything longer was not:
# the parser scaled by a thousand once per digit, so "sleep 33" waited
# 3003 seconds. Nothing in the boot path sleeps that long, so the only
# way to see it is to time a two-digit sleep against the clock.
T0=`date -e`
sleep 12
T1=`date -e`
ELAPSED=`calc $T1 - $T0 | head -1`
if test $ELAPSED -ge 11 ; then echo "PASS  sleep 12 waited at least 11s" ; else echo "FAIL  sleep 12 returned after ${ELAPSED}s" ; fi
if test $ELAPSED -le 20 ; then echo "PASS  and not much more (${ELAPSED}s)" ; else echo "FAIL  sleep 12 waited ${ELAPSED}s - the parser is scaling wrongly" ; fi
if test 12 -gt 9 ; then echo "PASS  test compares two-digit numbers" ; else echo "FAIL  test -gt is wrong above nine" ; fi

# The slow one goes last on purpose.
#
# It has to let a process hold a core for thirty seconds before guard is
# entitled to have noticed, and a serial session that gets cut short
# would otherwise take every check after it down too. Put at the end,
# a truncated run still reports everything else.
echo "=== IT DOES NOT DIE ==="
sh -c 'while : ; do : ; done' &
HOG=$!

# Wait for guard rather than assuming how long it takes.
#
# guard counts seconds a process was actually hot, not seconds on the
# clock, so on a busy machine thirty of them take longer than thirty
# seconds to accumulate. A fixed `sleep 33` passed on an idle board and
# failed on a fresh card that was still finishing its first boot - which
# looks exactly like guard being broken.
WAITED=0
while test $WAITED -lt 60 ; do
  if dmesg | grep -q "held a core" ; then break ; fi
  sleep 5
  WAITED=`calc $WAITED + 5 | head -1`
done

if test -d /proc/$HOG ; then echo "PASS  the hog is alive to be judged" ; else echo "FAIL  test hog died on its own" ; fi
if dmesg | grep -q "held a core" ; then echo "PASS  guard noticed the CPU hog after ${WAITED}s" ; else echo "FAIL  a core was held ${WAITED}s with no notice" ; fi
if test $WAITED -lt 60 ; then echo "PASS  break left the loop early" ; else echo "FAIL  the wait loop ran to its limit - break did not work" ; fi
kill -9 $HOG
if definitely-not-a-command > /tmp/t7 2>&1 ; then echo "FAIL  a missing command reported success" ; else echo "PASS  a missing command fails" ; fi
if grep -q "not found" /tmp/t7 ; then echo "PASS  and says so" ; else echo "FAIL  and said: `head -1 /tmp/t7`" ; fi

echo "=== SELFTEST COMPLETE ==="
