#!/usr/bin/env bash
#
# fetch-wifi-fw.sh - Zero 2 W 무선칩 펌웨어를 받는다.
#
# 직접 만들 수 없는 두 번째 블롭이다. 리눅스가 실행하는 코드가 아니라
# 무선칩 **내부로 업로드되는** 코드다. brcmfmac 드라이버가 부팅 시 SDIO 로
# 밀어 넣는다. 없으면 wlan0 인터페이스 자체가 생기지 않는다.
#
# ── Zero 2 W 는 보드 리비전마다 무선칩이 다르다 ──────────────────
# 시중에 최소 두 종류가 돌아다닌다:
#
#   BCM43430/1  ->  brcmfmac43430-sdio.bin
#   BCM43430/2  ->  실제 파일은 brcmfmac43436-sdio.bin 인데,
#                   드라이버는 칩 ID 를 43430 으로 읽어서
#                   brcmfmac43430c0-sdio.bin 이라는 이름으로 요청한다.
#
# 내 보드가 어느 쪽인지 미리 알 수 없으므로 **양쪽을 다 넣는다.**
# 드라이버가 칩 ID 를 보고 알아서 고른다. 전부 합쳐도 1.3MB 다.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${REPO_ROOT}/blobs/brcm"
REF="${FW_NONFREE_REF:-master}"
BASE="https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree/${REF}/brcm"

# 필수: 없으면 실패로 처리
REQUIRED=(
    "brcmfmac43430-sdio.bin"
    "brcmfmac43430-sdio.txt"
    "brcmfmac43436-sdio.bin"
    "brcmfmac43436-sdio.txt"
)
# 있으면 좋은 것 (칩에 따라 없을 수 있다)
OPTIONAL=(
    "brcmfmac43430-sdio.clm_blob"
    "brcmfmac43436-sdio.clm_blob"
    "brcmfmac43436s-sdio.bin"
    "brcmfmac43436s-sdio.txt"
)

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

command -v curl >/dev/null || die "curl 이 필요합니다"
mkdir -p "$OUT"

fetch() {
    curl --fail --location --silent --show-error \
         --retry 4 --retry-delay 2 --retry-all-errors \
         --output "${OUT}/$1" "${BASE}/$1" 2>/dev/null
}

echo "RPi-Distro/firmware-nonfree @ ${REF} 에서 받는 중..."

for f in "${REQUIRED[@]}"; do
    fetch "$f" || die "필수 파일을 받지 못했습니다: $f"
    printf "  OK    %-34s %s bytes\n" "$f" "$(stat -c%s "${OUT}/${f}")"
done

for f in "${OPTIONAL[@]}"; do
    if fetch "$f"; then
        printf "  OK    %-34s %s bytes\n" "$f" "$(stat -c%s "${OUT}/${f}")"
    else
        rm -f "${OUT}/${f}"
        printf "  없음  %s (칩에 따라 정상)\n" "$f"
    fi
done

# BCM43430/2 보드용 별칭.
# 드라이버가 43430c0 이라는 이름으로 요청하지만 배포되는 파일은 43436 이다.
# 심볼릭 링크가 아니라 복사본으로 둔다 - FAT 파티션이나 cpio 를 거칠 때
# 링크가 깨지는 것을 피하기 위해서다.
echo ""
echo "BCM43430/2 보드용 별칭 생성 (43436 -> 43430c0)"
for ext in bin txt clm_blob; do
    src="${OUT}/brcmfmac43436-sdio.${ext}"
    dst="${OUT}/brcmfmac43430c0-sdio.${ext}"
    [[ -f "$src" ]] && { cp "$src" "$dst"; printf "  %s\n" "$(basename "$dst")"; }
done

( cd "$OUT" && sha256sum ./* ) > "${REPO_ROOT}/tools/wifi-fw.sha256"

echo ""
echo "총 $(ls -1 "$OUT" | wc -l)개 파일, $(du -sh "$OUT" | cut -f1)  ->  blobs/brcm/"
echo "체크섬 기록: tools/wifi-fw.sha256"
