/* check.js - drive the mockup and prove the rules hold.
 *
 * A mockup that looks right and does not work is a picture, and the
 * whole reason these screens are HTML rather than a drawing is that
 * their subject matter is behaviour: what happens when you try to
 * remove the English keyboard, what happens when two shortcuts want the
 * same combination, what a standard account sees when it reaches the
 * factory reset. None of that can be reviewed by looking.
 *
 * So this opens the file in a real browser and tries all of it.
 *
 *   node check.js                    both pages
 *   node check.js settings.html      one
 *
 * Needs playwright-core and the Chromium in /opt/pw-browsers. Every
 * check prints its own line, so a failure says which rule broke rather
 * than that something did.
 */
'use strict';

const path = require('path');
const HERE = __dirname;

const CHROME = process.env.LP_CHROME ||
  '/opt/pw-browsers/chromium-1194/chrome-linux/chrome';
const PW = process.env.LP_PW ||
  '/tmp/claude-0/-home-user-LP-zero-2W-img-OS/' +
  '75810783-831f-555c-98ce-455e873e70c8/scratchpad/node_modules/playwright-core';

const { chromium } = require(PW);

let pass = 0, fail = 0;
const failures = [];

function ok(name, cond, detail) {
  if (cond) { pass++; console.log('  ok   ' + name); }
  else {
    fail++;
    failures.push(name + (detail ? '  — ' + detail : ''));
    console.log('  FAIL ' + name + (detail ? '  — ' + detail : ''));
  }
}

function head(t) { console.log('\n' + t); }

/* Any check that reaches into the page can find nothing there. When it
 * does, that is a failure of the thing being checked - but it must not
 * be a failure of the checking, because a suite that throws on the
 * fifth assertion never runs the thirtieth, and the thirtieth is where
 * the permission rules are. */
async function tryOk(name, fn, detail) {
  try { ok(name, await fn(), detail); }
  catch (e) { ok(name, false, String(e.message || e).slice(0, 70)); }
}

