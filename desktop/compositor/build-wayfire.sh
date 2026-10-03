#!/bin/bash
# build-wayfire.sh - Debian's wayfire 0.7.4-3+deb12u1 + wayfire-0.7.4-lp.patch
# (README.md).
#
#   desktop/compositor/build-wayfire.sh     -> desktop/compositor/wayfire
#                                              desktop/compositor/libplace.so
#                                              desktop/compositor/libdecoration.so
#
# Same arrangement as build-wlroots.sh and build-sway.sh: the build runs in
# a throwaway overlay of the Debian base tree ($DEB, the image's own root)
# inside a private mount namespace, the build dependencies are
# apt-installed into the overlay's upper layer (kept in $WORK/ovl as a
# cache, CLEAN=1 starts over), and nothing is written to the base. Only the
# wayfire program and the plugins the patch touches (place, where a new
# window goes; decoration, the frame wayfire draws) are rebuilt: the other
# plugins Debian ships (/usr/lib/x86_64-linux-gnu/wayfire) load into it
# unchanged, which is why its exported symbols are checked against
# Debian's program.
#
# Environment:
#   DEB        Debian base tree            (/home/user/kernel-work/deb)
#   WORK       scratch dir, big + fast     (/dev/shm/wayfire-build)
#   CA_BUNDLE  extra CA for apt over HTTPS (/root/.ccr/ca-bundle.crt if present)
#   CLEAN=1    drop the cached overlay and sources first
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
DEB=${DEB:-/home/user/kernel-work/deb}
WORK=${WORK:-/dev/shm/wayfire-build}
CA_BUNDLE=${CA_BUNDLE:-/root/.ccr/ca-bundle.crt}
POOL=https://deb.debian.org/debian/pool/main/w/wayfire
VER=0.7.4
DEBREV=3+deb12u1
PATCH=$HERE/wayfire-$VER-lp.patch
OUT=$HERE/wayfire

# From wayfire_0.7.4-3+deb12u1.dsc (Checksums-Sha256).
declare -A SHA256=(
  [wayfire_$VER.orig.tar.gz]=343093e3b062383d2a6965ca09eb26d0f68cb897d8efeb69f4967679bb128c8e
  [wayfire_$VER.orig-wf-touch.tar.gz]=cc42329f2b13e1feb16d979ee36645f45463067aaab37eb7e10f1e82edab811f
  [wayfire_$VER.orig-wf-utils.tar.gz]=a387fc4761dd34263b456f8b4164ef85914b9f236d0fe3fe0a8dd012adf54632
  [wayfire_$VER-$DEBREV.debian.tar.xz]=587b9499e7e1a24d92ae7dc814fd228e5b42e749b7c88623bfa9815cc84c98f3
)

# debian/control's Build-Depends (doctest is for the tests, not built).
BUILD_DEPS="meson ninja-build pkg-config dpkg-dev cmake libcairo2-dev libdrm-dev
  libevdev-dev libfreetype-dev libgl-dev libglm-dev libinput-dev libjpeg-dev
  libpango1.0-dev libpixman-1-dev libpng-dev libwayland-dev libwf-config-dev
  libwlroots-dev libxkbcommon-dev libxml2-dev wayland-protocols libxcb1-dev"

[ "$(id -u)" = 0 ] || { echo "build-wayfire.sh: needs root (mount, chroot)" >&2; exit 1; }
[ -x "$DEB/usr/bin/wayfire" ] || { echo "build-wayfire.sh: no Debian base with wayfire at $DEB" >&2; exit 1; }
[ -f "$PATCH" ] || { echo "build-wayfire.sh: missing $PATCH" >&2; exit 1; }

if [ "${CLEAN:-0}" = 1 ]; then
    rm -rf "$WORK/ovl" "$WORK/build"
fi
mkdir -p "$WORK/dl" "$WORK/ovl/up" "$WORK/ovl/wk" "$WORK/ovl/root"

# ── sources: Debian's, checked against the .dsc ──
for f in "${!SHA256[@]}"; do
    if ! echo "${SHA256[$f]}  $WORK/dl/$f" | sha256sum -c --quiet 2>/dev/null; then
        curl -fsSL ${CA_BUNDLE:+--cacert "$CA_BUNDLE"} -o "$WORK/dl/$f" "$POOL/$f"
        echo "${SHA256[$f]}  $WORK/dl/$f" | sha256sum -c --quiet
    fi
done

SRC=$WORK/build/wayfire-$VER
rm -rf "$WORK/build"
mkdir -p "$WORK/build"
tar -C "$WORK/build" -xf "$WORK/dl/wayfire_$VER.orig.tar.gz"
tar -C "$SRC" -xf "$WORK/dl/wayfire_$VER-$DEBREV.debian.tar.xz"
# What dpkg-source -x and debian/rules do with the two extra tarballs: they
# are the wf-touch and wf-utils subprojects.
for sub in wf-touch wf-utils; do
    mkdir -p "$SRC/subprojects/$sub"
    tar -C "$SRC/subprojects/$sub" --strip-components=1 -xf "$WORK/dl/wayfire_$VER.orig-$sub.tar.gz"
done
while read -r p _; do
    case $p in ''|'#'*) continue ;; esac
    patch -d "$SRC" -p1 -s --no-backup-if-mismatch < "$SRC/debian/patches/$p"
done < "$SRC/debian/patches/series"
patch -d "$SRC" -p1 -s --no-backup-if-mismatch < "$PATCH"

