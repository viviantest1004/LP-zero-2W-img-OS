# config.mk - 프로젝트 전역 설정. Makefile 과 tools/*.sh 가 같이 읽는다.
#
# 커널 이미지 파일명.
#   · 이 이름이 boot/config.txt 의 kernel= 값과 반드시 일치해야 한다.
#     어긋나면 GPU 가 커널을 못 찾고, 아무 출력도 없이 멈춘다.
#   · tools/mksdcard.sh 가 SD 이미지를 만들 때 둘이 같은지 검사한다.
#   · 이름을 바꾸려면 여기와 boot/config.txt 두 곳을 고친다.
KERNEL_IMAGE := test_a_123_LPzero2W.img
