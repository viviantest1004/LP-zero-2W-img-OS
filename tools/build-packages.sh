#!/usr/bin/env bash
#
# build-packages.sh - 저장소에 올릴 패키지들을 짓는다.
#
# 왜 있는가: 패키지 관리자가 있는데 깔 게 하나도 없으면 기능이 있는지
# 없는지 알 수가 없다. 여기 있는 것들은 "이미지에 넣기는 아깝지만 있으면
# 좋은" 것들이다 - 루트 파일시스템은 램에 상주하므로 안 쓰는 프로그램을
# 넣어두면 기계의 수명 내내 그 메모리를 쓴다.
#
#   ./tools/build-packages.sh
#   python3 -m http.server -d repo 8000
#
# 기기에서:
#   pkg repo http://<이 컴퓨터>:8000
#   pkg update && pkg install hexdump
#
# 사용법:
#   ./tools/build-packages.sh [저장소디렉터리]

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-${REPO_ROOT}/repo}"
SDK="${REPO_ROOT}/sdk"
STAGE_ROOT="$(mktemp -d)"
trap 'rm -rf "$STAGE_ROOT"' EXIT

die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
log()  { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }

# SDK 가 없으면 만든다. 이 스크립트가 SDK 를 실제로 쓰는 유일한 것이라
# 여기서 깨지면 SDK 가 깨진 것이다.
if [[ ! -x "${SDK}/bin/lp-gcc" ]]; then
    step "SDK 준비"
    "${REPO_ROOT}/tools/build-sdk.sh" > /dev/null
fi

VERSION=1.0
mkdir -p "$OUT"

# ── C 로 된 것들 ─────────────────────────────────────────────────
for name in hexdump watch; do
    step "$name"
    src="${REPO_ROOT}/packages/${name}/${name}.c"
    [[ -f "$src" ]] || die "$src 가 없습니다"

    stage="${STAGE_ROOT}/${name}"
    mkdir -p "${stage}/bin"
    "${SDK}/bin/lp-gcc" -o "${stage}/bin/${name}" "$src" \
        || die "${name} 빌드 실패"

    # 정적 aarch64 가 아니면 기기에서 실행되지 않는다. 여기서 잡는다.
    file "${stage}/bin/${name}" | grep -q "ARM aarch64" \
        || die "${name} 이 aarch64 가 아닙니다"

    log "$(stat -c%s "${stage}/bin/${name}") bytes"
    "${REPO_ROOT}/tools/mkpkg.sh" "$name" "$VERSION" "$stage" "$OUT" \
        | grep -v '^$' | head -2
done

# ── 스크립트 예제들 ──────────────────────────────────────────────
step "examples"
stage="${STAGE_ROOT}/examples"
mkdir -p "${stage}/examples"
cp "${REPO_ROOT}/examples/"*.sh "${stage}/examples/"
cp "${REPO_ROOT}/examples/README.md" "${stage}/examples/"
log "$(ls -1 "${stage}/examples" | wc -l) 개 파일"
"${REPO_ROOT}/tools/mkpkg.sh" examples "$VERSION" "$stage" "$OUT" \
    | grep -v '^$' | head -2

step "결과"
cat "${OUT}/index"
echo ""
echo "  저장소를 띄우려면:"
echo "    python3 -m http.server -d ${OUT} 8000"
echo "  기기에서:"
echo "    pkg repo http://<이 컴퓨터의 주소>:8000"
echo "    pkg update && pkg install hexdump"
