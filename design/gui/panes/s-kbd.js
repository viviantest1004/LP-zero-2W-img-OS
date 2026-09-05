/* 키보드 - input sources, the key that cycles them, typing, and the door
 * to the shortcut list.
 *
 * The reference mockup drew this screen as three fixed rows, and three
 * fixed rows is a picture. The moment the 한/영 pair becomes a list it
 * gains an add and a remove, and the moment it gains a remove the whole
 * screen turns on one sentence: what happens when somebody tries to
 * delete English (US). That refusal is a dialog and not a greyed-out
 * button, because a person who does not know why it will not go is going
 * to keep pressing until somebody tells them.
 */

(function () {
  'use strict';

  const PANE = 's-kbd';

  /* Repeat speed and delay are the only state this screen adds, and they
   * go on LP with everything else. A screen holding its own copy looks
   * fine until the account switches: LP re-renders, the pane rebuilds
   * from LP, and these two sliders alone snap back to values nobody can
   * account for. 0-100 is the slider's coordinate; the two functions
   * below are where it acquires a meaning. */
  LP.typing = LP.typing || { rate: 60, delay: 35 };

  const SWITCHES = [
    { id: 'hangul',     name: '한/영',         keys: ['한/영'] },
    { id: 'shiftspace', name: 'Shift + Space', keys: ['Shift', 'Space'] },
    { id: 'altspace',   name: 'Alt + Space',   keys: ['Alt', 'Space'] },
    /* The only one whose combination is not written here: a person chose
     * it, so it lives in LP.switchCustom where the capture screen can
     * change it. */
    { id: 'custom',     name: '직접 지정',      keys: null },
  ];

  function switchOf(id) {
    return SWITCHES.find(s => s.id === id) || SWITCHES[0];
  }

  function switchKeys(id) {
    return switchOf(id).keys || LP.switchCustom;
  }

  function caps(keys) {
    return '<span class="keys">' + LP.keyText(keys) + '</span>';
  }

  /* What the slider positions actually mean. Two characters a second is
   * slow enough to count by hand; thirty is the cursor sliding to the end
   * of the line. The delay range is the one a person can tell apart -
   * below 150ms a brushed key repeats, above a second it feels stuck. */
  function perSec()  { return Math.round(2 + LP.typing.rate / 100 * 28); }
  function delayMs() { return Math.round(150 + LP.typing.delay / 100 * 850); }

  function rateWord(v)  { return v < 34 ? '느리게' : v < 67 ? '보통' : '빠르게'; }
  function delayWord(v) { return v < 40 ? '짧게'   : v < 75 ? '보통' : '길게'; }

  /* ── the pane ─────────────────────────────────────────────────── */

  LP.panes[PANE] = function (el) {
    const may  = LP.can('keyboard');
    const src  = LP.sources;
    const solo = src.length <= 1;

    /* A key held down while the account switches would leave a timer
     * typing into a field that no longer exists. */
    stopRepeat(el);

    el.querySelector('#k-sub').textContent = solo
      ? '입력 소스 1개 · 전환할 대상 없음'
      : '입력 소스 ' + src.length + '개 · 전환 키 ' + switchOf(LP.switchKey).name;

    const mark = may ? '' : '<span class="lock">자물쇠</span>';
    const why  = may ? '' : '<div class="why">' + LP.whyLocked('keyboard') + '</div>';
    const shut = may ? '' : ' locked';
    const dis  = may ? '' : ' disabled';

    let h = '<div class="sec">입력 소스</div><div class="rows" id="k-list">';

    src.forEach((s, i) => {
      /* 기본 is the only pill that changes with the account. On a row
       * nobody may touch, a bright pill is the brightest thing on the
       * screen and it pulls the eye to the one row that will not answer.
       * 고정 stays amber either way - that English cannot be removed is
       * true whoever signed in. */
      const pills =
        (i === 0 ? '<span class="pill' + (may ? ' now' : '') + '">기본</span>' : '') +
        (s.fixed ? '<span class="pill hold">고정</span>' : '');

      /* The 삭제 button on the fixed source is deliberately not disabled.
       * A control that does nothing when pressed teaches nothing; this
       * one presses, refuses, and says what it is protecting. */
      h += '<div class="row' + shut + (may ? '' : ' fixed') + '" data-id="' + LP.esc(s.id) + '">' +
             '<span class="drag">⠿</span>' +
             '<div class="lb"><b>' + LP.esc(s.name) + pills + mark + '</b>' +
               '<i>' + LP.esc(s.desc) + '</i>' + why +
             '</div>' +
             '<button class="btn quiet pressable" data-del="' + LP.esc(s.id) + '"' + dis + '>삭제</button>' +
           '</div>';
    });

    h += '</div>' +
         '<div class="caption">목록의 첫 번째가 새 창이 시작하는 입력 소스입니다. 손잡이를 끌어 순서를 바꿉니다.</div>' +
         '<div class="btnrow"><button class="btn pressable" id="k-add-open"' + dis + '>입력 소스 추가</button></div>' +
         '<div class="inline" id="k-said"></div>';

    /* ── 입력 전환 ── */
    h += '<div class="sec">입력 전환</div><div class="rows">';
    if (solo) {
      /* Not locked: nobody took this away, there is simply nothing on the
       * other side of the switch. The key itself is still shown, because
       * it is still what is configured. */
      h += '<div class="row dim"><div class="lb"><b>전환 키</b>' +
             '<i>입력 소스가 하나뿐이라 전환할 곳이 없습니다</i></div>' +
             '<div class="vl">' + caps(switchKeys(LP.switchKey)) + '</div></div>';
    } else if (may) {
      h += '<div class="row click pressable" id="k-switch-open">' +
             '<div class="lb"><b>전환 키</b></div>' +
             '<div class="vl">' + caps(switchKeys(LP.switchKey)) + '</div></div>';
    } else {
      h += '<div class="row locked"><div class="lb"><b>전환 키' + mark + '</b>' + why + '</div>' +
             '<div class="vl">' + caps(switchKeys(LP.switchKey)) + '</div></div>';
    }
    h += '</div>';

    /* ── 타이핑 ── */
    function slider(key, name, word) {
      return '<div class="row' + shut + '" data-bar="' + key + '">' +
        '<div class="lb"><b>' + name + mark + '</b>' + why +
          '<div class="bar" style="width:190px"><div style="width:' + LP.typing[key] + '%"></div></div>' +
        '</div>' +
        '<div class="vl">' + word + '</div></div>';
    }

    h += '<div class="sec">타이핑</div><div class="rows">' +
         slider('rate',  '키 반복 속도',   rateWord(LP.typing.rate)) +
         slider('delay', '반복 시작 지연', delayWord(LP.typing.delay)) +
         '<div class="row"><div class="lb"><b>시험 입력</b>' +
           '<i>키를 길게 누르면 위에서 정한 그대로 반복됩니다</i>' +
           '<input class="field mono" id="k-try" placeholder="여기에 입력">' +
         '</div><div class="vl">' + (delayMs() / 1000).toFixed(2) + '초 · 초당 ' + perSec() + '자</div></div>' +
         '</div>';

    /* ── 단축키, 초기화 ──
     *
     * Both of these rows go somewhere; neither changes anything here, so
     * neither is locked even when the area is. It is the sidebar's rule:
     * a restricted section keeps its place in the list, and the lock is
     * drawn on the screen that owns the setting. Taking the way in away
     * as well leaves somebody hunting for a screen that exists on every
     * other machine. */
    h += '<div class="sec">단축키</div><div class="rows">' +
         '<div class="row click pressable" id="k-keys">' +
           '<div class="lb"><b>키 지정</b><i>설정된 단축키 ' + LP.shortcuts.length + '개</i></div>' +
           '<div class="vl">›</div></div></div>';

    h += '<div class="sec">초기화</div><div class="rows">' +
         '<div class="row click pressable" id="k-reset">' +
           '<div class="lb"><b>키보드 설정 초기화</b>' +
             '<i>입력 소스와 전환 키를 처음 상태로 되돌립니다. 단축키는 그대로 둡니다</i></div>' +
           '<div class="vl">›</div></div></div>';

    const body = el.querySelector('#k-body');
    body.innerHTML = h;

    /* The dialogs sit at the end of the window, outside this pane, so
     * they survive the rebuild and must not be wired again with it. */
    wireDialogs();

    if (may) {
      body.querySelector('#k-add-open').addEventListener('click', openAdd);
      body.querySelectorAll('[data-del]').forEach(b =>
        b.addEventListener('click', () => remove(b.dataset.del)));
      const sw = body.querySelector('#k-switch-open');
      if (sw) sw.addEventListener('click', openSwitch);
      wireSliders(body);
      wireDrag(body);
    }
    body.querySelector('#k-keys').addEventListener('click', () => LP.go('s-keys'));
    body.querySelector('#k-reset').addEventListener('click', () => LP.go('s-reset'));
    wireTry(el);
  };

  /* ── the list ─────────────────────────────────────────────────── */

  function said() { return document.querySelector('#k-said'); }

  function remove(id) {
    const verdict = LP.canRemoveSource(id);
    if (!verdict.ok) { refuse(id, verdict); return; }

    const row = document.querySelector('#k-list .row[data-id="' + id + '"]');
    if (!row) return;
    /* Taken out of LP only once the row has finished leaving, so the list
     * is never a frame ahead of what is on the screen. */
    row.classList.add('leave');
    row.addEventListener('animationend', () => {
      const i = LP.sources.findIndex(s => s.id === id);
      if (i >= 0) LP.sources.splice(i, 1);
      LP.render();
    }, { once: true });
  }

  /* The refusal. Everything it says comes from canRemoveSource - the
   * screen does not get to invent a second reason, and there is nowhere
   * for the two to drift apart. */
  function refuse(id, verdict) {
    const d = document.getElementById('k-nodrop');
    d.dataset.id = id;
    d.querySelector('#k-nodrop-why').textContent = verdict.why;

    const detail = d.querySelector('#k-nodrop-detail');
    detail.textContent = verdict.detail || '';
    detail.hidden = !verdict.detail;

    /* Moving it down is only worth offering when there is somewhere to
     * move it to. Offering it on a one-item list would be a button that
     * changes nothing, which is the thing this dialog exists to avoid. */
    const last = LP.sources[LP.sources.length - 1];
    const worth = LP.sources.length > 1 && last.id !== id && LP.can('keyboard');
    d.querySelector('#k-nodrop-offer').hidden = !worth;
    d.querySelector('#k-nodrop-demote').hidden = !worth;

    LP.open('k-nodrop');
  }

  function move(fromId, toId, after) {
    const from = LP.sources.findIndex(s => s.id === fromId);
    let to = LP.sources.findIndex(s => s.id === toId);
    if (from < 0 || to < 0 || from === to) return;

    const wasFirst = LP.sources[0].id;
    const [s] = LP.sources.splice(from, 1);
    if (from < to) to--;              /* pulling it out moved the target up one */
    LP.sources.splice(to + (after ? 1 : 0), 0, s);
    LP.render();

    /* A reorder is its own feedback - the rows moved. The only part worth
     * a sentence is the first place changing, because that is the part
     * that changes what a new window does. */
    if (LP.sources[0].id !== wasFirst)
      LP.say(said(), '이제 목록의 첫 번째는 ' + LP.sources[0].name + ' 입니다.');
  }

  function wireDrag(root) {
    const list = root.querySelector('#k-list');
    let held = null;

    function clearMarks() {
      list.querySelectorAll('.dropinto,.dropafter')
          .forEach(r => r.classList.remove('dropinto', 'dropafter'));
    }

    list.querySelectorAll('.row[data-id]').forEach(row => {
      /* Only the handle starts a drag. With the whole row draggable the
       * name cannot be selected and a reach for 삭제 drags the row. */
      row.querySelector('.drag').addEventListener('mousedown', () => { row.draggable = true; });

      row.addEventListener('dragstart', e => {
        held = row;
        row.classList.add('dragging');
        e.dataTransfer.effectAllowed = 'move';
        e.dataTransfer.setData('text/plain', row.dataset.id);
      });
      row.addEventListener('dragend', () => {
        row.draggable = false;
        row.classList.remove('dragging');
        clearMarks();
        held = null;
      });
      row.addEventListener('dragover', e => {
        if (!held || held === row) return;
        e.preventDefault();
        e.dataTransfer.dropEffect = 'move';
        const r = row.getBoundingClientRect();
        /* Above the hovered row, except past the middle of the last one:
         * every other position is reachable by aiming at the row below
         * it, and the end of the list is not. */
        const after = !row.nextElementSibling && (e.clientY - r.top) > r.height / 2;
        clearMarks();
        row.classList.add(after ? 'dropafter' : 'dropinto');
      });
      row.addEventListener('drop', e => {
        e.preventDefault();
        if (!held || held === row) return;
        move(held.dataset.id, row.dataset.id, row.classList.contains('dropafter'));
      });
    });
  }

  /* ── the sliders ──────────────────────────────────────────────── */

  function wireSliders(root) {
    root.querySelectorAll('[data-bar]').forEach(row => {
      const key  = row.dataset.bar;
      const bar  = row.querySelector('.bar');
      const fill = bar.firstElementChild;
      const val  = row.querySelector('.vl');
      const word = key === 'rate' ? rateWord : delayWord;

      function setFrom(e) {
        const r = bar.getBoundingClientRect();
        const v = Math.max(0, Math.min(100, Math.round((e.clientX - r.left) / r.width * 100)));
        LP.typing[key] = v;
        /* Mid-drag only these two nodes are touched. Re-rendering the
         * pane every frame would replace the bar under the finger that
         * is holding it. LP is still the value; this is painting ahead
         * of the redraw, not a second copy of it. */
        fill.style.width = v + '%';
        val.textContent = word(v);
      }

      /* The bar is 5px tall, which is a fine thing to look at and a poor
       * thing to hit. The row listens and the bar's own rectangle, grown
       * 10px each way, decides whether the press was aimed at it - so
       * pressing the name does not throw the value across the screen. */
      row.addEventListener('pointerdown', e => {
        const r = bar.getBoundingClientRect();
        if (e.clientY < r.top - 10 || e.clientY > r.bottom + 10) return;
        row.setPointerCapture(e.pointerId);
        setFrom(e);
      });
      row.addEventListener('pointermove', e => {
        if (row.hasPointerCapture(e.pointerId)) setFrom(e);
      });
      row.addEventListener('pointerup', e => {
        if (!row.hasPointerCapture(e.pointerId)) return;
        row.releasePointerCapture(e.pointerId);
        LP.render();
      });
    });
  }

  /* ── the try field ────────────────────────────────────────────
   *
   * The browser has its own repeat rate and it is not this machine's
   * setting. So the browser's repeat is refused and the characters are
   * put in by hand, at the delay and the rate the two sliders above are
   * holding. It is the difference between a slider and a drawing of one.
   */

  function wireTry(el) {
    const f = el.querySelector('#k-try');

    f.addEventListener('keydown', e => {
      if (e.isComposing) return;            /* never argue with the IME */
      if (e.key.length !== 1) return;       /* Backspace and the arrows stay the browser's */
      if (e.repeat) { e.preventDefault(); return; }
      stopRepeat(el);
      el._delay = setTimeout(() => {
        el._rep = setInterval(() => type(f, e.key), 1000 / perSec());
      }, delayMs());
    });
    f.addEventListener('keyup', () => stopRepeat(el));
    f.addEventListener('blur',  () => stopRepeat(el));
  }

  function type(f, ch) {
    const at = f.selectionStart;
    f.value = f.value.slice(0, at) + ch + f.value.slice(f.selectionEnd);
    f.selectionStart = f.selectionEnd = at + 1;
  }

  function stopRepeat(el) {
    clearTimeout(el._delay);
    clearInterval(el._rep);
    el._delay = el._rep = null;
  }

  /* ── the dialogs ──────────────────────────────────────────────── */

  function openAdd() {
    const q = document.getElementById('k-add-q');
    q.value = '';
    fillCatalogue('');
    LP.open('k-add');
    q.focus();
  }

  function fillCatalogue(query) {
    const list = document.getElementById('k-add-list');
    const term = query.trim().toLowerCase();
    const hits = LP.catalogue.filter(c =>
      (c.name + ' ' + c.desc).toLowerCase().indexOf(term) >= 0);

    list.innerHTML = hits.map(c => {
      /* Already in the list, so it is shown and not offered. Hiding it
       * would read as "we do not have that one". */
      const taken = LP.sources.some(s => s.id === c.id);
      return '<div class="row' + (taken ? ' taken' : ' click pressable') + '" data-add="' + LP.esc(c.id) + '">' +
        '<div class="lb"><b>' + LP.esc(c.name) + '</b><i>' + LP.esc(c.desc) + '</i></div>' +
        '<div class="vl' + (taken ? ' off' : '') + '">' + (taken ? '추가됨' : '추가') + '</div></div>';
    }).join('');

    document.getElementById('k-add-none').hidden = hits.length > 0;
  }

  function add(id) {
    const c = LP.catalogue.find(x => x.id === id);
    if (!c || LP.sources.some(s => s.id === id)) return;
    /* Written with the same shape as the seeded list, fixed included: an
     * entry missing the field hands the next screen an undefined to test
     * against, and undefined tests as "removable" by accident. */
    LP.sources.push({ id: c.id, name: c.name, desc: c.desc, fixed: false });
    LP.close('k-add');
    LP.render();
    const row = document.querySelector('#k-list .row[data-id="' + c.id + '"]');
    if (row) row.classList.add('arrive');
  }

  function openSwitch() {
    const list = document.getElementById('k-switch-list');
    list.innerHTML = SWITCHES.map(o => {
      const on = LP.switchKey === o.id;
      /* Only 직접 지정 carries a second line: the other three names are
       * already the combination, and repeating it under itself is how a
       * list of four turns into a list of eight things to read. */
      const sub = o.keys ? '' : '<i class="keys">' + LP.keyText(LP.switchCustom) + '</i>';
      return '<div class="row click pressable" data-sw="' + o.id + '">' +
        '<div class="lb"><b>' + LP.esc(o.name) + '</b>' + sub + '</div>' +
        '<div class="vl' + (on ? '' : ' off') + '">' + (on ? '사용 중' : '선택 안 함') + '</div></div>';
    }).join('');
    LP.open('k-switch');
  }

  /* Wired once. The dialogs are not rebuilt with the pane, so binding
   * them from the render function would stack a handler per redraw. */
  function wireDialogs() {
    const add_ = document.getElementById('k-add');
    if (add_._wired) return;

    ['k-add', 'k-switch', 'k-nodrop'].forEach(id => {
      const d = document.getElementById(id);
      /* Three ways out of every dialog: the button, Escape (lp-core), and
       * the dimmed window behind it. */
      d.addEventListener('click', e => { if (e.target === d) LP.close(id); });
      d.querySelectorAll('[data-close]').forEach(b =>
        b.addEventListener('click', () => LP.close(id)));
    });

    document.getElementById('k-add-q')
      .addEventListener('input', e => fillCatalogue(e.target.value));

    document.getElementById('k-add-list').addEventListener('click', e => {
      const row = e.target.closest('[data-add]');
      if (row && !row.classList.contains('taken')) add(row.dataset.add);
    });

    document.getElementById('k-switch-list').addEventListener('click', e => {
      const row = e.target.closest('[data-sw]');
      if (!row) return;
      LP.switchKey = row.dataset.sw;
      LP.close('k-switch');
      LP.render();
    });

    document.getElementById('k-nodrop-demote').addEventListener('click', () => {
      const id = document.getElementById('k-nodrop').dataset.id;
      const i = LP.sources.findIndex(s => s.id === id);
      if (i < 0) return;
      const [s] = LP.sources.splice(i, 1);
      LP.sources.push(s);
      LP.close('k-nodrop');
      LP.render();
      /* Said out loud because the dialog has just closed over the list and
       * the pill that moved is not where the eye is. */
      LP.say(said(), '맨 아래로 내렸습니다. 이제 목록의 첫 번째는 ' + LP.sources[0].name + ' 입니다.');
    });

    add_._wired = true;
  }

})();