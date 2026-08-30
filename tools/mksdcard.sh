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
IMAGE_MB=128            # 전체 이미지 크기
PART_START_SECTOR=8192  # 4MiB 지점 - 라즈베리파이 표준 정렬
SECTOR_SIZE=512
VOLUME_LABEL="LPZERO"

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
PART_SECTORS=$(( IMAGE_MB * 1024 * 1024 / SECTOR_SIZE - PART_START_SECTOR ))
PART_BYTES=$(( PART_SECTORS * SECTOR_SIZE ))

echo "SD카드 이미지 생성 중 (${IMAGE_MB}MiB, 모드: ${MODE})"
log "부트 파티션: ${PART_SECTORS} 섹터 ($(( PART_BYTES / 1024 / 1024 ))MiB)"

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
    OVL_SRC="${REPO_ROOT}/kernel/out/overlays/disable-bt.dtbo"
    if [[ -f "$OVL_SRC" ]]; then
        mmd -i "$PART_IMG" ::overlays 2>/dev/null || true
        mcopy -i "$PART_IMG" "$OVL_SRC" ::overlays/
        log "복사: overlays/disable-bt.dtbo            (PL011 을 헤더 핀으로)"
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
dd if="$PART_IMG" of="$IMAGE" bs="$SECTOR_SIZE" seek="$PART_START_SECTOR" \
   conv=notrunc status=none

python3 - "$IMAGE" "$PART_START_SECTOR" "$PART_SECTORS" <<'PY'
import sys, struct

image, start, count = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])

def chs(lba, heads=255, spt=63):
    """LBA -> CHS. 1023 실린더를 넘으면 관례대로 최대값으로 고정한다."""
    c, rem = divmod(lba, heads * spt)
    h, s = divmod(rem, spt)
    s += 1
    if c > 1023:
        c, h, s = 1023, 254, 63
    return bytes([h & 0xFF, ((c >> 2) & 0xC0) | (s & 0x3F), c & 0xFF])

entry = (
    b"\x00"                    # 부트 플래그 (Pi 는 보지 않음)
    + chs(start)               # 시작 CHS
    + b"\x0C"                  # 파티션 타입 0x0C = FAT32 (LBA)
    + chs(start + count - 1)   # 끝 CHS
    + struct.pack("<I", start) # 시작 LBA
    + struct.pack("<I", count) # 섹터 수
)
assert len(entry) == 16

with open(image, "r+b") as f:
    f.seek(446)
    f.write(entry + b"\x00" * 48)   # 파티션 1 + 나머지 3개는 비움
    f.seek(510)
    f.write(b"\x55\xAA")            # MBR 시그니처
PY

rm -f "$PART_IMG"

# ── 4. 결과 확인 ────────────────────────────────────────────────
echo ""
echo "완성: ${IMAGE}  ($(du -h "$IMAGE" | cut -f1))"
echo ""
echo "부트 파티션 내용:"
mdir -i "${IMAGE}@@$(( PART_START_SECTOR * SECTOR_SIZE ))" :: 2>/dev/null | sed 's/^/  /'

cat <<EOF

SD카드에 굽기:
  sudo dd if=${IMAGE} of=/dev/sdX bs=4M conv=fsync status=progress

시리얼 콘솔 연결 (USB-TTL 어댑터):
  Pi 헤더 8번 (GPIO14 TX) -> 어댑터 RX
  Pi 헤더 10번(GPIO15 RX) -> 어댑터 TX
  Pi 헤더 6번 (GND)       -> 어댑터 GND
  screen /dev/ttyUSB0 115200      (또는 minicom / picocom)
EOF
