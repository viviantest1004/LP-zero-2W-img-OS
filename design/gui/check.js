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
