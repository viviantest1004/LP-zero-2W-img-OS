# LP-zero

라즈베리파이 제로 2 W (BCM2710A1 / Cortex-A53) 용 **자작 펌웨어 · OS**.

GPU 부트ROM 이 요구하는 Broadcom 클로즈드 블롭 3개를 제외하면
부팅 코드부터 드라이버, 부트로더, 유저랜드까지 전부 직접 작성한다.

현재 상태: **Phase 1 완료** — 베어메탈 펌웨어가 부팅하고 시리얼 콘솔로
보드 정보를 덤프한 뒤 대화형 모니터를 띄운다. 이미지 크기 10.5KB.

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

```
 LP-zero firmware 0.1.0-phase1
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

## 빌드 요구사항

**크로스 툴체인을 따로 설치하지 않는다.** clang 은 태생이 크로스
컴파일러라 `--target=aarch64-none-elf` 만 주면 된다.

```bash
apt install clang lld llvm       # 컴파일
apt install mtools dosfstools    # SD 이미지 생성
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
  src/string.c        memset/memcpy 등
  linker.ld           0x80000 고정 배치
boot/config.txt     GPU 부팅 설정
tools/              블롭 다운로드, SD 이미지 생성
docs/               로드맵 · 부팅 사슬 · 하드웨어 참조
```

## 문서

- [로드맵](docs/00-roadmap.md) — 5단계 계획, 무엇을 직접 만들고 무엇을 받는가
- [부팅 사슬](docs/01-boot-chain.md) — 전원 ON 부터 우리 코드까지
- [하드웨어 참조](docs/02-hardware.md) — 메모리 맵, 시리얼 배선, 512MB 제약

## 라이선스

MIT — [LICENSE](LICENSE) 참조.

`blobs/` 에 받아지는 Broadcom 펌웨어는 이 라이선스에 포함되지 않으며
저장소에 커밋하지 않는다 (`raspberrypi/firmware` 의 자체 라이선스를 따른다).
