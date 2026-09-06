#!/usr/bin/env bash
#
# mkdesktop.sh - fold the Debian base and our own userland into one root.
#
# The desktop needs two things that cannot both be built the same way.
#
# A Wayland compositor is a hundred thousand lines against glibc, mesa,
# libinput and libxkbcommon. It is not going to be rewritten here and it
# is not going to link against a hand-written libc. So the root carries
# a Debian bookworm base underneath: glibc, GTK, wayfire, foot, fonts.
#
# But the machine is still this one. Its init is ours, its shell is
# ours, its hundred and thirty commands are ours, and its /etc/rc is
# what brings the system up. That is what the project is; a Debian
# system with our kernel on it would be a Debian system.
#
# ── Who owns /bin ──
#
# init.c hardcodes /bin/sh, /bin/splash and /bin/<service>. So /bin is
# ours, and Debian's binaries stay in /usr/bin where usrmerge put them
# anyway. PATH is /bin first: `ls` is ours, `wayfire` is Debian's.
#
# The cost, stated plainly: anything that calls system() gets our shell
# rather than dash. Our shell has pipes, redirection, if, while, for and
# functions, which covers what a GTK application asks of it, but it is
# not POSIX-complete and something will eventually find the gap. The
# alternative - our commands somewhere else and Debian's /bin intact -
# would mean init could not find its own shell, and a machine whose
# identity is its userland would boot into somebody else's.
#
#   ./tools/mkdesktop.sh          builds the merged root
#   LP_DEB=/path ./tools/...      a Debian base somewhere else
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && cd .. && pwd)"
source "${REPO_ROOT}/tools/common.sh"

DEB="${LP_DEB:-/home/user/kernel-work/deb}"
OURS="${REPO_ROOT}/userland/rootfs-amd64"
OUT="${LP_DESKTOP_ROOT:-${LPZERO_WORK}/desktop-root}"

log()  { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ -d "$DEB"  ]] || die "데비안 베이스가 없습니다: $DEB"
[[ -d "$OURS" ]] || die "우리 rootfs 가 없습니다: $OURS"

# ── 1. strip the base ────────────────────────────────────────────
#
# The compiler and headers were installed to build the file manager
# against the same GTK the image ships, which is the only way to know it
# links. They have no business on the machine afterwards.
step "베이스에서 빌드 도구 빼기"
if [[ -x "$DEB/usr/bin/dpkg" ]]; then
    for m in proc sys dev dev/pts; do
        mkdir -p "$DEB/$m"; mount --bind "/$m" "$DEB/$m" 2>/dev/null || true
    done
    chroot "$DEB" /bin/sh -c '
        export DEBIAN_FRONTEND=noninteractive
        apt-get -y purge libgtk-4-dev gcc libc6-dev pkg-config make \
            >/dev/null 2>&1 || true
        apt-get -y autoremove --purge >/dev/null 2>&1 || true
        apt-get clean
    ' 2>/dev/null || log "정리를 건너뜁니다"
    for m in dev/pts dev sys proc; do umount "$DEB/$m" 2>/dev/null || true; done
fi
log "$(du -sh --exclude=proc --exclude=sys --exclude=dev "$DEB" 2>/dev/null | cut -f1)"

# ── 2. the base becomes the root ─────────────────────────────────
step "루트 만들기"
rm -rf "$OUT"
mkdir -p "$OUT"
# -a keeps modes, owners and symlinks; the base is full of both and a
# copy that flattens them is a base that will not run.
tar -C "$DEB" --exclude=./proc/\* --exclude=./sys/\* --exclude=./dev/\* \
    --exclude=./setup.sh -cf - . | tar -C "$OUT" -xf -
rm -rf "$OUT/proc" "$OUT/sys"; mkdir -p "$OUT/proc" "$OUT/sys"
log "데비안 베이스 놓임"

# ── 3. our /bin over theirs ──────────────────────────────────────
#
# Debian's /bin is a symlink into /usr/bin. Replacing the symlink with a
# real directory is what puts our shell where init looks for it, and
# leaves every Debian program reachable at its /usr/bin path.
step "우리 유저랜드"
rm -f "$OUT/bin"
mkdir -p "$OUT/bin"
cp -a "$OURS/bin/." "$OUT/bin/"
log "$(ls "$OUT/bin" | wc -l)개 명령"

# init, and the files that decide how the machine comes up.
# /sbin/init has to be ours, and the reason is not tidiness.
#
# preinit looks for an init in order: /sbin/init, /bin/init, /init. The
# Debian base ships /sbin/init as a symlink to systemd, so preinit finds
# that one first and execs it - and a systemd that was never configured
# for this machine either dies or hangs before anything reaches the
# console. The boot stops one line after "root is /dev/vda2" with no
# explanation, which is the worst kind of failure to be handed.
#
# So the symlink is replaced. systemd stays on disk where a package that
# wants it can still find it; nothing starts it.
rm -f "$OUT/sbin/init" "$OUT/init"
mkdir -p "$OUT/sbin"
cp -a "$OUT/bin/init" "$OUT/sbin/init" 2>/dev/null || \
    cp -a "$OURS/bin/init" "$OUT/sbin/init"
