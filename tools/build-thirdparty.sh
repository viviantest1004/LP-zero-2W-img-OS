#!/usr/bin/env bash
#
# build-thirdparty.sh - 이미지에 들어가는 남의 코드 두 개를 짓는다.
#
#   dropbear         SSH 서버
#   wpa_supplicant   WPA2 인증 (libnl 을 먼저 짓는다)
#
# 이 두 개만 남의 것인 이유는 하나다: 암호 구현을 직접 쓰는 것은
# 이 프로젝트에서 유일하게 "틀려도 조용한" 종류의 코드이기 때문이다.
# 나머지 - 커널 설정, libc, 셸, 모든 명령어 - 는 전부 우리 것이다.
#
# 설정 파일은 저장소 안에 있다:
#   thirdparty/dropbear-localoptions.h   비밀번호 인증을 뺀 빌드 옵션
#   thirdparty/wpa_supplicant.config     WPA2 에 필요한 것만
#
# 여기 있는 이유: 이 두 파일이 보안 결정을 담고 있는데, 빌드 트리에만
# 두면 저장소를 새로 받은 사람에게는 전해지지 않는다. 비밀번호 인증을
# 껐다는 사실이 아무 데도 적혀 있지 않은 채로 dropbear 를 다시 지으면,
# 다음 이미지에는 비밀번호 인증이 켜져 있게 된다.
#
# 사용법:
#   ./tools/build-thirdparty.sh            없는 것만
#   ./tools/build-thirdparty.sh --force    다시 짓는다
#   ./tools/build-thirdparty.sh --verify   받은 소스 체크섬만 검증

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${WORK:-/home/user/kernel-work/thirdparty}"
SYSROOT="${SYSROOT:-${WORK}/sysroot}"
SUMS_FILE="${REPO_ROOT}/tools/thirdparty.sha256"
CONF_DIR="${REPO_ROOT}/thirdparty"

JOBS="${JOBS:-$(nproc)}"
CROSS=aarch64-linux-gnu-
HOST=aarch64-linux-gnu
BUILD=x86_64-linux-gnu

DROPBEAR_VER=2024.86
WPA_VER=2.11
LIBNL_VER=3.11.0

die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
log()  { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }

FORCE=false
VERIFY=false
case "${1:-}" in
    --force)  FORCE=true ;;
    --verify) VERIFY=true ;;
    "")       ;;
    *)        die "알 수 없는 인자: $1" ;;
esac

command -v curl >/dev/null          || die "curl 이 필요합니다"
command -v "${CROSS}gcc" >/dev/null || die "${CROSS}gcc 가 없습니다"
[[ -f "${CONF_DIR}/dropbear-localoptions.h" ]] \
    || die "${CONF_DIR}/dropbear-localoptions.h 가 없습니다"
[[ -f "${CONF_DIR}/wpa_supplicant.config" ]] \
    || die "${CONF_DIR}/wpa_supplicant.config 가 없습니다"

mkdir -p "$WORK" "$SYSROOT/lib" "$SYSROOT/include"

# 이름|URL|압축을푼디렉터리|확장자
PKGS=(
  "dropbear|https://matt.ucc.asn.au/dropbear/releases/dropbear-${DROPBEAR_VER}.tar.bz2|dropbear-${DROPBEAR_VER}|tar.bz2"
  "wpa|https://w1.fi/releases/wpa_supplicant-${WPA_VER}.tar.gz|wpa_supplicant-${WPA_VER}|tar.gz"
  "libnl|https://github.com/thom311/libnl/releases/download/libnl3_11_0/libnl-${LIBNL_VER}.tar.gz|libnl-${LIBNL_VER}|tar.gz"
)

fetch() {   # fetch <이름> <url> <디렉터리> <확장자>
    local name="$1" url="$2" dir="$3" ext="$4"
    local tarball="${WORK}/${name}.${ext}"

    if [[ ! -f "$tarball" ]]; then
        log "GET  ${name}"
        # --fail 이 없으면 404 HTML 을 tarball 로 저장해놓고
        # 나중에 압축 풀 때 엉뚱한 오류가 난다.
        curl --fail --location --silent --show-error \
             --retry 4 --retry-delay 2 --retry-all-errors \
             --output "$tarball" "$url" || die "${name} 다운로드 실패"
    fi

    local want have
    have="$(sha256sum "$tarball" | cut -d' ' -f1)"
    if [[ -f "$SUMS_FILE" ]] && want="$(awk -v n="${name}.${ext}" '$2==n {print $1}' "$SUMS_FILE")" \
       && [[ -n "$want" ]]; then
        [[ "$want" == "$have" ]] || die "${name} 체크섬 불일치
       기대: ${want}
       실제: ${have}"
    else
        printf '%s  %s.%s\n' "$have" "$name" "$ext" >> "$SUMS_FILE"
        log "체크섬 기록: ${name}"
    fi

    [[ -d "${WORK}/${dir}" ]] || tar -xf "$tarball" -C "$WORK"
}

step "소스 받기"
for p in "${PKGS[@]}"; do
    IFS='|' read -r name url dir ext <<< "$p"
    fetch "$name" "$url" "$dir" "$ext"
done
sort -u -o "$SUMS_FILE" "$SUMS_FILE" 2>/dev/null || true

if $VERIFY; then
    step "체크섬 검증"
    ( cd "$WORK" && sha256sum -c "$SUMS_FILE" )
    exit 0
fi

