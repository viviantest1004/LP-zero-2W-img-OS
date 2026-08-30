# common.sh - tools/*.sh 가 공유하는 설정. source 해서 쓴다.
# config.mk 를 파싱해 Makefile 과 같은 이미지 이름을 쓰게 한다.

_read_kernel_image() {
    local mk="${REPO_ROOT}/config.mk"
    if [[ -f "$mk" ]]; then
        sed -n 's/^[[:space:]]*KERNEL_IMAGE[[:space:]]*:*=[[:space:]]*\(.*\)$/\1/p' "$mk" \
            | tail -1 | tr -d '[:space:]'
    fi
}

KERNEL_IMAGE="$(_read_kernel_image)"
: "${KERNEL_IMAGE:=kernel8.img}"
