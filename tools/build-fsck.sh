#!/usr/bin/env bash
#
# build-fsck.sh - e2fsck 를 aarch64 정적 바이너리로 크로스 빌드한다.
#
# 왜 이게 시스템 이미지(initramfs) 안에 들어가야 하는가:
#
#   ext4 가 깨졌을 때 고칠 도구가 /data 에 있으면 소용이 없다. 고쳐야 할
#   대상이 바로 그 /data 이기 때문이다. errors=remount-ro 가 손상을
#   알아채고 읽기 전용으로 바꾸는 데까지는 하지만, 그 다음에 할 수 있는
#   일이 없으면 카드를 뽑아 PC 로 가져가는 것 말고는 방법이 없다.
#   헤드리스로 선반에 올려둔 보드에서 그건 사실상 복구 불가와 같다.
#
# 정적 링크인 이유:
#
#   우리 initramfs 에는 동적 로더가 없다. 정적 바이너리는 로더가 필요
#   없으므로 glibc 로 정적 링크해도 그대로 돈다.
#
# e2fsck 와 mke2fs 만 남기고 나머지(resize2fs, dumpe2fs ...)는 버린다.
# 파티션을 만들거나 늘리는 일은 이미 mksdcard.sh 와 expandfs 가 한다.
#
# 환경변수:
#   E2FS_SRC   소스 경로 (기본 thirdparty/e2fsprogs-1.47.0)
#   JOBS       병렬 빌드 수

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/common.sh
source "${REPO_ROOT}/tools/common.sh"

THIRDPARTY="${THIRDPARTY:-${WORK}}"
E2FS_SRC="${E2FS_SRC:-${THIRDPARTY}/e2fsprogs-1.47.0}"
E2FS_TAR="${THIRDPARTY}/e2fsprogs.tar.gz"
OUT="${REPO_ROOT}/userland/prebuilt"
JOBS="${JOBS:-$(nproc)}"
CROSS=aarch64-linux-gnu-

die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }

command -v "${CROSS}gcc" >/dev/null || die "${CROSS}gcc 가 없습니다"

if [[ ! -d "$E2FS_SRC" ]]; then
    [[ -f "$E2FS_TAR" ]] || die "소스도 tarball 도 없습니다: $E2FS_TAR"
    step "압축 해제"
    tar xf "$E2FS_TAR" -C "$THIRDPARTY"
fi

cd "$E2FS_SRC"

if [[ ! -f Makefile || "${RECONFIGURE:-0}" == "1" ]]; then
    step "설정"
    # --disable-* 로 빼는 것들:
    #   nls        번역 파일. 영어만 쓴다
    #   uuidd      UUID 데몬. 데몬을 하나 더 돌릴 이유가 없다
    #   fsck       래퍼. 우리가 우리 것을 쓴다
    #   tdb        추적 DB. e2fsck 에 필요 없다
    #
    # libuuid 와 libblkid 는 끄지 않는다. --disable-libuuid 는 "번들
    # 라이브러리를 만들지 말고 시스템 것을 쓰라"는 뜻이라, 크로스
    # 환경에서는 없는 aarch64 libuuid 를 찾다가 configure 가 멈춘다.
    # 번들된 것을 그대로 쓰면 정적으로 함께 링크된다.
    ./configure \
        --host=aarch64-linux-gnu \
        --build=x86_64-linux-gnu \
        --disable-nls \
        --disable-uuidd \
        --disable-fsck \
        --disable-tdb \
        --disable-debugfs \
        --disable-imager \
        --disable-resizer \
        --disable-defrag \
        --disable-e2initrd-helper \
        --disable-testio-debug \
        CC="${CROSS}gcc" \
        AR="${CROSS}ar" \
        RANLIB="${CROSS}ranlib" \
        CFLAGS="-Os" \
        LDFLAGS="-static" \
        > /tmp/e2fs-conf.log 2>&1 \
        || { tail -25 /tmp/e2fs-conf.log; die "configure 실패"; }
    echo "  완료"
fi

step "빌드 (-j${JOBS})"
if ! make -j"$JOBS" > /tmp/e2fs-make.log 2>&1; then
    grep -E "error:|undefined reference|cannot find -l|^make.*Error" \
        /tmp/e2fs-make.log | awk 'NR <= 20' || true
    die "빌드 실패 (전체 로그: /tmp/e2fs-make.log)"
fi
echo "  완료"

# 두 개를 가져간다.
#
#   e2fsck   망가진 /data 를 부팅할 때 고친다
#   mke2fs   새 디스크를 /data 로 만든다 (datadisk --format)
#
# 둘 다 부트 파티션(FAT)에 실린다. 시스템 이미지에 넣으면 initramfs 가
# 램에 상주하므로 3MB 를 영구히 먹는데, 둘 다 평소에는 아무 일도 하지
# 않는 프로그램이다. 부트 파티션에 두면 램을 쓰지 않고, 읽기 전용으로
# 마운트되므로 자기가 고칠 파일시스템의 고장에 휩쓸리지도 않는다.
step "결과"
mkdir -p "$OUT"

for tool in e2fsck mke2fs; do
    case "$tool" in
        e2fsck) BIN="${E2FS_SRC}/e2fsck/e2fsck" ;;
        mke2fs) BIN="${E2FS_SRC}/misc/mke2fs" ;;
    esac
    [[ -f "$BIN" ]] || die "${tool} 이 만들어지지 않았습니다"

    cp "$BIN" "${OUT}/${tool}"
    "${CROSS}strip" "${OUT}/${tool}"

    # 동적 링크로 나오면 우리 시스템에서 실행되지 않는다. 여기서 잡는다.
    INTERP=$("${CROSS}readelf" -l "${OUT}/${tool}" 2>/dev/null \
             | grep -c "interpreter" || true)
    [[ "$INTERP" == "0" ]] \
        || die "${tool} 이 정적이 아닙니다 - 이 시스템에는 로더가 없습니다"

    printf '  %s  (%s bytes, 정적)\n' \
        "${OUT}/${tool}" "$(stat -c%s "${OUT}/${tool}")"
done

# mke2fs 는 /etc/mke2fs.conf 를 읽는다. 없으면 컴파일 시 박힌 기본값으로
# 돌긴 하지만, 어떤 기능을 켤지가 그 파일에 있으므로 함께 실어둔다.
if [[ -f "${E2FS_SRC}/misc/mke2fs.conf.in" ]]; then
    sed -e 's/@[A-Z_]*@//g' "${E2FS_SRC}/misc/mke2fs.conf.in" \
        > "${OUT}/mke2fs.conf"
    printf '  %s  (%s bytes)\n' \
        "${OUT}/mke2fs.conf" "$(stat -c%s "${OUT}/mke2fs.conf")"
fi
