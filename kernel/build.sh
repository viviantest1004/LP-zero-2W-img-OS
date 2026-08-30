#!/usr/bin/env bash
#
# build.sh - 최소 리눅스 커널을 빌드한다.
#
# 공식 bcm2711_defconfig 에서 출발해 lp-zero.config 조각을 덮어쓴다.
# 처음부터(tinyconfig) 쌓아올리는 방법도 있지만, 부팅에 꼭 필요한 옵션을
# 하나씩 찾아내느라 시간이 훨씬 많이 든다. 검증된 defconfig 에서 깎는 쪽이
# 확실하다.
#
# 우리 유저랜드(userland/rootfs)를 커널 이미지에 내장하므로 별도의
# initramfs 파일이 필요 없다 - 커널 하나만 SD 에 넣으면 부팅된다.
#
# 환경변수:
#   LINUX_SRC   커널 소스 경로 (기본 /home/user/kernel-work/linux)
#   JOBS        병렬 빌드 수 (기본 nproc)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LINUX_SRC="${LINUX_SRC:-/home/user/kernel-work/linux}"
BUILD_DIR="${BUILD_DIR:-/home/user/kernel-work/build}"
ROOTFS="${REPO_ROOT}/userland/rootfs"
FRAGMENT="${REPO_ROOT}/kernel/lp-zero.config"
OUT_DIR="${REPO_ROOT}/kernel/out"

ARCH=arm64
CROSS=aarch64-linux-gnu-
JOBS="${JOBS:-$(nproc)}"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }

[[ -d "$LINUX_SRC" ]]  || die "커널 소스가 없습니다: $LINUX_SRC"
[[ -f "$FRAGMENT" ]]   || die "설정 조각이 없습니다: $FRAGMENT"
command -v "${CROSS}gcc" >/dev/null || die "${CROSS}gcc 가 없습니다 (apt install gcc-aarch64-linux-gnu)"

# 유저랜드가 준비되어 있어야 커널에 내장할 수 있다
if [[ ! -d "$ROOTFS" ]]; then
    step "유저랜드 rootfs 가 없어 먼저 만듭니다"
    make -C "${REPO_ROOT}/userland" >/dev/null
    ( cd "${REPO_ROOT}/userland" && ./mkrootfs.sh >/dev/null )
fi
[[ -e "${ROOTFS}/dev/console" ]] || \
    echo "경고: ${ROOTFS}/dev/console 이 없습니다. init 이 출력을 못 낼 수 있습니다."

MAKE_ARGS=(-C "$LINUX_SRC" O="$BUILD_DIR" ARCH="$ARCH" CROSS_COMPILE="$CROSS")

mkdir -p "$BUILD_DIR" "$OUT_DIR"

# ── 1. 공식 defconfig ────────────────────────────────────────────
step "bcm2711_defconfig 적용 (Pi 3 / Zero 2 W / Pi 4 공용 arm64 설정)"
make "${MAKE_ARGS[@]}" bcm2711_defconfig >/dev/null

BEFORE_M=$(grep -c '=m$' "${BUILD_DIR}/.config" || true)
BEFORE_Y=$(grep -c '=y$' "${BUILD_DIR}/.config" || true)
echo "    기준: =y ${BEFORE_Y}개, =m ${BEFORE_M}개"

# ── 2. 우리 조각 병합 ────────────────────────────────────────────
step "lp-zero.config 병합"

# initramfs 경로는 환경마다 다르므로 여기서 만들어 붙인다.
GEN="${BUILD_DIR}/lp-zero-generated.config"
{
    echo "# build.sh 가 생성. 직접 수정하지 말 것."
    echo "CONFIG_INITRAMFS_SOURCE=\"${ROOTFS}\""
    echo "CONFIG_INITRAMFS_ROOT_UID=0"
    echo "CONFIG_INITRAMFS_ROOT_GID=0"
} > "$GEN"

"${LINUX_SRC}/scripts/kconfig/merge_config.sh" -m -O "$BUILD_DIR" \
    "${BUILD_DIR}/.config" "$FRAGMENT" "$GEN" >/dev/null

# merge_config 는 의존성을 풀지 않는다. olddefconfig 가 정리한다.
make "${MAKE_ARGS[@]}" olddefconfig >/dev/null

AFTER_M=$(grep -c '=m$' "${BUILD_DIR}/.config" || true)
AFTER_Y=$(grep -c '=y$' "${BUILD_DIR}/.config" || true)
echo "    결과: =y ${AFTER_Y}개, =m ${AFTER_M}개"

# 조각이 실제로 반영됐는지 확인. olddefconfig 가 의존성 때문에 되돌릴 수
# 있으므로 중요한 항목은 직접 검사한다.
check() {
    if grep -qx "$1" "${BUILD_DIR}/.config"; then
        echo "    OK   $1"
    else
        echo "    !!   $1 이 반영되지 않았습니다"
        grep -E "^(# )?${1%%=*}[ =]" "${BUILD_DIR}/.config" | head -1 | sed 's/^/         현재: /'
    fi
}
step "핵심 옵션 확인"
check "# CONFIG_MODULES is not set"
check "CONFIG_BLK_DEV_INITRD=y"
check "CONFIG_EXT4_FS=y"
check "CONFIG_VFAT_FS=y"
check "CONFIG_SERIAL_AMBA_PL011_CONSOLE=y"
check "CONFIG_DEVTMPFS=y"

# ── 3. 빌드 ──────────────────────────────────────────────────────
step "빌드 시작 (-j${JOBS}) — 몇 분 걸립니다"
time make "${MAKE_ARGS[@]}" -j"$JOBS" Image dtbs

# ── 4. 결과 수집 ─────────────────────────────────────────────────
step "결과"
cp "${BUILD_DIR}/arch/arm64/boot/Image" "${OUT_DIR}/Image"

DTB_SRC="${BUILD_DIR}/arch/arm64/boot/dts/broadcom/bcm2710-rpi-zero-2-w.dtb"
if [[ -f "$DTB_SRC" ]]; then
    cp "$DTB_SRC" "${OUT_DIR}/"
    echo "  DTB   $(stat -c%s "${OUT_DIR}/bcm2710-rpi-zero-2-w.dtb") bytes"
else
    echo "  경고: Zero 2 W DTB 를 찾지 못했습니다"
fi

IMG_SIZE=$(stat -c%s "${OUT_DIR}/Image")
printf "  Image %s bytes (%.1f MB)\n" "$IMG_SIZE" "$(echo "scale=2; $IMG_SIZE/1048576" | bc)"
echo ""
echo "  출력: ${OUT_DIR}"
