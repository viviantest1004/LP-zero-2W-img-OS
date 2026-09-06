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
# LP_INPLACE=1 lays our userland straight into the Debian base instead
# of copying it somewhere first.
#
# The copy is the safer shape - the base stays pristine and a build can
# be repeated from it - and it costs a second copy of the whole tree.
# With a browser in the base that is 1.7GB twice over, and on a machine
# with three gigabytes free the build fills the disk somewhere in the
# middle and leaves a half-written image. In place, the peak is the
# image alone.
if [[ "${LP_INPLACE:-0}" == "1" ]]; then
    OUT="$DEB"
else
    OUT="${LP_DESKTOP_ROOT:-${LPZERO_WORK}/desktop-root}"
fi

log()  { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ -d "$DEB"  ]] || die "데비안 베이스가 없습니다: $DEB"
[[ -d "$OURS" ]] || die "우리 rootfs 가 없습니다: $OURS"

# ── 1. the toolchain stays ───────────────────────────────────────
#
# 이 단계는 원래 gcc 와 GTK 개발 파일을 지웠다. 두 가지 이유로 그만뒀다.
#
# 첫째, 지우는 자리가 틀렸다. 이 스크립트는 in-place 로 도는 일이 많고
# (LP_INPLACE), 그러면 지워지는 것은 이미지가 아니라 **빌드에 쓰는
# chroot** 다. 앱을 하나 고쳐서 다시 빌드하려고 하면 컴파일러가 없다.
# 이미지의 GTK 가 4.8 이고 빌드 호스트의 GTK 가 4.14 라서 반드시
# chroot 안에서 빌드해야 하는데, 그 chroot 를 매 실행마다 스스로
# 부수고 있었던 셈이다. 실제로 한 번 그렇게 막혔다.
#
# 둘째, 지울 이유가 약하다. 우분투급으로 쓰겠다는 기계에서 C 컴파일러가
# 있는 것은 흠이 아니라 기능이고, 4GB 이미지에서 200MB 다.
#
# 그래도 빼고 싶으면 LP_STRIP_DEV=1. 그때는 이 chroot 로 앱을 다시
# 빌드할 수 없게 된다는 것을 알고 쓰는 것이다.
step "베이스 정리"
if [[ "${LP_STRIP_DEV:-0}" == "1" && -x "$DEB/usr/bin/dpkg" ]]; then
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
    log "빌드 도구를 뺐습니다 (LP_STRIP_DEV=1)"
else
    log "빌드 도구를 남겨 둡니다 - 기기에서 컴파일할 수 있습니다"
fi
log "$(du -sh --exclude=proc --exclude=sys --exclude=dev "$DEB" 2>/dev/null | cut -f1)"

# ── 2. the base becomes the root ─────────────────────────────────
step "루트 만들기"
if [[ "$OUT" != "$DEB" ]]; then
rm -rf "$OUT"
mkdir -p "$OUT"
# -a keeps modes, owners and symlinks; the base is full of both and a
# copy that flattens them is a base that will not run.
tar -C "$DEB" --exclude=./proc/\* --exclude=./sys/\* --exclude=./dev/\* \
    --exclude=./setup.sh -cf - . | tar -C "$OUT" -xf -
rm -rf "$OUT/proc" "$OUT/sys"
log "데비안 베이스 놓임"
else
log "베이스 위에 그 자리에서 (LP_INPLACE)"
fi
mkdir -p "$OUT/proc" "$OUT/sys"

