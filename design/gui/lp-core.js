/* lp-core.js - the state the settings mockup is drawn from.
 *
 * This is a mockup, not the settings app. Nothing here talks to a
 * machine. What it does do is keep one copy of the state that the
 * screens disagree about if each holds its own: which input sources
 * exist, which shortcuts are taken, and what the account signed in may
 * change. Every screen reads this and re-renders when it changes, which
 * is why switching between 관리자 and 표준 at the top of the page really
 * does lock the rows rather than showing a second drawing of them.
 *
 * The rule the permission model follows, and the reason it is here
 * rather than in each screen: a restricted setting must look restricted
 * everywhere it appears. A lock drawn only on the screen somebody
 * remembered to draw it on is worse than no lock, because it teaches
 * that the absence of one means permission.
 */

const LP = {

  /* ── who is signed in ──────────────────────────────────────────
   *
   * Two account types, as apps-and-settings.md §5-12 has it. The
   * mockup switches between them; a real machine gets this from the
   * account that logged in. */
  account: 'admin',          /* 'admin' | 'std' */
  userName: { admin: 'cho', std: 'guest' },

  /* ── input sources ────────────────────────────────────────────
   *
   * An ordered list. The first is the one a new window starts in.
   *
   * `fixed` is the whole reason this is a list of objects rather than
   * a list of names. English (US) cannot be removed: it is the layout
   * every key label on the physical keyboard is printed with, the one
   * the password field is typed in before any IME is running, and the
   * one a recovery console falls back to. A machine whose only input
   * source is Hangul is a machine you cannot type `fsck` into. */
  sources: [
    { id: 'ko', name: '한국어',      desc: '두벌식',  fixed: false },
    { id: 'en', name: 'English (US)', desc: 'QWERTY', fixed: true  },
  ],

  /* Everything that can be added, for the picker. */
  catalogue: [
    { id: 'ko',    name: '한국어',        desc: '두벌식' },
    { id: 'ko3',   name: '한국어',        desc: '세벌식 390' },
    { id: 'en',    name: 'English (US)',  desc: 'QWERTY' },
    { id: 'endv',  name: 'English (US)',  desc: 'Dvorak' },
    { id: 'engb',  name: 'English (UK)',  desc: 'QWERTY' },
    { id: 'ja',    name: '日本語',         desc: 'Anthy' },
    { id: 'zh',    name: '中文',           desc: '拼音' },
    { id: 'de',    name: 'Deutsch',       desc: 'QWERTZ' },
    { id: 'fr',    name: 'Français',      desc: 'AZERTY' },
    { id: 'es',    name: 'Español',       desc: 'QWERTY' },
    { id: 'ru',    name: 'Русский',       desc: 'ЙЦУКЕН' },
    { id: 'vi',    name: 'Tiếng Việt',    desc: 'Telex' },
    { id: 'ar',    name: 'العربية',        desc: 'AZERTY' },
    { id: 'compose', name: 'Compose',     desc: '기호 조합' },
  ],

  /* How the sources are cycled. */
  switchKey: 'hangul',       /* 'hangul' | 'shiftspace' | 'altspace' | 'custom' */
  switchCustom: ['Super', 'Space'],

  /* ── shortcuts ────────────────────────────────────────────────
   *
   * `reserved` marks a combination the compositor answers before any
   * window sees it, and which therefore cannot be given away. */
  shortcuts: [
    { id: 'search',   grp: '셸',      name: '검색 열기',        keys: ['Ctrl','Space'] },
    { id: 'ws',       grp: '셸',      name: '작업공간 전환',     keys: ['Super','1–9'] },
    { id: 'overview', grp: '셸',      name: '작업공간 개요',     keys: ['Super','Tab'] },
    { id: 'lock',     grp: '셸',      name: '화면 잠금',        keys: ['Super','L'] },
    { id: 'power',    grp: '셸',      name: '전원 메뉴',        keys: ['Super','Escape'] },
    { id: 'quick',    grp: '셸',      name: '퀵메뉴 열기',      keys: ['Super','A'] },

    { id: 'winswitch',grp: '창',      name: '창 전환',          keys: ['Alt','Tab'] },
    { id: 'winclose', grp: '창',      name: '창 닫기',          keys: ['Super','Q'] },
    { id: 'winmax',   grp: '창',      name: '최대화',           keys: ['Super','Up'] },
    { id: 'winleft',  grp: '창',      name: '왼쪽 반쪽',        keys: ['Super','Left'] },
    { id: 'winright', grp: '창',      name: '오른쪽 반쪽',      keys: ['Super','Right'] },

    { id: 'term',     grp: '앱',      name: '터미널 열기',      keys: ['Super','T'] },
    { id: 'files',    grp: '앱',      name: '파일 관리자 열기', keys: ['Super','E'] },
    { id: 'settings', grp: '앱',      name: '설정 열기',        keys: ['Super','I'] },

    { id: 'shotall',  grp: '화면',    name: '전체 스크린샷',     keys: ['Print'] },
    { id: 'shotarea', grp: '화면',    name: '영역 스크린샷',     keys: ['Shift','Print'] },
    { id: 'shotwin',  grp: '화면',    name: '창 스크린샷',      keys: ['Alt','Print'] },
    { id: 'record',   grp: '화면',    name: '화면 녹화',        keys: ['Ctrl','Shift','R'] },

    { id: 'ime',      grp: '입력',    name: '입력 소스 전환',    keys: ['한/영'], reserved: true },
    { id: 'volup',    grp: '입력',    name: '볼륨 올리기',      keys: ['XF86AudioRaiseVolume'] },
    { id: 'voldown',  grp: '입력',    name: '볼륨 내리기',      keys: ['XF86AudioLowerVolume'] },
  ],

  /* Combinations the compositor keeps for itself. Handing one of these
   * to a user shortcut would make the machine unrecoverable from its
   * own keyboard, so the capture dialog refuses them by name. */
  reserved: [
    { keys: ['Ctrl','Alt','F1'],       why: '가상 터미널 1' },
    { keys: ['Ctrl','Alt','F2'],       why: '가상 터미널 2' },
    { keys: ['Ctrl','Alt','Backspace'],why: '셸 강제 재시작' },
    { keys: ['Ctrl','Alt','Delete'],   why: '전원 메뉴' },
  ],

  /* ── what a standard account may do ───────────────────────────
   *
   * Set by an administrator on 사용자 › 권한. true means the standard
   * account may change it. The defaults are the ones a machine ships
   * with: a standard account can set up its own screen, sound and
   * shortcuts, and cannot touch anything that decides what the machine
   * is or who else may use it. */
  allow: {
    network:   false,   /* Wi-Fi 접속·삭제, 고정 IP, 프록시 */
    display:   true,
    sound:     true,
    power:     true,
    keyboard:  true,    /* 자기 입력 소스와 단축키 */
    look:      true,
    apps:      false,   /* 기본 앱, 설치·삭제 */
    storage:   false,
    users:     false,
    privacy:   false,
    datetime:  false,
    reset:     false,   /* 항목별 초기화 */
    factory:   false,   /* 언제나 false. 관리자만. */
  },

  /* Features an administrator can take away from a standard account
   * entirely, rather than merely making read-only. */
  features: {
    terminal:  { on: true,  name: '터미널',        why: '명령을 직접 실행합니다' },
    install:   { name: '앱 설치', on: false, why: 'apt 와 소프트웨어 센터로 앱을 추가합니다' },
    usb:       { on: true,  name: 'USB 저장장치',  why: '연결한 드라이브를 읽고 씁니다' },
    net:       { on: true,  name: '네트워크 설정', why: 'Wi-Fi 를 바꾸고 새 네트워크에 접속합니다' },
    dev:       { on: false, name: '개발자 도구',   why: '컴파일러와 디버거를 씁니다' },
  },

  /* Apps a standard account may not start. */
  blockedApps: ['터미널', '디스크 유틸리티'],

  /* ── permission helpers ───────────────────────────────────────
   *
   * One question, asked the same way everywhere: may the account signed
   * in change this? The screens never test `LP.account` themselves -
   * that is how a rule ends up implemented five ways and enforced in
   * four of them. */
  can(area) {
    if (this.account === 'admin') return true;
    if (area === 'factory') return false;
    return !!this.allow[area];
  },

  /* What to say on a row that is locked. Never a bare "권한이 없습니다":
   * it has to name who can, or the person is left with nothing to do. */
  whyLocked(area) {
    if (area === 'factory')
      return '공장 초기화는 관리자만 할 수 있습니다';
    return '관리자만 바꿀 수 있습니다';
  },

  /* ── shortcut helpers ─────────────────────────────────────────── */

  /* Same combination? Order of modifiers must not matter: Ctrl+Shift+R
   * and Shift+Ctrl+R are one shortcut, and a conflict check that misses
   * that is a conflict check that lets two things answer one key. */
  sameKeys(a, b) {
    if (!a || !b || a.length !== b.length) return false;
    const norm = k => k.slice().sort().join('+').toLowerCase();
    return norm(a) === norm(b);
  },

  /* What already holds this combination, or null. */
  heldBy(keys, exceptId) {
    for (const r of this.reserved)
      if (this.sameKeys(r.keys, keys))
        return { name: r.why, system: true };
    for (const s of this.shortcuts)
      if (s.id !== exceptId && this.sameKeys(s.keys, keys))
        return { name: s.name, id: s.id, system: !!s.reserved };
    return null;
  },

  keyText(keys) {
    return keys.map(k => `<span class="key">${LP.esc(k)}</span>`).join('<em>+</em>');
  },

  /* ── input source helpers ─────────────────────────────────────── */

  /* Removing the last non-fixed source is allowed; removing a fixed one
   * is not. The refusal explains rather than greying the control out,
   * because a person who wants English gone is going to keep trying
   * until they are told why it will not go. */
  canRemoveSource(id) {
    const s = this.sources.find(x => x.id === id);
    if (!s) return { ok: false, why: '없는 입력 소스입니다' };
    if (s.fixed)
      return {
        ok: false,
        why: 'English (US) 는 지울 수 없습니다',
        detail: '키보드에 인쇄된 글자, 로그인 화면의 비밀번호 입력, ' +
                '복구 콘솔이 모두 이 배열을 씁니다. 순서는 바꿀 수 있습니다.',
      };
    if (this.sources.length <= 1)
      return { ok: false, why: '입력 소스는 하나 이상 있어야 합니다' };
    return { ok: true };
  },

  /* ── small utilities ──────────────────────────────────────────── */

  esc(s) {
    return String(s).replace(/[&<>"]/g, c =>
      ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  },

  /* Inline feedback: next to the control that caused it, gone on its
   * own. shell-design-spec.md §5-1 - the position is what says where it
   * came from, so this never floats. */
  say(el, msg, kind) {
    if (!el) return;
    el.className = 'inline on' + (kind ? ' ' + kind : '');
    el.textContent = msg;
    clearTimeout(el._t);
    el._t = setTimeout(() => { el.className = 'inline'; }, 4000);
  },

  /* ── panes ────────────────────────────────────────────────────
   *
   * Each screen registers a function that fills its own pane. render()
   * runs the visible one. A screen that draws once and never again is a
   * screen that still shows the administrator's view after the account
   * switch. */
  panes: {},
  current: 's-kbd',

  go(id) {
    this.current = id;
    document.querySelectorAll('.pane').forEach(p => p.classList.remove('on'));
    const el = document.getElementById(id);
    if (el) el.classList.add('on');
    document.querySelectorAll('.side a').forEach(a =>
      a.classList.toggle('on', a.dataset.pane === id));
    this.render();
    const main = document.querySelector('.main');
    if (main) main.scrollTop = 0;
  },

  render() {
    const fn = this.panes[this.current];
    if (fn) fn(document.getElementById(this.current));
    this.renderSidebar();
  },

  /* The sidebar carries the same lock the rows do, so a restricted
   * section is visible as restricted before it is opened. */
  sidebarAreas: {
    's-net': 'network', 's-disp': 'display', 's-snd': 'sound',
    's-pow': 'power', 's-kbd': 'keyboard', 's-keys': 'keyboard',
    's-look': 'look', 's-apps': 'apps', 's-store': 'storage',
    's-user': 'users', 's-priv': 'privacy', 's-reset': 'reset',
  },

  renderSidebar() {
    document.querySelectorAll('.side a').forEach(a => {
      const area = this.sidebarAreas[a.dataset.pane];
      const old = a.querySelector('.sidelock');
      if (old) old.remove();
      if (area && !this.can(area)) {
        const b = document.createElement('span');
        b.className = 'sidelock';
        b.textContent = '자물쇠';
        a.appendChild(b);
      }
    });
    const who = document.getElementById('who');
    if (who)
      who.textContent = this.userName[this.account] +
        (this.account === 'admin' ? ' · 관리자' : ' · 표준 사용자');
  },

  /* ── dialogs ──────────────────────────────────────────────────
   *
   * Inside the window, never over the page. The backdrop dims the app
   * that raised it, which is what says which app is asking. */
  open(id)  { const e = document.getElementById(id); if (e) e.classList.add('on'); },
  close(id) { const e = document.getElementById(id); if (e) e.classList.remove('on'); },
  closeAll() { document.querySelectorAll('.backdrop').forEach(b => b.classList.remove('on')); },
};

/* Escape closes the topmost dialog. Every dialog in this mockup can be
 * left without answering, including the factory reset one - a
 * confirmation you cannot back out of is a trap, not a confirmation. */
document.addEventListener('keydown', e => {
  if (e.key === 'Escape' && !window.LP_CAPTURING) LP.closeAll();
});
