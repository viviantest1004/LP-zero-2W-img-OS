#!/usr/bin/env bash
#
# mksdcard.sh - 부팅 가능한 SD카드 이미지를 만든다.
#
# root 권한도 loop 마운트도 쓰지 않는다. mtools 가 FAT 파일시스템을
# 유저스페이스에서 직접 조작하고, MBR 은 파이썬으로 바이트를 찍는다.
# 덕분에 컨테이너/CI 안에서도 그대로 돌아간다.
#
# 두 가지 모드가 있다:
#   (기본)   베어메탈 펌웨어를 부팅하는 이미지
#   --linux  우리가 빌드한 리눅스 커널을 부팅하는 이미지
#
# 결과: sdcard/lp-zero.img  (dd 로 SD카드에 그대로 쓰면 부팅됨)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/common.sh
source "${REPO_ROOT}/tools/common.sh"
BLOB_DIR="${REPO_ROOT}/blobs"
OUT_DIR="${REPO_ROOT}/sdcard"
IMAGE="${OUT_DIR}/lp-zero.img"
MODE=firmware
[[ "${1:-}" == "--linux" ]] && MODE=linux
PART_IMG="${OUT_DIR}/.boot-part.img"

# 이미지 레이아웃
#
#   섹터 0                 MBR
#   섹터 8192   (4MiB)     파티션 1: FAT32 부트 (GPU 블롭, 커널, config.txt)
#   섹터 139264 (68MiB)    파티션 2: ext4 데이터 (WiFi 설정, SSH 키, 파일)
#
# 루트는 커널에 내장된 initramfs(RAM)이고 데이터만 여기에 남는다.
# 루트에 쓰기가 없으므로 전원을 갑자기 뽑아도 시스템이 깨지지 않는다.
IMAGE_MB=256
SECTOR_SIZE=512
BOOT_START_SECTOR=8192          # 4MiB
BOOT_SIZE_MB=64
DATA_START_SECTOR=139264        # 4 + 64 = 68MiB
VOLUME_LABEL="LPZERO"
DATA_LABEL="LPZERODATA"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
log() { printf '  %s\n' "$*"; }

for t in mkfs.vfat mcopy python3 dd; do
    command -v "$t" >/dev/null 2>&1 || die "$t 가 필요합니다 (apt install mtools dosfstools)"
done

# ── 입력 확인 ────────────────────────────────────────────────────
if [[ "$MODE" == "linux" ]]; then
    KERNEL="${REPO_ROOT}/kernel/out/Image"
    KERNEL_NAME="$LINUX_IMAGE"
    CONFIG_SRC="${REPO_ROOT}/boot/config-linux.txt"
    DTB="${REPO_ROOT}/kernel/out/bcm2710-rpi-zero-2-w.dtb"
    [[ -f "$KERNEL" ]] || die "kernel/out/Image 가 없습니다. 'make kernel' 을 먼저 실행하세요."
    [[ -f "$DTB" ]]    || die "kernel/out 에 Zero 2 W DTB 가 없습니다."
else
    KERNEL="${REPO_ROOT}/firmware/${KERNEL_IMAGE}"
    KERNEL_NAME="$KERNEL_IMAGE"
    CONFIG_SRC="${REPO_ROOT}/boot/config.txt"
    DTB=""
    [[ -f "$KERNEL" ]] || die "firmware/${KERNEL_IMAGE} 가 없습니다. 먼저 'make firmware' 를 실행하세요."
fi

# config.txt 의 kernel= 과 실제 파일명이 다르면 GPU 가 커널을 못 찾는다.
# 그 경우 화면도 시리얼도 아무 것도 안 나와서 원인 찾기가 매우 어렵다.
# 이미지를 굽기 전에 여기서 잡는다.
CFG_KERNEL="$(sed -n 's/^[[:space:]]*kernel=\(.*\)$/\1/p' "$CONFIG_SRC" \
              | tail -1 | tr -d '"'"'[:space:]'"'"')"
if [[ "$CFG_KERNEL" != "$KERNEL_NAME" ]]; then
    die "이름 불일치: config.mk 는 '${KERNEL_NAME}', $(basename "$CONFIG_SRC") 는 '${CFG_KERNEL}'.
       둘을 같게 맞추세요. 다르면 부팅 시 아무 출력 없이 멈춥니다."
fi

BLOBS=(bootcode.bin start.elf fixup.dat)
for b in "${BLOBS[@]}"; do
    [[ -f "${BLOB_DIR}/${b}" ]] || die "blobs/${b} 가 없습니다. './tools/fetch-blobs.sh' 를 먼저 실행하세요."
done

mkdir -p "$OUT_DIR"
rm -f "$IMAGE" "$PART_IMG"

# ── 1. FAT32 부트 파티션 만들기 ──────────────────────────────────
PART_SECTORS=$(( BOOT_SIZE_MB * 1024 * 1024 / SECTOR_SIZE ))
PART_BYTES=$(( PART_SECTORS * SECTOR_SIZE ))

echo "SD카드 이미지 생성 중 (${IMAGE_MB}MiB, 모드: ${MODE})"
log "부트 파티션: ${PART_SECTORS} 섹터 ($(( PART_BYTES / 1024 / 1024 ))MiB, FAT32)"

truncate -s "$PART_BYTES" "$PART_IMG"
mkfs.vfat -F 32 -n "$VOLUME_LABEL" "$PART_IMG" >/dev/null

