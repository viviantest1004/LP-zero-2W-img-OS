/* 초기화 - putting one part of the settings back to what it shipped with.
 *
 * A list of resets rather than one button, because the question people
 * actually arrive with is narrower than "reset everything": the
 * shortcuts have got into a state, or the Wi-Fi list is full of networks
 * from an office they left two jobs ago. A single reset that takes the
 * keyboard and the display with it is why the button goes unpressed and
 * the machine gets reinstalled instead.
 *
 * Every row answers two questions and the second one is the one that
 * decides whether the button is pressable at all:
 *
 *     무엇이 돌아오는가   what comes back
 *     무엇이 남는가       what is left alone
 *
 * 웹 is the clearest case. The only thing anybody wants to know before
 * pressing it is whether the bookmarks and the saved passwords go too,
 * and a description that does not say leaves them to find out by trying.
 * 네트워크 is the other one: it drops the machine off the network, and
 * that is a surprise exactly once per person.
 *
 * 모든 설정 is the six of them at once and is not the factory reset. It
 * says so twice before anything happens - once in the row and once in
 * the confirmation - rather than in a note under the button, because by
 * the time somebody is under the button they have decided.
 */

(function () {
  'use strict';

  const PANE   = 's-reset';
  const AREA   = 'reset';        /* 사용자 › 권한 decides who may do any of this */
  const DIALOG = 'dlg-reset';

  /* ── the defaults ───────────────────────────────────────────────
   *
   * Taken while the module loads, which is before any screen has had a
   * chance to change anything.
   *
   * Copied per object and per array, not per list. 키 지정 edits a
   * shortcut where it lies - it changes the keys array on the object it
   * found in LP.shortcuts - so a defaults table holding the same objects
   * would be edited along with the live list, and the reset would put
   * back whatever was there a moment ago. That failure has no symptom
   * except that the button quietly does nothing, which is the worst
   * kind. Frozen so that the next screen to try it fails here instead. */
  function frozen(list) {
    return Object.freeze(list.map(function (item) {
      const copy = {};
      for (const k in item)
        copy[k] = Array.isArray(item[k]) ? Object.freeze(item[k].slice()) : item[k];
      return Object.freeze(copy);
    }));
  }

  /* And back out again. What goes into LP has to be editable, or the
   * first shortcut somebody changes after a reset throws. */
  function live(list) {
    return list.map(function (item) {
      const copy = {};
      for (const k in item)
        copy[k] = Array.isArray(item[k]) ? item[k].slice() : item[k];
      return copy;
    });
  }

  /* Held for the life of the page rather than re-read per render, and
   * that is correct rather than stale: these are what the machine
   * shipped with, which is the one thing on this screen that does not
   * depend on who is signed in. */
  const DEFAULT = Object.freeze({
    shortcuts:    frozen(LP.shortcuts),
    sources:      frozen(LP.sources),
    switchKey:    LP.switchKey,
    switchCustom: Object.freeze(LP.switchCustom.slice()),
  });

  /* ── what each row does ─────────────────────────────────────────
   *
   * 단축키 and 키보드 설정 really change the shared state, so opening
   * 키보드 afterwards shows the result rather than a screen that claims
   * a reset happened somewhere else. The other four have nothing on LP
   * to put back - they are drawn from static markup in this mockup - so
   * they carry the confirmation, the progress and the feedback and
   * change nothing. Which of the two a row is must not be visible: a
   * reset that behaves differently because the mockup is thin is a
   * mockup teaching the wrong screen. */
  const KINDS = [
    {
      key:  'shortcuts',
      name: '단축키',
      desc: '모든 단축키가 처음 값으로 돌아가고, 직접 만든 단축키는 지워집니다',
      title: '단축키를 초기화합니다',
      back: '검색 열기, 창 전환, 스크린샷을 비롯한 단축키 전부가 처음 값으로 돌아갑니다. 직접 만든 단축키는 지워집니다.',
      keep: '입력 소스와 입력 전환 키는 그대로 있습니다.',
      done: '단축키를 처음 값으로 되돌렸습니다',
      /* A new list rather than the old one emptied and refilled: 키 지정
       * reads LP.shortcuts on every render and holds nothing between
       * them, and a fresh list is the version that cannot leave a
       * deleted custom shortcut alive in somebody's closure. */
      apply: function () { LP.shortcuts = live(DEFAULT.shortcuts); },
    },
    {
      key:  'keyboard',
      name: '키보드 설정',
      desc: '입력 소스와 전환 키, 키 반복 속도와 시작 지연을 되돌립니다',
      title: '키보드 설정을 초기화합니다',
      back: '입력 소스가 한국어와 English (US) 둘로 돌아가고, 입력 전환 키와 키 반복 속도, 반복 시작 지연이 처음 값이 됩니다. 나중에 추가한 입력 소스는 목록에서 빠집니다.',
      keep: '단축키는 그대로 있습니다. English (US) 는 초기화해도 남습니다 — 키보드에 인쇄된 글자와 로그인 화면, 복구 콘솔이 이 배열을 씁니다.',
      done: '키보드 설정을 처음 값으로 되돌렸습니다',
      apply: function () {
        LP.sources       = live(DEFAULT.sources);
        LP.switchKey     = DEFAULT.switchKey;
        LP.switchCustom  = DEFAULT.switchCustom.slice();
        /* 키 반복 속도 and 반복 시작 지연 are named in the row and in
         * the dialog because they are what the reset covers on a
         * machine. They are not on LP - the 키보드 screen draws them
         * from its own controls - so there is nothing here to put back.
         * The wording follows the machine rather than the mockup, so
         * that the day the setting becomes real the sentence is already
         * right. */
      },
    },
    {
      key:  'display',
      name: '화면',
      desc: '해상도와 주사율, 배율, 방향, 야간 모드를 되돌립니다',
      title: '화면 설정을 초기화합니다',
      back: '해상도와 주사율이 연결된 화면이 알려 온 값으로 돌아가고, 배율과 방향, 야간 모드가 처음 값이 됩니다.',
      keep: '배경 화면은 그대로 있습니다.',
      done: '화면 설정을 처음 값으로 되돌렸습니다',
    },
    {
      key:  'sound',
      name: '소리',
      desc: '출력·입력 볼륨과 앱별 볼륨, 알림음을 되돌립니다',
      title: '소리 설정을 초기화합니다',
      back: '출력 볼륨과 입력 볼륨, 앱마다 따로 맞춰 둔 볼륨, 알림음이 처음 값으로 돌아갑니다.',
      keep: '골라 둔 출력 장치와 입력 장치는 그대로 있습니다.',
      done: '소리 설정을 처음 값으로 되돌렸습니다',
    },
    {
      key:  'network',
      name: '네트워크',
      desc: '저장된 Wi-Fi 와 비밀번호, 고정 주소, 프록시, VPN 을 지웁니다. 연결이 끊어집니다',
      title: '네트워크 설정을 초기화합니다',
      back: '저장된 Wi-Fi 네트워크와 비밀번호, 직접 넣은 고정 주소와 DNS, 프록시, 등록한 VPN 이 모두 지워집니다.',
      keep: '파일과 계정, 다른 설정은 그대로 있습니다.',
      /* Amber. This row and 모든 설정 are the only two that carry a
       * warn, and what it describes is a limit on what the machine can
       * do the moment the button is pressed - which is what the colour
       * is for. Red would say the reset failed. */
      warn: '초기화하면 이 컴퓨터가 네트워크에서 떨어집니다. 다시 연결하려면 Wi-Fi 비밀번호를 한 번 더 입력해야 합니다.',
      done: '네트워크 설정을 지웠습니다. 연결이 끊어졌습니다',
    },
    {
      key:  'web',
      name: '웹',
      desc: '홈페이지와 검색 엔진, 사이트 권한, 쿠키와 캐시를 되돌립니다. 즐겨찾기와 비밀번호, 기록은 그대로입니다',
      title: '웹 설정을 초기화합니다',
      back: '브라우저의 홈페이지와 검색 엔진이 처음 값으로 돌아가고, 사이트마다 허용해 둔 권한과 쿠키, 캐시가 지워집니다. 로그인해 둔 사이트에서는 로그아웃됩니다.',
      keep: '즐겨찾기와 저장한 비밀번호, 방문 기록은 지워지지 않습니다.',
      done: '웹 설정을 처음 값으로 되돌렸습니다',
    },
    {
      key:  'all',
      name: '모든 설정',
      desc: '위 여섯 항목을 한 번에 되돌립니다. 공장 초기화가 아닙니다',
      title: '모든 설정을 초기화합니다',
      back: '단축키와 키보드, 화면, 소리, 네트워크, 웹 설정이 한 번에 처음 값으로 돌아갑니다. 저장된 Wi-Fi 와 비밀번호도 함께 지워집니다.',
      keep: '공장 초기화가 아닙니다. 계정과 로그인 비밀번호, 문서와 사진, 내려받은 파일, 설치한 앱은 그대로 있습니다.',
      warn: '초기화하면 네트워크 연결이 끊어집니다. 다시 연결하려면 Wi-Fi 비밀번호를 한 번 더 입력해야 합니다.',
      done: '모든 설정을 처음 값으로 되돌렸습니다',
      /* Runs the six rather than repeating what they do. Two lists of
       * what 모든 설정 covers would be two lists to keep in step, and
       * the one that fell behind would be the one nobody was reading. */
      apply: function () {
        KINDS.forEach(function (k) { if (k.key !== 'all' && k.apply) k.apply(); });
      },
    },
  ];

  function isAll(k)  { return k.key === 'all'; }
  function notAll(k) { return k.key !== 'all'; }
  function byKey(key) {
    return KINDS.find(function (k) { return k.key === key; });
  }

  /* Which rows have a bar still filling.
   *
   * Cleared on every render, and that is the point of it being here
   * rather than on the row: a run that was mid-flight was animating
   * elements the render has just thrown away, and a flag left standing
   * would refuse the next press for the rest of the session. */
  let running = {};

  /* Opening and closing the block the bar sits in.
   *
   * The bar carries 14px of margin above it, and a margin that is not
   * inside the block being opened collapses out through it - the row
   * would take the 14px in one frame and then spring the 4 that are
   * left, which is a jump with an animation after it. overflow makes
   * the wrapper a formatting context, so the gap is measured with the
   * bar and the whole thing travels together. Set on both directions
   * because the spring clears it again when it settles open, and the
   * collapse has to measure the same height the expansion did. */
  function slide(slot, open) {
    slot.style.overflow = 'hidden';
    LPSpring.height(slot, open);
  }

  /* ── drawing ────────────────────────────────────────────────────
   *
   * The buttons are disabled rather than dropped. A standard account
   * needs to see that the reset exists and that somebody has it -
   * a screen that hides what it will not do sends people looking for a
   * setting that is right in front of them.
   *
   * A row therefore ends one of two ways: somewhere to report progress
   * and say it is done, or the line naming who can. It never needs
   * both, and building the empty bar for a row that can never run it
   * costs a forced layout each, on the account this screen is most
   * often looked at as. */
  function rowHtml(kind, may) {
    return '' +
      '<div class="row' + (may ? '' : ' locked') + '" data-kind="' + kind.key + '">' +
        '<div class="lb">' +
          '<b>' + kind.name + (may ? '' : '<span class="lock">자물쇠</span>') + '</b>' +
          '<i>' + kind.desc + '</i>' +
          (may ? '<div data-prog></div><div class="inline"></div>'
               : '<div class="why">' + LP.whyLocked(AREA) + '</div>') +
        '</div>' +
        '<button class="btn' + (may ? ' pressable' : '') + '"' +
          (may ? '' : ' disabled') + '>초기화</button>' +
      '</div>';
  }

  function fillRows(box, kinds, may) {
    box.innerHTML = kinds.map(function (k) { return rowHtml(k, may); }).join('');

    /* A locked row has no handler at all rather than one that refuses:
     * the refusal is the disabled button and the line under the name,
     * and a second one that only appeared after a click would be
     * telling somebody off for clicking. */
    if (!may) return;

    box.querySelectorAll('.row').forEach(function (row) {
      const kind = byKey(row.dataset.kind);

      /* The block the progress bar lives in starts closed, and it has to
       * start closed with an inline display: LPSpring.height opens a
       * block by clearing el.style.display, which a class would survive
       * - the bar would then fill inside something nobody can see. This
       * is the same state the spring leaves a block in when it finishes
       * collapsing one. */
      const slot = row.querySelector('[data-prog]');
      slot.style.display = 'none';
      /* Seed the spring closed. It is created on the first call at
       * whatever that call asks for, so a block whose first call is
       * "open" is already at its target and arrives with no motion at
       * all. Measuring a hidden block costs a zero and buys the first
       * expansion. */
      slide(slot, false);

      /* Bound to a button this render has just made, so nothing is left
       * over from the drawing before it. */
      row.querySelector('button')
         .addEventListener('click', function () { ask(kind); });
    });
  }

  /* ── the confirmation ───────────────────────────────────────────── */
  function ask(kind) {
    /* The disabled button is a drawing of the permission. This is the
     * permission. Asked again here so that a click arriving another way
     * - a focused button and a stray Enter, a row drawn before the
     * account changed - is refused for the real reason. */
    if (!LP.can(AREA)) return;

    const modal = document.getElementById(DIALOG).querySelector('.modal');
    modal.innerHTML =
      '<h3>' + kind.title + '</h3>' +
      '<p>' + kind.back + '</p>' +
      '<p>' + kind.keep + '</p>' +
      (kind.warn ? '<div class="note">▲ ' + kind.warn + '</div>' : '') +
      '<div class="foot">' +
        '<button class="btn quiet pressable" data-close="' + DIALOG + '">취소</button>' +
        '<button class="btn danger pressable" data-confirm>초기화</button>' +
      '</div>';

    /* Rewriting the modal threw the previous confirm button away with
     * its listener, so seven rows sharing one dialog cannot end up with
     * seven handlers on it. 취소 and the backdrop are not wired here at
     * all - lp-boot answers data-close and a click on the backdrop for
     * every dialog in the window. */
    modal.querySelector('[data-confirm]').addEventListener('click', function () {
      LP.close(DIALOG);
      run(kind);
    });

    LP.open(DIALOG);
  }

  /* ── doing it ───────────────────────────────────────────────────── */
  function run(kind) {
    if (!LP.can(AREA)) return;
    if (running[kind.key]) return;      /* the bar from the last press is still filling */
    running[kind.key] = true;

    /* The state changes now, not when the bar reaches the end. The bar
     * reports work rather than doing it, and a screen that waits for its
     * own animation before changing anything is lying about the order
     * things happen in - visibly so, the first time somebody switches
     * away mid-fill and finds the reset half done. */
    if (kind.apply) kind.apply();

    /* Looked up now rather than captured when the row was drawn. The
     * confirmation sits between the click and this, and a render can
     * land in that gap - the row this writes into has to be the one on
     * screen, not the one the button was made in. */
    const row  = document.getElementById(PANE)
                         .querySelector('[data-kind="' + kind.key + '"]');
    const slot = row.querySelector('[data-prog]');
    const line = row.querySelector('.inline');

    /* A new bar every time. A bar left over from the run before is
     * already at 100% and has nowhere to travel, so it would finish
     * without a transition and without the transitionend the rest of
     * this hangs on. */
    slot.innerHTML = '<div class="prog"><div></div></div>';
    const fill = slot.querySelector('.prog div');

    slide(slot, true);

    /* And the bar has to be laid out at zero before it is told to go to
     * 100%, or the browser only ever sees the final value: no
     * transition, no transitionend, a bar that snaps full and a row
     * that never says it is done. Reading offsetWidth is that layout.
     * It is asked for here rather than taken from the scrollHeight
     * LPSpring.height happens to read one line above, because a
     * completion path resting on another module's internals breaks the
     * day that module stops measuring. */
    void fill.offsetWidth;
    fill.style.width = '100%';

    /* Nothing here picks a duration. lp-ui.css gives .prog div a linear
     * 0.4s on width - linear because a bar filling at a known rate is a
     * readout, and easing it would make it lie about the rate. When it
     * lands the block collapses on the expand spring and the line
     * appears under the row that caused it. */
    fill.addEventListener('transitionend', function () {
      running[kind.key] = false;
      slide(slot, false);
      LP.say(line, kind.done);
    }, { once: true });
  }

  /* ── the pane ───────────────────────────────────────────────────
   *
   * Rebuilt from LP on every call, because it is called again every time
   * the account changes. Only the two lists are rewritten - never the
   * pane - because the block the assembler drops into the slot at the
   * bottom belongs to 공장 초기화, which registers under the id of that
   * block and redraws itself. An innerHTML on the pane would take it
   * away every time somebody switched account. */
  function render(el) {
    running = {};
    const may = LP.can(AREA);
    fillRows(el.querySelector('#reset-rows'), KINDS.filter(notAll), may);
    fillRows(el.querySelector('#reset-all'),  KINDS.filter(isAll),  may);
  }

  LP.panes[PANE] = render;

})();