ln -sf bin/init "$OUT/init"
for f in rc services passwd group osname motd boot-tools.sha256 profile \
         firewall.conf beacon.conf authorized_keys wpa_supplicant.conf; do
    [[ -f "$OURS/etc/$f" ]] && cp -a "$OURS/etc/$f" "$OUT/etc/$f"
done
log "init 과 /etc"

# Everything else of ours that is not /bin or /etc - /usr/share, /opt.
for d in usr opt srv var lib; do
    [[ -d "$OURS/$d" ]] && cp -an "$OURS/$d/." "$OUT/$d/" 2>/dev/null || true
done

# PATH. Ours first, or `ls` is GNU ls and the machine stops being this
# one. /etc/profile is read by our shell at login.
cat > "$OUT/etc/profile" <<'PROFILE'
# Read by our shell at login.
#
# /bin before /usr/bin, deliberately. /bin is this system's own hundred
# and thirty commands and /usr/bin is the Debian base underneath it, so
# `ls` and `grep` are ours and `wayfire` and `foot` are theirs. Putting
# the base first would leave a machine that boots our kernel and our
# init into somebody else's userland.
export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin
export HOME=/root
export TERM=linux
export PAGER=more
export EDITOR=edit
export LANG=ko_KR.UTF-8

# Where the compositor and everything it talks to put their sockets.
# Nothing on this machine creates it - there is no logind - so it is
# made here, before anything can want it.
export XDG_RUNTIME_DIR=/run/user/0
PROFILE
log "PATH: /bin 먼저"

# ── 4. the desktop ───────────────────────────────────────────────
step "데스크탑"
if [[ -d "${REPO_ROOT}/desktop/theme" ]]; then
    mkdir -p "$OUT/usr/share/themes/LP/gtk-4.0" \
             "$OUT/usr/share/themes/LP/gtk-3.0" \
             "$OUT/etc/xdg/gtk-4.0" "$OUT/root/.config/gtk-4.0"
    cp -a "${REPO_ROOT}/desktop/theme/gtk-4.0/gtk.css" \
          "$OUT/usr/share/themes/LP/gtk-4.0/" 2>/dev/null || true
    cp -a "${REPO_ROOT}/desktop/theme/gtk-3.0/gtk.css" \
          "$OUT/usr/share/themes/LP/gtk-3.0/" 2>/dev/null || true
    # GTK 4 reads ~/.config/gtk-4.0/gtk.css directly and does not look
    # up a theme by name for CSS the way GTK 3 did, so the file is put
    # where it will actually be read rather than only where it belongs.
    cp -a "${REPO_ROOT}/desktop/theme/gtk-4.0/gtk.css" \
          "$OUT/root/.config/gtk-4.0/gtk.css" 2>/dev/null || true
    [[ -f "${REPO_ROOT}/desktop/theme/settings.ini" ]] && {
        mkdir -p "$OUT/root/.config/gtk-4.0" "$OUT/root/.config/gtk-3.0"
        cp -a "${REPO_ROOT}/desktop/theme/settings.ini" "$OUT/root/.config/gtk-4.0/"
        cp -a "${REPO_ROOT}/desktop/theme/settings.ini" "$OUT/root/.config/gtk-3.0/"
    }
    log "테마"
fi

if [[ -d "${REPO_ROOT}/desktop/session" ]]; then
    mkdir -p "$OUT/root/.config/wayfire" "$OUT/usr/local/bin"
    [[ -f "${REPO_ROOT}/desktop/session/wayfire.ini" ]] && \
        cp -a "${REPO_ROOT}/desktop/session/wayfire.ini" "$OUT/root/.config/wayfire.ini"
    [[ -f "${REPO_ROOT}/desktop/session/start-desktop" ]] && {
        cp -a "${REPO_ROOT}/desktop/session/start-desktop" "$OUT/bin/start-desktop"
        chmod +x "$OUT/bin/start-desktop"
    }
    log "세션"
fi

if [[ -x "${REPO_ROOT}/desktop/files/lp-files" ]]; then
    cp -a "${REPO_ROOT}/desktop/files/lp-files" "$OUT/usr/local/bin/lp-files"
    log "파일 매니저"
fi

# The places the file manager and the shell expect to exist. Making them
# here rather than at first boot means the sidebar never shows a row
# that leads nowhere.
mkdir -p "$OUT/root/문서" "$OUT/root/다운로드" "$OUT/root/사진" \
         "$OUT/root/.local/share/Trash/files" \
         "$OUT/run/user/0" "$OUT/data" "$OUT/boot" "$OUT/tmp"
chmod 700 "$OUT/run/user/0"
chmod 1777 "$OUT/tmp"

step "결과"
SZ=$(du -sh --exclude=proc --exclude=sys "$OUT" 2>/dev/null | cut -f1)
log "$SZ  $OUT"
log ""
log "이미지로:  LP_ROOTFS_OVERRIDE=$OUT ./tools/mkdisk.sh"
