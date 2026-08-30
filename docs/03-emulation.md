# 에뮬레이터에서 테스트하기 (QEMU / UTM)

실기 없이도 ARM 쪽 코드는 전부 검증할 수 있다.

## 핵심: SD 이미지는 에뮬레이터에서 부팅되지 않는다

**QEMU 는 VideoCore GPU 를 흉내내지 않는다.** 그래서
`sdcard/lp-zero.img` 를 통째로 물려도 부팅되지 않는다 —
`bootcode.bin` 과 `start.elf` 를 실행할 GPU 자체가 없기 때문이다.

대신 QEMU 는 GPU 가 하던 일(커널을 `0x80000` 에 올리고 ARM 을 깨우기)을
자기가 대신 해준다. 그래서 `-kernel kernel8.img` 로 직접 올려야 한다.

| | 실기 | QEMU |
|---|---|---|
| 부팅 시작 | GPU 부트ROM | QEMU 내장 스텁 |
| Broadcom 블롭 | 실행됨 | **실행 안 됨** |
| 넣는 것 | `sdcard/lp-zero.img` (SD카드) | `firmware/kernel8.img` (`-kernel`) |

ARM 쪽 코드는 양쪽에서 동일하게 돈다. 우리가 쓴 코드는 전부 ARM 쪽이므로
에뮬레이터 검증에 의미가 있다.

## 머신 선택

QEMU 에 Zero 2 W 전용 머신은 없다. 가장 가까운 것은 **`raspi3ap`**
(Raspberry Pi 3 Model A+) 이다.

| | Zero 2 W | raspi3ap | raspi3b |
|---|---|---|---|
| SoC | BCM2710A1 | BCM2837 | BCM2837 |
| 코어 | Cortex-A53 ×4 | 동일 | 동일 |
| RAM | 512MB | **512MB** | 1GB |
| 페리페럴 베이스 | `0x3F000000` | 동일 | 동일 |

메모리 크기까지 같아서 `raspi3ap` 이 가장 가깝다. 보드 리비전 코드만
Pi 3A+ 로 보고된다 (우리 코드가 그대로 해석해서 표시한다).

## 데스크탑에서

```bash
apt install qemu-system-arm      # 또는 brew install qemu

make qemu          # 대화형 - 시리얼이 터미널에 붙는다 (종료: Ctrl-A 다음 X)
make qemu-log      # 12초 돌리고 시리얼 로그를 뿌린다
make qemu-shot     # 부팅 화면을 qemu-out/*.ppm 으로 캡처
```

직접 실행하려면:
```bash
qemu-system-aarch64 -M raspi3ap -kernel firmware/kernel8.img \
    -serial mon:stdio -display none
```

### 함정: `-serial stdio` 가 파이프에서 조용하다

QEMU 의 stdio 캐릭터 장치는 stdout 이 TTY 가 아니면 출력을 내보내지
않는다. `qemu ... | head` 처럼 파이프로 넘기면 **아무것도 안 나온다**.
펌웨어가 멀쩡한데도 죽은 것처럼 보인다.

CI 나 스크립트에서는 파일로 받아야 확실하다:
```bash
qemu-system-aarch64 -M raspi3ap -kernel firmware/kernel8.img \
    -serial file:serial.log -display none
```
`make qemu-log` 가 이 방식을 쓴다.

## UTM SE (아이폰/아이패드) — 사실상 안 된다

UTM SE 는 QEMU 를 iOS 로 포팅한 것이라 원리상 될 것 같지만,
**라즈베리파이 머신 타입이 iOS UTM 에서 동작하지 않는다는 보고가 있다.**

UTM 이슈 #6546 (UTM 4.5.3 / iOS 17, 작성 시점 기준 미해결):

> "All other architectures worked, but AARCH64 and ARM32,
>  I found that only QEMU Virtual Machine worked"

즉 iOS UTM 의 ARM64 에서는 generic `virt` 머신만 쓸 수 있고
`raspi*` 계열은 못 쓴다는 뜻이다.

**이게 왜 치명적인가**

우리 펌웨어는 BCM2710 하드웨어 주소에 하드코딩되어 있다:

| | 우리 펌웨어 (BCM2710) | QEMU `virt` 머신 |
|---|---|---|
| 페리페럴 베이스 | `0x3F000000` | 해당 없음 |
| PL011 UART | `0x3F201000` | `0x09000000` |
| 메일박스 | `0x3F00B880` | 없음 (VideoCore 자체가 없다) |

`virt` 에서는 주소가 전부 다르고 메일박스라는 개념 자체가 없다.
따라서 **`raspi*` 를 못 쓰면 우리 펌웨어는 아이폰에서 돌릴 수 없다.**
`virt` 용으로 따로 포팅하면 돌긴 하겠지만, 그건 더 이상 라즈베리파이
펌웨어가 아니다.

**결론: 데스크탑 QEMU 를 쓴다.** `make qemu` 한 줄이고, 스크린샷 캡처와
디스어셈블 로그, gdb 연결까지 전부 된다.

## 실기와 다른 점

에뮬레이터에서 통과해도 실기에서 다를 수 있는 것들:

| 항목 | QEMU | 실기 |
|---|---|---|
| UART 클럭 | 3MHz | 48MHz (`init_uart_clock`) |
| 화면 해상도 | 640×480 고정 | 모니터 EDID 에 따름 |
| 온도 | 25.0°C 고정 | 실제 센서 |
| 보드 시리얼 | 0 | 실제 값 |
| SD/EMMC | 부분 구현 | 실물 |
| 타이밍 | 실시간과 무관 | 실제 |

보레이트를 하드코딩하지 않고 메일박스로 UART 클럭을 조회하도록 만든
이유가 이것이다 — 3MHz 와 48MHz 양쪽에서 그대로 동작한다.
