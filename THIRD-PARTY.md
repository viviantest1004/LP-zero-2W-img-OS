# 외부 구성요소와 라이선스

LP의 자체 코드(이 저장소의 `desktop/`, `userland/`, `tools/` 등, 아래에 따로 적은 것
말고)는 [MIT 라이선스](LICENSE)입니다.

LP 이미지에는 다른 사람들이 만든 오픈소스 소프트웨어와 재배포가 허락된 펌웨어가 함께
들어 있고, 각각 자기 라이선스를 따릅니다. 이 문서는 그 목록과, 소스 코드를 받는 곳입니다.
설치된 시스템에서는 같은 내용이 `/usr/share/doc/lp/` 에 있습니다.

## 리눅스 커널 — GPL-2.0

- 출처: <https://github.com/raspberrypi/linux> 의 `rpi-6.12.y` 브랜치,
  커밋은 [`kernel/linux.commit`](kernel/linux.commit)
- LP가 바꾼 것: [`kernel/patches/`](kernel/patches) 의 패치들
- 설정: [`kernel/`](kernel) 의 `*.config`, `*.fragment`
- 빌드 방법: [`kernel/build.sh`](kernel/build.sh)

위 커밋에 패치를 적용하고 같은 설정으로 빌드하면 이미지에 든 커널이 나옵니다.

## 데비안 12(bookworm) 패키지

데스크톱의 바탕은 데비안 패키지입니다(GTK, GLib, Firefox ESR, LibreOffice, Mesa,
PipeWire, wayfire, foot, bash, coreutils 등). 라이선스는 패키지마다 다르며
(GPL-2.0/3.0, LGPL-2.1/3.0, MPL-2.0, MIT, BSD 등), 각 패키지의 저작권과 라이선스 전문은
설치된 시스템의 `/usr/share/doc/<패키지>/copyright` 에 있습니다.

- 이미지에 든 패키지와 버전 목록: [`dist/SOURCES.txt`](dist/SOURCES.txt)
  (설치된 시스템에서는 `/usr/share/doc/lp/SOURCES.txt`)
- 소스: 목록의 소스 패키지 이름과 버전 그대로
  <https://snapshot.debian.org/> 에서 받을 수 있습니다.
  예: `https://snapshot.debian.org/package/<소스 패키지>/<버전>/`

## LP가 고친 데비안 외 프로그램

| 구성요소 | 라이선스 | 소스 |
| --- | --- | --- |
| sway 1.7 (LP 패치) | MIT | 원본 <https://github.com/swaywm/sway/releases/tag/1.7>, 패치 [`desktop/compositor/sway-1.7-lp.patch`](desktop/compositor/sway-1.7-lp.patch), 빌드 [`desktop/compositor/build-sway.sh`](desktop/compositor/build-sway.sh) |
| wayfire 0.7.4 (LP 패치) | MIT | 원본 <https://github.com/WayfireWM/wayfire/releases/tag/v0.7.4> (데비안 `wayfire_0.7.4-3+deb12u1`), 패치 [`desktop/compositor/wayfire-0.7.4-lp.patch`](desktop/compositor/wayfire-0.7.4-lp.patch), 빌드 [`desktop/compositor/build-wayfire.sh`](desktop/compositor/build-wayfire.sh) |
| wlroots 0.15.1 (LP 패치) | MIT | 원본 <https://gitlab.freedesktop.org/wlroots/wlroots/-/tags/0.15.1>, 패치 [`desktop/compositor/wlroots-0.15.1-lp.patch`](desktop/compositor/wlroots-0.15.1-lp.patch), 빌드 [`desktop/compositor/build-wlroots.sh`](desktop/compositor/build-wlroots.sh) |

## LP 자체 사용자 공간에 들어간 외부 코드

| 구성요소 | 라이선스 | 소스 |
| --- | --- | --- |
| BearSSL | MIT | [`thirdparty/bearssl/`](thirdparty/bearssl) (라이선스: `LICENSE.txt`) |
| Dropbear 2024.86 | MIT 계열 (원본의 `LICENSE`) | <https://matt.ucc.asn.au/dropbear/releases/dropbear-2024.86.tar.bz2>, 설정 [`thirdparty/dropbear-localoptions.h`](thirdparty/dropbear-localoptions.h) |
| wpa_supplicant 2.11 | BSD-3-Clause | <https://w1.fi/releases/wpa_supplicant-2.11.tar.gz>, 설정 [`thirdparty/wpa_supplicant.config`](thirdparty/wpa_supplicant.config) |
| e2fsprogs 1.47.0 (e2fsck, mke2fs, resize2fs) | GPL-2.0 | <https://mirrors.edge.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v1.47.0/e2fsprogs-1.47.0.tar.gz>, 빌드 [`tools/build-fsck.sh`](tools/build-fsck.sh) |

빌드 방법은 [`tools/build-thirdparty.sh`](tools/build-thirdparty.sh) 에 있습니다.

## 글꼴 — SIL Open Font License 1.1

| 글꼴 | 저작권 | 라이선스 전문 |
| --- | --- | --- |
| Pretendard 1.3.9 | Copyright (c) 2021, Kil Hyung-jin | 설치된 시스템의 `/usr/share/fonts/truetype/pretendard/LICENSE.txt` |
| D2Coding 1.3.2 | Copyright (c) 2015-2016 NHN Corporation | [`desktop/fonts/LICENSE.D2Coding.txt`](desktop/fonts/LICENSE.D2Coding.txt), 설치된 시스템의 `/usr/share/fonts/truetype/d2coding/LICENSE.txt` |
| Noto Sans CJK, 나눔 글꼴 등 | 각 저작권자 | 데비안 패키지(`fonts-noto-cjk`, `fonts-nanum` 등)의 `copyright` |

## 펌웨어

무선랜·블루투스·그래픽 칩을 움직이는 펌웨어(Intel i915·마이크로코드, AMD 마이크로코드, Qualcomm ath10k,
Broadcom 블루투스, 무선 규제 데이터베이스)는 linux-firmware 에서 가져온 것으로, 각
제조사의 재배포 라이선스를 따릅니다. 저장소에는 넣지 않고 빌드할 때
[`tools/fetch-pc-fw.sh`](tools/fetch-pc-fw.sh) 가 받으며, 라이선스 전문은 이미지와
설치된 시스템의 `/usr/lib/firmware/LICENSES/` 에 함께 들어갑니다.

## 소스를 받을 수 없을 때

위 방법으로 소스를 받을 수 없으면 저장소의 이슈로 요청해 주세요. 이미지를 배포한
날부터 3년 동안, 그 이미지에 든 GPL 구성요소의 소스 코드를 제공합니다.

## 상표

Linux®는 Linus Torvalds의 등록 상표입니다. Debian은 Software in the Public Interest,
Inc.의 등록 상표이고, Firefox는 Mozilla Foundation의 상표이며, Dell은 Dell Inc.의
상표입니다. LP는 이들과 관계가 없으며 이들의 보증을 받지 않았습니다. 부팅 화면에 나오는
컴퓨터 제조사 로고는 LP에 들어 있지 않으며, 컴퓨터 펌웨어에 저장된 것을 그대로 표시합니다.
