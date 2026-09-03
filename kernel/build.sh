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
#   LINUX_SRC   커널 소스 경로 (기본 .build/linux — tools/fetch-kernel.sh 가 받는다)
#   JOBS        병렬 빌드 수 (기본 nproc)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/common.sh
source "${REPO_ROOT}/tools/common.sh"

LINUX_SRC="${LINUX_SRC:-${LPZERO_WORK}/linux}"
BUILD_DIR="${BUILD_DIR:-${LPZERO_WORK}/build}"
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
# EFI_ZBOOT 가 vmlinuz.efi 를 만들 때 커널 Makefile 이 hexdump 로 이미지
# 크기를 읽는다. 없으면 "truncate: Invalid number" 라는, 원인이 전혀
# 드러나지 않는 오류로 7분짜리 빌드가 끝난다.
command -v hexdump >/dev/null || die "hexdump 가 없습니다 (apt install bsdextrautils)"

# 유저랜드가 준비되어 있어야 커널에 내장할 수 있다
if [[ ! -d "$ROOTFS" ]]; then
    step "유저랜드 rootfs 가 없어 먼저 만듭니다"
    make -C "${REPO_ROOT}/userland" >/dev/null
    ( cd "${REPO_ROOT}/userland" && ./mkrootfs.sh >/dev/null )
fi
[[ -e "${ROOTFS}/dev/console" ]] || \
    echo "경고: ${ROOTFS}/dev/console 이 없습니다. init 이 출력을 못 낼 수 있습니다."

MAKE_ARGS=(-C "$LINUX_SRC" O="$BUILD_DIR" ARCH="$ARCH" CROSS_COMPILE="$CROSS")

# CLEAN=1 이면 빌드 디렉터리를 비운다.
# 설정을 바꿔 다시 빌드하면 이전 설정으로 만든 .o 가 남아 있어서,
# 나중에 "무엇이 이미지를 키우나"를 오브젝트 크기로 재려 할 때 왜곡된다.
if [[ "${CLEAN:-0}" == "1" ]]; then
    step "빌드 디렉터리 비우기 (CLEAN=1)"
    rm -rf "$BUILD_DIR"
fi

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

# 조각이 실제로 반영됐는지 전부 대조한다.
#
# kconfig 의 select 는 사용자 설정을 무시하고 심볼을 강제로 켠다. 그래서
# 조각에 CONFIG_X=n 을 적어도 다른 켜진 옵션이 X 를 select 하면 y 로
# 남는다. 이 경우 X 가 아니라 "X 를 select 하는 쪽"을 꺼야 한다.
#
# 조용히 무시되면 왜 이미지가 안 줄어드는지 알 수 없으므로 전부 보고한다.
step "조각 반영 상태 대조"

MISSED=0
while read -r line; do
    sym="${line%%=*}"
    want="${line##*=}"
    case "$want" in
    n)
        if grep -qx "${sym}=y" "${BUILD_DIR}/.config" 2>/dev/null; then
            echo "    미반영(y)  ${sym}"
            MISSED=$((MISSED + 1))
        elif grep -qx "${sym}=m" "${BUILD_DIR}/.config" 2>/dev/null; then
            echo "    미반영(m)  ${sym}"
            MISSED=$((MISSED + 1))
        fi
        ;;
    y)
        grep -qx "${sym}=y" "${BUILD_DIR}/.config" 2>/dev/null || {
            echo "    미반영      ${sym} (=y 를 원했음)"
            MISSED=$((MISSED + 1))
        }
        ;;
    esac
done < <(grep -E '^CONFIG_[A-Z0-9_]+=(y|n)$' "$FRAGMENT")

TOTAL=$(grep -cE '^CONFIG_[A-Z0-9_]+=(y|n)$' "$FRAGMENT")
if [[ "$MISSED" == "0" ]]; then
    echo "    전부 반영됨 (${TOTAL}개)"
else
    echo ""
    echo "    ${TOTAL}개 중 ${MISSED}개가 반영되지 않았습니다."
    echo "    대부분 다른 옵션이 select 로 강제하는 경우입니다."
    echo "    선택자를 찾으려면:"
    echo "      grep -rn --include='Kconfig*' 'select <심볼이름>\\b' ${LINUX_SRC}"
fi

# ── 3. 빌드 ──────────────────────────────────────────────────────
step "빌드 시작 (-j${JOBS}) — 몇 분 걸립니다"
# vmlinuz.efi 는 EFI_ZBOOT 의 산물이다. Image 를 압축해 EFI 스텁으로
# 감싼 것으로, UEFI 부팅 경로(QEMU/UTM)에서 FAT 파티션 공간을 절반 넘게
# 아낀다. 실기 Pi 는 GPU 펌웨어가 압축을 풀 줄 모르므로 Image 를 그대로
# 쓴다 - 그래서 둘 다 만든다.
time make "${MAKE_ARGS[@]}" -j"$JOBS" Image vmlinuz.efi dtbs

# ── 4. 결과 수집 ─────────────────────────────────────────────────
step "결과"
cp "${BUILD_DIR}/arch/arm64/boot/Image" "${OUT_DIR}/Image"

ZBOOT="${BUILD_DIR}/arch/arm64/boot/vmlinuz.efi"
if [[ -f "$ZBOOT" ]]; then
    cp "$ZBOOT" "${OUT_DIR}/vmlinuz.efi"
else
    rm -f "${OUT_DIR}/vmlinuz.efi"
    echo "  경고: vmlinuz.efi 가 없습니다 (CONFIG_EFI_ZBOOT)."
    echo "        UEFI 경로도 압축되지 않은 Image 를 쓰게 됩니다."
fi

DTB_SRC="${BUILD_DIR}/arch/arm64/boot/dts/broadcom/bcm2710-rpi-zero-2-w.dtb"
if [[ -f "$DTB_SRC" ]]; then
    cp "$DTB_SRC" "${OUT_DIR}/"
    echo "  DTB   $(stat -c%s "${OUT_DIR}/bcm2710-rpi-zero-2-w.dtb") bytes"
else
    echo "  경고: Zero 2 W DTB 를 찾지 못했습니다"
fi

# disable-bt 오버레이. Zero 2 W 에서 PL011 을 40핀 헤더로 돌리는 데
# 필요하다. 없으면 시리얼 콘솔이 나오지 않는다.
OVL_DIR="${BUILD_DIR}/arch/arm64/boot/dts/overlays"
mkdir -p "${OUT_DIR}/overlays"
for ovl in disable-bt dwc2; do
    if [[ -f "${OVL_DIR}/${ovl}.dtbo" ]]; then
        cp "${OVL_DIR}/${ovl}.dtbo" "${OUT_DIR}/overlays/"
        echo "  오버레이 ${ovl}.dtbo"
    else
        echo "  경고: ${ovl}.dtbo 를 찾지 못했습니다"
    fi
done

IMG_SIZE=$(stat -c%s "${OUT_DIR}/Image")
printf "  Image %s bytes (%.1f MB)\n" "$IMG_SIZE" "$(echo "scale=2; $IMG_SIZE/1048576" | bc)"
if [[ -f "${OUT_DIR}/vmlinuz.efi" ]]; then
    Z_SIZE=$(stat -c%s "${OUT_DIR}/vmlinuz.efi")
    printf "  vmlinuz.efi %s bytes (%.1f MB) - UEFI 부팅용\n" \
        "$Z_SIZE" "$(echo "scale=2; $Z_SIZE/1048576" | bc)"
fi
echo ""
echo "  출력: ${OUT_DIR}"