# ── libnl ────────────────────────────────────────────────────────
# wpa_supplicant 의 nl80211 드라이버가 요구한다. 정적으로만 쓴다.
if [[ -f "${SYSROOT}/lib/libnl-3.a" ]] && ! $FORCE; then
    log "libnl 이미 있음"
else
    step "libnl ${LIBNL_VER}"
    ( cd "${WORK}/libnl-${LIBNL_VER}"
      ./configure --host="$HOST" --build="$BUILD" --prefix="$SYSROOT" \
          --disable-shared --enable-static --disable-cli \
          CC="${CROSS}gcc" CFLAGS="-Os" > /tmp/libnl-conf.log 2>&1 \
          || { tail -15 /tmp/libnl-conf.log; die "libnl configure 실패"; }
      make -j"$JOBS" > /tmp/libnl-make.log 2>&1 \
          || { grep -iE "error" /tmp/libnl-make.log | head -10; die "libnl 빌드 실패"; }
      make install > /dev/null 2>&1 || die "libnl 설치 실패" )
    log "libnl-3.a $(stat -c%s "${SYSROOT}/lib/libnl-3.a") bytes"
fi

# ── dropbear ─────────────────────────────────────────────────────
DB_DIR="${WORK}/dropbear-${DROPBEAR_VER}"
if [[ -f "${DB_DIR}/dropbear" ]] && ! $FORCE; then
    log "dropbear 이미 있음"
else
    step "dropbear ${DROPBEAR_VER}"
    # 설정은 저장소에서 복사한다. 빌드 트리 쪽을 고쳐봐야 다음에 지울 때
    # 사라지고, 그 사실을 알아차릴 방법이 없다.
    cp "${CONF_DIR}/dropbear-localoptions.h" "${DB_DIR}/localoptions.h"
    ( cd "$DB_DIR"
      ./configure --host="$HOST" --build="$BUILD" \
          --disable-zlib --disable-pam --disable-utmp --disable-wtmp \
          --disable-lastlog --disable-loginfunc --disable-harden \
          CC="${CROSS}gcc" > /tmp/db-conf.log 2>&1 \
          || { tail -15 /tmp/db-conf.log; die "dropbear configure 실패"; }
      make -j"$JOBS" PROGRAMS="dropbear dropbearkey" \
          STATIC=1 SCPPROGRESS=0 > /tmp/db-make.log 2>&1 \
          || { grep -iE "error" /tmp/db-make.log | head -10; die "dropbear 빌드 실패"; }
      "${CROSS}strip" dropbear dropbearkey )
    log "dropbear $(stat -c%s "${DB_DIR}/dropbear") bytes"
fi

# 비밀번호 인증이 정말 빠졌는지 확인한다. 이것이 이 스크립트가 있는
# 이유의 절반이다: 설정을 놓친 빌드는 아무 오류도 내지 않고, 그냥
# 비밀번호로 들어올 수 있는 SSH 서버가 된다.
if "${CROSS}strings" "${DB_DIR}/dropbear" 2>/dev/null | grep -q "^password$"; then
    die "dropbear 에 비밀번호 인증이 들어 있습니다. localoptions.h 가 적용되지 않았습니다."
fi
log "비밀번호 인증 없음 (공개키만)"

# ── wpa_supplicant ───────────────────────────────────────────────
WPA_DIR="${WORK}/wpa_supplicant-${WPA_VER}/wpa_supplicant"
if [[ -f "${WPA_DIR}/wpa_supplicant" ]] && ! $FORCE; then
    log "wpa_supplicant 이미 있음"
else
    step "wpa_supplicant ${WPA_VER}"
    sed "s|@SYSROOT@|${SYSROOT}|g" "${CONF_DIR}/wpa_supplicant.config" \
        > "${WPA_DIR}/.config"
    ( cd "$WPA_DIR"
      make clean > /dev/null 2>&1 || true
      make -j"$JOBS" CC="${CROSS}gcc" wpa_supplicant wpa_cli \
          > /tmp/wpa-make.log 2>&1 \
          || { grep -iE "error" /tmp/wpa-make.log | head -15; die "wpa_supplicant 빌드 실패"; }
      "${CROSS}strip" wpa_supplicant wpa_cli )
    log "wpa_supplicant $(stat -c%s "${WPA_DIR}/wpa_supplicant") bytes"
fi

step "결과"
MISSING=0
for f in "${DB_DIR}/dropbear" "${DB_DIR}/dropbearkey" \
         "${WPA_DIR}/wpa_supplicant" "${WPA_DIR}/wpa_cli"; do
    if [[ -f "$f" ]]; then
        # 정적 aarch64 인지 확인한다. 크로스 빌드가 조용히 호스트
        # 컴파일러로 넘어가면 여기서 잡힌다.
        arch=$("${CROSS}objdump" -f "$f" 2>/dev/null \
               | awk '/file format/ { print $NF }')
        printf '  %-32s %9s bytes  %s\n' \
            "$(basename "$f")" "$(stat -c%s "$f")" "$arch"
        [[ "$arch" == *aarch64* ]] || { echo "     !! aarch64 가 아닙니다"; MISSING=1; }
    else
        printf '  %-32s %s\n' "$(basename "$f")" "없음"
        MISSING=1
    fi
done
echo ""
[[ "$MISSING" == "0" ]] || die "빠진 것이 있습니다"
echo "  준비 완료. userland/mkrootfs.sh 가 여기서 가져갑니다."
