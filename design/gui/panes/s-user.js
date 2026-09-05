/* 사용자 - the accounts on this machine, and which of them you are.
 *
 * The reference mockup drew one account and three settings under it,
 * which is the picture of a machine nobody else has ever sat at. The
 * moment there is a second account every row on the screen turns on one
 * question - is this row mine, or is it everybody's - and the answer is
 * not the same for the two halves of the screen:
 *
 *     내 계정   my password, my picture. Mine even when I may change
 *               nothing else on this computer.
 *     관리      who else may use this machine, and as what. Not mine
 *               unless I am the administrator.
 *
 * Getting that split wrong in the safe direction is what does the
 * damage. A permission model that locks a standard account out of its
 * own password is one that gets worked around by handing out the
 * administrator password, and then nothing on the machine is protected
 * by anything. So 내 계정 stays live for both accounts and 관리 locks
 * whole, and the line under a locked row names who can, because a
 * refusal with nobody's name in it leaves the reader with nothing to do.
 *
 * ── Two refusals this screen exists to make ──
 *
 * 마지막 관리자를 표준으로 내리는 것. A machine with no administrator is
 * a machine nobody can install anything on, take back, or promote
 * anybody with - and the account that would have fixed it is the one
 * that was just demoted. Nothing else on this screen can put it back.
 *
 * 로그인해 있는 계정을 지우는 것. The session running the settings app
 * belongs to that account, and the windows on the screen are its
 * windows.
 *
 * Both refuse in a dialog with the reason in it rather than by greying
 * a control out, for the reason 키보드 refuses in a dialog: somebody who
 * does not know why it will not go keeps pressing until they are told.
 */