# ── 3. our /bin over theirs ──────────────────────────────────────
#
# Debian's /bin is a symlink into /usr/bin. Replacing the symlink with a
# real directory is what puts our shell where init looks for it, and
# leaves every Debian program reachable at its /usr/bin path.
step "우리 유저랜드"
# 심볼릭 링크일 때만 지운다. 두 번째 빌드에서는 이미 진짜 디렉터리라
# 그냥 rm -f 하면 "Is a directory" 로 멈춘다 - 그리고 여기서 멈추면
# 루트가 반쯤 만들어진 채로 남는다.
[[ -L "$OUT/bin" ]] && rm -f "$OUT/bin"
mkdir -p "$OUT/bin"
# --remove-destination 를 반드시 붙인다.
#
# 데비안의 /bin/sh 는 /usr/bin/dash 를 가리키는 절대 심볼릭 링크이고,
# apt 가 무언가를 다시 설치할 때마다 그 링크가 되살아난다. cp 는
# 기본적으로 링크를 따라가서 쓰므로, 그 상태에서 우리 sh 를 덮으면
# 목적지가 chroot 안의 dash 가 아니라 **빌드 호스트의** /usr/bin/dash
# 가 된다. 이번에는 그 파일이 실행 중이라 "Text file busy" 로 멈췄고,
# 그래서 들켰다. 멈추지 않았으면 빌드 머신의 셸을 갈아 끼웠을 것이다.
cp -a --remove-destination "$OURS/bin/." "$OUT/bin/"
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
for f in rc services osname motd boot-tools.sha256 profile \
         firewall.conf beacon.conf authorized_keys wpa_supplicant.conf; do
    [[ -f "$OURS/etc/$f" ]] && cp -a "$OURS/etc/$f" "$OUT/etc/$f"
done
log "init 과 /etc"

# ── /etc/passwd 와 /etc/group ────────────────────────────────────
#
# 이 둘만은 우리 것으로 덮지 않는다. 덮으면 어떻게 되는지 한 번
# 겪었다: 데비안의 시스템 계정 스무 개가 통째로 사라지고, 그 다음
# apt 가
#
#   unknown system group 'messagebus' in statoverride file
#
# 하면서 dpkg 를 통째로 멈춘다. 패키지를 하나도 더 설치할 수 없고,
# 이미 풀어 놓은 것도 설정되지 않은 채로 남는다. 계정 파일은 데비안이
# 관리하는 상태이지 우리가 쓸 설정이 아니다.
#
# 그래서 base-passwd 가 들고 있는 원본을 바닥에 깔고, 우리 계정만
# 얹는다. root 의 셸은 우리 셸이어야 하고, uid 1000 은 데스크탑이
# 쓰는 계정이다.
if [[ -f "$OUT/usr/share/base-passwd/passwd.master" ]]; then
    cp -a "$OUT/usr/share/base-passwd/passwd.master" "$OUT/etc/passwd"
    cp -a "$OUT/usr/share/base-passwd/group.master"  "$OUT/etc/group"
fi
sed -i 's|^root:[^:]*:0:0:root:/root:.*$|root:x:0:0:root:/root:/bin/sh|' \
    "$OUT/etc/passwd"

# 설치한 패키지들이 만든 계정. postinst 가 adduser 로 만드는 것들이고,
# 이미지에서 그 계정들이 없으면 dbus 도 polkit 도 시작하지 못한다.
add_account() {
    grep -q "^$1:" "$OUT/etc/passwd" || printf '%s\n' "$2" >> "$OUT/etc/passwd"
    grep -q "^$1:" "$OUT/etc/group"  || printf '%s\n' "$3" >> "$OUT/etc/group"
}
add_account messagebus \
    'messagebus:x:100:101::/nonexistent:/usr/sbin/nologin' \
    'messagebus:x:101:'
add_account _apt \
    '_apt:x:101:65534::/nonexistent:/usr/sbin/nologin' \
    '_apt:x:102:'
add_account polkitd \
    'polkitd:x:102:103:polkitd:/var/lib/polkit-1:/usr/sbin/nologin' \
    'polkitd:x:103:'

# 데스크탑 계정. 이름이 lp 가 아니라 user 인 이유는 session-run 에
# 적어 두었다 - 데비안에 이미 lp 라는 uid 7 계정이 있다.
grep -q '^user:' "$OUT/etc/passwd" || \
    printf 'user:x:1000:1000:LP:/home/user:/bin/sh\n' >> "$OUT/etc/passwd"
