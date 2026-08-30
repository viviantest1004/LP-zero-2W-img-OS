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

## UTM SE (아이폰/아이패드)

UTM SE 는 QEMU 를 iOS 로 포팅한 것이다. iOS 가 JIT 를 막기 때문에
**TCG 인터프리터**로만 돌아 매우 느리지만, 우리 펌웨어는 20KB 라
문제되지 않는다.

설정해야 할 것은 데스크탑과 같다:

| 항목 | 값 |
|---|---|
| 아키텍처 | ARM64 (aarch64) |
| 시스템 / 머신 | `raspi3ap` (없으면 `raspi3b`) |
| 부팅 | `-kernel` 로 `kernel8.img` |
| 디스플레이 | 콘솔/시리얼 + 그래픽 |

QEMU 인자로 직접 주면:
```
-M raspi3ap -kernel kernel8.img -serial mon:stdio
```

**주의할 점**

- `sdcard/lp-zero.img` 를 디스크로 붙이면 안 된다. 위에서 설명한 대로
  부팅되지 않는다. `firmware/kernel8.img` 만 `-kernel` 로 준다.
- UTM 의 UI 는 디스크/ISO 부팅을 전제로 만들어져 있어서, `-kernel` 을
  주려면 "QEMU 추가 인자" 항목을 써야 한다.
- UTM SE 가 빌드에 `raspi*` 머신을 포함했는지는 버전에 따라 다를 수 있다.
  머신 목록에 없으면 이 방법은 쓸 수 없다.

아이폰에서 되긴 하지만 설정이 번거롭다. **가능하면 데스크탑 QEMU 를
권한다** — 스크린샷 캡처, 디스어셈블 로그, gdb 연결이 전부 된다.

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
