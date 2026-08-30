# LP-zero - 라즈베리파이 제로 2 W 자작 펌웨어/OS
#
# 주요 타겟:
#   make            펌웨어 빌드
#   make blobs      Broadcom GPU 펌웨어 다운로드 (최초 1회)
#   make sdcard     부팅 가능한 SD카드 이미지 생성
#   make all-in-one blobs + firmware + sdcard 한 번에
#   make clean      정리

.PHONY: all firmware blobs verify-blobs sdcard all-in-one clean distclean disasm syms qemu qemu-log qemu-shot help

all: firmware

firmware:
	@$(MAKE) --no-print-directory -C firmware

disasm:
	@$(MAKE) --no-print-directory -C firmware disasm

syms:
	@$(MAKE) --no-print-directory -C firmware syms

blobs:
	@./tools/fetch-blobs.sh

verify-blobs:
	@./tools/fetch-blobs.sh --verify

sdcard: firmware
	@./tools/mksdcard.sh

all-in-one: blobs firmware sdcard

# QEMU 에뮬레이션 (실기 없이 테스트).
# 주의: SD 이미지가 아니라 kernel8.img 를 직접 올린다.
# QEMU 는 VideoCore GPU 를 흉내내지 않아서 Broadcom 블롭이 돌지 않는다.
qemu: firmware
	@./tools/run-qemu.sh interactive

qemu-log: firmware
	@./tools/run-qemu.sh log

qemu-shot: firmware
	@./tools/run-qemu.sh shot

clean:
	@$(MAKE) --no-print-directory -C firmware clean
	@rm -rf qemu-out
	@rm -rf sdcard
	@echo "  sdcard/ 제거"

# 다운로드받은 블롭까지 전부 제거
distclean: clean
	@rm -rf blobs
	@echo "  blobs/ 제거"

help:
	@echo "LP-zero 빌드 타겟:"
	@echo "  make firmware      펌웨어(kernel8.img) 빌드"
	@echo "  make blobs         Broadcom GPU 펌웨어 다운로드"
	@echo "  make verify-blobs  받은 블롭 체크섬 검증"
	@echo "  make sdcard        부팅 가능한 SD 이미지 생성"
	@echo "  make all-in-one    위 세 개를 순서대로"
	@echo "  make qemu          QEMU 에서 실행 (대화형)"
	@echo "  make qemu-log      QEMU 실행 후 시리얼 로그 출력"
	@echo "  make qemu-shot     QEMU 부팅 화면 캡처"
	@echo "  make disasm        펌웨어 디스어셈블"
	@echo "  make syms          심볼 테이블(주소순)"
	@echo "  make clean         빌드 산출물 제거"
	@echo "  make distclean     블롭까지 제거"