(function () {
  'use strict';

  const PANE = 's-user';
  const AREA = 'users';

  const D_INFO  = 'dlg-user-info';
  const D_PW    = 'dlg-user-pw';
  const D_PHOTO = 'dlg-user-photo';
  const D_ADD   = 'dlg-user-add';
  const D_TYPE  = 'dlg-user-type';
  const D_DEL   = 'dlg-user-del';

  /* The accounts, on LP with everything else rather than in this file.
   * 권한 draws the same standard account this screen adds and deletes,
   * and two lists of accounts is two answers to "who uses this machine"
   * that drift apart the first time one of them is edited. */
  LP.accounts = LP.accounts || [
    { name: 'cho',   admin: true,  photo: '이름 첫 글자', pwDays: 92 },
    { name: 'guest', admin: false, photo: '이름 첫 글자', pwDays: 214 },
  ];

  /* Which account the machine starts without asking, or null. One value
   * rather than a flag per account, because the login screen can only
   * do this for one of them and two flags is a state it cannot honour. */
  if (!('autoLogin' in LP)) LP.autoLogin = null;

  const PHOTOS = ['이름 첫 글자', '무늬', '사진 없음'];

  /* A Unix account name, and the machine's rule for one rather than this
   * screen's: it becomes a directory under /home and a column in
   * /etc/passwd, so what is refused here is what useradd would refuse. */
  const NAME_OK = /^[a-z][a-z0-9_-]{0,31}$/;
  const PW_MIN  = 8;

  /* Both dialogs that ask more than one question keep their answer here
   * while they are open. Reset when the dialog opens, never read after
   * it closes - a dialog that reopens holding the last visit's answer is
   * the one that gets a wrong account deleted. */
  let addAdmin = false;      /* 사용자 추가 defaults to 표준: the account
                              * that can do less is the safe thing to
                              * create by not reading the question. */
  let delName  = null;
  let delErase = false;

  /* ── who is who ─────────────────────────────────────────────────
   *
   * LP.userName is the owner's table of the name signed in, so this is
   * identity read from the one place that has it. It is not a permission
   * test - that question is LP.can(AREA) and it is asked nowhere else. */
  function meName() { return LP.userName[LP.account]; }
  function find(name) { return LP.accounts.filter(a => a.name === name)[0]; }
  function isMe(a) { return a.name === meName(); }
  function admins() { return LP.accounts.filter(a => a.admin); }
  function home(a) { return '/home/' + a.name; }

  /* The account signed in, guaranteed to exist.
   *
   * The window's own header names it, and a list that does not have the
   * person reading it in it is worse than a list that is out of date.
   * Deleting the other account and then switching to it is the way to
   * get there, and healing on the next render costs one object. */
  function seat() {
    let a = find(meName());
    if (!a) {
      a = {
        name: meName(),
        admin: LP.userName.admin === meName(),
        photo: PHOTOS[0],
        pwDays: 0,
      };
      LP.accounts.unshift(a);
    }
    return a;
  }

  /* 다음 로그인부터: the group change is written now and a session picks
   * it up when it logs in. Saying so is the only way a 관리 section that
   * is still locked makes sense to somebody who was made an
   * administrator a minute ago. */
  function typeLine(a) {
    return (a.admin ? '관리자' : '표준') +
           (a.pending ? ' · 다음 로그인부터 적용됩니다' : '');
  }

  function said()     { return document.getElementById('user-said'); }
  function mineSaid() { return document.getElementById('user-mine-said'); }
  function modal(id)  { return document.getElementById(id).querySelector('.modal'); }

  /* ── the pane ───────────────────────────────────────────────────
   *
   * Rebuilt from LP on every call. It is called again on every account
   * switch, and half of what is drawn here - the pill, the lock, which
   * rows answer - is a statement about who is reading it. */
  LP.panes[PANE] = function (el) {
    const may  = LP.can(AREA);
    const mine = seat();

    el.querySelector('#user-sub').textContent =
      '계정 ' + LP.accounts.length + '개 · 지금은 ' + mine.name + ' 로 로그인해 있습니다';

    fillList(el.querySelector('#user-list'));
    fillMine(el.querySelector('#user-mine'), mine, may);
    fillAdmin(el.querySelector('#user-admin'), may);

    /* On while the setting is on, gone when it is off. A caution that is
     * there whether or not the thing is happening is one people read
     * past on the day it matters. */
    el.querySelector('#user-mine-note').innerHTML =
      LP.autoLogin === mine.name
        ? '<div class="note">▲ 자동 로그인이 켜져 있습니다. 이 컴퓨터를 켜는 사람은 ' +
          '비밀번호 없이 ' + LP.esc(mine.name) + ' 로 들어옵니다.</div>'
        : '';

    /* The administrators by name, under the section that needs them.
     * LP.whyLocked says 관리자만, which is the rule; this says which
     * person, which is what somebody locked out can act on. */
    const who = admins().map(a => a.name);
    el.querySelector('#user-admin-why').textContent = may
      ? (who.length === 1
          ? '이 컴퓨터의 관리자는 ' + who[0] + ' 하나뿐입니다. 관리자가 하나뿐이면 그 계정은 표준으로 바꿀 수 없습니다.'
          : '이 컴퓨터의 관리자는 ' + who.join(', ') + ' 입니다.')
      : (who.length
          ? '계정을 추가하거나 지우려면 관리자 ' + who.join(', ') + ' 에게 요청하십시오.'
          : '');
  };

  /* ── the account list ───────────────────────────────────────────
   *
   * Shown to both accounts. A standard account may not change anything
   * here, and knowing who else uses the machine is not a change - a list
   * that hides the other accounts only means the person asks around
   * instead. */
  function fillList(box) {
    box.innerHTML = LP.accounts.map(function (a) {
      const pill = isMe(a) ? '<span class="pill now">현재 로그인</span>' : '';
      return '<div class="row click pressable" data-acct="' + LP.esc(a.name) + '">' +
        '<div class="lb"><b>' + LP.esc(a.name) + pill + '</b>' +
          '<i>' + typeLine(a) + '</i></div>' +
        '<div class="vl">›</div></div>';
    }).join('');

    box.querySelectorAll('[data-acct]').forEach(function (row) {
      row.addEventListener('click', function () { openInfo(row.dataset.acct); });
    });
  }

  /* ── 내 계정 ────────────────────────────────────────────────────
   *
   * Two of these three rows are live whatever the account may do, and
   * the third is not. The line between them is who else it affects: a
   * password and a picture end at the person, and 자동 로그인 decides
   * whether anybody who opens the lid is already inside the machine. */
  function fillMine(box, mine, may) {
    const lock = '<span class="lock">자물쇠</span>';
    const why  = '<div class="why">' + LP.whyLocked(AREA) + '</div>';

    box.innerHTML =
      '<div class="row click pressable" data-act="pw">' +
        '<div class="lb"><b>비밀번호 변경</b></div><div class="vl">›</div></div>' +

      '<div class="row click pressable" data-act="photo">' +
        '<div class="lb"><b>프로필 사진</b></div>' +
        '<div class="vl">' + LP.esc(mine.photo) + '</div></div>' +

      '<div class="row' + (may ? ' click pressable' : ' locked') + '" data-act="auto">' +
        '<div class="lb"><b>자동 로그인' + (may ? '' : lock) + '</b>' +
          '<i>켜면 이 컴퓨터를 켤 때 비밀번호를 묻지 않고 이 계정으로 들어갑니다</i>' +
          (may ? '' : why) +
        '</div>' +
        '<div class="sw' + (LP.autoLogin === mine.name ? ' on' : '') + '"></div></div>' +

      /* Not locked and not dimmed: nobody took this away and no
       * administrator can give it back. The sentence is about the
       * hardware, so it reads the same to both accounts. */
      '<div class="row"><div class="lb"><b>지문 인증</b>' +
        '<i>이 컴퓨터에는 지문 센서가 없습니다</i></div>' +
        '<div class="vl off">사용 불가</div></div>';

    box.querySelector('[data-act="pw"]').addEventListener('click', openPw);
    box.querySelector('[data-act="photo"]').addEventListener('click', openPhoto);

    /* A locked row gets no handler at all rather than one that refuses.
     * The lock and the line under the name are the refusal; a second one
     * that only arrives after a click would be telling somebody off for
     * clicking. */
    if (may)
      box.querySelector('[data-act="auto"]')
         .addEventListener('click', function () { toggleAuto(mine); });
  }

  /* ── 관리 ───────────────────────────────────────────────────────
   *
   * 권한 is in this list rather than beside it because what a standard
   * account may change is the same decision as who the accounts are. It
   * locks with the rest: it is the screen where an administrator decides
   * what everybody else may do, and reading it is not what a standard
   * account is missing - every screen it names already carries its own
   * lock. */
  function fillAdmin(box, may) {
    const rows = [
      { act: 'add',  name: '사용자 추가' },
      { act: 'type', name: '계정 유형 바꾸기' },
      { act: 'perm', name: '권한',
        desc: '표준 계정이 스스로 바꿀 수 있는 설정을 정합니다' },
      { act: 'del',  name: '사용자 삭제',
        desc: '계정을 지웁니다. 홈 폴더는 남겨 둘 수 있습니다' },
    ];

    box.innerHTML = rows.map(function (r) {
      return '<div class="row' + (may ? ' click pressable' : ' locked') +
               '" data-act="' + r.act + '">' +
        '<div class="lb"><b>' + r.name +
          (may ? '' : '<span class="lock">자물쇠</span>') + '</b>' +
          (r.desc ? '<i>' + r.desc + '</i>' : '') +
          (may ? '' : '<div class="why">' + LP.whyLocked(AREA) + '</div>') +
        '</div><div class="vl">›</div></div>';
    }).join('');

    if (!may) return;
    /* Wrapped rather than handed over directly: the listener would call
     * each of these with the click event as its first argument, and both
     * 계정 유형 바꾸기 and 사용자 삭제 take one - a message to show and an
     * account to delete. A MouseEvent in either place draws a dialog
     * about nothing. */
    const go = {
      add:  function () { openAdd(); },
      type: function () { openType(); },
      del:  function () { openDel(); },
      perm: function () { LP.go('s-perm'); },
    };
    box.querySelectorAll('[data-act]').forEach(function (row) {
      row.addEventListener('click', go[row.dataset.act]);
    });
  }

  /* ── 자동 로그인 ────────────────────────────────────────────────── */
  function toggleAuto(mine) {
    /* The lock on the row is the drawing of the rule. This is the rule,
     * asked again so that a click arriving another way - a row drawn
     * before the account switched - is refused for the real reason. */
    if (!LP.can(AREA)) return;

    const was = LP.autoLogin;
    LP.autoLogin = (was === mine.name) ? null : mine.name;
    LP.render();

    /* Said out loud only when turning it on took it from somebody else.
     * The switch moving is the feedback for everything else, and the
     * account that lost it is not on the screen to be seen losing it. */
    if (LP.autoLogin && was && was !== mine.name)
      LP.say(mineSaid(), was + ' 의 자동 로그인이 함께 꺼졌습니다. 자동 로그인은 한 계정만 켤 수 있습니다.', 'warn');
  }

  /* ── 계정 정보 ──────────────────────────────────────────────────
   *
   * What the chevron opens. It is the one place the account's facts sit
   * together, and the actions in its foot are the same three flows the
   * 관리 rows open - pointed at the account already on the screen, so
   * that reaching a dialog from here and from the list cannot end up
   * meaning two different things. */
  function openInfo(name) {
    const a = find(name);
    if (!a) return;
    const may = LP.can(AREA);

    const facts = [
      ['유형',        typeLine(a)],
      ['로그인',      isMe(a) ? '현재 로그인' : '로그인해 있지 않습니다'],
      ['홈 폴더',     home(a)],
      ['자동 로그인', LP.autoLogin === a.name ? '켬' : '끔'],
      ['비밀번호',    a.pwDays === 0 ? '오늘 바꿨습니다' : a.pwDays + '일 전에 바꿨습니다'],
    ];

    modal(D_INFO).innerHTML =
      '<h3>' + LP.esc(a.name) + '</h3>' +
      '<div class="list">' +
        facts.map(function (f) {
          return '<div class="row"><div class="lb"><b>' + f[0] + '</b></div>' +
                 '<div class="vl">' + LP.esc(f[1]) + '</div></div>';
        }).join('') +
      '</div>' +
      '<div class="foot">' +
        '<button class="btn quiet pressable" data-close="' + D_INFO + '">닫기</button>' +
        (isMe(a) ? '<button class="btn pressable" data-go="pw">비밀번호 변경</button>' : '') +
        (may ? '<button class="btn pressable" data-go="type">유형 바꾸기</button>' +
               '<button class="btn danger pressable" data-go="del">삭제</button>' : '') +
      '</div>';

    /* 삭제 is offered for the account signed in as well, and refuses in
     * the dialog it opens. A button that is simply not there teaches
     * nothing about why the machine will not do it. */
    const jump = {
      pw:   function () { LP.close(D_INFO); openPw(); },
      type: function () { LP.close(D_INFO); openType(); },
      del:  function () { LP.close(D_INFO); openDel(a.name); },
    };
    modal(D_INFO).querySelectorAll('[data-go]').forEach(function (b) {
      b.addEventListener('click', jump[b.dataset.go]);
    });

    LP.open(D_INFO);
  }

  /* ── 비밀번호 변경 ──────────────────────────────────────────────
   *
   * About the account signed in and no other, whatever LP.can says. This
   * is the row the whole permission model is judged by: lock it and the
   * administrator password starts getting shared. */
  function openPw() {
    const mine = seat();

    modal(D_PW).innerHTML =
      '<h3>비밀번호 변경</h3>' +
      '<p>' + LP.esc(mine.name) + ' 로 로그인할 때 쓰는 비밀번호를 바꿉니다. ' +
        '바꾸고 나면 다음 로그인부터 새 비밀번호를 씁니다.</p>' +
      '<div class="fieldlabel">지금 비밀번호</div>' +
      '<input class="field" type="password" autocomplete="off" data-old>' +
      '<div class="fieldlabel">새 비밀번호</div>' +
      '<input class="field" type="password" autocomplete="off" data-new>' +
      '<div class="fieldlabel">새 비밀번호 다시 입력</div>' +
      '<input class="field" type="password" autocomplete="off" data-again>' +
      '<div class="inline"></div>' +
      '<div class="foot">' +
        '<button class="btn quiet pressable" data-close="' + D_PW + '">취소</button>' +
        '<button class="btn primary pressable" data-go disabled>바꾸기</button>' +
      '</div>';

    const box   = modal(D_PW);
    const old   = box.querySelector('[data-old]');
    const fresh = box.querySelector('[data-new]');
    const again = box.querySelector('[data-again]');
    const go    = box.querySelector('[data-go]');
    const line  = box.querySelector('.inline');

    function ready() {
      go.disabled = !(old.value && fresh.value && again.value);
    }

    [old, fresh, again].forEach(function (f) {
      f.addEventListener('input', function () {
        /* The refusal clears itself after four seconds; the red border
         * has to go the moment the person acts on it, or it outlives
         * what it was about. */
        f.classList.remove('bad');
        ready();
      });
      /* isComposing is not optional even here: an IME commit arrives as
       * Enter, and a password field can be typed into with one running. */
      f.addEventListener('keydown', function (e) {
        if (e.key !== 'Enter' || e.isComposing) return;
        if (!go.disabled) go.click();
      });
    });

    go.addEventListener('click', function () {
      /* Any password is taken, because there is no account behind this
       * screen to ask. One literal is refused so that the wrong-password
       * branch is reachable - the same literal 공장 초기화 uses, so a
       * reviewer only has to learn one. */
      if (old.value === 'wrong') {
        old.value = '';
        old.classList.add('bad');
        ready();
        old.focus();
        LP.say(line, '지금 비밀번호가 맞지 않습니다', 'bad');
        return;
      }
      /* Amber, not red: a length rule is a limit on what may be typed,
       * and nothing has failed yet. */
      if (fresh.value.length < PW_MIN) {
        LP.say(line, '비밀번호는 ' + PW_MIN + '자 이상이어야 합니다', 'warn');
        return;
      }
      if (fresh.value !== again.value) {
        again.value = '';
        again.classList.add('bad');
        ready();
        again.focus();
        LP.say(line, '새 비밀번호 두 개가 서로 다릅니다', 'bad');
        return;
      }
      if (fresh.value === old.value) {
        LP.say(line, '지금 쓰고 있는 비밀번호와 같습니다', 'warn');
        return;
      }

      mine.pwDays = 0;
      LP.close(D_PW);
      LP.render();
      LP.say(mineSaid(), '비밀번호를 바꿨습니다');
    });

    LP.open(D_PW);
    old.focus();
  }

  /* ── 프로필 사진 ────────────────────────────────────────────────── */
  function openPhoto() {
    const mine = seat();

    modal(D_PHOTO).innerHTML =
      '<h3>프로필 사진</h3>' +
      '<p>로그인 화면과 상단바에 이 계정을 나타내는 그림입니다.</p>' +
      '<div class="picker">' +
        PHOTOS.map(function (p) {
          const on = mine.photo === p;
          return '<div class="row click pressable' + (on ? ' on' : '') +
                   '" data-photo="' + LP.esc(p) + '">' +
            '<div class="lb"><b>' + LP.esc(p) + '</b></div>' +
            '<div class="vl' + (on ? '' : ' off') + '">' +
              (on ? '사용 중' : '선택 안 함') + '</div></div>';
        }).join('') +
      '</div>' +
      '<div class="foot">' +
        '<button class="btn quiet pressable" data-close="' + D_PHOTO + '">닫기</button>' +
      '</div>';

    modal(D_PHOTO).querySelectorAll('[data-photo]').forEach(function (row) {
      row.addEventListener('click', function () {
        mine.photo = row.dataset.photo;
        LP.close(D_PHOTO);
        LP.render();
        LP.say(mineSaid(), '프로필 사진을 바꿨습니다');
      });
    });

    LP.open(D_PHOTO);
  }

  /* ── 사용자 추가 ────────────────────────────────────────────────
   *
   * The two account types are explained a line each rather than named
   * and left. 관리자 and 표준 are words somebody picks between once,
   * usually while setting up a machine for a person who is not in the
   * room, and the difference is the whole of what they are choosing. */
  function openAdd() {
    if (!LP.can(AREA)) return;
    addAdmin = false;

    modal(D_ADD).innerHTML =
      '<h3>사용자 추가</h3>' +
      '<p>이 컴퓨터에 새 계정을 만듭니다. 홈 폴더는 계정 이름으로 만들어집니다.</p>' +
      '<div class="fieldlabel">이름</div>' +
      '<input class="field mono" autocomplete="off" placeholder="영문 소문자로 시작" data-name>' +
      '<div class="fieldlabel">비밀번호</div>' +
      '<input class="field" type="password" autocomplete="off" data-pw>' +
      '<div class="sec">계정 유형</div>' +
      '<div class="list" data-types></div>' +
      '<div class="inline"></div>' +
      '<div class="foot">' +
        '<button class="btn quiet pressable" data-close="' + D_ADD + '">취소</button>' +
        '<button class="btn primary pressable" data-go disabled>추가</button>' +
      '</div>';

    const box  = modal(D_ADD);
    const name = box.querySelector('[data-name]');
    const pw   = box.querySelector('[data-pw]');
    const go   = box.querySelector('[data-go]');
    const line = box.querySelector('.inline');

    function types() {
      const list = [
        { admin: true,  name: '관리자',
          desc: '무엇이든 바꾸고, 계정을 추가하거나 지울 수 있습니다' },
        { admin: false, name: '표준',
          desc: '이 컴퓨터를 쓰지만, 모두에게 영향을 주는 설정은 바꾸지 않습니다' },
      ];
      box.querySelector('[data-types]').innerHTML = list.map(function (t) {
        const on = addAdmin === t.admin;
        return '<div class="row click pressable" data-type="' + (t.admin ? 'admin' : 'std') + '">' +
          '<div class="lb"><b>' + t.name + '</b><i>' + t.desc + '</i></div>' +
          '<div class="vl' + (on ? '' : ' off') + '">' +
            (on ? '선택함' : '선택 안 함') + '</div></div>';
      }).join('');

      box.querySelectorAll('[data-type]').forEach(function (row) {
        row.addEventListener('click', function () {
          addAdmin = row.dataset.type === 'admin';
          types();
        });
      });
    }
    types();

    [name, pw].forEach(function (f) {
      f.addEventListener('input', function () {
        f.classList.remove('bad');
        go.disabled = !(name.value && pw.value);
      });
      f.addEventListener('keydown', function (e) {
        if (e.key !== 'Enter' || e.isComposing) return;
        if (!go.disabled) go.click();
      });
    });

    go.addEventListener('click', function () {
      const want = name.value.trim();

      /* Both refusals are amber: they are limits on what a name and a
       * password may be, and nothing has gone wrong yet. */
      if (!NAME_OK.test(want)) {
        name.classList.add('bad');
        LP.say(line, '이름은 영문 소문자로 시작하고, 소문자와 숫자, - 와 _ 만 쓸 수 있습니다', 'warn');
        return;
      }
      if (find(want)) {
        name.classList.add('bad');
        LP.say(line, want + ' 는 이미 있는 계정입니다. 계정 이름은 겹칠 수 없습니다', 'warn');
        return;
      }
      if (pw.value.length < PW_MIN) {
        pw.classList.add('bad');
        LP.say(line, '비밀번호는 ' + PW_MIN + '자 이상이어야 합니다', 'warn');
        return;
      }

      LP.accounts.push({
        name: want, admin: addAdmin, photo: PHOTOS[0], pwDays: 0,
      });
      LP.close(D_ADD);
      LP.render();

      /* The row is put down with the insert spring after the render that
       * made it, so the list is never a frame ahead of what is on the
       * screen. */
      const row = document.querySelector('#user-list .row[data-acct="' + want + '"]');
      if (row) row.classList.add('arrive');
      LP.say(said(), want + ' 계정을 만들었습니다 · ' + (addAdmin ? '관리자' : '표준'));
    });

    LP.open(D_ADD);
    name.focus();
  }

  /* ── 계정 유형 바꾸기 ───────────────────────────────────────────
   *
   * Every account in one list with the change it can take, because the
   * question people arrive with is comparative - somebody should be able
   * to do more, or should not have been able to do so much - and it is
   * answered by seeing the accounts next to each other.
   *
   * The refusals are notes in this dialog rather than a disabled button
   * per row. A row that cannot be pressed says only that it cannot; the
   * whole content of both rules is why. */
  function openType(msg) {
    if (!LP.can(AREA)) return;

    modal(D_TYPE).innerHTML =
      '<h3>계정 유형 바꾸기</h3>' +
      '<p>관리자는 무엇이든 바꾸고 계정을 추가하거나 지울 수 있습니다. ' +
        '표준 계정은 이 컴퓨터를 쓰지만 모두에게 영향을 주는 설정은 바꾸지 않습니다.</p>' +
      '<div class="picker">' +
        LP.accounts.map(function (a) {
          const pill = isMe(a) ? '<span class="pill now">현재 로그인</span>' : '';
          return '<div class="row" data-acct="' + LP.esc(a.name) + '">' +
            '<div class="lb"><b>' + LP.esc(a.name) + pill + '</b>' +
              '<i>' + typeLine(a) + '</i></div>' +
            '<button class="btn quiet pressable" data-to="' + (a.admin ? 'std' : 'admin') + '">' +
              (a.admin ? '표준으로' : '관리자로') + '</button></div>';
        }).join('') +
      '</div>' +
      (msg ? '<div class="note' + (msg.ok ? ' plain' : '') + '">' +
               (msg.ok ? '' : '▲ ') + msg.text + '</div>' : '') +
      '<div class="foot">' +
        '<button class="btn quiet pressable" data-close="' + D_TYPE + '">닫기</button>' +
      '</div>';

    modal(D_TYPE).querySelectorAll('[data-acct]').forEach(function (row) {
      const btn = row.querySelector('[data-to]');
      btn.addEventListener('click', function () {
        setType(find(row.dataset.acct), btn.dataset.to === 'admin');
      });
    });

    LP.open(D_TYPE);
  }

  function setType(a, toAdmin) {
    if (!LP.can(AREA) || !a || a.admin === toAdmin) return;

    /* Asked before the self rule, so that the machine's own reason comes
     * first: on a machine with one administrator this is the refusal,
     * and being told "다른 관리자가 바꿉니다" when there is no other
     * administrator would be an instruction nobody can follow. */
    if (a.admin && admins().length <= 1) {
      openType({ text: '관리자가 한 명도 남지 않습니다. 관리자가 없는 컴퓨터에서는 ' +
                       '아무도 앱을 설치하거나 설정을 되돌릴 수 없고, 계정을 다시 ' +
                       '관리자로 바꿀 수도 없습니다. 다른 계정을 먼저 관리자로 바꾼 ' +
                       '뒤에 이 계정을 표준으로 내립니다.' });
      return;
    }

    if (isMe(a)) {
      const other = admins().filter(x => !isMe(x)).map(x => x.name);
      openType({ text: '지금 로그인해 있는 계정의 유형은 다른 관리자가 바꿉니다. ' +
                       '세션은 로그인할 때 받은 권한을 그대로 들고 있어서, 쓰는 ' +
                       '도중에 자기 권한만 따로 내려놓을 수 없습니다. 관리자 ' +
                       other.join(', ') + ' 가 바꿀 수 있습니다.' });
      return;
    }

    a.admin = toAdmin;
    /* The change is written now and the account's next login is where it
     * starts to mean anything. A session already open keeps what it had. */
    a.pending = true;
    LP.render();
    LP.say(said(), a.name + ' 의 계정 유형을 ' + (toAdmin ? '관리자' : '표준') + ' 로 바꿨습니다');
    openType({ ok: true, text: a.name + ' 의 계정 유형을 ' +
               (toAdmin ? '관리자' : '표준') + ' 로 바꿨습니다. 다음 로그인부터 적용됩니다.' });
  }

  /* ── 사용자 삭제 ────────────────────────────────────────────────
   *
   * Two steps in one dialog: which account, and then what happens to its
   * files. They are not one step because the second question is the one
   * that cannot be taken back, and it should not be answered in the same
   * glance as the first. */
  function canDelete(a) {
    if (isMe(a))
      return {
        ok: false,
        text: '지금 로그인해 있는 계정은 지울 수 없습니다. 열려 있는 창과 ' +
              '세션이 이 계정의 것이라, 쓰는 도중에 그 계정이 사라집니다. ' +
              '다른 관리자로 로그인해서 지웁니다.',
      };
    return { ok: true };
  }

  function openDel(name) {
    if (!LP.can(AREA)) return;

    delName = null;
    delErase = false;

    const a = name ? find(name) : null;
    if (a) {
      const verdict = canDelete(a);
      if (verdict.ok) { delName = a.name; drawDelConfirm(); return; }
      drawDelPick(verdict.text);
      return;
    }
    drawDelPick('');
  }

  /* Step one. The account signed in is in the list and refuses when it
   * is pressed, for the reason 키보드 keeps 삭제 on the fixed input
   * source: a control that quietly does nothing teaches nothing. */
  function drawDelPick(msg) {
    modal(D_DEL).innerHTML =
      '<h3>사용자 삭제</h3>' +
      '<p>지울 계정을 고릅니다. 다음 화면에서 홈 폴더를 남길지 정합니다.</p>' +
      '<div class="picker">' +
        LP.accounts.map(function (a) {
          const pill = isMe(a) ? '<span class="pill now">현재 로그인</span>' : '';
          return '<div class="row click pressable" data-acct="' + LP.esc(a.name) + '">' +
            '<div class="lb"><b>' + LP.esc(a.name) + pill + '</b>' +
              '<i>' + typeLine(a) + ' · ' + home(a) + '</i></div>' +
            '<div class="vl">›</div></div>';
        }).join('') +
      '</div>' +
      (msg ? '<div class="note">▲ ' + msg + '</div>' : '') +
      '<div class="foot">' +
        '<button class="btn quiet pressable" data-close="' + D_DEL + '">닫기</button>' +
      '</div>';

    modal(D_DEL).querySelectorAll('[data-acct]').forEach(function (row) {
      row.addEventListener('click', function () {
        const a = find(row.dataset.acct);
        const verdict = canDelete(a);
        if (!verdict.ok) { drawDelPick(verdict.text); return; }
        delName = a.name;
        delErase = false;
        drawDelConfirm();
      });
    });

    LP.open(D_DEL);
  }

  /* Step two. The two answers are a list rather than a switch because
   * neither of them is the off state of the other: one keeps the files
   * and one destroys them, and a switch would make destroying them the
   * absence of a setting. */
  function drawDelConfirm() {
    const a = find(delName);
    if (!a) { drawDelPick(''); return; }

    const choice = [
      { erase: false, name: '홈 폴더 보관',
        desc: home(a) + ' 의 문서와 사진이 그대로 남습니다' },
      { erase: true,  name: '홈 폴더도 지우기',
        desc: '이 계정의 문서와 사진, 내려받은 파일이 함께 지워집니다' },
    ];

    modal(D_DEL).innerHTML =
      '<h3>' + LP.esc(a.name) + ' 계정을 지웁니다</h3>' +
      '<p>이 계정으로는 더 이상 로그인할 수 없게 됩니다. ' +
        '다른 계정과 그 파일은 그대로 있습니다.</p>' +
      '<div class="list">' +
        choice.map(function (c) {
          const on = delErase === c.erase;
          return '<div class="row click pressable" data-erase="' + c.erase + '">' +
            '<div class="lb"><b>' + c.name + '</b><i>' + c.desc + '</i></div>' +
            '<div class="vl' + (on ? '' : ' off') + '">' +
              (on ? '선택함' : '선택 안 함') + '</div></div>';
        }).join('') +
      '</div>' +
      /* Red, and only under the answer that earns it: this is the one
       * choice on the screen that loses data. */
      '<div data-warn><div class="note bad">▲ ' + home(a) + ' 안의 파일이 모두 지워집니다. ' +
        '되돌릴 수 없습니다.</div></div>' +
      '<div class="foot">' +
        '<button class="btn quiet pressable" data-close="' + D_DEL + '">취소</button>' +
        '<button class="btn danger pressable" data-go></button>' +
      '</div>';

    const box  = modal(D_DEL);
    const warn = box.querySelector('[data-warn]');
    const go   = box.querySelector('[data-go]');

    /* Starts closed the way LPSpring.height leaves a collapsed block, so
     * that the first expansion is measured rather than jumped to. The
     * class would survive the spring clearing display and the note would
     * then fill a block nobody can see. */
    warn.style.display = 'none';
    go.textContent = '계정 삭제';

    box.querySelectorAll('[data-erase]').forEach(function (row) {
      row.addEventListener('click', function () {
        delErase = row.dataset.erase === 'true';

        box.querySelectorAll('[data-erase]').forEach(function (r) {
          const on = (r.dataset.erase === 'true') === delErase;
          const vl = r.querySelector('.vl');
          vl.className = 'vl' + (on ? '' : ' off');
          vl.textContent = on ? '선택함' : '선택 안 함';
        });
        go.textContent = delErase ? '계정과 홈 폴더 삭제' : '계정 삭제';

        /* The live integrator rather than a transition, because this is
         * the block whose target changes mid-flight: somebody weighing
         * the two answers flips between them faster than 240ms, and a
         * CSS transition would stop dead and set off again each time. */
        warn.style.overflow = 'hidden';
        LPSpring.height(warn, delErase);
      });
    });

    go.addEventListener('click', function () { doDelete(a, delErase); });

    /* Also reached from 계정 정보, which opens this step with the account
     * already chosen and no picker behind it, so the opening cannot be
     * left to step one. Opening a dialog that is already open changes
     * nothing. */
    LP.open(D_DEL);
  }

  function doDelete(a, erase) {
    /* The dialog is the drawing of the rule; these two lines are the
     * rule. A click can arrive from a focused button after the account
     * switched underneath it. */
    if (!LP.can(AREA) || !canDelete(a).ok) return;

    LP.close(D_DEL);
    if (LP.autoLogin === a.name) LP.autoLogin = null;

    const row = document.querySelector('#user-list .row[data-acct="' + a.name + '"]');
    const done = function () {
      const i = LP.accounts.indexOf(a);
      if (i >= 0) LP.accounts.splice(i, 1);
      LP.render();
      LP.say(said(), erase
        ? a.name + ' 계정과 홈 폴더를 지웠습니다'
        : a.name + ' 계정을 지웠습니다. ' + home(a) + ' 는 그대로 있습니다');
    };

    /* Taken out of LP only once the row has finished leaving, so the
     * list is never a frame ahead of what is on the screen. */
    if (!row) { done(); return; }
    row.classList.add('leave');
    row.addEventListener('animationend', done, { once: true });
  }

})();