# ── 2. 파일 복사 ────────────────────────────────────────────────
# mtools 는 이미지 파일을 드라이브처럼 다룬다. 설정 파일 검사는 건너뛴다.
export MTOOLS_SKIP_CHECK=1

log "복사: bootcode.bin, start.elf, fixup.dat  (Broadcom GPU 펌웨어)"
for b in "${BLOBS[@]}"; do
    mcopy -i "$PART_IMG" "${BLOB_DIR}/${b}" ::
done

log "복사: config.txt                          (GPU 부팅 설정)"
mcopy -i "$PART_IMG" "$CONFIG_SRC" ::config.txt

log "복사: ${KERNEL_NAME}"
mcopy -i "$PART_IMG" "$KERNEL" "::${KERNEL_NAME}"

if [[ "$MODE" == "linux" ]]; then
    log "복사: $(basename "$DTB")            (디바이스 트리)"
    mcopy -i "$PART_IMG" "$DTB" ::

    log "복사: cmdline.txt                        (커널 커맨드라인)"
    mcopy -i "$PART_IMG" "${REPO_ROOT}/boot/cmdline.txt" ::

    # disable-bt 오버레이가 있어야 PL011 이 헤더 핀으로 나온다.
    # 없으면 부팅은 되는데 시리얼에 아무것도 안 보인다.
    OVL_DIR="${REPO_ROOT}/kernel/out/overlays"
    if [[ -d "$OVL_DIR" ]] && compgen -G "${OVL_DIR}/*.dtbo" >/dev/null; then
        mmd -i "$PART_IMG" ::overlays 2>/dev/null || true
        for o in "${OVL_DIR}"/*.dtbo; do
            mcopy -i "$PART_IMG" "$o" ::overlays/
            log "복사: overlays/$(basename "$o")"
        done
    else
        echo ""
        echo "  경고: disable-bt.dtbo 가 없습니다."
        echo "        이 오버레이 없이는 PL011(ttyAMA0)이 블루투스에 물려 있어"
        echo "        40핀 헤더의 시리얼 콘솔에 아무것도 나오지 않습니다."
        echo ""
    fi
fi

# ── 3. MBR 을 붙여 완성 ─────────────────────────────────────────
log "MBR 작성 + 파티션 결합"

truncate -s "${IMAGE_MB}M" "$IMAGE"
dd if="$PART_IMG" of="$IMAGE" bs="$SECTOR_SIZE" seek="$BOOT_START_SECTOR" \
   conv=notrunc status=none

# ── 데이터 파티션 (ext4) ────────────────────────────────────────
# root 권한이나 loop 마운트 없이 파일 안에 직접 만든다.
TOTAL_SECTORS=$(( IMAGE_MB * 1024 * 1024 / SECTOR_SIZE ))
DATA_SECTORS=$(( TOTAL_SECTORS - DATA_START_SECTOR ))
DATA_IMG="${OUT_DIR}/.data-part.img"

log "데이터 파티션: ${DATA_SECTORS} 섹터 ($(( DATA_SECTORS * SECTOR_SIZE / 1024 / 1024 ))MiB, ext4)"
rm -f "$DATA_IMG"
truncate -s "$(( DATA_SECTORS * SECTOR_SIZE ))" "$DATA_IMG"
mkfs.ext4 -q -F -L "$DATA_LABEL" -m 0 "$DATA_IMG"

# 첫 부팅에 쓸 WiFi 설정을 미리 넣어둔다.
# SD 를 다시 굽지 않고 여기만 고쳐서 공유기 설정을 바꿀 수 있다.
WPA_SRC="${REPO_ROOT}/boot/rootfs-overlay/etc/wpa_supplicant.conf"
if command -v debugfs >/dev/null 2>&1 && [[ -f "$WPA_SRC" ]]; then
    debugfs -w -R "write ${WPA_SRC} wpa_supplicant.conf" "$DATA_IMG" \
        >/dev/null 2>&1 && log "데이터 파티션에 wpa_supplicant.conf 배치"
fi

dd if="$DATA_IMG" of="$IMAGE" bs="$SECTOR_SIZE" seek="$DATA_START_SECTOR" \
   conv=notrunc status=none
rm -f "$DATA_IMG"

python3 "${REPO_ROOT}/tools/write-mbr.py" "$IMAGE" \
    "$BOOT_START_SECTOR" "$PART_SECTORS" "$DATA_START_SECTOR" "$DATA_SECTORS"

rm -f "$PART_IMG"

# ── 4. 결과 확인 ────────────────────────────────────────────────
echo ""
echo "완성: ${IMAGE}  ($(du -h "$IMAGE" | cut -f1))"
echo ""
echo "부트 파티션 내용:"
mdir -i "${IMAGE}@@$(( BOOT_START_SECTOR * SECTOR_SIZE ))" :: 2>/dev/null | sed 's/^/  /'

cat <<EOF

SD카드에 굽기:
  sudo dd if=${IMAGE} of=/dev/sdX bs=4M conv=fsync status=progress

시리얼 콘솔 연결 (USB-TTL 어댑터):
  Pi 헤더 8번 (GPIO14 TX) -> 어댑터 RX
  Pi 헤더 10번(GPIO15 RX) -> 어댑터 TX
  Pi 헤더 6번 (GND)       -> 어댑터 GND
  screen /dev/ttyUSB0 115200      (또는 minicom / picocom)
EOF
