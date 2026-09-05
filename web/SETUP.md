# lp.cholab.kr 세우기

이미지 세 개를 내려받을 수 있는 페이지를 `lp.cholab.kr` 에 올리는
방법입니다. **cholab.kr 에 이미 돌고 있는 것은 한 줄도 건드리지
않습니다** — 서브도메인이라 nginx 서버 블록을 하나 더할 뿐이고,
문제가 생기면 그 파일만 지우면 원래대로 돌아갑니다.

한국어가 먼저, 영어가 뒤에 있습니다.

---

## 0. 누가 무엇을 하나

이 저장소에는 세 가지가 들어 있습니다.

| 파일 | 하는 일 | 어디서 도나 |
|---|---|---|
| `web/template.html` | 페이지의 원본. 문구를 고칠 때 여기를 고칩니다 | — |
| `tools/mkweb.sh` | `dist/` 의 실제 파일에서 페이지를 만듭니다 | 내 컴퓨터 |
| `tools/deploy-web.sh` | 만든 페이지와 이미지를 서버로 밀어넣습니다 | 내 컴퓨터 |
| `web/nginx/lp.cholab.kr.conf` | nginx 설정 | 서버에 한 번 복사 |

**서버 접속 정보를 누구에게도 줄 필요가 없습니다.** 배포 스크립트는
본인 컴퓨터에서 돌면서 본인의 SSH 키로 접속합니다. `ssh` 로 서버에
들어갈 수 있으면 그걸로 끝입니다.

---

## 1. 서버에 들어가기

이미 `cholab.kr` 이 그 서버에서 돌고 있으니, 접속 방법은 이미 어딘가에
있습니다. 기억이 안 난다면:

**AWS 콘솔에서 찾기**

1. AWS 콘솔 → EC2 → 왼쪽 메뉴 **인스턴스(Instances)**
2. 돌고 있는 인스턴스를 클릭 → **퍼블릭 IPv4 주소** 를 적어둡니다
3. 같은 화면의 **키 페어 이름** 이 접속에 필요한 `.pem` 파일 이름입니다

**접속**

```bash
chmod 400 ~/Downloads/그키이름.pem        # 처음 한 번만
ssh -i ~/Downloads/그키이름.pem ubuntu@퍼블릭IP
```

사용자 이름은 AMI 에 따라 다릅니다. `ubuntu` 가 안 되면 `ec2-user`,
`admin`, `centos` 를 차례로 시도하세요. 우분투면 `ubuntu` 입니다.

---

## 1b. 키를 잃어버렸을 때

**인스턴스를 지우거나 새로 만들지 마세요.** 그 서버에서 cholab.kr 이
돌고 있습니다. 키 없이 들어가는 길이 두 개 있고, 둘 다 AWS 콘솔에서
브라우저로 됩니다.

### 먼저: 절대 하면 안 되는 것

**탄력적 IP(Elastic IP)가 붙어 있지 않다면 인스턴스를 중지하지
마세요.** EC2 는 중지했다 시작하면 퍼블릭 IP 가 바뀝니다. cholab.kr 의
DNS 가 그 IP 를 가리키고 있으므로, 중지하는 순간 사이트가 죽고 DNS 를
다시 고칠 때까지 돌아오지 않습니다.

붙어 있는지 확인: EC2 → 인스턴스 → 해당 인스턴스 →
**탄력적 IP 주소** 칸에 값이 있으면 안전합니다. 비어 있으면 중지가
필요한 방법은 쓰지 마세요.

### 방법 1: Session Manager (권장)

SSH 포트가 필요 없습니다. 밖에서 22번이 막혀 있어도 됩니다.

1. EC2 → 인스턴스 → 해당 인스턴스 선택 → 오른쪽 위 **연결(Connect)**
2. **Session Manager** 탭
3. **연결** 버튼이 활성화돼 있으면 누르면 끝입니다 — 브라우저에서
   바로 셸이 열립니다

