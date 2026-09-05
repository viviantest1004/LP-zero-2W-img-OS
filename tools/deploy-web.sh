#!/usr/bin/env bash
#
# deploy-web.sh - push the download page and the images to the server.
#
#   ./tools/deploy-web.sh                 배포
#   ./tools/deploy-web.sh --dry-run       무엇이 올라갈지만 보여준다
#
# 서버와 사용자는 환경변수로 바꾼다:
#   LP_WEB_HOST=ubuntu@1.2.3.4 ./tools/deploy-web.sh
#
# ── 왜 rsync 인가 ──
# 이미지 세 개가 85MB 다. 매번 통째로 올리면 느리고, 느린 배포는
# 결국 "이번엔 그냥 넘어가자" 가 된다. rsync 는 바뀐 것만 올리므로
# 이미지를 하나만 다시 빌드했으면 그 하나만 간다.
#
# ── 순서가 중요하다 ──
# 파일을 먼저, 페이지를 나중에 올린다. 반대로 하면 그 사이에 접속한
# 사람이 아직 없는 파일을 가리키는 다운로드 버튼을 누르게 된다.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && cd .. && pwd)"

HOST="${LP_WEB_HOST:-ubuntu@lp.cholab.kr}"
FILES_DIR="${LP_WEB_FILES:-/srv/lp-zero/files}"
SITE_DIR="${LP_WEB_SITE:-/srv/lp-zero/site}"

DRY=""
[[ "${1:-}" == "--dry-run" ]] && DRY="--dry-run"

die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }

command -v rsync >/dev/null || die "rsync 가 없습니다"

# 페이지를 먼저 만든다. 이미지가 없으면 여기서 멈추므로, 반쯤 된
# 배포로 서버를 어중간하게 만들 일이 없다.
step "페이지 만들기"
"${REPO_ROOT}/tools/mkweb.sh"

step "이미지 올리기 (${HOST}:${FILES_DIR})"
# --partial: 30MB 짜리가 중간에 끊겨도 다음 실행이 이어서 올린다.
# 삭제는 하지 않는다 - 서버에 옛 이미지를 남겨두고 싶을 수 있고,
# 지우는 것은 사람이 판단할 일이다.
rsync -av --progress --partial $DRY \
      --chmod=F644 \
      "${REPO_ROOT}/dist/"*.img.xz \
      "${REPO_ROOT}/dist/"*.zip \
      "${REPO_ROOT}/dist/SHA256SUMS.txt" \
      "${HOST}:${FILES_DIR}/"

step "페이지 올리기 (${HOST}:${SITE_DIR})"
rsync -av $DRY --chmod=F644 \
      "${REPO_ROOT}/web/site/" \
      "${HOST}:${SITE_DIR}/"

if [[ -n "$DRY" ]]; then
    printf '\n(--dry-run 이라 아무것도 올리지 않았습니다)\n'
    exit 0
fi

step "확인"
# 올린 것이 실제로 받아지는지 서버 밖에서 확인한다. 배포가 끝났다는
# 말은 파일이 거기 있다는 뜻이 아니라 사람이 받을 수 있다는 뜻이다.
URL="https://lp.cholab.kr"
for f in SHA256SUMS.txt; do
    code=$(curl -sS -o /dev/null -w '%{http_code}' --max-time 20 "${URL}/files/${f}" || echo "000")
    printf '  %-24s %s\n' "$f" "$code"
done
code=$(curl -sS -o /dev/null -w '%{http_code}' --max-time 20 "${URL}/" || echo "000")
printf '  %-24s %s\n' "index.html" "$code"

# 큰 파일은 첫 바이트만 받아본다. Range 요청이 되는지까지 같이 확인된다.
head=$(curl -sS -r 0-1 -o /dev/null -w '%{http_code} %{content_type}' --max-time 20 \
       "${URL}/files/linux-LP_amd64.img.xz" || echo "000")
printf '  %-24s %s\n' "amd64 이미지 (첫 바이트)" "$head"
printf '\n206 이면 이어받기가 됩니다. 200 이면 Range 요청이 무시되고 있습니다.\n'
