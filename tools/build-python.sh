#!/usr/bin/env bash
#
# build-python.sh - CPython 을 aarch64 정적 바이너리로 크로스 빌드한다.
#
# 결과는 /data 파티션에 들어간다. 시스템(initramfs)에 넣지 않는 이유:
# CPython 은 인터프리터 + 표준 라이브러리가 15~25MB 다. 커널에 내장하면
# 이미지가 23MB 에서 40MB 이상으로 커진다. 반면 /data 는 첫 부팅에
# 카드 전체로 늘어나므로 크기가 문제되지 않는다.
#
# 크로스 빌드의 핵심:
#   --with-build-python  빌드 도중 파이썬 코드를 실행해야 하는데
#                        타겟 바이너리는 여기서 돌지 않는다. 그래서
#                        호스트에 같은 **major.minor** 버전이 있어야 한다.
#   ac_cv_file__dev_*    configure 가 타겟에서 파일 존재를 시험하려 하지만
#                        크로스 환경에서는 불가능하다. 답을 미리 준다.
#
# 환경변수:
#   PY_SRC     소스 경로 (기본 /home/user/kernel-work/thirdparty/Python-3.12.3)
#   BUILD_PY   호스트 파이썬 (기본 /usr/bin/python3.12)
#   JOBS       병렬 빌드 수

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY_SRC="${PY_SRC:-/home/user/kernel-work/thirdparty/Python-3.12.3}"
BUILD_PY="${BUILD_PY:-/usr/bin/python3.12}"
STAGE="${STAGE:-/home/user/kernel-work/python-stage}"
SYSROOT="${SYSROOT:-/home/user/kernel-work/thirdparty/sysroot}"
JOBS="${JOBS:-$(nproc)}"
CROSS=aarch64-linux-gnu-

die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }

[[ -d "$PY_SRC" ]] || die "소스가 없습니다: $PY_SRC"
# _ctypes 는 libffi, zlib 모듈은 zlib 를 요구한다. 둘 다 aarch64 로
# 미리 크로스 빌드해 sysroot 에 넣어두어야 한다.
[[ -f "${SYSROOT}/include/ffi.h" ]]  || die "libffi 가 없습니다: ${SYSROOT}"
[[ -f "${SYSROOT}/include/zlib.h" ]] || die "zlib 가 없습니다: ${SYSROOT}"
command -v "$BUILD_PY" >/dev/null || die "$BUILD_PY 가 없습니다"
command -v "${CROSS}gcc" >/dev/null || die "${CROSS}gcc 가 없습니다"

# 호스트와 타겟의 major.minor 가 같아야 한다
SRC_VER=$(basename "$PY_SRC" | sed 's/Python-//')
HOST_VER=$("$BUILD_PY" -c 'import sys; print("%d.%d" % sys.version_info[:2])')
case "$SRC_VER" in
    "$HOST_VER".*) ;;
    *) die "버전 불일치: 소스 $SRC_VER, 호스트 $HOST_VER" ;;
esac

step "설정 (소스 ${SRC_VER}, 호스트 파이썬 ${HOST_VER})"
cd "$PY_SRC"

# 설정을 바꿔 다시 빌드할 때는 이전 Makefile 을 지워야 한다
[[ -f Makefile && "${RECONFIGURE:-0}" == "1" ]] && make distclean >/dev/null 2>&1