grep -q '^user:' "$OUT/etc/group" || \
    printf 'user:x:1000:\n' >> "$OUT/etc/group"

# 비밀번호 없음. 이 기계는 콘솔에 앉은 사람이 곧 주인이라는 전제로
# 시작했고, 그 전제를 바꾸는 것은 설정 앱의 '사용자' 항목이 할 일이다.
printf 'root::20000:0:99999:7:::\nuser::20000:0:99999:7:::\n' > "$OUT/etc/shadow"
chmod 640 "$OUT/etc/shadow"
log "계정: 데비안 것 위에 root 와 user"

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
# 이 기계의 기본 언어는 영어다. 데스크탑 세션은 session-run 이
# ~/.config/lp/locale 을 읽어 계정마다 따로 정하고, 콘솔은 이 값을
# 쓴다. 둘이 다른 답을 하면 안 되므로 여기도 영어로 둔다.
export LANG=en_US.UTF-8

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

# The bar and the launcher. Both are configured rather than written -
# waybar and fuzzel already do what the spec's top bar and search panel
# describe, and the part worth our time is the layout and the colours,
# which are entirely in these two files.
if [[ -d "${REPO_ROOT}/desktop/bar" ]]; then
    for h in "$OUT/root" "$OUT/home/user"; do
        mkdir -p "$h/.config/waybar" "$h/.config/fuzzel"
        cp -a "${REPO_ROOT}/desktop/bar/config.jsonc" "$h/.config/waybar/config" 2>/dev/null || true
        cp -a "${REPO_ROOT}/desktop/bar/style.css"    "$h/.config/waybar/style.css" 2>/dev/null || true
        cp -a "${REPO_ROOT}/desktop/bar/fuzzel.ini"   "$h/.config/fuzzel/fuzzel.ini" 2>/dev/null || true
    done
    # 상단바가 부르는 작은 스크립트. /usr/local/bin 이라 PATH 에 있다.
    if [[ -f "${REPO_ROOT}/desktop/bar/lp-bar-label" ]]; then
        mkdir -p "$OUT/usr/local/bin"
        cp -a "${REPO_ROOT}/desktop/bar/lp-bar-label" "$OUT/usr/local/bin/"
        chmod +x "$OUT/usr/local/bin/lp-bar-label"
    fi
    log "상단바와 런처"
fi

# 세션과 컴포지터 설정.
if [[ -f "${REPO_ROOT}/desktop/session/sway.config" ]]; then
    for h in "$OUT/root" "$OUT/home/user"; do
        mkdir -p "$h/.config/sway" "$h/.config/gtk-4.0" "$h/.config/gtk-3.0"
        cp -a "${REPO_ROOT}/desktop/session/sway.config" "$h/.config/sway/config"
        # 설정 앱이 키보드 배열을 여기에 쓴다. sway 는 없는 파일을
        # include 하면 오류를 찍으므로 빈 파일을 미리 만들어 둔다.
        [[ -f "$h/.config/sway/input.conf" ]] || \
            printf '# 설정 > 키보드 에서 배열을 고르면 여기에 적힙니다.\n' \
                > "$h/.config/sway/input.conf"
        cp -a "${REPO_ROOT}/desktop/theme/gtk-4.0/gtk.css" "$h/.config/gtk-4.0/gtk.css" 2>/dev/null || true
        [[ -f "${REPO_ROOT}/desktop/theme/settings.ini" ]] && {
            cp -a "${REPO_ROOT}/desktop/theme/settings.ini" "$h/.config/gtk-4.0/settings.ini"
            cp -a "${REPO_ROOT}/desktop/theme/settings.ini" "$h/.config/gtk-3.0/settings.ini"
        }
    done
    cp -a "${REPO_ROOT}/desktop/session/session-run" "$OUT/bin/session-run"
    chmod +x "$OUT/bin/session-run"
    mkdir -p "$OUT/usr/local/bin"
    cp -a "${REPO_ROOT}/desktop/session/lp-audio-start" \
          "$OUT/usr/local/bin/lp-audio-start"
    chmod +x "$OUT/usr/local/bin/lp-audio-start"
    cp -a "${REPO_ROOT}/desktop/session/lp-idle" "$OUT/usr/local/bin/lp-idle"
    chmod +x "$OUT/usr/local/bin/lp-idle"
    log "sway 설정"
