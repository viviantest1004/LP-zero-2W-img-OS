linux-LP 데스크탑 이미지
========================

4GB 짜리 디스크 이미지 하나를 xz 로 압축하고 24MB 씩 잘라 둔 것입니다.
깃허브가 파일 하나에 100MB 를 넘기지 못하게 하기 때문이고, 그것 말고
다른 이유는 없습니다. 순서대로 이어 붙이면 원래 파일이 됩니다.


1. 조각 받기
------------

폴더를 하나 만들고 그 안에서:

    git clone --depth 1 -b image https://github.com/viviantest1004/LP-zero-2W-img-OS.git lpzero
    cd lpzero

git 이 없으면 curl 로 하나씩:

    for i in $(seq -w 0 23); do
        curl -LO https://raw.githubusercontent.com/viviantest1004/LP-zero-2W-img-OS/image/lpzero.img.xz.$i
    done

윈도우 cmd 라면:

    for /L %i in (0,1,9) do curl -LO https://raw.githubusercontent.com/viviantest1004/LP-zero-2W-img-OS/image/lpzero.img.xz.0%i
    for /L %i in (10,1,23) do curl -LO https://raw.githubusercontent.com/viviantest1004/LP-zero-2W-img-OS/image/lpzero.img.xz.%i


2. 다 받았는지 확인
-------------------

    sha256sum -c SHA256SUMS

조각이 하나라도 덜 받아졌으면 이어 붙인 뒤에야 알게 되고, 그때는 다시
받는 것 말고 할 수 있는 일이 없습니다. 그래서 여기서 먼저 봅니다.


3. 이어 붙이고 풀기
-------------------

리눅스 / macOS:

    cat lpzero.img.xz.* > linux-LP_desktop.img.xz
    sha256sum linux-LP_desktop.img.xz
    # a86e26c34b80eaabd49b5724893a971d575fb1f5ed44fc107855fd6df0ec5984
    xz -d linux-LP_desktop.img.xz

윈도우 cmd:

    copy /b lpzero.img.xz.00+lpzero.img.xz.01+lpzero.img.xz.02+lpzero.img.xz.03+lpzero.img.xz.04+lpzero.img.xz.05+lpzero.img.xz.06+lpzero.img.xz.07+lpzero.img.xz.08+lpzero.img.xz.09+lpzero.img.xz.10+lpzero.img.xz.11+lpzero.img.xz.12+lpzero.img.xz.13+lpzero.img.xz.14+lpzero.img.xz.15+lpzero.img.xz.16+lpzero.img.xz.17+lpzero.img.xz.18+lpzero.img.xz.19+lpzero.img.xz.20+lpzero.img.xz.21+lpzero.img.xz.22+lpzero.img.xz.23 linux-LP_desktop.img.xz
    certutil -hashfile linux-LP_desktop.img.xz SHA256

    7z x linux-LP_desktop.img.xz          (7-Zip 이 있으면)

풀면 linux-LP_desktop.img 가 나옵니다. 4GB 입니다.


4. 돌리기
---------

QEMU (UEFI 로 뜹니다):

    cp /usr/share/OVMF/OVMF_VARS_4M.fd vars.fd
    qemu-system-x86_64 -machine q35 -cpu qemu64 -m 3072 -smp 2 \
      -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
      -drive if=pflash,format=raw,file=vars.fd \
      -drive file=linux-LP_desktop.img,format=raw,if=virtio \
      -device virtio-vga -device virtio-keyboard-pci -device virtio-tablet-pci \
      -netdev user,id=n0 -device virtio-net-pci,netdev=n0

USB 나 SSD 에 굽기 (장치 이름을 반드시 확인하고 하십시오 - 그 디스크의
내용은 전부 사라집니다):

    sudo dd if=linux-LP_desktop.img of=/dev/sdX bs=4M conv=fsync status=progress

윈도우에서는 Rufus 나 balenaEtcher 로 같은 파일을 쓰면 됩니다.

VirtualBox / VMware 라면 먼저 바꿔야 합니다:

    qemu-img convert -O vdi  linux-LP_desktop.img linux-LP.vdi
    qemu-img convert -O vmdk linux-LP_desktop.img linux-LP.vmdk

첫 부팅에서 파티션이 디스크 끝까지 늘어납니다 (expandfs). 4GB 이미지를
128GB SSD 에 구우면 128GB 를 다 씁니다.


5. 이 안에 든 것
----------------

커널, libc, 셸, 명령 125개, init, 그리고 데스크탑을 직접 만든 것입니다.
컴포지터(sway)와 GTK 는 데비안 bookworm 베이스에서 가져옵니다 - 십만 줄
짜리 Wayland 컴포지터를 다시 쓰는 것은 이 프로젝트가 하려는 일이 아닙니다.

  상단바      작업공간 / 검색 / 작업표시줄 · 모드 / 자판 / 시계 ·
              그리고 오른쪽 끝의 알약(신호 / 밝기 / 소리 / 배터리 / ⌄).
              그 알약 어디를 눌러도 퀵메뉴가 열립니다. 없는 장치는
              칸이 아예 나오지 않습니다 - 배터리 없는 기계에는
              배터리가 없습니다.
  퀵메뉴      소리·밝기 슬라이더, Wi-Fi, 블루투스, 방해 금지, 절전,
              비행기 모드, 설정, 전원(재시작·끄기)
  시스템 앱   파일, 설정, 작업 관리자, 스크린샷
  그 외       터미널, 편집기, 그림판(GIMP), 사진 편집, 영상 편집, 브라우저

기본 언어는 영어이고 한국어가 같이 들어 있습니다. 설정 > Region &
Language 에서 고르면 다음 로그인부터 바뀝니다.

로그인은 자동입니다 (user, uid 1000). 관리자는 root 이고 su 로 갑니다.


6. 알려진 것
------------

소리가 QEMU 의 에뮬레이트된 HDA 에서는 나지 않습니다. wireplumber 가
pipewire 에 붙었다가 떨어집니다. 카드는 보이고 노드 권한도 맞는데 원인을
아직 찾지 못했습니다. 실제 기계에서는 확인하지 못했습니다.


7. 기기에서 프로그램 설치하기
-----------------------------

    sudo apt update
    sudo apt install <이름>

데비안 bookworm 의 저장소를 그대로 씁니다. 관리자 스크립트가 이 기계의
/bin/sh 로 도는데, 그 셸은 직접 만든 것이라 오래 여기서 막혔습니다 -
줄 이음(\), local, here-document, ${VAR:-기본값} 이 없었습니다. 지금은
있고, dpkg 가 오류 없이 패키지를 설치하는 것을 확인했습니다.

C 컴파일러와 GTK 개발 파일이 이미지에 들어 있습니다. 기기에서 바로
빌드할 수 있습니다:

    cc -o hello hello.c
    cc $(pkg-config --cflags --libs gtk4) -o app app.c
