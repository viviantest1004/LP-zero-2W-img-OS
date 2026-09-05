/* s-keys — 키 지정. Every shortcut on the machine, and the only screen in
 * the settings app that reads the keyboard instead of being typed into.
 *
 * Two things follow from that, and they shape everything below.
 *
 *   While a capture is open, the document's keydown belongs to this
 *   screen. Ctrl+W closes the window the mockup runs in, Super hands the
 *   key to the host, F5 reloads and takes the state the screen is drawn
 *   from with it — so every event is taken, and lp-core.js's Escape
 *   handler is held off with window.LP_CAPTURING while it is. A grab
 *   that broad has to end when the dialog does, including when somebody
 *   else ends it: lp-boot.js hides every dialog on an account switch and
 *   tells no screen it did. So the capture checks its own dialog is
 *   still on screen before it takes another key, rather than trusting
 *   that whoever closed the dialog also came here.
 *
 *   A combination is never refused with a bare no. Either something
 *   holds it and can be made to let go, or something holds it that
 *   cannot — and then the screen says which one, because somebody who
 *   is not told what took Super+L will press Super+L again.
 */
(function () {
  'use strict';

  const AREA = 'keyboard';

  const $ = function (id) { return document.getElementById(id); };
  const pane = function () { return document.getElementById('s-keys'); };
  /* Looked up on use. The line lives outside #keys-list so it survives a
   * render, but the pane itself is rebuilt by the assembler and cached
   * elements are how a screen ends up writing into a detached node. */
  const said = function () { return document.getElementById('keys-say'); };

  /* ── naming a key ───────────────────────────────────────────────
   *
   * e.code, not e.key. e.key is what the layout produced, so on the
   * Hangul layout the key printed R arrives as ㄱ — and a shortcut
   * stored as ㄱ is one that stops working the moment the input source
   * changes. A shortcut is a place on the keyboard, not a letter.
   */
  const CODE = {
    Space: 'Space', Enter: 'Enter', NumpadEnter: 'Enter', Tab: 'Tab',
    Backspace: 'Backspace', Delete: 'Delete', Insert: 'Insert',
    Home: 'Home', End: 'End', PageUp: 'PageUp', PageDown: 'PageDown',
    ArrowUp: 'Up', ArrowDown: 'Down', ArrowLeft: 'Left', ArrowRight: 'Right',
    PrintScreen: 'Print', Pause: 'Pause', ContextMenu: 'Menu',
    Minus: '-', Equal: '=', BracketLeft: '[', BracketRight: ']',
    Backslash: '\\', Semicolon: ';', Quote: '\'', Comma: ',', Period: '.',
    Slash: '/', Backquote: '`'
  };

  /* Media and IME keys carry no useful e.code on every keyboard. The
   * names on the right are the ones LP.shortcuts already stores, so
   * pressing the volume key really does land on 볼륨 올리기 and the
   * conflict that follows is the real one rather than a demonstration. */
  const KEY = {
    HangulMode: '한/영', Hangul: '한/영', HanjaMode: '한자',
    AudioVolumeUp: 'XF86AudioRaiseVolume',
    AudioVolumeDown: 'XF86AudioLowerVolume',
    AudioVolumeMute: 'XF86AudioMute',
    MediaPlayPause: 'XF86AudioPlay',
    MediaTrackNext: 'XF86AudioNext',
    MediaTrackPrevious: 'XF86AudioPrev'
  };

  const MOD = ['Control', 'Alt', 'Shift', 'Meta', 'AltGraph', 'CapsLock', 'NumLock', 'OS'];

  function keyName(e) {
    const c = e.code || '';
    if (/^Key[A-Z]$/.test(c)) return c.slice(3);
    if (/^Digit[0-9]$/.test(c)) return c.slice(5);
    if (/^Numpad[0-9]$/.test(c)) return 'KP_' + c.slice(6);
    if (/^F[0-9]{1,2}$/.test(c)) return c;
    if (CODE[c]) return CODE[c];
    if (KEY[e.key]) return KEY[e.key];
    /* A key this machine will not name is not one a row can draw, and a
     * row that cannot be drawn is worse than a combination not taken. */
    return null;
  }

  /* Always this order. LP.sameKeys sorts before it compares, so
   * Shift+Ctrl+R and Ctrl+Shift+R are already one shortcut — but a list
   * that spells one combination two ways depending on which key the
   * hand reached first is a list nobody can scan down. */
  function mods(e) {
    const m = [];
    if (e.ctrlKey) m.push('Ctrl');
    if (e.altKey) m.push('Alt');
    if (e.shiftKey) m.push('Shift');
    if (e.metaKey) m.push('Super');
    return m;
  }

  /* ── drawing ────────────────────────────────────────────────── */

  /* kind is '' for caps that are simply what the combination is, 'wait'
   * while the hand is still on the keys, 'bad' for a combination that
   * would cost somebody their way back into the machine. Nothing else:
   * a combination that is merely spoken for is drawn plain, and the note
   * under the box carries why. */
  function caps(box, keys, kind) {
    if (!keys || !keys.length) {
      box.innerHTML = '<span class="key free">없음</span>';
      return;
    }
    box.innerHTML = LP.keyText(keys);
    if (kind) box.querySelectorAll('.key').forEach(function (k) { k.classList.add(kind); });
  }

  function plain(keys) { return keys.join(' + '); }

  /* The line under the capture box opens and closes while somebody tries
   * one combination after another, and the next try can arrive before
   * the last one has finished opening. A height that has to be redirected
   * mid-flight is the case lp-spring.js exists for. */
  function note(box, kind, text) {
    box.innerHTML = '<div class="note' + (kind ? ' ' + kind : '') + '">▲ ' + LP.esc(text) + '</div>';
    LPSpring.height(box, true);
  }
  function unnote(box) { LPSpring.height(box, false); }

  /* ── who already answers this combination ───────────────────── */

  function judge(keys, exceptId) {
    /* LP.reserved is read here as well as inside LP.heldBy, and not by
     * oversight. heldBy answers "may this be taken", which for both
     * kinds of system combination is no; the refusal has to say more
     * than no. The table is the only place that carries what the
     * combination is for, and somebody about to lose Ctrl+Alt+F1 has to
     * be told it is 가상 터미널 1 rather than that something holds it. */
    const sys = LP.reserved.find(function (r) { return LP.sameKeys(r.keys, keys); });
    if (sys) return { kind: 'reserved', why: sys.why };
    const held = LP.heldBy(keys, exceptId);
    if (held && held.system) return { kind: 'system', name: held.name };
    if (held) return { kind: 'held', name: held.name, id: held.id };
    return { kind: 'free' };
  }

  /* ── the capture ────────────────────────────────────────────────
   *
   * One controller, driven by both dialogs. What differs between them is
   * only what an accepted combination is written into, and that is the
   * one thing passed in. Two copies of a keydown handler that must
   * preventDefault everything would be two chances to leave one of them
   * attached to a document nobody is capturing on any more.
   */
  let live = null;

  /* The dialog the running capture belongs to, still on screen.
   *
   * LP.closeAll() hides every backdrop, and lp-boot.js calls it whenever
   * the account switches. It does not know this module exists, so the
   * listeners below outlive the dialog they were opened for — and they
   * do not merely sit there. The next key pressed anywhere on the page
   * is a keydown with no combination in front of it, which the capture
   * reads as an answer: switch account with 단축키 지정 open, press a
   * letter, and 검색 열기 quietly becomes that letter with no dialog on
   * screen and nothing said. Asking the DOM whether the dialog is still
   * up is cheaper than asking every caller of closeAll to remember. */
  function alive() {
    if (!live) return false;
    const dlg = document.getElementById(live.dlg);
    return !!dlg && dlg.classList.contains('on');
  }

  function start(cap) {
    stop();
    live = cap;
    /* lp-core.js closes the topmost dialog on Escape. While keys are
     * being read Escape is one of the keys, and the dialog must not be
     * pulled out from under the capture that is reading it. */
    window.LP_CAPTURING = true;
    document.addEventListener('keydown', onDown, true);
    document.addEventListener('keyup', onUp, true);
    cap.reset();
  }

  function stop() {
    document.removeEventListener('keydown', onDown, true);
    document.removeEventListener('keyup', onUp, true);
    live = null;
    window.LP_CAPTURING = false;
  }

  function onDown(e) {
    /* Before preventDefault, never after: a key pressed once the dialog
     * has gone belongs to whatever is on screen now. */
    if (!alive()) { stop(); return; }

    /* Everything, with no exception worth making: Ctrl+W closes the
     * window this is running in, Super hands the key to the host, F5
     * reloads. Each of those ends the capture by ending the page. */
    e.preventDefault();
    e.stopPropagation();
    if (e.repeat) return;              /* a held key is one combination, not forty */

    if (e.key === 'Escape') { live.cancel(); return; }

    const m = mods(e);
    /* A modifier on its own is not a shortcut, so it is shown and waited
     * on rather than proposed. */
    if (MOD.indexOf(e.key) >= 0) { live.holding(m); return; }
    if (e.key === 'Backspace' && !m.length) { live.propose([]); return; }

    const k = keyName(e);
    if (k) live.propose(m.concat([k]));
  }

  function onUp(e) {
    if (!alive()) { stop(); return; }
    e.preventDefault();
    /* Let go without having pressed anything else and the box goes back
     * to what the shortcut is now, rather than leaving half a
     * combination on screen that nothing would save. */
    if (!mods(e).length) live.holding([]);
  }

  function capture(p) {
    const cap = {
      dlg: p.dlg,
      phase: 'idle',
      offer: null,

      reset: function () {
        cap.phase = 'idle';
        cap.offer = null;
        unnote(p.note);
        p.hint.textContent = p.idle();
        caps(p.caps, p.current(), '');
        p.foot('idle');
      },

      holding: function (m) {
        if (m.length) {
          /* A new combination is on its way in, so whatever the last one
           * said — and whatever it offered — belongs to keys that are no
           * longer on screen. An offer about something invisible is a
           * trap rather than an offer. */
          if (cap.phase === 'note') { cap.offer = null; unnote(p.note); p.foot('idle'); }
          cap.phase = 'wait';
          caps(p.caps, m, 'wait');
          /* Said while the hand is still on the keys, because this is
           * where somebody presses Super, sees it appear, and lets go
           * expecting to have set Super. */
          p.hint.textContent = '이어서 키를 하나 더 누르십시오';
          return;
        }
        if (cap.phase === 'wait') cap.reset();
      },

      propose: function (keys) {
        cap.offer = null;
        if (!keys.length) { cap.phase = 'idle'; p.accept([], null); return; }

        const v = judge(keys, p.except());

        if (v.kind === 'reserved') {
          /* The only red on this screen, and it is not about tidiness.
           * Ctrl+Alt+F1 and the rest are how somebody gets back into a
           * machine whose session has stopped answering; an editor that
           * hands one of them to a shortcut is an editor that can leave
           * a machine with no way in from its own keyboard. That is the
           * loss red is for. */
          cap.phase = 'note';
          caps(p.caps, keys, 'bad');
          note(p.note, 'bad', plain(keys) + ' 조합은 ' + v.why +
            '에 쓰입니다. 세션이 멈춘 기계에 키보드로 다시 들어가는 길이라 단축키로 지정할 수 없습니다.');
          p.hint.textContent = '다른 조합을 누르십시오';
          p.foot('idle');
          return;
        }

        if (v.kind === 'system') {
          /* Drawn exactly like a combination that is spoken for, because
           * that is what it is: a limit, not a failure and nothing lost.
           * The amber note says who has it and why it cannot be had, and
           * red caps here would spend the one colour that means somebody
           * is about to lose something. */
          cap.phase = 'note';
          caps(p.caps, keys, '');
          note(p.note, '', '「' + v.name + '」 단축키가 이 조합을 쓰고 있습니다. ' +
            '컴포지터가 창보다 먼저 받는 조합이라 가져올 수 없습니다.');
          p.hint.textContent = '다른 조합을 누르십시오';
          p.foot('idle');
          return;
        }

        if (v.kind === 'held') {
          cap.phase = 'note';
          cap.offer = { keys: keys, id: v.id, name: v.name };
          caps(p.caps, keys, '');
          note(p.note, '', '「' + v.name + '」 단축키가 이 조합을 쓰고 있습니다. 가져오면 그 단축키는 없음이 됩니다.');
          p.hint.textContent = '다른 조합을 눌러도 됩니다';
          /* Neither way out is the default. Taking it costs the other
           * shortcut its combination and leaving it costs this one its
           * assignment, and a primary button here would be the screen
           * making that choice on somebody's behalf. */
          p.foot('held');
          return;
        }

        cap.phase = 'idle';
        p.accept(keys, null);
      },

      take: function () {
        if (!cap.offer) return;
        cap.phase = 'idle';
        p.accept(cap.offer.keys, cap.offer.id);
      },
      leave: function () { cap.reset(); },
      cancel: function () { p.cancel(); }
    };
    return cap;
  }

  const FOOT_HELD =
    '<button class="btn quiet pressable" data-do="cancel">취소</button>' +
    '<button class="btn pressable" data-do="leave">그대로 두기</button>' +
    '<button class="btn pressable" data-do="take">가져오기</button>';

  /* ── one shortcut ───────────────────────────────────────────── */

  function openKey(id) {
    const sc = LP.shortcuts.find(function (s) { return s.id === id; });
    /* The row is not clickable when any of these is true. Asked again
     * here because a refusal that lives only in the drawing is one that
     * disappears the first time the drawing is wrong. */
    if (!sc || sc.reserved || !LP.can(AREA)) return;

    $('key-what').textContent = '「' + sc.name + '」 단축키에 쓸 조합을 누르십시오.';

    const foot = function (phase) {
      $('key-foot').innerHTML = phase === 'held' ? FOOT_HELD
        : (sc.custom ? '<button class="btn danger pressable" data-do="del">삭제</button>' : '') +
          '<button class="btn quiet pressable" data-do="cancel">취소</button>';
    };

    const shut = function () { stop(); LP.close('dlg-key'); };

    const cap = capture({
      dlg: 'dlg-key',
      caps: $('key-caps'), hint: $('key-hint'), note: $('key-note'), foot: foot,
      idle: function () {
        return (sc.keys && sc.keys.length) ? '지금 지정된 조합입니다' : '아직 지정되지 않았습니다';
      },
      current: function () { return sc.keys || []; },
      except: function () { return sc.id; },
      cancel: shut,
      accept: function (keys, from) {
        /* Asked again at the moment of the write. The row asked before
         * it opened this, and an administrator can take 키보드 away from
         * a standard account while the dialog is still up — lp-boot
         * hides it when that happens, but hiding a dialog does not
         * un-press the button somebody had already pressed. */
        if (!LP.can(AREA)) { shut(); return; }

        sc.keys = keys;
        const lost = from ? LP.shortcuts.find(function (s) { return s.id === from; }) : null;
        if (lost) lost.keys = [];
        shut();
        LP.render();
        /* A row that gained a combination says so by showing it. This
         * line is for the shortcut elsewhere in the list that quietly
         * lost one, which is the only change nobody watched happen. */
        if (lost) LP.say(said(),
          '조합을 가져왔습니다. 「' + lost.name + '」 단축키는 없음이 되었습니다.', 'warn');
      }
    });

    $('dlg-key').onclick = function (e) {
      if (e.target.id === 'dlg-key') { shut(); return; }
      const b = e.target.closest('[data-do]');
      if (!b) return;
      const act = b.dataset.do;
      if (act === 'cancel') shut();
      else if (act === 'leave') cap.leave();
      else if (act === 'take') cap.take();
      else if (act === 'del') { shut(); removeCustom(sc.id); }
    };

    LP.open('dlg-key');
    start(cap);
  }

  function removeCustom(id) {
    const sc = LP.shortcuts.find(function (s) { return s.id === id; });
    if (!sc || !sc.custom || !LP.can(AREA)) return;

    const drop = function () {
      /* The row spent 154ms leaving. Asked again on the far side of it
       * for the same reason accept asks: the account can have changed
       * in between. */
      if (!LP.can(AREA)) { LP.render(); return; }
      const i = LP.shortcuts.indexOf(sc);
      if (i >= 0) LP.shortcuts.splice(i, 1);
      LP.render();
      LP.say(said(), '「' + sc.name + '」 단축키를 지웠습니다.');
    };

    /* The row leaves on the insert spring before the list is rebuilt, so
     * what disappears is the row somebody just deleted rather than the
     * whole list blinking and coming back one shorter. .row.leave takes
     * pointer-events with it, so the row cannot be clicked open again on
     * its way out. */
    const row = pane().querySelector('.row[data-id="' + id + '"]');
    if (!row) { drop(); return; }
    row.classList.add('leave');
    row.addEventListener('animationend', drop, { once: true });
  }

  /* ── a shortcut of one's own ────────────────────────────────── */

  function openAdd() {
    if (!LP.can(AREA)) return;

    const draft = { keys: [], from: null };
    const name = $('new-name'), cmd = $('new-cmd'), box = $('new-box');

    name.value = '';
    cmd.value = '';
    name.classList.remove('bad');
    cmd.classList.remove('bad');
    $('new-say').className = 'inline';
    unnote($('new-note'));
    caps($('new-caps'), draft.keys, '');

    const foot = function (phase) {
      $('new-foot').innerHTML = phase === 'held' ? FOOT_HELD
        : '<button class="btn quiet pressable" data-do="cancel">취소</button>' +
          '<button class="btn primary pressable" data-do="add">추가</button>';
    };

    /* This dialog is typed into and listened to, and it cannot be both at
     * once: a capture that swallows every keydown on the document would
     * swallow the command being typed into the field above it. So the
     * box is pressed before it listens, and stops listening the moment a
     * field is focused or a combination lands. Pressing the box takes
     * focus off the field by itself - it is not focusable - so reaching
     * back for the field really does raise the focus event this hangs
     * on. */
    const disarm = function () {
      stop();
      box.classList.add('arm');
      $('new-hint').textContent = draft.keys.length
        ? '여기를 다시 누르면 조합을 바꿉니다'
        : '여기를 누르고 조합을 누르십시오';
    };

    const shut = function () { stop(); LP.close('dlg-new'); };

    const cap = capture({
      dlg: 'dlg-new',
      caps: $('new-caps'), hint: $('new-hint'), note: $('new-note'), foot: foot,
      idle: function () { return '키 조합을 누르십시오'; },
      current: function () { return draft.keys; },
      except: function () { return null; },
      cancel: shut,
      accept: function (keys, from) {
        /* Nothing reaches LP.shortcuts here. This dialog can still be
         * left without answering, and a shortcut that lost its
         * combination to an addition that never happened is a change
         * nobody asked for. */
        draft.keys = keys;
        draft.from = from;
        caps($('new-caps'), keys, '');
        foot('idle');
        const lost = from ? LP.shortcuts.find(function (s) { return s.id === from; }) : null;
        if (lost) note($('new-note'), '', '추가하면 「' + lost.name + '」 단축키는 없음이 됩니다.');
        else unnote($('new-note'));
        disarm();
      }
    });

    const add = function () {
      if (!LP.can(AREA)) { shut(); return; }
      const n = name.value.trim(), c = cmd.value.trim();
      name.classList.toggle('bad', !n);
      cmd.classList.toggle('bad', !c);
      if (!n || !c) {
        LP.say($('new-say'), '이름과 실행할 명령을 모두 적어야 합니다.', 'bad');
        return;
      }
      const lost = draft.from ? LP.shortcuts.find(function (s) { return s.id === draft.from; }) : null;
      if (lost) lost.keys = [];
      const id = 'u' + Date.now();
      LP.shortcuts.push({
        id: id, grp: '사용자 지정', name: n, cmd: c, keys: draft.keys, custom: true
      });
      shut();
      LP.render();
      const row = pane().querySelector('.row[data-id="' + id + '"]');
      if (row) row.classList.add('arrive');
      if (lost) LP.say(said(),
        '조합을 가져왔습니다. 「' + lost.name + '」 단축키는 없음이 되었습니다.', 'warn');
    };

    name.onfocus = disarm;
    cmd.onfocus = disarm;

    $('dlg-new').onclick = function (e) {
      if (e.target.id === 'dlg-new') { shut(); return; }
      if (e.target.closest('#new-box')) { box.classList.remove('arm'); start(cap); return; }
      const b = e.target.closest('[data-do]');
      if (!b) return;
      const act = b.dataset.do;
      if (act === 'cancel') shut();
      else if (act === 'leave') cap.leave();
      else if (act === 'take') cap.take();
      else if (act === 'add') add();
    };

    foot('idle');
    disarm();
    LP.open('dlg-new');
    name.focus();
  }

  /* ── the rows ───────────────────────────────────────────────── */

  function rowHtml(s, can) {
    const fixed = !!s.reserved;
    let nm = LP.esc(s.name);
    let why = '';

    if (fixed) {
      /* 고정 and never the lock, in either account.
       *
       * A lock says an administrator could grant this. Nobody can grant
       * a combination the compositor answers before any window sees it,
       * so this is a limit and wears the amber; putting a lock here as
       * well would print "go and ask an administrator" beside a sentence
       * saying an administrator cannot either. The reason sits on the
       * row rather than waiting inside a dialog because, unlike a
       * refusal that can only be found by trying, this one never
       * changes. */
      nm += '<span class="pill hold">고정</span>';
      why = '컴포지터가 창보다 먼저 받는 조합이라 ' +
            (can ? '바꿀 수 없습니다' : '관리자도 바꿀 수 없습니다');
    } else if (!can) {
      nm += '<span class="lock">자물쇠</span>';
      why = LP.whyLocked(AREA);
    }
    if (why) why = '<div class="why">' + LP.esc(why) + '</div>';

    return '<div class="row ' + (can && !fixed ? 'click pressable' : 'locked') +
        '" data-id="' + LP.esc(s.id) + '">' +
      '<div class="lb"><b>' + nm + '</b>' +
        (s.cmd ? '<i class="mono latin">' + LP.esc(s.cmd) + '</i>' : '') + why +
      '</div>' +
      '<div class="vl"><span class="keys">' +
        (s.keys && s.keys.length ? LP.keyText(s.keys) : '<span class="key free">없음</span>') +
      '</span></div>' +
    '</div>';
  }

  LP.panes['s-keys'] = function (el) {
    const can = LP.can(AREA);
    const list = LP.shortcuts;
    const mine = list.filter(function (s) { return s.custom; });
    const blank = list.filter(function (s) { return !s.keys || !s.keys.length; }).length;

    /* Counts only. Who may change this is said once on every row that
     * will not answer and once beside the one control that has no row of
     * its own, which is where 키보드 says it too; a fourth copy in the
     * subtitle is the sentence starting to sound like an excuse. */
    el.querySelector('#keys-sub').textContent =
      '단축키 ' + list.length + '개' +
      (blank ? ' · 지정되지 않은 것 ' + blank + '개' : '');

    let html = '', grp = null;
    list.forEach(function (s) {
      if (s.custom) return;              /* the section at the bottom is theirs */
      if (s.grp !== grp) {
        if (grp !== null) html += '</div>';
        grp = s.grp;
        html += '<div class="sec">' + LP.esc(grp) + '</div><div class="rows">';
      }
      html += rowHtml(s, can);
    });
    if (grp !== null) html += '</div>';

    html += '<div class="sec">사용자 지정 단축키</div>';
    html += mine.length
      ? '<div class="rows">' + mine.map(function (s) { return rowHtml(s, can); }).join('') + '</div>'
      : '<div class="caption">직접 만든 단축키가 없습니다.</div>';
    html += '<div class="btnrow"><button class="btn pressable" id="keys-add"' +
      (can ? '' : ' disabled') + '>단축키 추가</button></div>';
    if (!can) html += '<div class="caption">' + LP.esc(LP.whyLocked(AREA)) + '</div>';

    /* 초기화 is navigation and not an action taken here, so the row stays
     * open to a standard account for the same reason the sidebar keeps a
     * locked section in the list: the screen it leads to decides who may
     * press what, and two screens deciding one thing is how the two
     * answers drift apart. It carries the lock that renderSidebar puts
     * on 초기화 for that account, so the two ways to the same screen do
     * not disagree about what is waiting there. */
    html += '<div class="sec">되돌리기</div><div class="rows">' +
      '<div class="row click pressable" data-go="s-reset">' +
        '<div class="lb"><b>단축키 전체 초기화' +
          (LP.can('reset') ? '' : '<span class="lock">자물쇠</span>') +
        '</b></div><div class="vl">›</div>' +
      '</div></div>';

    el.querySelector('#keys-list').innerHTML = html;

    /* Assigned rather than added: this runs on every render, and a
     * listener added on every render is one that fires four times on the
     * fourth visit. */
    el.onclick = function (e) {
      if (e.target.closest('.back')) { LP.go('s-kbd'); return; }
      const go = e.target.closest('[data-go]');
      if (go) { LP.go(go.dataset.go); return; }
      if (e.target.closest('#keys-add')) { openAdd(); return; }
      const row = e.target.closest('.row.click[data-id]');
      if (row) openKey(row.dataset.id);
    };
  };
})();