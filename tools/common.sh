# common.sh - tools/*.sh 가 공유하는 설정. source 해서 쓴다.
# config.mk 를 파싱해 Makefile 과 같은 이미지 이름을 쓰게 한다.

_read_kernel_image() {
    local mk="${REPO_ROOT}/config.mk"
    if [[ -f "$mk" ]]; then
        sed -n 's/^[[:space:]]*KERNEL_IMAGE[[:space:]]*:*=[[:space:]]*\(.*\)$/\1/p' "$mk" \
            | tail -1 | tr -d '[:space:]'
    fi
}

_read_mk_var() {
    local mk="${REPO_ROOT}/config.mk"
    [[ -f "$mk" ]] || return 0
    sed -n "s/^[[:space:]]*$1[[:space:]]*:*=[[:space:]]*\(.*\)$/\1/p" "$mk" \
        | tail -1 | tr -d '[:space:]'
}

KERNEL_IMAGE="$(_read_kernel_image)"
: "${KERNEL_IMAGE:=kernel8.img}"

LINUX_IMAGE="$(_read_mk_var LINUX_IMAGE)"
: "${LINUX_IMAGE:=Image}"

# ── 빌드 산출물이 놓이는 곳 ──────────────────────────────────────
#
# 커널 소스, 서드파티 소스, 크로스 sysroot, 파이썬 스테이징 - 저장소에
# 커밋하지 않는 것들이 전부 여기 들어간다.
#
# 예전에는 스크립트마다 /home/user/kernel-work 를 기본값으로 박아두었다.
# 그 경로가 있는 기계는 이것을 만든 기계 하나뿐이므로, 저장소를 받은
# 사람은 빌드를 시작할 수조차 없었다. 이제 저장소 옆의 .build 다.
#
#   LPZERO_WORK=/mnt/big/lpzero ./kernel/build.sh
#
# 처럼 환경변수로 옮길 수 있다. 디스크가 부족하거나 여러 저장소가 소스를
# 공유하게 하고 싶을 때 쓴다.
LPZERO_WORK="${LPZERO_WORK:-${REPO_ROOT}/.build}"

WORK="${WORK:-${LPZERO_WORK}/thirdparty}"
SYSROOT="${SYSROOT:-${WORK}/sysroot}"
LINUX_SRC="${LINUX_SRC:-${LPZERO_WORK}/linux}"
BUILD_DIR="${BUILD_DIR:-${LPZERO_WORK}/build}"
PYSTAGE_ROOT="${PYSTAGE_ROOT:-${LPZERO_WORK}/python-stage}"
