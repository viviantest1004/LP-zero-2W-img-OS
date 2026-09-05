/* s-perm — 사용자 › 권한. What the standard account may do.
 *
 * The screen is about the guest account and about nothing else. An
 * administrator can already change everything, so there is nothing here
 * to set for one, and the subtitle says that rather than leaving
 * somebody to work out why every switch on the screen reads as allowed.
 *
 * ── Three lists, and they are not three of the same thing ──
 *
 *   바꿀 수 있는 설정   whether a settings screen can be edited. Off
 *                       leaves the rows visible and locked.
 *   쓸 수 있는 기능     whether the thing runs at all. Off is not a
 *                       read-only screen, it is a program that does not
 *                       start.
 *   실행할 수 있는 앱   one named window that will not open.
 *
 * One heading over all three would be tidier and would bury the
 * difference that decides what actually happens when a switch goes off,
 * which is the only thing the person granting it came here to find out.
 *
 * ── Why every line says what stops ──
 *
 * "네트워크 — 네트워크 설정" is a row that has told the reader nothing.
 * A permission is worth granting only if the person granting it knows
 * what it takes away, so each description names the thing that stops
 * working rather than restating the name above it.
 *
 * ── The one that cannot be delegated ──
 *
 * 공장 초기화 sits in the list and is the only row with no switch. A
 * switch that cannot move is a lie about what the control does; a value
 * reading 관리자만 is not. LP.can('factory') is false for a standard
 * account whatever LP.allow holds, so the row draws a rule that lives in
 * lp-core.js instead of one this screen invented.
 */