버튼이 회색이면 인스턴스에 IAM 역할이 없는 것입니다. **중지하지 않고**
붙일 수 있습니다:

1. IAM → 역할 → **역할 만들기** → 신뢰할 수 있는 엔터티: **AWS 서비스**
   → 사용 사례: **EC2**
2. 권한에서 **AmazonSSMManagedInstanceCore** 를 검색해 체크
3. 이름을 아무거나 (예: `ec2-ssm`) 주고 만듭니다
4. EC2 → 인스턴스 우클릭 → **보안** → **IAM 역할 수정** → 방금 만든
   역할 선택 → 저장
5. 2~5분 기다린 뒤 다시 **연결 → Session Manager**

들어가면 사용자가 `ssm-user` 입니다. `sudo su - ubuntu` 로 바꾸세요.

### 방법 2: EC2 Instance Connect

우분투 20.04 이상이면 대개 됩니다.

1. EC2 → 인스턴스 → **연결** → **EC2 Instance Connect** 탭
2. 사용자 이름 `ubuntu` → **연결**

"연결할 수 없습니다" 가 나오면 보안 그룹이 22번을 막고 있는
것입니다. EC2 → 보안 그룹 → 인바운드 규칙 편집에서 임시로
**SSH / TCP 22 / 내 IP** 를 추가하고 다시 시도하세요. (`0.0.0.0/0`
말고 **내 IP** 로 두세요. 열어둘 이유가 없는 문은 닫아둡니다.)

### 들어간 다음: 새 키 만들기

브라우저 셸에서 계속 작업할 수도 있지만, 배포 스크립트는 SSH 가
필요합니다. 그러니 새 키를 만들어 넣으세요.

**내 컴퓨터에서** 새 키를 만듭니다:

```bash
ssh-keygen -t ed25519 -f ~/.ssh/cholab -C "cholab deploy"
cat ~/.ssh/cholab.pub          # 이 한 줄을 복사
```

**브라우저 셸에서** 그 줄을 붙여넣습니다:

```bash
sudo mkdir -p /home/ubuntu/.ssh
echo '아까_복사한_ssh-ed25519_한_줄' | sudo tee -a /home/ubuntu/.ssh/authorized_keys
sudo chown -R ubuntu:ubuntu /home/ubuntu/.ssh
sudo chmod 700 /home/ubuntu/.ssh
sudo chmod 600 /home/ubuntu/.ssh/authorized_keys
```

**내 컴퓨터에서** 확인합니다:

```bash
ssh -i ~/.ssh/cholab ubuntu@퍼블릭IP
```

들어가지면 끝났습니다. `~/.ssh/config` 에 이름을 붙여두세요:

```
Host cholab
    HostName 퍼블릭IP
    User ubuntu
    IdentityFile ~/.ssh/cholab
```

> 새 키는 **이번 컴퓨터에도 백업**해두세요. 같은 일이 또 생깁니다.
> `~/.ssh/cholab` 파일 하나입니다.

### 그래도 안 될 때

위 둘이 모두 안 되면 남은 방법은 루트 볼륨을 떼어 다른 인스턴스에
붙여 `authorized_keys` 를 고치는 것입니다. 인스턴스 중지가 필요하므로
**탄력적 IP 가 없다면 먼저 붙이고** 하세요 — 그래야 IP 가 안 바뀝니다.
AWS 문서에서 "Connect to your Linux instance if you lose your private
key" 를 찾으면 순서가 나옵니다.

---

접속이 편해지도록 `~/.ssh/config` 에 이름을 붙여두면 좋습니다.

```
Host cholab
    HostName 퍼블릭IP
    User ubuntu
    IdentityFile ~/Downloads/그키이름.pem
```

이러면 `ssh cholab` 만으로 들어갑니다.

---

## 2. DNS: lp.cholab.kr 이 서버를 가리키게

도메인을 산 곳(가비아, Route 53, Cloudflare 등)의 DNS 설정에서 레코드를
하나 추가합니다.