cat > "$WORK/build/inside.sh" <<'INSIDE'
#!/bin/sh
set -eu
export DEBIAN_FRONTEND=noninteractive LC_ALL=C.UTF-8
APT_OPTS="-o Acquire::Retries=3"
[ -f /etc/ssl/lp-build-ca.crt ] && APT_OPTS="$APT_OPTS -o Acquire::https::CAInfo=/etc/ssl/lp-build-ca.crt"
missing=
for p in $BUILD_DEPS; do
    dpkg-query -W -f='${Status}\n' "$p" 2>/dev/null | grep -q '^install ok installed' || missing="$missing $p"
done
if [ -n "$missing" ]; then
    apt-get $APT_OPTS update -qq
    apt-get $APT_OPTS install -y -qq --no-install-recommends $missing
fi
cd /build
# debian/rules: DEB_BUILD_MAINT_OPTIONS=hardening=+all, dh_auto_configure
# (meson, buildtype plain, multiarch libdir).
eval "$(DEB_BUILD_MAINT_OPTIONS=hardening=+all dpkg-buildflags --export=sh)"
meson setup b wayfire-0.7.4 --wrap-mode=nodownload --buildtype=plain --prefix=/usr \
    --sysconfdir=/etc --localstatedir=/var --libdir=lib/x86_64-linux-gnu \
    -Duse_system_wlroots=enabled -Duse_system_wfconfig=enabled -Dxwayland=enabled
ninja -C b src/wayfire plugins/single_plugins/libplace.so plugins/decor/libdecoration.so
cp b/src/wayfire /build/wayfire.out
cp b/plugins/single_plugins/libplace.so /build/libplace.out
cp b/plugins/decor/libdecoration.so /build/libdecoration.out
# dh_strip
strip --remove-section=.comment --remove-section=.note /build/wayfire.out
strip --remove-section=.comment --remove-section=.note --strip-unneeded /build/libplace.out
strip --remove-section=.comment --remove-section=.note --strip-unneeded /build/libdecoration.out
INSIDE
chmod +x "$WORK/build/inside.sh"

R=$WORK/ovl/root
unshare -m bash -euc "
mount --make-rprivate /
mount -t overlay overlay -o lowerdir=$DEB,upperdir=$WORK/ovl/up,workdir=$WORK/ovl/wk $R
mount -t proc proc $R/proc
mount --rbind /dev $R/dev
mkdir -p $R/build
mount --bind $WORK/build $R/build
# The base's /bin/sh is LP's own shell: dash stands in for it here. (A
# base whose /bin is still empty, before LP's own tools go in, gets
# Debian's merged-/usr link in the overlay.)
if [ -d $R/bin ] && [ ! -L $R/bin ] && [ -z \"\$(ls -A $R/bin)\" ]; then
    rmdir $R/bin
    ln -s usr/bin $R/bin
fi
mount --bind $R/usr/bin/dash $R/bin/sh
cp /etc/resolv.conf $R/etc/resolv.conf
mkdir -p $R/etc/ssl
if [ -f '$CA_BUNDLE' ]; then cp '$CA_BUNDLE' $R/etc/ssl/lp-build-ca.crt; fi
chroot $R /usr/bin/env -i PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin HOME=/root \
    https_proxy='${https_proxy:-${HTTPS_PROXY:-}}' HTTPS_PROXY='${HTTPS_PROXY:-${https_proxy:-}}' \
    BUILD_DEPS='$(echo $BUILD_DEPS)' /usr/bin/dash /build/inside.sh
"

# ── what the plugins load into: every symbol Debian's program exports ──
# The patch adds one (input_method_relay::grab_for - the core exports all
# of its functions) and must take none away.
ORIG=$DEB/usr/bin/wayfire
nm -D --defined-only "$ORIG" | awk '{print $2, $3}' | sort > "$WORK/build/syms.orig"
nm -D --defined-only "$WORK/build/wayfire.out" | awk '{print $2, $3}' | sort > "$WORK/build/syms.new"
gone=$(comm -23 "$WORK/build/syms.orig" "$WORK/build/syms.new")
if [ -n "$gone" ]; then
    echo "build-wayfire.sh: exported by $ORIG but not by the build:" >&2
    echo "$gone" >&2
    exit 1
fi
added=$(comm -13 "$WORK/build/syms.orig" "$WORK/build/syms.new")
[ -z "$added" ] || echo "build-wayfire.sh: added: $added"
objdump -p "$ORIG" | awk '/NEEDED/ {print $2}' > "$WORK/build/needed.orig"
objdump -p "$WORK/build/wayfire.out" | awk '/NEEDED/ {print $2}' > "$WORK/build/needed.new"
if ! diff -u "$WORK/build/needed.orig" "$WORK/build/needed.new"; then
    echo "build-wayfire.sh: NEEDED differs from $ORIG" >&2
    exit 1
fi

# The plugins export what Debian's do (the plugin entry points).
for p in place decoration; do
    PORIG=$DEB/usr/lib/x86_64-linux-gnu/wayfire/lib$p.so
    if ! diff <(nm -D --defined-only "$PORIG" | awk '{print $3}' | sort) \
              <(nm -D --defined-only "$WORK/build/lib$p.out" | awk '{print $3}' | sort) >&2; then
        echo "build-wayfire.sh: lib$p.so exports differ from $PORIG" >&2
        exit 1
    fi
done

install -m 755 "$WORK/build/wayfire.out" "$OUT"
install -m 644 "$WORK/build/libplace.out" "$HERE/libplace.so"
install -m 644 "$WORK/build/libdecoration.out" "$HERE/libdecoration.so"
echo "build-wayfire.sh: $OUT ($(wc -l < "$WORK/build/syms.orig") of Debian's exported symbols, all there)"
sha256sum "$OUT" "$HERE/libplace.so" "$HERE/libdecoration.so"
