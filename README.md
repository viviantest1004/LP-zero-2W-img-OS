# LP-zero

라즈베리파이 제로 2 W (BCM2710A1 / Cortex-A53) 용 **자작 펌웨어 · OS**.

GPU 부트ROM 이 요구하는 Broadcom 클로즈드 블롭 3개를 제외하면
부팅 코드부터 드라이버, 부트로더, 유저랜드까지 전부 직접 작성한다.

현재 상태: **리눅스 부팅까지 동작.** 자체 커널이 자체 유저랜드를 띄운다. 베어메탈 펌웨어가 부팅해서
HDMI 화면에 스플래시를 띄우고, 시리얼과 화면 양쪽으로 보드 정보를 덤프한 뒤
대화형 모니터를 띄운다. 이미지 크기 20KB. QEMU 로 검증 완료.

아직 리눅스 커널도 OS 도 없다 — 펌웨어 단계다. [로드맵](docs/00-roadmap.md) 참조.

## 빠른 시작

```bash
# 1. Broadcom GPU 펌웨어 받기 (최초 1회)
make blobs

# 2. 펌웨어 빌드
make firmware

# 3. 부팅 가능한 SD 이미지 만들기
make sdcard

# 또는 한 번에
make all-in-one
```

SD카드에 굽기:
```bash
sudo dd if=sdcard/lp-zero.img of=/dev/sdX bs=4M conv=fsync status=progress
```

시리얼 콘솔 연결 후 (115200 8N1) 전원을 넣으면:

> ⚠️ USB-TTL 어댑터는 **반드시 3.3V**. 5V 를 GPIO15 에 물리면 보드가 죽는다.
> 구분법과 배선은 [하드웨어 문서](docs/02-hardware.md#시리얼-콘솔-배선-필수) 참조.

```
 LP-zero firmware 0.1.0-phase1
 test_a_123_LPzero2W_img
 Raspberry Pi Zero 2 W / BCM2710A1 / Cortex-A53
================================================

[cpu]
  Exception level  : EL1
  MIDR_EL1         : 0x410fd034  (part 0xd03, rev 4)
  ...
[board]
  model            : Raspberry Pi Zero 2 W (rev 1.0)
  processor        : BCM2837
  ...
```

`h` 를 누르면 명령 목록이 나온다.

## 부팅 화면 문구 바꾸기

`firmware/include/splash.h` 한 파일만 고치면 된다.

```c
#define SPLASH_TITLE     "test_a_123_LPzero2W_img"       // 큰 글씨 (폭에 맞춰 자동 축소)
#define SPLASH_SUBTITLE  "Raspberry Pi Zero 2 W  /  BCM2710A1"
#define SPLASH_LINES { \
    "bare-metal firmware, built from scratch", \
    NULL }
#define SPLASH_DWELL_MS  2500      // 몇 ms 보여줄지. 0 이면 바로 로그로
```

고치고 `make firmware && make sdcard`.

**이미지 파일명**을 바꾸려면 `config.mk` 의 `KERNEL_IMAGE` 와
`boot/config.txt` 의 `kernel=` **두 곳**을 같게 맞춘다. 어긋나면 GPU 가
커널을 못 찾아 아무 출력 없이 멈춘다 — `make sdcard` 가 굽기 전에 검사한다.

폰트는 ASCII(0x20~0x7E)만 구워져 있어서 **한글은 `?` 로 나온다.**
한글이 필요하면 16x16 셀 글리프를 추가로 구워야 한다.

## 에뮬레이터로 테스트

실기 없이 QEMU 에서 바로 돌려볼 수 있다.

```bash
apt install qemu-system-arm

make qemu        # 대화형 (종료: Ctrl-A 다음 X)
make qemu-log    # 시리얼 로그만 뽑기
make qemu-shot   # 부팅 화면 캡처 -> qemu-out/*.ppm
```

**주의: SD 이미지는 에뮬레이터에서 부팅되지 않는다.** QEMU 는 VideoCore
GPU 를 흉내내지 않아서 Broadcom 블롭이 실행될 수 없다. 대신
`kernel8.img` 를 `-kernel` 로 직접 올린다. 자세한 내용은 [에뮬레이션 문서](docs/03-emulation.md) 참조.
(아이폰 UTM 은 라즈베리파이 머신 타입을 못 써서 사실상 불가하다.)

## 빌드 요구사항

**크로스 툴체인을 따로 설치하지 않는다.** clang 은 태생이 크로스
컴파일러라 `--target=aarch64-none-elf` 만 주면 된다.

```bash
apt install clang lld llvm       # 컴파일
apt install mtools dosfstools    # SD 이미지 생성
apt install qemu-system-arm      # (선택) 에뮬레이터 테스트
```

## 구조

```
firmware/           베어메탈 펌웨어 (전부 직접 작성)
  src/boot.S          최초 진입점 - 코어 파킹, EL 강하, 스택, BSS
  src/main.c          시스템 정보 덤프 + 시리얼 모니터
  src/uart.c          PL011 UART0 드라이버
  src/mbox.c          VideoCore 메일박스
  src/gpio.c          GPIO
  src/timer.c         1MHz 시스템 타이머
  src/printf.c        libc 없는 printf
  src/board.c         리비전 코드 해석, 워치독 재부팅
  src/fb.c            메일박스 프레임버퍼 할당 + 픽셀 그리기
  src/console.c       프레임버퍼 텍스트 콘솔 (스크롤, 확대 글씨)
  src/font_8x16.c     비트맵 폰트 (tools/mkfont.py 가 생성)
  src/string.c        memset/memcpy 등
  include/splash.h    ★ 부팅 화면 문구는 여기서 고친다
  linker.ld           0x80000 고정 배치
config.mk           ★ 커널 이미지 파일명 (config.txt 의 kernel= 과 일치해야 함)
kernel/             최소 리눅스 커널 설정과 빌드
  lp-zero.config      bcm2711_defconfig 에 덮어쓰는 조각
  build.sh            빌드 + 조각 반영 전수 검증
  test-qemu.sh        QEMU 부팅 검증
userland/           자체 libc + init + 셸 (외부 libc 없음)
  libc/               crt0 부터 printf 까지 전부 직접
  init/               PID 1
  sh/                 셸 - 리다이렉션, 파이프
  cat/  ls/           유틸리티
boot/config.txt     GPU 부팅 설정
tools/              블롭 다운로드, SD 이미지 생성, QEMU 실행, 폰트 굽기
docs/               로드맵 · 부팅 사슬 · 하드웨어 참조
```

## 문서

- [로드맵](docs/00-roadmap.md) — 5단계 계획, 무엇을 직접 만들고 무엇을 받는가
- [부팅 사슬](docs/01-boot-chain.md) — 전원 ON 부터 우리 코드까지
- [하드웨어 참조](docs/02-hardware.md) — 메모리 맵, 시리얼 배선, 512MB 제약
- [에뮬레이션](docs/03-emulation.md) — QEMU / UTM 으로 실기 없이 테스트
- [유저랜드](docs/04-userland.md) — 자체 libc · init · 셸, AArch64 시스템콜
- [커널](docs/05-kernel.md) — 최소 리눅스 만들기, 37.7MB → 15.7MB 과정

## 라이선스

MIT — [LICENSE](LICENSE) 참조.

`blobs/` 에 받아지는 Broadcom 펌웨어는 이 라이선스에 포함되지 않으며
저장소에 커밋하지 않는다 (`raspberrypi/firmware` 의 자체 라이선스를 따른다).