| 종류 | 이름 | 값 |
|---|---|---|
| A | `lp` | 서버의 퍼블릭 IP |

`cholab.kr` 이 이미 그 IP 를 가리키고 있을 테니 같은 값을 쓰면 됩니다.

**퍼지기를 기다린 뒤 확인하세요.** 보통 몇 분, 길면 한 시간입니다.

```bash
dig +short lp.cholab.kr
```

IP 가 나오면 다음으로 갑니다. 이게 안 되면 3단계의 인증서 발급이
반드시 실패합니다 — Let's Encrypt 가 그 이름으로 서버에 접속해서
확인하기 때문입니다.

---

## 3. 서버 준비 (한 번만)

서버에 접속해서:

```bash
# 파일이 놓일 자리
sudo mkdir -p /srv/lp-zero/files /srv/lp-zero/site
sudo chown -R $USER:$USER /srv/lp-zero

# certbot 이 인증서를 받을 때 쓰는 자리
sudo mkdir -p /var/www/certbot
```

내 컴퓨터에서 nginx 설정을 올립니다:

```bash
scp web/nginx/lp.cholab.kr.conf cholab:/tmp/
```

다시 서버에서:

```bash
sudo cp /tmp/lp.cholab.kr.conf /etc/nginx/sites-available/
sudo ln -s /etc/nginx/sites-available/lp.cholab.kr.conf \
           /etc/nginx/sites-enabled/
```

**아직 `nginx -t` 를 하면 실패합니다.** 인증서가 없기 때문입니다.
certbot 이 그 두 줄을 채워줍니다:

```bash
sudo apt install certbot python3-certbot-nginx     # 없으면
sudo certbot --nginx -d lp.cholab.kr
```

certbot 이 인증서를 받고, 설정 파일에 `ssl_certificate` 두 줄을 직접
넣고, nginx 를 다시 읽습니다. 갱신은 알아서 됩니다.

확인:

```bash
sudo nginx -t && sudo systemctl reload nginx
curl -I https://lp.cholab.kr        # 404 가 나오면 정상입니다 - 아직 아무것도 안 올렸으니까요
```

### 방화벽

AWS 보안 그룹에서 **80 과 443 이 0.0.0.0/0 에 열려 있어야** 합니다.
`cholab.kr` 이 이미 돌고 있으면 열려 있을 것입니다. 아니라면
EC2 → 인스턴스 → 보안 → 보안 그룹 → 인바운드 규칙에서 추가하세요.

---

## 4. 배포

내 컴퓨터에서, 저장소 폴더 안에서:

```bash
LP_WEB_HOST=cholab ./tools/deploy-web.sh --dry-run   # 무엇이 올라갈지만 본다
LP_WEB_HOST=cholab ./tools/deploy-web.sh             # 실제로 올린다
```

`~/.ssh/config` 에 `cholab` 을 만들어두지 않았다면
`LP_WEB_HOST=ubuntu@퍼블릭IP` 로 쓰면 됩니다.

이 스크립트는:

1. `dist/` 의 실제 파일에서 페이지를 다시 만들고 (크기와 sha256 이
   손으로 적은 값이 아니라 진짜 파일에서 나옵니다)
2. 이미지를 먼저, 페이지를 나중에 올리고
   (반대로 하면 그 사이에 들어온 사람이 없는 파일을 받으려 합니다)
3. 밖에서 실제로 받아지는지 확인합니다

새 이미지를 빌드했을 때는 `./tools/mkdist.sh` 를 돌린 뒤 이 스크립트만
다시 실행하면 됩니다. rsync 라 바뀐 파일만 올라갑니다.

---

## 5. 잘 안 될 때

