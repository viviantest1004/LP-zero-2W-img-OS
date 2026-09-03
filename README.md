# LP-zero

라즈베리파이 제로 2 W (BCM2710A1 / Cortex-A53) 용 **자작 리눅스 배포판**.

GPU 부트ROM 이 요구하는 Broadcom 클로즈드 블롭과, 암호 구현 두 개
(dropbear, wpa_supplicant) 를 빼면 전부 직접 작성했다 — 커널 설정,
libc, init, 셸, 그리고 90개 가까운 명령어.

시스템 전체가 파일 하나다. 루트 파일시스템은 커널 이미지 안의 cpio 이고
부팅할 때 램으로 풀린다. 디스크 위의 어떤 것도 돌아가는 시스템의 일부가
아니다 — 그래서 전원을 갑자기 뽑아도 시스템이 깨질 데가 없고,
업데이트가 파일 교체 하나로 끝난다.

| | |
|---|---|
| 커널 + 유저랜드 | 22MB (파일 하나) |
| 부팅 후 남는 램 | 480MB / 512MB |
| SSH | 공개키 전용 (비밀번호 인증은 아예 컴파일 안 됨) |
| 파이썬 | CPython 3.12 + pip. manylinux 휠도 설치됨 (numpy 확인) |
| 방화벽 | 기본 켜짐. nftables 를 netlink 로 직접 |

## 이걸로 뭘 하나

`examples/` 에 바로 쓰는 `/data/rc.local` 스크립트 네 개가 있다.

```bash
scp examples/temperature-log.sh root@<보드>:/data/rc.local
```

| | 하는 일 |
|---|---|
| `temperature-log.sh` | 온도·메모리·부하를 CSV 로 계속 기록 |
| `webhook-notify.sh` | 주소가 바뀌면 URL 로 알림 |
| `http-server.sh` | 디렉터리를 HTTP 로 서비스 |
| `usb-backup.sh` | USB 를 꽂으면 자동 백업 |

## 몇 달 방치하는 기계라는 것

- **워치독** — 커널이 멎으면 보드가 스스로 리셋
- **guard** — 메모리·발열·전압·CPU·디스크를 한 데몬이 감시. SSH 는 마지막까지 살린다
- **beacon** — 5분마다 상태를 URL 로 보고. *보고가 끊기는 게 신호*다
- **update** — 시스템 교체가 원자적이고, 세 번 짧게 부팅하면 스스로 되돌아간다
- **bootcount** — 부팅 루프면 `rc.local` 을 건너뛴다
- **authkey** — `/data` 가 통째로 날아가도 SSH 키를 부트 파티션에서 복구

자세히: [docs/06-safety.md](docs/06-safety.md)

## 빠른 시작

이미 만들어진 이미지를 쓰려면 `dist/` 의 것을 구우면 된다.

```bash
xz -d < dist/LPzero2W-universal.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
```

구운 카드의 FAT 파티션(어느 PC 에서나 열린다)에서 두 파일을 고친다:

- `authorized_keys` — **여기에 공개키를 넣지 않으면 아무도 못 들어간다**
- `wpa_supplicant.conf` — WiFi 이름과 비밀번호

넣고 전원을 넣으면 `ssh -i ~/.ssh/lpzero root@<주소>` 로 들어간다.

### 직접 빌드하기

```bash
./tools/fetch-kernel.sh       # 커널 소스 (고정된 커밋)
./tools/build-sysroot.sh      # CPython 이 링크할 라이브러리들
./tools/build-thirdparty.sh   # dropbear, wpa_supplicant
./tools/build-python.sh       # CPython 3.12 + pip + glibc
./tools/build-fsck.sh         # e2fsck, mke2fs

make image                    # 유저랜드 -> 커널 -> SD 이미지
```

앞의 다섯은 한 번만 돌리면 되고, 매일 도는 것은 `make image` 다.
빌드 산출물은 저장소 옆 `.build/` 에 들어간다 (`LPZERO_WORK` 로 옮길 수 있다).

전체 툴체인은 `Dockerfile` 에 고정되어 있다:

```bash
docker build -t lpzero .
docker run --rm -it -v "$PWD":/src -w /src lpzero
```

자세히: [docs/07-building.md](docs/07-building.md)

### 이 시스템용 프로그램 만들기

여기 바이너리는 전부 정적이고 libc 도 자체 구현이라, 보통의 aarch64
크로스 컴파일러로 지은 것은 실행되지 않는다.

```bash
./tools/build-sdk.sh
sdk/bin/lp-gcc -o myprog myprog.c

mkdir -p stage/bin && cp myprog stage/bin/
./tools/mkpkg.sh myprog 1.0 stage      # 패키지로
```

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