if [[ ! -f Makefile ]]; then
    # 외부 라이브러리가 필요한 모듈은 끈다. 우리 시스템에는 그 라이브러리가
    # 없고, 파이썬을 스크립팅 용도로 쓰는 데는 없어도 된다.
    #   _ssl/_hashlib  OpenSSL     _sqlite3  SQLite
    #   _curses        ncurses     readline  libreadline
    #   _lzma/_bz2     xz/bzip2    _dbm/_gdbm  BerkeleyDB/GDBM
    # 필요해지면 그 라이브러리를 크로스 빌드해 sysroot 에 넣고 여기서 뺀다.
    DISABLED=(
        py_cv_module__ssl=n/a      py_cv_module__hashlib=n/a
        py_cv_module__sqlite3=n/a  py_cv_module__curses=n/a
        py_cv_module__curses_panel=n/a
        py_cv_module_readline=n/a  py_cv_module__lzma=n/a
        py_cv_module__bz2=n/a      py_cv_module__dbm=n/a
        py_cv_module__gdbm=n/a     py_cv_module__tkinter=n/a
        py_cv_module_nis=n/a       py_cv_module__uuid=n/a
    )

    ./configure \
        --host=aarch64-linux-gnu \
        --build=x86_64-linux-gnu \
        --with-build-python="$BUILD_PY" \
        --prefix=/data/python \
        --disable-shared \
        --disable-test-modules \
        --without-ensurepip \
        --without-doc-strings \
        --with-ensurepip=no \
        ac_cv_file__dev_ptmx=no \
        ac_cv_file__dev_ptc=no \
        ac_cv_buggy_getaddrinfo=no \
        CC="${CROSS}gcc" \
        CXX="${CROSS}g++" \
        AR="${CROSS}ar" \
        RANLIB="${CROSS}ranlib" \
        READELF="${CROSS}readelf" \
        CFLAGS="-Os -fno-semantic-interposition -I${SYSROOT}/include" \
        CPPFLAGS="-I${SYSROOT}/include" \
        LDFLAGS="-static -L${SYSROOT}/lib" \
        LIBFFI_INCLUDEDIR="${SYSROOT}/include" \
        "${DISABLED[@]}" \
        > /tmp/py-conf.log 2>&1 || { tail -25 /tmp/py-conf.log; die "configure 실패"; }
    echo "  완료"
else
    echo "  이미 설정됨 (다시 하려면 make distclean)"
fi

step "빌드 (-j${JOBS}) — 오래 걸립니다"
make -j"$JOBS" > /tmp/py-make.log 2>&1 \
    || { grep -iE "error" /tmp/py-make.log | head -20; die "빌드 실패"; }
echo "  완료"

step "스테이징 디렉터리에 설치"
rm -rf "$STAGE"
make install DESTDIR="$STAGE" > /tmp/py-install.log 2>&1 \
    || { tail -20 /tmp/py-install.log; die "설치 실패"; }

PYDIR="${STAGE}/data/python"
[[ -d "$PYDIR" ]] || die "설치 결과가 없습니다: $PYDIR"

step "불필요한 것 정리"
BEFORE=$(du -sm "$PYDIR" | cut -f1)

# 타겟에서 쓰지 않는 것들
rm -rf "${PYDIR}/lib/python${HOST_VER}/idlelib" \
       "${PYDIR}/lib/python${HOST_VER}/tkinter" \
       "${PYDIR}/lib/python${HOST_VER}/turtledemo" \
       "${PYDIR}/lib/python${HOST_VER}/lib2to3" \
       "${PYDIR}/lib/python${HOST_VER}/ensurepip" \
       "${PYDIR}/lib/python${HOST_VER}/config-"* \
       "${PYDIR}/lib/pkgconfig" \
       "${PYDIR}/share" \
       "${PYDIR}/include" 2>/dev/null || true

# 소스(.py)를 지우고 바이트코드만 남기면 절반쯤 줄지만, 오류 메시지에
# 소스 줄이 안 나와 디버깅이 어려워진다. 지금은 소스를 남긴다.
find "$PYDIR" -name '__pycache__' -prune -exec rm -rf {} + 2>/dev/null || true
find "$PYDIR" -name '*.pyc' -delete 2>/dev/null || true

"${CROSS}strip" "${PYDIR}/bin/python${HOST_VER}" 2>/dev/null || true

AFTER=$(du -sm "$PYDIR" | cut -f1)
echo "  ${BEFORE}MB -> ${AFTER}MB"

step "결과"
file "${PYDIR}/bin/python${HOST_VER}" 2>/dev/null | cut -c1-100 || true
echo ""
echo "  설치 위치: ${PYDIR}"
echo "  (SD 이미지의 /data 파티션에 넣으려면 make sdcard-linux)"
