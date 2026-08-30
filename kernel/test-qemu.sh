#!/usr/bin/env bash
#
# test-qemu.sh - 빌드한 커널을 QEMU 에서 부팅시켜 본다.
#
# 우리 유저랜드가 커널에 내장되어 있으므로 Image 파일 하나만 주면
# 부팅해서 셸까지 올라온다. SD 이미지도 initramfs 파일도 필요 없다.
#
# 머신은 raspi3ap (BCM2837, 512MB) - Zero 2 W 와 SoC 계열·메모리가 같다.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${REPO_ROOT}/kernel/out/Image"
LOG_DIR="${REPO_ROOT}/kernel/out"
LOG="${LOG_DIR}/boot.log"
MACHINE=raspi3ap
TIMEOUT="${TIMEOUT:-90}"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ -f "$IMAGE" ]] || die "kernel/out/Image 가 없습니다. ./kernel/build.sh 를 먼저."
command -v qemu-system-aarch64 >/dev/null || die "qemu-system-aarch64 가 없습니다"

MODE="${1:-log}"

case "$MODE" in
interactive)
    echo "QEMU ${MACHINE}. 종료: Ctrl-A 다음 X"
    exec qemu-system-aarch64 -M "$MACHINE" -kernel "$IMAGE" \
        -append "console=ttyAMA0,115200 rootwait" \
        -serial mon:stdio -display none
    ;;
log)
    rm -f "$LOG"
    echo "${TIMEOUT}초간 부팅하고 로그를 수집합니다..."
    # QEMU 의 stdio 채드브는 stdout 이 TTY 가 아니면 출력을 버린다.
    # 파일로 받아야 파이프/CI 에서도 확실하다.
    timeout "$TIMEOUT" qemu-system-aarch64 -M "$MACHINE" -kernel "$IMAGE" \
        -append "console=ttyAMA0,115200 rootwait" \
        -serial "file:${LOG}" -display none >/dev/null 2>&1 || true

    echo ""
    cat "$LOG"
    echo ""
    echo "── 판정 ──"
    if grep -q "LP-zero OS" "$LOG" 2>/dev/null; then
        echo "  OK: init 이 실행되었습니다"
    else
        echo "  실패: init 배너가 없습니다"
    fi
    if grep -qE '\$ $|\$ ' "$LOG" 2>/dev/null; then
        echo "  OK: 셸 프롬프트가 나왔습니다"
    else
        echo "  실패: 셸 프롬프트가 없습니다"
    fi
    grep -qi "kernel panic" "$LOG" 2>/dev/null && echo "  !! 커널 패닉 발생"
    echo ""
    echo "  전체 로그: ${LOG}"
    ;;
*)
    die "알 수 없는 모드 '$MODE' (interactive | log)"
    ;;
esac