fi

# 터미널. 명세서 2-4 - foot 을 쓰되 기본값을 이 OS 로 맞춘다.
if [[ -f "${REPO_ROOT}/desktop/terminal/foot.ini" ]]; then
    mkdir -p "$OUT/etc/xdg/foot"
    cp -a "${REPO_ROOT}/desktop/terminal/foot.ini" "$OUT/etc/xdg/foot/foot.ini"
    log "터미널"
fi

# ── 글꼴 ─────────────────────────────────────────────────────────
#
# Pretendard 와 D2Coding. 데비안에 없어서 받아 온다. local.conf 가
# 없으면 GTK 가 sans-serif 를 물었을 때 DejaVu 가 나오고, 그러면
# 테마가 무슨 글꼴을 적어 두었든 화면은 데비안 기본값으로 뜬다.
if [[ -x "${REPO_ROOT}/tools/fetch-fonts.sh" ]]; then
    "${REPO_ROOT}/tools/fetch-fonts.sh" "$OUT/usr/share/fonts/truetype" \
        || log "글꼴을 받지 못했습니다 - Noto 로 떨어집니다"
fi
if [[ -f "${REPO_ROOT}/desktop/fonts/local.conf" ]]; then
    mkdir -p "$OUT/etc/fonts"
    cp -a "${REPO_ROOT}/desktop/fonts/local.conf" "$OUT/etc/fonts/local.conf"
fi

# ── 우리가 만든 앱 ───────────────────────────────────────────────
#
# 명세서 2절이 '시스템 앱' 이라고 부르는 것들. apt 로 가져온 앱들과
# 달리 이 셋은 이 OS 의 것이고, 그래서 /usr/local/bin 에 들어간다 -
# 패키지 관리자가 건드리지 않는 자리다.
mkdir -p "$OUT/usr/local/bin" "$OUT/usr/local/share/applications"
for app in files settings tasks shot quick; do
    bin="${REPO_ROOT}/desktop/${app}/lp-${app}"
    [[ "$app" == files ]] && bin="${REPO_ROOT}/desktop/files/lp-files"
    if [[ -x "$bin" ]]; then
        cp -a "$bin" "$OUT/usr/local/bin/$(basename "$bin")"
        log "$(basename "$bin")"
    fi
    d="${REPO_ROOT}/desktop/${app}/$(basename "$bin").desktop"
    [[ -f "$d" ]] && cp -a "$d" "$OUT/usr/local/share/applications/"
done

# The places the file manager and the shell expect to exist. Making them
# here rather than at first boot means the sidebar never shows a row
# that leads nowhere.
# ── 홈의 표준 폴더 ───────────────────────────────────────────────
#
# 디스크 위의 이름은 영어다. 화면에 보이는 이름은 그것과 별개이고,
# 파일 관리자가 로케일에 따라 '다운로드' 라고 찍는다.
#
# 처음에는 폴더 자체를 한국어로 만들었고, 기계가 한 언어만 쓸 때는
# 그래도 됐다. 두 언어를 쓰기 시작하면 그 순간 깨진다 - 영어 세션은
# ~/Downloads 를 만들어 쓰고 한국어 세션은 ~/다운로드 를 만들어 써서,
# 같은 사람의 파일이 그때그때 다른 폴더에 들어간다.
#
# user-dirs.dirs 는 XDG 의 표준 파일이고, GLib 의
# g_get_user_special_dir 이 이것을 읽는다. 여기에 적어 두면 앱이
# "다운로드 폴더" 를 물었을 때 전부 같은 곳을 답한다.
for h in "$OUT/root" "$OUT/home/user"; do
    mkdir -p "$h/Desktop" "$h/Documents" "$h/Downloads" "$h/Music" \
             "$h/Pictures" "$h/Videos" \
             "$h/.local/share/Trash/files" \
             "$h/.local/share/Trash/info" "$h/.cache" "$h/.config/lp"
    cat > "$h/.config/user-dirs.dirs" <<'DIRS'