| 증상 | 원인 |
|---|---|
| certbot 이 실패 | DNS 가 아직 안 퍼졌습니다. `dig +short lp.cholab.kr` 부터 |
| 502 / 404 | `/srv/lp-zero/site/index.html` 이 있는지, nginx 의 `root` 와 맞는지 |
| 브라우저에 이진 파일이 쏟아짐 | `location /files/` 의 `default_type` 이 적용됐는지 |
| 이어받기가 안 됨 | 배포 스크립트 끝의 확인이 `206` 이 아니라 `200` 이면 앞단에 뭔가 Range 를 지우고 있습니다 |
| rsync 가 권한 거부 | `/srv/lp-zero` 의 소유자가 접속 사용자인지 |

로그는 서버의 `/var/log/nginx/lp.cholab.kr.{access,error}.log` 입니다.

---

## 6. 돈 이야기

이미지 세 개가 85MB 입니다. AWS 는 밖으로 나가는 트래픽에 과금하고,
프리 티어를 넘기면 리전에 따라 GB 당 대략 $0.09~0.12 입니다.

- 100명이 하나씩 받으면 약 3GB, 몇 백 원 수준
- 어딘가에 링크가 퍼져서 10,000명이 받으면 약 300GB, 30달러쯤

많이 퍼질 것 같으면 파일만 S3 나 Cloudflare R2 로 옮기고 페이지는
여기 두는 편이 낫습니다. R2 는 나가는 트래픽이 무료입니다. 그때는
`web/template.html` 의 `files/...` 링크만 바꾸면 됩니다.

---

## English

The page lets people download the three images from your own server at
`lp.cholab.kr`. Nothing about the existing `cholab.kr` site changes: a
subdomain means one extra nginx server block, and removing that one file
puts everything back.

**You do not give anybody access to your server.** The deploy script
runs on your own machine and connects with your own SSH key. If you can
`ssh` in, that is all it needs.

The steps are the same as above:

1. **Find the server.** AWS console → EC2 → Instances → the running one.
   Note its public IPv4 address and its key pair name.
   `ssh -i ~/Downloads/<key>.pem ubuntu@<ip>` — try `ec2-user` if
   `ubuntu` is refused. Add a `Host cholab` entry to `~/.ssh/config` so
   later commands are shorter.

   **Lost the key?** Do not delete or recreate the instance - cholab.kr
   is running on it - and do not stop it unless an Elastic IP is
   attached, because stopping changes the public IP that DNS points at.
   Use Session Manager (EC2 → Connect → Session Manager; if the button
   is greyed out, attach an IAM role with `AmazonSSMManagedInstanceCore`,
   which can be done without stopping the instance) or EC2 Instance
   Connect. Once in a browser shell, generate a new key on your own
   machine and append its `.pub` line to
   `/home/ubuntu/.ssh/authorized_keys`. Section 1b above has the exact
   commands.

2. **DNS.** Add an `A` record for `lp` pointing at the same IP that
   `cholab.kr` uses. Wait for `dig +short lp.cholab.kr` to answer before
   going on — Let's Encrypt connects to that name to issue the
   certificate, so it cannot work before DNS does.

3. **Server, once.** Create `/srv/lp-zero/{files,site}` and
   `/var/www/certbot`, copy `web/nginx/lp.cholab.kr.conf` into
   `/etc/nginx/sites-available/`, link it into `sites-enabled/`, then
   `sudo certbot --nginx -d lp.cholab.kr`. certbot fills in the two
   `ssl_certificate` lines and reloads nginx. AWS security group needs
   80 and 443 open, which they will already be.

4. **Deploy.** `LP_WEB_HOST=cholab ./tools/deploy-web.sh`. It rebuilds
   the page from the real files in `dist/` so the sizes and checksums
   cannot drift, uploads the images before the page that links to them,
   and then checks from outside that the files really download - a
   deploy is finished when somebody can fetch it, not when the files
   are sitting on disk.

**On cost:** 85MB of images, and AWS bills egress. A hundred downloads
is a few cents; ten thousand is around $30. If it spreads further than
expected, move the files to Cloudflare R2 (free egress) and change the
`files/...` links in `web/template.html` - the page can stay where it is.
