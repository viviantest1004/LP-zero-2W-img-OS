# 커널 — 최소 리눅스 만들기

## 출발점

공식 `bcm2711_defconfig` 에서 깎는다. `tinyconfig` 에서 쌓아올리는 방법도
있지만, 부팅에 꼭 필요한 옵션을 하나씩 찾아내느라 시간이 훨씬 많이 든다.
검증된 defconfig 에서 빼는 쪽이 확실하다.

라즈베리파이 공식 문서 기준 `bcm2711_defconfig` 가 **Pi 3 / 3+ / CM3 /
CM3+ / Zero 2 W / 4 / 400 / CM4** 를 모두 덮는 arm64 defconfig 다.
(`bcmrpi3_defconfig` 는 6.12.20 에서 삭제됐다.)

```bash
make blobs      # 최초 1회
make kernel     # 유저랜드 빌드 -> rootfs 조립 -> 커널 빌드
make kernel-test
```

커널 소스 위치는 `LINUX_SRC` 환경변수로 바꾼다 (기본
`/home/user/kernel-work/linux`).

## 유저랜드를 커널에 내장한다

`CONFIG_INITRAMFS_SOURCE` 에 `userland/rootfs` 디렉터리를 지정하면 커널
빌드가 그 내용을 cpio 로 만들어 이미지 안에 넣는다.

**결과적으로 `Image` 파일 하나만 있으면 셸까지 부팅된다.** 별도의
initramfs 파일도, 루트 파티션도 필요 없다.

## ⚠️ `CONFIG_MODULES=n` 만으로는 커널이 줄지 않는다

가장 크게 헛디딘 부분이라 기록해둔다.

"모듈 1,916개를 없애면 작아지겠지" 하고 `CONFIG_MODULES=n` 만 넣고
빌드했더니 결과가 이랬다:

| | `=y` | `=m` | Image |
|---|---|---|---|
| `bcm2711_defconfig` 원본 | 1,895 | 1,916 | (모듈 별도 ~100MB) |
| `CONFIG_MODULES=n` 만 추가 | **2,426** | 0 | **37.7 MB** |

**늘었다.** 이유는 `olddefconfig` 의 동작에 있다. 모듈 지원이 꺼지면
tristate 옵션에 `m` 값이 유효하지 않게 되고, `olddefconfig` 는 그 옵션을
**기본값으로 되돌린다.** 그런데 많은 드라이버의 기본값이 `y` 다.
그래서 모듈이던 것들이 사라지는 대신 **커널 안으로 들어왔다.**

1차 빌드의 부팅 로그가 증거다:

```
libceph: loaded (mon/osd proto 15/24)
batman_adv: B.A.T.M.A.N. advanced 2024.2 loaded
openvswitch: Open vSwitch switching datapath
NET: Registered PF_VSOCK protocol family
mpls_gso: MPLS GSO support
```

Ceph 분산 파일시스템, 메시 네트워크 라우팅, 가상 스위치가 우리
라즈베리파이에 들어가 있었다.

**해결: 서브시스템 루트 옵션을 직접 꺼야 한다.**
`CONFIG_USB_SUPPORT=n`, `CONFIG_ETHERNET=n`, `CONFIG_DRM=n`,
벤더별 `CONFIG_WLAN_VENDOR_*=n` 같은 것들이다.

## 무엇을 끄나

Zero 2 W 의 하드웨어가 정해져 있다는 점이 근거다.

| 끈 것 | 근거 |
|---|---|
| `CONFIG_USB_SUPPORT` | **WiFi 가 SDIO 로 붙는다.** USB 가 아예 필요 없다 |
| `CONFIG_ETHERNET`, `CONFIG_PHYLIB` | 이더넷 포트가 없는 보드다 |
| `CONFIG_WLAN_VENDOR_*` (브로드컴 외) | 칩이 CYW43438 하나로 정해져 있다 |
| `CONFIG_DRM`, `CONFIG_FB` | 1단계는 시리얼 콘솔만. 프레임버퍼는 나중에 |
| `CONFIG_I2C`, `CONFIG_SPI`, `CONFIG_MTD` | 지금 쓰는 주변장치가 없다 |
| `CONFIG_SOUND`, `CONFIG_MEDIA_SUPPORT` | 오디오·카메라 미사용 |
| `CONFIG_IPV6`, `CONFIG_NETFILTER` | IPv4 만 쓴다 |
| `CONFIG_IIO`, `CONFIG_HWMON`, `CONFIG_W1` | 센서 미사용 |
| `CONFIG_FTRACE`, `CONFIG_KPROBES`, 디버그 정보 | 이미지 크기를 크게 먹는다 |

남기는 것은 `docs/00-roadmap.md` 의 저장 구조 절에 정리되어 있다.

## QEMU 테스트는 `virt` 머신으로

`raspi3ap` 이 하드웨어에는 더 가깝지만 **커널 테스트에는 쓸 수 없다.**

QEMU 의 raspi 머신은 디바이스 트리를 만들어주지 않는다. 실기에서는
`start.elf` 가 DTB 를 읽어 메모리 크기·시리얼 번호·활성화할 노드를
패치한 뒤 커널에 넘기는데, QEMU 에는 그 과정이 없다. 공식 DTB 를
`-dtb` 로 직접 줘도 패치되지 않은 상태라 콘솔조차 올라오지 않는다
(실제로 시도했고, 커널은 MMU 설정까지 진행했지만 출력이 한 글자도 없었다).

`virt` 는 QEMU 가 DTB 를 직접 생성한다. CPU 를 `cortex-a53`, 메모리를
512MB 로 맞추면 Zero 2 W 와 같은 조건이 된다.

```bash
qemu-system-aarch64 -M virt -cpu cortex-a53 -m 512 \
    -kernel kernel/out/Image \
    -append "earlycon console=ttyAMA0 rootwait" \
    -serial mon:stdio -display none
```

BCM 고유 주변장치(메일박스, VideoCore, SDIO WiFi)는 여기서 검증되지
않는다. 그건 실기에서 확인해야 한다. 하지만 **커널이 부팅하고
initramfs 를 풀고 우리 init 이 PID 1 로 뜨고 셸이 도는지**는 확인할 수
있고, 그게 이 단계에서 알고 싶은 전부다.

성공하면 이렇게 나온다:

```
[    1.842085] Run /init as init process

  LP-zero OS
  init (pid 1)

/ $
```