# XDG 표준 폴더. 이름은 영어이고, 화면에 보이는 이름은 앱이 정한다.
XDG_DESKTOP_DIR="$HOME/Desktop"
XDG_DOCUMENTS_DIR="$HOME/Documents"
XDG_DOWNLOAD_DIR="$HOME/Downloads"
XDG_MUSIC_DIR="$HOME/Music"
XDG_PICTURES_DIR="$HOME/Pictures"
XDG_VIDEOS_DIR="$HOME/Videos"
XDG_TEMPLATES_DIR="$HOME"
XDG_PUBLICSHARE_DIR="$HOME"
DIRS
    printf 'en_US.UTF-8\n' > "$h/.config/lp/locale"
done
mkdir -p "$OUT/run/user/0" "$OUT/run/user/1000" \
         "$OUT/data" "$OUT/boot" "$OUT/tmp"
chmod 700 "$OUT/run/user/0" "$OUT/run/user/1000"
chown -R 1000:1000 "$OUT/home/user" "$OUT/run/user/1000"
chmod 1777 "$OUT/tmp"

# ── /bin/sh 는 우리 셸이어야 한다 ────────────────────────────────
#
# apt 를 돌리는 동안에는 이것을 dash 로 바꿔 둔다. dpkg 가 유지보수
# 스크립트를 /bin/sh 로 돌리고, 그 스크립트들이 우리 셸에 없던 문법을
# 쓰기 때문이다 (case 는 이제 있다). 바꿔 둔 채로 이미지를 만들면
# 부팅한 기계의 셸이 dash 가 되므로, 여기서 되돌린다.
if [[ -f "$OUT/bin/sh.lp" ]]; then
    mv -f "$OUT/bin/sh.lp" "$OUT/bin/sh"
    log "/bin/sh 를 우리 셸로 되돌림"
fi

# ── 이 이미지에서 setuid 인 단 하나 ────────────────────────────
#
# 세션이 uid 1000 으로 도는 한, 거기 앉은 사람은 기계를 끌 수 없다 -
# poweroff 는 init 에 신호를 보내고 그것은 root 의 일이기 때문이다.
# 다른 배포판은 logind 와 polkit 으로 그 구멍을 메우는데, 버튼 하나를
# 얻자고 그 둘을 들이는 것은 남는 장사가 아니다.
#
# 대신 lp-power 하나만 setuid 로 둔다. 낱말 하나만 받고, 파일도 환경
# 변수도 읽지 않고, 아무것도 exec 하지 않는다.
if [[ -f "$OUT/bin/lp-power" ]]; then
    chown 0:0 "$OUT/bin/lp-power"
    chmod 4755 "$OUT/bin/lp-power"
    log "lp-power (setuid - 이 이미지에서 유일하다)"
fi

# 데스크탑을 띄우는 것은 /etc/rc 가 직접 한다 - 여기서 한 줄을
# 덧붙이던 것을 그만두었다. 덧붙일지 말지를 grep 으로 정했는데,
# rc 안의 주석 하나가 그 낱말을 담고 있어서 검사가 늘 참이 되었고,
# 두 번째 빌드부터는 데스크탑이 뜨지 않았다. 조건이 파일 내용에
# 달려 있으면 그 파일에 무엇이 적히든 조건이 흔들린다.

step "결과"
SZ=$(du -sh --exclude=proc --exclude=sys "$OUT" 2>/dev/null | cut -f1)
log "$SZ  $OUT"
log ""
log "이미지로:  LP_ROOTFS_OVERRIDE=$OUT ./tools/mkdisk.sh"