(function () {
  'use strict';

  const PANE = 's-perm';
  const AREA = 'users';          /* who may open this screen at all */
  const PICK = 'dlg-perm-app';

  /* ── the settings a standard account may be given ───────────────
   *
   * In LP.allow's own order, which is the order the settings list has
   * them. The switch state is read from LP.allow on every render and
   * never from here - this table is wording and sequence, not state.
   *
   * 사용자 carries the sentence it does because it is the row that hands
   * over this screen: an account allowed 사용자 can open 권한 and grant
   * itself the other eleven. That is what the switch means, so it is on
   * the row rather than in a note underneath somebody may not read. */
  const AREAS = [
    { key: 'network',  name: '네트워크',
      off: '끄면 Wi-Fi 를 바꾸거나 새 네트워크에 접속하지 못합니다' },
    { key: 'display',  name: '화면',
      off: '끄면 해상도와 배율, 야간 모드가 지금 상태로 고정됩니다' },
    { key: 'sound',    name: '소리',
      off: '끄면 출력·입력 장치를 바꾸지 못합니다. 볼륨은 그대로 조절할 수 있습니다' },
    { key: 'power',    name: '전원',
      off: '끄면 전원 모드와 화면 끄기 시간을 바꾸지 못합니다' },
    { key: 'keyboard', name: '키보드',
      off: '끄면 입력 소스를 더하거나 단축키를 바꾸지 못합니다' },
    { key: 'look',     name: '모양',
      off: '끄면 배경 화면과 강조색, 어두운 화면을 바꾸지 못합니다' },
    { key: 'apps',     name: '앱',
      off: '끄면 기본 앱과 시작 프로그램, 앱별 권한을 바꾸지 못합니다' },
    { key: 'storage',  name: '저장 공간',
      off: '끄면 캐시를 지우거나 휴지통을 비우지 못합니다' },
    { key: 'users',    name: '사용자',
      off: '끄면 다른 계정을 만들거나 지우지 못합니다. 켜면 이 권한 화면도 함께 열립니다' },
    { key: 'privacy',  name: '개인 정보',
      off: '끄면 카메라와 마이크, 위치를 앱에 열어 주지 못합니다' },
    { key: 'datetime', name: '날짜와 시간',
      off: '끄면 시간대와 시계 형식을 바꾸지 못합니다. 시각은 계속 자동으로 맞춰집니다' },
    { key: 'reset',    name: '초기화',
      off: '끄면 설정을 항목별로 처음 값으로 되돌리지 못합니다' },
  ];

  /* ── the apps the picker offers ─────────────────────────────────
   *
   * Everyday apps first, the ones that can take the machine apart after
   * them, and 설정 last because it is the one nobody may block.
   *
   * There is no app list on LP. This is copy rather than state - nothing
   * else reads it and nothing writes it - and it sits beside the wording
   * it belongs to, the way 초기화 keeps its own table of what each reset
   * puts back.
   *
   * 설정 is fixed for the same reason lp-static.js keeps 비밀번호 변경
   * live inside a locked 사용자 screen: an account that cannot reach its
   * own password is an account whose owner ends up using the
   * administrator's, which is worse than whatever the block was for. */
  const APPS = [
    { name: '웹 브라우저' },
    { name: '파일 관리자' },
    { name: '텍스트 편집기' },
    { name: '사진' },
    { name: '음악' },
    { name: '계산기' },
    { name: '스크린샷' },
    { name: '터미널' },
    { name: '작업 관리자',     desc: '실행 중인 프로그램을 끝냅니다' },
    { name: '소프트웨어 센터', desc: '앱을 설치하고 지웁니다' },
    { name: '디스크 유틸리티', desc: '파티션을 만들고 지우고 포맷합니다' },
    { name: '설정', fixed: true,
      no: '이 권한을 정하는 앱이고, 표준 사용자가 자기 비밀번호를 바꾸는 곳입니다' },
  ];

  function appByName(name) {
    return APPS.filter(function (a) { return a.name === name; })[0];
  }

  /* ── drawing ────────────────────────────────────────────────────
   *
   * The whole row is the target, not the 34px switch. It is the shape
   * every other list in this app uses for a row that answers to a click,
   * and on twelve rows of one-line descriptions the difference between
   * hitting the switch and hitting the name is the difference between
   * granting 소리 and granting 전원. */
  function areaRow(a) {
    return '<div class="row click pressable" data-area="' + a.key + '">' +
        '<div class="lb"><b>' + a.name + '</b><i>' + a.off + '</i></div>' +
        '<div class="sw' + (LP.allow[a.key] ? ' on' : '') + '"></div>' +
      '</div>';
  }

  /* The reason is joined to LP.whyLocked's sentence rather than written
   * out again here. whyLocked already names who can, which is the half a
   * reader can act on; this adds the half that says not to come back and
   * ask - it is not a permission somebody forgot to grant. */
  function factoryRow() {
    return '<div class="row locked">' +
        '<div class="lb">' +
          '<b>공장 초기화<span class="lock">자물쇠</span></b>' +
          '<i>이 컴퓨터의 계정과 파일을 모두 지우고 처음 상태로 되돌립니다</i>' +
          '<div class="why">' + LP.whyLocked('factory') +
            ' · 되돌릴 수 없어서 다른 계정에 넘길 수 있는 항목이 아닙니다</div>' +
        '</div>' +
        '<div class="vl off">관리자만</div>' +
      '</div>';
  }

  function featureRow(key, f) {
    return '<div class="row click pressable" data-feature="' + LP.esc(key) + '">' +
        '<div class="lb"><b>' + LP.esc(f.name) + '</b>' +
          '<i>' + LP.esc(f.why) + '</i></div>' +
        '<div class="sw' + (f.on ? ' on' : '') + '"></div>' +
      '</div>';
  }

  function blockedRow(name) {
    const app = appByName(name);
    return '<div class="row" data-app="' + LP.esc(name) + '">' +
        '<div class="lb"><b>' + LP.esc(name) + '</b>' +
          (app && app.desc ? '<i>' + LP.esc(app.desc) + '</i>' : '') + '</div>' +
        '<button class="btn quiet pressable" data-unblock>해제</button>' +
      '</div>';
  }

  function subLine(may) {
    if (!may)
      return '표준 사용자가 무엇을 바꿀 수 있는지 관리자가 정하는 화면입니다';

    const feats = Object.keys(LP.features);
    const areasOn = AREAS.filter(function (a) { return LP.allow[a.key]; }).length;
    const featsOn = feats.filter(function (k) { return LP.features[k].on; }).length;

    return '표준 사용자 ' + LP.esc(LP.userName.std) + ' 가 바꿀 수 있는 것을 정합니다 · ' +
      '설정 ' + AREAS.length + '개 중 ' + areasOn + '개, ' +
      '기능 ' + feats.length + '개 중 ' + featsOn + '개 허용. ' +
      '관리자는 전부 바꿀 수 있어 여기서 정할 것이 없습니다.';
  }

  /* Named, because a bare 권한이 없습니다 leaves the reader with nothing
   * to do. The account is named rather than the role: 관리자에게
   * 요청하십시오 on a machine with one administrator is a sentence that
   * still has to be looked up. */
  function refusal() {
    const admin = LP.esc(LP.userName.admin);
    return '<div class="note"><b>관리자만 바꿀 수 있습니다.</b>&nbsp;' +
      '이 컴퓨터의 관리자는 ' + admin + ' 입니다. 열어야 할 항목이 있으면 ' +
      admin + ' 에게 요청하거나 관리자 계정으로 로그인하십시오.</div>';
  }

  function controls() {
    const feats = Object.keys(LP.features);
    const blocked = LP.blockedApps;

    let h = '';

    h += '<div class="sec">바꿀 수 있는 설정</div>';
    h += '<div class="rows">' +
      AREAS.map(areaRow).join('') + factoryRow() + '</div>';
    h += '<div class="inline" id="perm-say"></div>';

    /* Every entry LP.features holds, rather than a list written here.
     * A feature an administrator can take away and this screen does not
     * draw is one nobody can give back. */
    h += '<div class="sec">쓸 수 있는 기능</div>';
    h += '<div class="rows">' + feats.map(function (k) {
      return featureRow(k, LP.features[k]);
    }).join('') + '</div>';
    h += '<div class="caption">기능을 끄면 설정 화면이 잠기는 것이 아니라 ' +
      '그 프로그램이 시작되지 않습니다. 앱 하나를 막는 것과는 다릅니다 — ' +
      '앱을 막으면 그 창이 열리지 않고, 기능을 끄면 다른 앱을 거쳐 들어와도 막힙니다.</div>';

    h += '<div class="sec">실행할 수 있는 앱</div>';
    h += blocked.length
      ? '<div class="rows" id="perm-blocked">' +
          blocked.map(function (n) { return blockedRow(n); }).join('') + '</div>'
      : '<div class="caption">막아 둔 앱이 없습니다. 표준 사용자가 설치된 앱을 모두 실행합니다.</div>';
    /* An app that dies without a word is reported as a broken machine,
     * and the report goes to the person who blocked it. */
    h += '<div class="caption">막은 앱은 시작되지 않습니다. 열려고 하면 ' +
      '관리자가 막았다는 안내가 뜨고, 아무 반응 없이 사라지지는 않습니다.</div>';
    h += '<div class="btnrow"><button class="btn pressable" id="perm-block">앱 막기</button></div>';

    h += '<div class="caption">이 제한은 앱이 아니라 시스템이 지킵니다. ' +
      '표준 사용자가 다른 길로 터미널에 닿아도 거절당합니다 — ' +
      '검사하는 것은 어느 창이 요청했는지가 아니라 어느 계정이 요청했는지입니다.</div>';
    h += '<div class="caption">다만 이 컴퓨터를 직접 만질 수 있고 카드 리더가 있는 ' +
      '사람은 부팅 파티션을 고칠 수 있습니다. 이런 종류의 기계는 전부 그렇습니다.</div>';

    return h;
  }

  /* ── changing it ────────────────────────────────────────────────
   *
   * LP.render() rather than a class flipped on the switch that was
   * pressed. It redraws this pane from LP.allow and re-runs the sidebar
   * locks, so the count in the subtitle, the switch and the sidebar
   * cannot disagree about a value one of them changed. When the reviewer
   * is looking at this as the standard account - LP.allow.users on - the
   * sidebar lock on the section just withdrawn appears in the same
   * frame, which is the whole thing this screen is for. */
  function flipArea(key) {
    /* The screen having no switches for a standard account is the
     * drawing of the rule. This is the rule, asked again because a click
     * can arrive another way - a focused row and a stray Enter, or a
     * pane drawn before the account changed. */
    if (!LP.can(AREA)) return;

    const on = !LP.allow[key];
    LP.allow[key] = on;
    LP.render();

    /* Said after the redraw and not before it: the redraw replaces the
     * node the line is written into. Only this one row gets a sentence,
     * because it is the only switch here that hands over the screen the
     * switch is on. */
    if (key === 'users' && on)
      LP.say(document.getElementById('perm-say'),
             '이제 표준 사용자도 이 화면을 열어 권한을 바꿀 수 있습니다', 'warn');
  }

  function flipFeature(key) {
    if (!LP.can(AREA)) return;
    const f = LP.features[key];
    if (!f) return;
    f.on = !f.on;
    LP.render();
  }

  /* The row element rather than the name. An app is named 디스크
   * 유틸리티, spaces included, and building a selector out of it is a
   * quoting problem with no upside. */
  function unblock(row) {
    if (!LP.can(AREA)) return;
    const name = row.dataset.app;

    /* Taken off LP.blockedApps when the row has finished leaving, so the
     * list and the screen are never a frame apart. .row.leave also stops
     * the row answering a second press on its way out. */
    row.classList.add('leave');
    row.addEventListener('animationend', function () {
      const i = LP.blockedApps.indexOf(name);
      if (i >= 0) LP.blockedApps.splice(i, 1);
      LP.render();
    }, { once: true });
  }

  function block(name) {
    if (!LP.can(AREA)) return;
    const app = appByName(name);
    if (!app || app.fixed) return;
    if (LP.blockedApps.indexOf(name) >= 0) return;

    LP.blockedApps.push(name);
    LP.close(PICK);
    LP.render();

    /* Appended, so the row that arrives is the last one. Found that way
     * rather than by name for the same reason unblock takes an element. */
    const row = document.querySelector('#perm-blocked .row:last-child');
    if (row) row.classList.add('arrive');
  }

  /* ── the picker ─────────────────────────────────────────────────── */

  function openPicker() {
    if (!LP.can(AREA)) return;
    const q = document.getElementById('perm-app-q');
    /* Emptied on the way in. A dialog left by Escape keeps what was
     * typed, and the next person to open it would find the list already
     * filtered by somebody else's search. */
    q.value = '';
    fillPicker('');
    LP.open(PICK);
    q.focus();
  }

  function fillPicker(query) {
    const list = document.getElementById('perm-app-list');
    const term = query.trim().toLowerCase();
    const hits = APPS.filter(function (a) {
      return (a.name + ' ' + (a.desc || '')).toLowerCase().indexOf(term) >= 0;
    });

    list.innerHTML = hits.map(function (a) {
      /* Already blocked, and 설정, are shown and not offered. Dropping
       * them out of the list would read as "this machine does not have
       * that app", and the person would go looking for it. */
      const on = LP.blockedApps.indexOf(a.name) >= 0;
      const shut = on || a.fixed;
      const why = a.fixed ? a.no : a.desc;
      return '<div class="row' + (shut ? ' taken' : ' click pressable') + '"' +
          (shut ? '' : ' data-block="' + LP.esc(a.name) + '"') + '>' +
          '<div class="lb"><b>' + LP.esc(a.name) + '</b>' +
            (why ? '<i>' + LP.esc(why) + '</i>' : '') + '</div>' +
          '<div class="vl' + (shut ? ' off' : '') + '">' +
            (a.fixed ? '막을 수 없음' : on ? '막힘' : '막기') + '</div>' +
        '</div>';
    }).join('');

    document.getElementById('perm-app-none').hidden = hits.length > 0;
  }

  /* Wired once, at load. The dialog is static markup outside the pane, so
   * no render throws its nodes away - and wiring it from the renderer
   * would stack a second handler on it with every account switch.
   * Closing it is lp-boot's job: the button carries data-close, the
   * backdrop closes on itself, and Escape is lp-core's. */
  function wire() {
    document.getElementById('perm-app-q')
      .addEventListener('input', function (e) { fillPicker(e.target.value); });

    document.getElementById('perm-app-list')
      .addEventListener('click', function (e) {
        const row = e.target.closest('[data-block]');
        if (row) block(row.dataset.block);
      });
  }

  /* ── the pane ───────────────────────────────────────────────────── */

  function render(el) {
    const may = LP.can(AREA);

    el.querySelector('#perm-sub').textContent = subLine(may);
    el.querySelector('#perm-body').innerHTML = may ? controls() : refusal();

    /* Assigned rather than added. This runs again on every account
     * switch and every flip, and a listener added each time is one that
     * fires four times on the fourth visit.
     *
     * The way back is outside the branch: a screen somebody may not use
     * still has to be one they can leave. */
    el.onclick = function (e) {
      if (e.target.closest('.back')) { LP.go('s-user'); return; }

      const area = e.target.closest('[data-area]');
      if (area) { flipArea(area.dataset.area); return; }

      const feat = e.target.closest('[data-feature]');
      if (feat) { flipFeature(feat.dataset.feature); return; }

      const un = e.target.closest('[data-unblock]');
      if (un) { unblock(un.closest('[data-app]')); return; }

      if (e.target.closest('#perm-block')) openPicker();
    };
  }

  wire();
  LP.panes[PANE] = render;

})();