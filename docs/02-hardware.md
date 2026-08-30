# Raspberry Pi Zero 2 W 하드웨어 참조

## SoC

| 항목 | 값 |
|---|---|
| SoC | BCM2710A1 (RP3A0 패키지 — SoC + RAM 을 한 패키지에) |
| CPU | Cortex-A53 ×4, ARMv8-A, 1.0 GHz |
| 캐시 | L1 32KB I + 32KB D (코어당), L2 512KB (공유) |
| RAM | **512MB LPDDR2** ← 이 보드의 핵심 제약 |
| GPU | VideoCore IV @ 400MHz |
| 무선 | CYW43438 — WiFi 2.4GHz b/g/n + BT 4.2 BLE |
| 저장 | microSD (SDIO) |
| USB | micro-USB OTG ×1 |
| 영상 | mini HDMI, CSI-2 카메라 커넥터 |

Pi 3 와 같은 BCM2837 계열이라 **페리페럴 베이스가 `0x3F000000`** 이다.
Pi 4 코드(`0xFE000000`)나 Pi 1 코드(`0x20000000`)를 그대로 가져오면 안 된다.

## 메모리 맵 (ARM 물리 주소)

| 주소 | 블록 |
|---|---|
| `0x00000000` | SDRAM 시작 |
| `0x00000080`~`0x000000FF` | 펌웨어 스핀테이블 (보조 코어 릴리즈용) |
| `0x00080000` | **커널 로드 주소 (kernel8.img)** |
| `0x3F003000` | 시스템 타이머 (1MHz) |
| `0x3F00B200` | 인터럽트 컨트롤러 |
| `0x3F00B880` | VideoCore 메일박스 |
| `0x3F100000` | 파워매니지먼트 / 워치독 |
| `0x3F200000` | GPIO |
| `0x3F201000` | PL011 UART0 |
| `0x3F215000` | AUX (미니 UART, SPI1/2) |
| `0x3F300000` | EMMC (SD카드) |
| `0x40000000` | ARM 로컬 (코어별 타이머/메일박스) |

데이터시트(BCM2835 ARM Peripherals)는 VideoCore 버스 주소 `0x7Exxxxxx`
로 적혀 있다. ARM 물리 주소로 바꾸려면 `0x7E` → `0x3F` 로 치환한다.

## 시리얼 콘솔 배선 (필수)

베어메탈 개발에서 시리얼은 눈이다. USB-TTL(3.3V) 어댑터가 하나 필요하다.
**5V 어댑터를 쓰면 SoC 가 죽는다. 반드시 3.3V.**

```
   Pi Zero 2 W 40핀 헤더 (USB 포트가 아래로 오게 놓고 봤을 때)

   핀 1  ┌─┬─┐  핀 2   (5V)
         │ │ │
   핀 5  ├─┼─┤  핀 6   GND         ──→ 어댑터 GND
   핀 7  ├─┼─┤  핀 8   GPIO14 TXD0 ──→ 어댑터 RX   (교차!)
   핀 9  ├─┼─┤  핀 10  GPIO15 RXD0 ──→ 어댑터 TX   (교차!)
```

터미널:
```bash
screen /dev/ttyUSB0 115200        # 종료: Ctrl-A 다음 K
# 또는
picocom -b 115200 /dev/ttyUSB0    # 종료: Ctrl-A 다음 Ctrl-X
minicom -D /dev/ttyUSB0 -b 115200
```

설정은 **115200 8N1, 흐름제어 없음**.

## ACT LED

온보드 초록 LED 는 **GPIO 29** 에 물려 있다 (업스트림 디바이스트리
`bcm2710-rpi-zero-2-w.dts` 기준, active-low).

극성이 확실하지 않아도 우리 코드는 *토글*하므로 어느 쪽이든 깜빡인다.
시리얼이 안 잡힐 때 "펌웨어가 살아는 있는지" 확인하는 최후의 수단이다.

## 512MB 라는 제약

이 보드에서 가장 중요한 숫자다. `gpu_mem` 으로 GPU 에 떼어주는 만큼
ARM 이 잃는다.

| gpu_mem | ARM 가용 | 용도 |
|---|---|---|
| 16 | 496MB | 완전 헤드리스 |
| 64 | 448MB | 프레임버퍼 사용 (현재 설정) |
| 128 | 384MB | 하드웨어 비디오 디코딩 |

참고로 Raspberry Pi OS Lite 는 부팅 직후 90~120MB 를 쓴다.
우리가 직접 만들면 20~40MB 로 끝난다 — 이 차이가 512MB 에서는 크다.
