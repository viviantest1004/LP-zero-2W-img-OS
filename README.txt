linux-LP 데스크탑 이미지 — 합치는 법
=====================================

받으신 조각 20개 (lpzero.img.xz.00 ~ .19) 를 한 폴더에 모으고:

    cat lpzero.img.xz.* > linux-LP_desktop.img.xz

제대로 합쳐졌는지 확인 — 아래 값이 나와야 합니다:

    sha256sum linux-LP_desktop.img.xz
    d41dbbf9329131c5f4526d5c72f346c519f655cf45be04d42743bfc538c9a68f

풀기 (5GB 가 됩니다):

    xz -d linux-LP_desktop.img.xz

굽기:

    sudo dd if=linux-LP_desktop.img of=/dev/sdX bs=4M conv=fsync status=progress

QEMU 로 바로 띄우기:

    qemu-system-x86_64 -m 4096 -smp 4 \
      -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
      -drive if=pflash,format=raw,file=vars.fd \
      -drive file=linux-LP_desktop.img,format=raw,if=virtio \
      -device virtio-vga -device virtio-keyboard-pci -device virtio-tablet-pci


단축키
------
  Ctrl+Space        검색 / 앱 런처
  Super+E           파일
  Super+T           터미널
  Super+I           설정
  Super+N           퀵메뉴
  Ctrl+Shift+Esc    작업 관리자
  Print             화면 전체 스크린샷 (클립보드로)
  Shift+Print       영역 선택
  Ctrl+Shift+Print  스크린샷 앱 (지연·미리보기·그림판으로 열기)
  Super+L           화면 잠금


들어 있는 것
------------
  Firefox, 그림판(Drawing), gThumb(사진 자르기·크기 조절),
  Pitivi + ffmpeg(영상), 문서 뷰어, 계산기, 압축 관리자,
  텍스트 편집기, 이미지 뷰어, mpv, 디스크 유틸리티, 터미널(foot)

  직접 만든 것: 파일 · 설정 · 작업 관리자 · 스크린샷 · 퀵메뉴


알려진 문제
-----------
소리가 나지 않습니다. 커널이 사운드 카드를 잡고 /dev/snd 노드도
열려 있는데, wireplumber 가 pipewire 에 붙자마자 끊깁니다. 원인은
찾지 못했고, 실제 사운드 카드가 있는 기계에서는 시험하지 못했습니다.
부팅 콘솔에 [audio] 로 시작하는 줄이 카드 목록과 실패를 남깁니다.

소스: https://github.com/viviantest1004/LP-zero-2W-img-OS
      브랜치 claude/hohho-xvzof5
