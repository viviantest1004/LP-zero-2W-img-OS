#!/usr/bin/env bash
#
# mkpkg.sh - 패키지 하나를 만들고, 저장소 인덱스를 다시 쓴다.
#
# 패키지는 압축하지 않은 평범한 tar 다. 우리만의 형식이 아니라서
# 아무 기계에서나 tar 로 열어볼 수 있고, 기기 쪽에서 풀어야 하는 코드도
# 절반이면 된다 (userland/pkg/pkg.c 참고).
#
# tar 안의 경로는 상대경로여야 하고, 기기에서는 /data 아래로 풀린다.
# bin/foo 를 담으면 /data/bin/foo 가 되고, /data/bin 은 이미 PATH 에 있다.
# 절대경로나 ".." 이 들어 있으면 기기 쪽 pkg 가 통째로 거부한다.
#
# 사용법:
#   tools/mkpkg.sh <이름> <버전> <디렉터리> [저장소디렉터리]
#   tools/mkpkg.sh --index [저장소디렉터리]
#
# 예:
#   mkdir -p stage/bin && cp myprog stage/bin/
#   tools/mkpkg.sh myprog 1.0 stage
#   python3 -m http.server -d repo 8000
#   # 기기에서: pkg repo http://<주소>:8000 ; pkg update ; pkg install myprog

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DEFAULT="${REPO_ROOT}/repo"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
log() { printf '  %s\n' "$*"; }

# 인덱스 한 줄: 이름 버전 크기 sha256 파일명
# 기기 쪽 index_find() 가 공백으로 잘라 읽으므로 필드에 공백이 있으면 안 된다.
write_index() {
    local out="$1"
    local idx="${out}/index"
    {
        printf '# name version size sha256 file\n'
        # 정렬해서 쓴다. 그래야 같은 내용이면 인덱스도 같은 파일이 되고,
        # 저장소를 diff 해서 무엇이 바뀌었는지 볼 수 있다.
        local t
        for t in $(cd "$out" && ls -1 ./*.tar 2>/dev/null | sort); do
            t="${t#./}"
            local base="${t%.tar}"
            local name="${base%-*}"
            local ver="${base##*-}"
            [[ "$name" == "$base" ]] && die "$t: 이름-버전.tar 형식이 아닙니다"
            printf '%s %s %s %s %s\n' \
                "$name" "$ver" \
                "$(stat -c%s "${out}/${t}")" \
                "$(sha256sum "${out}/${t}" | cut -d' ' -f1)" \
                "$t"
        done
    } > "$idx"
    log "index  $(grep -cv '^#' "$idx") 개 패키지"
}

if [[ "${1:-}" == "--index" ]]; then
    OUT="${2:-$OUT_DEFAULT}"
    [[ -d "$OUT" ]] || die "저장소 디렉터리가 없습니다: $OUT"
    write_index "$OUT"
    exit 0
fi

[[ $# -ge 3 ]] || {
    printf '사용법: mkpkg.sh <이름> <버전> <디렉터리> [저장소디렉터리]\n' >&2
    printf '        mkpkg.sh --index [저장소디렉터리]\n' >&2
    exit 2
}

NAME="$1"; VERSION="$2"; SRC="$3"; OUT="${4:-$OUT_DEFAULT}"

# 이름과 버전에는 공백도 '-' 도 들어가면 안 된다. 파일 이름을
# "이름-버전.tar" 로 만들고 다시 갈라 읽기 때문이다.
[[ "$NAME"    =~ ^[A-Za-z0-9_.]+$ ]] || die "이름에 쓸 수 없는 글자: $NAME"
[[ "$VERSION" =~ ^[A-Za-z0-9_.]+$ ]] || die "버전에 쓸 수 없는 글자: $VERSION"
[[ -d "$SRC" ]] || die "디렉터리가 없습니다: $SRC"

mkdir -p "$OUT"
TAR="${OUT}/${NAME}-${VERSION}.tar"

# --format=ustar: 기기 쪽 tar 는 GNU 확장 헤더를 읽지 않는다.
# --numeric-owner + --owner/--group: 빌드한 사람의 계정 이름이 아카이브에
#   들어가지 않게. 기기에는 그 사용자가 없다.
# --mtime + --sort: 같은 내용이면 같은 tar 가 나오게 (재현 가능한 빌드).
( cd "$SRC" && tar --format=ustar --numeric-owner \
      --owner=0 --group=0 --mtime='@0' --sort=name \
      -cf "$TAR" . )

# 기기 쪽 pkg 가 거부할 것을 여기서 먼저 잡는다. 만든 다음에 기기에서
# 실패하는 것보다, 만들 때 실패하는 편이 낫다.
BAD=$(tar -tf "$TAR" | grep -E '^/|(^|/)\.\.(/|$)' || true)
[[ -z "$BAD" ]] || die "절대경로나 '..' 가 들어 있습니다:
$BAD"

log "$(basename "$TAR")  $(stat -c%s "$TAR") bytes, $(tar -tf "$TAR" | grep -cv '/$') 개 파일"
write_index "$OUT"
echo ""
echo "저장소를 띄우려면:"
echo "  python3 -m http.server -d ${OUT} 8000"
echo "기기에서:"
echo "  pkg repo http://<이 컴퓨터의 주소>:8000"
echo "  pkg update && pkg install ${NAME}"