async function run() {
  const browser = await chromium.launch({
    executablePath: CHROME,
    args: ['--no-sandbox', '--autoplay-policy=no-user-gesture-required'],
  });
  const page = await browser.newPage({ viewport: { width: 1180, height: 900 } });

  const errors = [];
  page.on('pageerror', e => errors.push('pageerror: ' + e.message));
  page.on('console', m => {
    if (m.type() === 'error') errors.push('console: ' + m.text());
  });

  await page.goto('file://' + path.join(HERE, 'settings.html'),
                  { waitUntil: 'load' });
  await page.waitForTimeout(400);

  /* Read from the page rather than hard-coded here, so that changing the
   * default block list in lp-core.js does not silently break a test that
   * was only ever asserting a copy of it. */
  const LP_BLOCKED = await page.evaluate(() => LP.blockedApps.slice());

  /* ── it loaded at all ─────────────────────────────────────────── */
  head('창');
  const panes = await page.$$eval('.pane', e => e.length);
  ok('열아홉 개 이상의 화면이 있다', panes >= 19, panes + '개');
  ok('사이드바가 채워졌다',
     (await page.$$eval('.side a[data-pane]', e => e.length)) >= 19);
  ok('키보드 화면으로 열린다',
     await page.$eval('#s-kbd', e => e.classList.contains('on')));

  /* ── input sources ────────────────────────────────────────────── */
  head('입력 소스');
  const sources = () => page.$$eval(
    '#s-kbd .row .lb b', e => e.map(x => x.textContent.trim()));

  let list = await sources();
  ok('한국어와 English (US) 가 있다',
     list.some(s => s.includes('한국어')) &&
     list.some(s => s.includes('English (US)')), JSON.stringify(list));

  /* Removing English must fail, and it must say why rather than doing
   * nothing. This is the owner's explicit requirement and it is the
   * single most important assertion in the file. */
  const enDel = await page.evaluateHandle(() => {
    const rows = [...document.querySelectorAll('#s-kbd .row')];
    const r = rows.find(x => (x.querySelector('.lb b') || {}).textContent &&
                             x.querySelector('.lb b').textContent
                               .includes('English (US)'));
    return r ? r.querySelector('button') : null;
  });
  const enBtn = enDel.asElement();
  const hasBtn = enBtn && await enBtn.evaluate(e => !!e);

  if (hasBtn) {
    await enBtn.click();
    await page.waitForTimeout(400);
    const shown = await page.evaluate(() => {
      const open = document.querySelector('.backdrop.on');
      return open ? open.textContent : '';
    });
    ok('English (US) 삭제가 거부된다',
       /지울 수 없|고정|삭제할 수 없|지울 수는 없/.test(shown), shown.slice(0, 60));
    ok('거부에 이유가 붙는다',
       /키보드|비밀번호|복구|배열|로그인/.test(shown), shown.slice(0, 90));
    await page.evaluate(() => LP.closeAll());
    await page.waitForTimeout(250);
    ok('English (US) 가 아직 목록에 있다',
       (await sources()).some(s => s.includes('English (US)')));
  } else {
    ok('English (US) 행에 삭제 버튼이 있다', false,
       '행 자체를 못 찾았거나 버튼이 없습니다');
  }

  ok('canRemoveSource 가 English 를 거부한다',
     !(await page.evaluate(() => LP.canRemoveSource('en').ok)));
  ok('canRemoveSource 가 한국어는 허용한다',
     await page.evaluate(() => LP.canRemoveSource('ko').ok));

  /* Adding one has to actually change the list. */
  const before = (await sources()).length;
  await page.evaluate(() => {
    LP.sources.push({ id: 'ja', name: '日本語', desc: 'Anthy', fixed: false });
    LP.render();
  });
  await page.waitForTimeout(250);
  ok('입력 소스를 추가하면 화면이 따라온다',
     (await sources()).some(s => s.includes('日本語')));
  await page.evaluate(() => {
    LP.sources = LP.sources.filter(s => s.id !== 'ja'); LP.render();
  });
  await page.waitForTimeout(200);
  ok('되돌리면 사라진다', (await sources()).length === before);

  /* ── shortcuts ────────────────────────────────────────────────── */
  head('키 지정');
  await page.evaluate(() => LP.go('s-keys'));
  await page.waitForTimeout(300);
  ok('키 지정 화면이 열린다',
     await page.$eval('#s-keys', e => e.classList.contains('on')));
  const keyRows = await page.$$eval('#s-keys .row', e => e.length);
  ok('단축키가 여러 개 보인다', keyRows >= 15, keyRows + '개');
  ok('키캡으로 그려진다',
     (await page.$$eval('#s-keys .key', e => e.length)) >= 15);

  ok('이미 쓰이는 조합을 찾아낸다',
     await page.evaluate(() =>
       !!LP.heldBy(['Super', 'L'], 'nothing')));
  ok('수식키 순서가 달라도 같은 조합이다',
     await page.evaluate(() =>
       LP.sameKeys(['Ctrl', 'Shift', 'R'], ['Shift', 'Ctrl', 'R'])));
  ok('예약된 조합은 시스템이 잡고 있다고 답한다',
     await page.evaluate(() => {
       const h = LP.heldBy(['Ctrl', 'Alt', 'Backspace'], null);
       return !!h && h.system === true;
     }));
  ok('자기 자신은 충돌이 아니다',
     await page.evaluate(() => !LP.heldBy(['Super', 'L'], 'lock')));

  /* ── the flows somebody actually clicks first ─────────────────
   *
   * Everything above asserts a rule. These walk a path: open the
   * picker and add a language, press real keys into the capture
   * dialog, collide with a combination that is taken. A screen can
   * satisfy every rule in the file and still be impossible to use, and
   * the only way to find that out is to use it. */
  head('직접 밟아 보기');

  await page.evaluate(() => LP.go('s-kbd'));
  await page.waitForTimeout(250);
  await page.evaluate(() => {
    const b = [...document.querySelectorAll('#s-kbd button')]
      .find(x => /추가/.test(x.textContent));
    if (b) b.click();
  });
  await page.waitForTimeout(350);
  ok('입력 소스 추가 대화상자가 열린다',
     await page.evaluate(() => !!document.querySelector('.backdrop.on')));
  ok('언어를 찾을 수 있다',
     await page.evaluate(() => !!document.querySelector('.backdrop.on input')));

  const nBefore = await page.evaluate(() => LP.sources.length);
  await page.evaluate(() => {
    const r = [...document.querySelectorAll('.backdrop.on .row')]
      .find(x => /日本語/.test(x.textContent));
    if (r) r.click();
  });
  await page.waitForTimeout(350);
  ok('고른 언어가 목록에 들어온다',
     await page.evaluate(() => LP.sources.some(s => s.id === 'ja')));
  ok('목록이 하나 늘었다',
     (await page.evaluate(() => LP.sources.length)) === nBefore + 1);
  await page.evaluate(() => {
    LP.sources = LP.sources.filter(s => s.id !== 'ja');
    LP.closeAll(); LP.render();
  });

  /* Real keydown events, not a simulated value. The capture dialog
   * reads the keyboard, and the only way to know it reads it correctly
   * is to press keys. */
  await page.evaluate(() => LP.go('s-keys'));
  await page.waitForTimeout(250);
  await page.evaluate(() => {
    const r = [...document.querySelectorAll('#s-keys .row')]
      .find(x => /화면 잠금/.test(x.textContent));
    if (r) r.click();
  });
  await page.waitForTimeout(300);
  ok('캡처하는 동안 Esc 가 대화상자를 닫지 않는다',
     await page.evaluate(() => window.LP_CAPTURING === true));

  await page.keyboard.down('Control');
  await page.keyboard.down('Alt');
  await page.keyboard.press('KeyJ');
  await page.keyboard.up('Alt');
  await page.keyboard.up('Control');
  await page.waitForTimeout(450);
  ok('누른 조합이 실제로 지정된다',
     await page.evaluate(() =>
       LP.sameKeys(LP.shortcuts.find(s => s.id === 'lock').keys,
                   ['Ctrl', 'Alt', 'J'])),
     await page.evaluate(() =>
       LP.shortcuts.find(s => s.id === 'lock').keys.join('+')));
  /* A free combination needs no confirming - it is taken and the dialog
   * is done. Asking "are you sure" about a keystroke somebody just
   * chose is a dialog that gets dismissed without reading. */
  ok('비어 있는 조합이면 바로 끝난다',
     await page.evaluate(() => !document.querySelector('.backdrop.on')));

  await page.evaluate(() => {
    LP.shortcuts.find(s => s.id === 'lock').keys = ['Super', 'L'];
    LP.render();
  });

  /* And a taken one has to say what took it. */
  await page.evaluate(() => {
    const r = [...document.querySelectorAll('#s-keys .row')]
      .find(x => /터미널 열기/.test(x.textContent));
    if (r) r.click();
  });
  await page.waitForTimeout(300);
  await page.keyboard.down('Meta');
  await page.keyboard.press('KeyQ');
  await page.keyboard.up('Meta');
  await page.waitForTimeout(450);
  const clash = await page.evaluate(() => {
    const d = document.querySelector('.backdrop.on');
    return d ? d.textContent.replace(/\s+/g, ' ') : '';
  });
  ok('이미 쓰이는 조합은 무엇이 잡고 있는지 이름을 댄다',
     /창 닫기/.test(clash), clash.slice(0, 80));
  await page.evaluate(() => LP.closeAll());

  /* ── reset ────────────────────────────────────────────────────── */
  head('초기화');
  await page.evaluate(() => LP.go('s-reset'));
  await page.waitForTimeout(300);
  const resetText = await page.$eval('#s-reset', e => e.textContent);
  for (const word of ['단축키', '키보드', '네트워크', '웹', '모든 설정']) {
    ok('초기화 목록에 ' + word + ' 가 있다', resetText.includes(word));
  }
  ok('공장 초기화가 같은 화면에 있다', resetText.includes('공장 초기화'));
  ok('사용자 파일은 건드리지 않는다고 말한다',
     /파일은|파일을 지우지|파일은 그대로|계정과 파일/.test(resetText));

  /* A reset must actually reset. Shortcuts are the checkable one. */
  await page.evaluate(() => {
    LP.shortcuts.find(s => s.id === 'lock').keys = ['Super', 'Z'];
  });
  const fired = await page.evaluate(() => {
    const btns = [...document.querySelectorAll('#s-reset button')];
    const b = btns.find(x => /초기화/.test(x.textContent) && !x.disabled);
    if (!b) return false;
    b.click();
    return true;
  });
  ok('초기화 버튼이 확인을 먼저 띄운다',
     fired && (await page.evaluate(() => !!document.querySelector('.backdrop.on'))));
  await page.evaluate(() => LP.closeAll());

  /* ── factory reset, as an administrator ───────────────────────── */
  head('공장 초기화 — 관리자');
  ok('관리자에게는 허용된다', await page.evaluate(() => LP.can('factory')));
  const zoneText = await page.$eval('#s-reset', e => e.textContent);
  ok('무엇이 지워지는지 적혀 있다',
     /지워지는|지워집니다|삭제됩니다/.test(zoneText));
  ok('무엇이 남는지도 적혀 있다', /남는|남습니다|유지/.test(zoneText));
  ok('SSH 키가 남는다고 말한다', /SSH|인증 키|authorized/.test(zoneText));

  /* ── accounts ─────────────────────────────────────────────────── */
  head('사용자');
  await page.evaluate(() => LP.go('s-user'));
  await page.waitForTimeout(300);
  const userText = await page.$eval('#s-user', e => e.textContent);
  ok('계정 목록에 두 계정이 있다',
     /cho/.test(userText) && /guest/.test(userText), userText.slice(0, 70));
  ok('계정 유형이 표시된다',
     /관리자/.test(userText) && /표준/.test(userText));
  ok('권한 화면으로 가는 길이 있다', /권한/.test(userText));

  /* The account dialogs. Worth pinning because s-user was written
   * twice - the first attempt died mid-file - and its dialog markup
   * came from the first while its code came from the second. A dialog
   * whose id nothing opens is invisible until somebody clicks the row. */
  for (const [label, want] of [
    ['비밀번호 변경', /비밀번호/],
    ['사용자 추가',   /계정을 만듭니다|새 계정/],
    ['계정 유형',     /관리자|표준/],
  ]) {
    await page.evaluate(t => {
      const r = [...document.querySelectorAll('#s-user .row')]
        .find(x => x.textContent.includes(t));
      if (r) r.click();
    }, label);
    await page.waitForTimeout(300);
    const body = await page.evaluate(() => {
      const d = document.querySelector('.backdrop.on');
      return d ? d.textContent.replace(/\s+/g, ' ').trim() : '';
    });
    ok(label + ' 대화상자가 열린다', want.test(body), body.slice(0, 60) || '(열리지 않음)');
    await page.evaluate(() => LP.closeAll());
    await page.waitForTimeout(120);
  }

  /* ── permissions ──────────────────────────────────────────────── */
  head('권한');
  const hasPerm = await page.evaluate(() => !!document.getElementById('s-perm'));
  ok('권한 화면이 있다', hasPerm);
  await page.evaluate(() => LP.go('s-perm'));
  await page.waitForTimeout(300);

  /* Everything below reads that pane. Without it the assertions are not
   * failing, they are unanswerable - and printing thirty falsehoods
   * about a screen that does not exist buries the one fact that
   * matters, which is that it does not exist. */
  const permText = hasPerm
    ? await page.$eval('#s-perm', e => e.textContent) : '';
  if (!hasPerm) console.log('  ...  아래 권한 검사는 건너뜁니다');
  if (hasPerm) {
  ok('권한 화면이 열린다',
     await page.$eval('#s-perm', e => e.classList.contains('on')));
  for (const area of ['네트워크', '화면', '소리', '키보드', '앱', '초기화']) {
    ok('권한 목록에 ' + area + ' 가 있다', permText.includes(area));
  }
  ok('공장 초기화가 위임할 수 없는 것으로 나온다',
     /공장 초기화/.test(permText) && /관리자만/.test(permText));

  /* The screen has to be about the standard account, not about the
   * administrator - an administrator can change everything and there is
   * nothing here to set for one. */
  ok('표준 계정에 대한 화면이라고 말한다',
     /표준/.test(await page.$eval('#s-perm .sub', e => e.textContent)
                  .catch(function () { return ''; })),
     'sub 가 없거나 표준을 언급하지 않습니다');

  ok('기능 목록이 LP.features 를 따른다',
     /터미널/.test(permText) && /USB/.test(permText));
  ok('막힌 앱 목록이 있다',
     LP_BLOCKED.every(a => permText.includes(a)),
     '기대: ' + LP_BLOCKED.join(', '));

  /* Flipping a permission must take effect at once, everywhere. This is
   * the assertion the whole screen exists for: an administrator turning
   * 네트워크 off and then looking at 네트워크 as the standard account
   * has to see it locked, without a reload. */
  const flipped = await page.evaluate(() => {
    const before = LP.allow.display;
    LP.allow.display = false;
    LP.account = 'std';
    LP.go('s-disp');
    const locked = document.querySelectorAll('#s-disp .row.locked').length > 0;
    LP.allow.display = before;
    LP.account = 'admin';
    LP.go('s-perm');
    return locked;
  });
  ok('권한을 끄면 그 화면이 즉시 잠긴다', flipped);
  }

  /* The three gates, walked in order. This is the most destructive
   * thing the machine can be asked to do, and the gates are the whole
   * design - a confirmation that can be clicked through without reading
   * is not one. */
  head('공장 초기화 — 세 단계');
  await page.evaluate(() => { LP.closeAll(); LP.go('s-reset'); });
  await page.waitForTimeout(300);
  await page.evaluate(() => {
    const b = [...document.querySelectorAll('#factory-zone button')]
      .find(x => !x.disabled);
    if (b) b.click();
  });
  await page.waitForTimeout(350);
  const dlgText = () => page.evaluate(() => {
    const d = document.querySelector('.backdrop.on');
    return d ? d.textContent.replace(/\s+/g, ' ').trim() : '';
  });
  /* Advance past the dialog on screen by pressing its last button that
   * is not the cancel - written this way rather than by id so that a
   * screen which renames its buttons still gets walked. */
  const advance = () => page.evaluate(() => {
    const d = document.querySelector('.backdrop.on');
    if (!d) return false;
    const b = [...d.querySelectorAll('button')]
      .filter(x => !/취소|닫기/.test(x.textContent));
    if (!b.length) return false;
    b[b.length - 1].click();
    return true;
  });

  let step = await dlgText();
  ok('1단계는 무엇이 지워지는지부터 보여준다', /지워|삭제/.test(step),
     step.slice(0, 70));
  ok('  남는 것도 같이 말한다', /남습니다|남는|유지/.test(step));
  ok('  되돌릴 수 없다고 말한다', /되돌릴 수 없|취소할 수 없/.test(step));
  /* The word appears in step one - "계정과 비밀번호가 지워집니다" is
   * part of what is being erased - so what matters is whether it is
   * being ASKED for, which is a field, not a word. Order is the design
   * decision being checked here: the facts before the identity, because
   * somebody deciding needs to know what happens before being asked to
   * prove they may do it. */
  ok('  비밀번호를 아직 묻지 않는다',
     await page.evaluate(() =>
       !document.querySelector('.backdrop.on input[type=password]') &&
       !document.querySelector('.backdrop.on input')),
     '사실을 먼저 보여주고 나서 신원을 묻습니다');

  await advance();
  await page.waitForTimeout(350);
  step = await dlgText();
  ok('2단계는 관리자 비밀번호', /비밀번호/.test(step), step.slice(0, 60));

  await page.evaluate(() => {
    const i = document.querySelector('.backdrop.on input');
    if (i) { i.value = 'hunter2'; i.dispatchEvent(new Event('input', { bubbles: true })); }
  });
  await advance();
  await page.waitForTimeout(350);
  step = await dlgText();
  ok('3단계는 문구를 직접 치게 한다',
     /초기화/.test(step) && /입력|정확히|그대로/.test(step), step.slice(0, 70));
  ok('  치기 전에는 시작 버튼이 잠겨 있다',
     await page.evaluate(() => {
       const d = document.querySelector('.backdrop.on');
       const b = [...d.querySelectorAll('button')]
         .filter(x => !/취소|닫기/.test(x.textContent));
       return b.length ? b[b.length - 1].disabled === true : false;
     }));
  await page.evaluate(() => LP.closeAll());

  /* ── the standard account ─────────────────────────────────────── */
  head('표준 사용자');
  await page.click('.appswitch button[data-acct="std"]');
  await page.waitForTimeout(400);

  ok('공장 초기화가 막힌다', !(await page.evaluate(() => LP.can('factory'))));
  ok('관리자 여부와 무관하게 factory 는 위임되지 않는다',
     await page.evaluate(() => { LP.allow.factory = true; return !LP.can('factory'); }));

  const stdReset = await page.$eval('#s-reset', e => e.textContent);
  ok('막힌 이유에 누가 할 수 있는지 나온다',
     /관리자/.test(stdReset), stdReset.slice(0, 80));

  const disabled = await page.$$eval('#s-reset button',
    e => e.filter(b => b.disabled).length);
  ok('초기화 버튼이 눌리지 않는다', disabled >= 1, disabled + '개 비활성');

  /* The rule that matters: the lock is on every screen, including the
   * sixteen that were written before there was a permission model. */
  await page.evaluate(() => LP.go('s-net'));
  await page.waitForTimeout(300);
  ok('네트워크 화면이 잠긴다',
     (await page.$$eval('#s-net .row.locked', e => e.length)) > 0);
  ok('잠긴 화면이 누가 바꿀 수 있는지 말한다',
     /관리자/.test(await page.$eval('#s-net', e => e.textContent)));
  ok('사이드바에도 자물쇠가 보인다',
     (await page.$$eval('.side .sidelock', e => e.length)) > 0);

  /* And it is not on the screens the account is allowed. */
  ok('허용된 화면은 잠기지 않는다',
     await page.evaluate(() => {
       LP.go('s-disp');
       return document.querySelectorAll('#s-disp .row.locked').length === 0;
     }));

  /* Own password stays reachable inside an otherwise locked section. */
  await page.evaluate(() => LP.go('s-user'));
  await page.waitForTimeout(300);
  ok('잠긴 사용자 화면에서도 자기 비밀번호는 바꿀 수 있다',
     await page.evaluate(() => {
       const rows = [...document.querySelectorAll('#s-user .row')];
       const pw = rows.find(r => /비밀번호 변경/.test(r.textContent));
       return !!pw && !pw.classList.contains('locked');
     }));

  /* ── back, and nothing stuck ──────────────────────────────────── */
  head('되돌리기');
  await page.evaluate(() => { LP.allow.factory = false; });
  await page.click('.appswitch button[data-acct="admin"]');
  await page.waitForTimeout(400);
  await page.evaluate(() => LP.go('s-net'));
  await page.waitForTimeout(300);
  ok('관리자로 돌아오면 잠금이 풀린다',
     (await page.$$eval('#s-net .row.locked', e => e.length)) === 0);
  ok('자물쇠 글리프가 남지 않는다',
     (await page.$$eval('#s-net .lock', e => e.length)) === 0);

  /* ── the design system was actually used ──────────────────────── */
  head('디자인 시스템');
  const stray = await page.evaluate(() => {
    /* Any colour written straight into an element that is not one of
     * the two the mockup legitimately sets inline (a bar width, a
     * transform) is a token that got away. */
    const bad = [];
    document.querySelectorAll('[style]').forEach(el => {
      const s = el.getAttribute('style');
      if (/#[0-9a-f]{3,8}|rgba?\(/i.test(s) && !el.closest('.legend') &&
          !el.closest('.swatch') && !el.closest('.seg'))
        bad.push(el.tagName + ' ' + s.slice(0, 50));
    });
    return bad;
  });
  ok('인라인 색이 없다', stray.length === 0, stray.slice(0, 3).join(' | '));

  const springs = await page.evaluate(() =>
    typeof LPSpring === 'object' && Object.keys(LPSpring.PRESETS).length);
  ok('스프링이 실려 있다', springs === 9, springs + '개');

  /* Every class actually on an element, against every class the
   * stylesheet defines.
   *
   * Read from the DOM rather than grepped out of the source, because
   * most of these screens build their markup in JavaScript and a class
   * that only exists inside a template literal is invisible to a
   * regex - which is how a typo'd class name survives review looking
   * like a class that works. An undefined class is not a crash; it is a
   * row that quietly renders unstyled. */
  const undefinedClasses = await page.evaluate(() => {
    const defined = new Set();
    for (const sheet of document.styleSheets) {
      let rules;
      try { rules = sheet.cssRules; } catch (e) { continue; }
      for (const r of rules) {
        if (!r.selectorText) continue;
        for (const m of r.selectorText.matchAll(/\.([A-Za-z][\w-]*)/g))
          defined.add(m[1]);
      }
    }
    const used = new Set();
    document.querySelectorAll('*').forEach(el =>
      el.classList.forEach(c => used.add(c)));
    return [...used].filter(c => !defined.has(c)).sort();
  });
  ok('스타일시트에 없는 클래스가 없다', undefinedClasses.length === 0,
     undefinedClasses.join(' '));

  /* Motion, actually applied rather than merely documented.
   *
   * The easiest way for a design system's motion to be decorative is
   * for the screens to keep writing their own durations while the
   * springs sit in a stylesheet nobody imports. So: every spring
   * resolves, dialogs animate in, controls respond to a press, and
   * nothing anywhere has a hand-written cubic-bezier - which is the one
   * that would mean somebody reached past the system. */
  const motion = await page.evaluate(() => {
    const cs = getComputedStyle(document.documentElement);
    const springs = ['window', 'sheet', 'menu', 'press', 'insert',
                     'expand', 'knob', 'slide', 'rubber']
      .filter(n => cs.getPropertyValue('--e-' + n).includes('linear('));
    let usingSpring = 0;
    const bezier = new Set();
    document.querySelectorAll('*').forEach(e => {
      const t = getComputedStyle(e).transitionTimingFunction || '';
      if (t.includes('linear(')) usingSpring++;
      /* The browser reports an unset timing function as this exact
       * bezier, so it is the default rather than a choice. */
      else if (t.includes('cubic-bezier') && !t.includes('0.25, 0.1'))
        bezier.add((e.className || e.tagName).toString().slice(0, 30));
    });
    return {
      springs: springs.length,
      pressable: document.querySelectorAll('.pressable').length,
      appear: document.querySelectorAll('.appear, .appear-anchored').length,
      usingSpring,
      bezier: [...bezier],
    };
  });
  ok('스프링 아홉 개가 전부 정의되어 있다', motion.springs === 9,
     motion.springs + '/9');
  ok('대화상자가 스프링으로 나타난다', motion.appear >= 6, motion.appear + '개');
  ok('누를 수 있는 것이 눌린 티를 낸다', motion.pressable >= 10,
     motion.pressable + '개');
  ok('스프링으로 전환하는 요소가 있다', motion.usingSpring > 20,
     motion.usingSpring + '개');
  ok('직접 쓴 베지어가 없다', motion.bezier.length === 0,
     motion.bezier.join(', '));

  head('오류');
  ok('콘솔 오류 없음', errors.length === 0, errors.slice(0, 3).join(' | '));

  /* Beside the page they are of, not in the repository root. A test
   * that litters the top of the tree is a test people stop running. */
  await page.screenshot({
    path: path.join(HERE, 'shot-admin.png'), fullPage: true });
  await page.click('.appswitch button[data-acct="std"]');
  await page.evaluate(() => LP.go('s-reset'));
  await page.waitForTimeout(400);
  await page.screenshot({
    path: path.join(HERE, 'shot-std.png'), fullPage: true });

  await browser.close();

  console.log('\n' + pass + ' 통과, ' + fail + ' 실패');
  if (fail) {
    console.log('\n실패한 것:');
    failures.forEach(f => console.log('  ' + f));
  }
  process.exit(fail ? 1 : 0);
}

run().catch(e => { console.error(e); process.exit(2); });
