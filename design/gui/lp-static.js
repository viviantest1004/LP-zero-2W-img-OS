/* lp-static.js - the lock, applied to screens that know nothing about it.
 *
 * Sixteen of the nineteen screens in this mockup come from the owner's
 * original file unchanged. They are static markup with no render
 * function, written before there was a permission model, and they will
 * stay that way - redrawing somebody's finished work to add a feature to
 * it is how a review of one section turns into an argument about all the
 * others.
 *
 * But the rule the permission model rests on is:
 *
 *     A restricted setting looks restricted everywhere it appears.
 *
 * and that is not a rule about the screens somebody remembered. A lock
 * drawn only where it was thought of is worse than no lock at all,
 * because it teaches that the absence of one means permission - and then
 * the first screen that was forgotten reads as allowed.
 *
 * So the lock is not written into the screens. It is applied to them,
 * from outside, by one function that runs on every render, for every
 * pane, whether or not anybody thought about that pane. A screen added
 * next month is covered on the day it is added, without being told.
 *
 * ── Why the original markup is kept ──
 *
 * Locking is a transformation of the DOM: rows gain a class, a glyph and
 * a line of explanation. Applying it twice would give two glyphs, and
 * unlocking by undoing it would have to reverse each step exactly.
 * Keeping the untouched HTML and re-applying from it makes both
 * problems go away, and makes switching accounts back and forth exact
 * rather than approximately reversible.
 */

(function () {
  'use strict';

  /* Some rows in a locked section are still the account's own business.
   * Changing your own password is the clearest: a permission model that
   * locks a person out of their own password is one that gets worked
   * around by sharing the administrator account, which is worse than
   * whatever it was protecting. A row whose name matches one of these
   * stays live inside an otherwise locked section. */
  const ALWAYS_MINE = ['비밀번호 변경', '프로필 사진', '기록 지금 지우기'];

  function lockRow(row, why) {
    if (row.classList.contains('locked')) return;
    const name = row.querySelector('.lb b');
    if (name && ALWAYS_MINE.indexOf(name.textContent.trim()) !== -1) return;

    row.classList.add('locked');
    if (name && !name.querySelector('.lock')) {
      const g = document.createElement('span');
      g.className = 'lock';
      g.textContent = '자물쇠';
      name.appendChild(g);
    }
    /* One explanation per section rather than one per row. Twenty rows
     * each carrying the same sentence is not twenty times as clear; it
     * is a wall of grey text with the actual settings hidden in it. The
     * note at the top of the pane carries the reason, and the rows just
     * look unavailable. */
    row.querySelectorAll('input,select,button').forEach(function (el) {
      el.disabled = true;
    });
  }

  function noteFor(area) {
    const n = document.createElement('div');
    n.className = 'note';
    n.innerHTML = '<b>관리자만 바꿀 수 있습니다.</b>&nbsp;이 화면의 값은 ' +
      '볼 수 있지만 바꿀 수 없습니다. 바꾸려면 관리자 계정으로 ' +
      '로그인하거나, 관리자에게 사용자 › 권한에서 이 항목을 열어 달라고 ' +
      '요청하십시오.';
    return n;
  }

  /* Register a renderer for every pane that does not already have one.
   * Called once at boot, after every screen module has registered its
   * own - so a screen that manages its own locks is never touched. */
  function adopt() {
    document.querySelectorAll('.pane').forEach(function (pane) {
      const id = pane.id;
      if (LP.panes[id]) return;

      const original = pane.innerHTML;
      const area = LP.sidebarAreas[id];

      LP.panes[id] = function (el) {
        /* No permission attached to this screen at all - 블루투스,
         * 접근성, 프린터. Nothing to do, and re-writing the markup on
         * every render for nothing would throw away scroll position and
         * any half-typed field. */
        if (!area) return;

        const locked = !LP.can(area);
        if (el._locked === locked) return;
        el._locked = locked;

        el.innerHTML = original;
        if (!locked) return;

        el.querySelectorAll('.row').forEach(function (r) { lockRow(r); });
        const sub = el.querySelector('.sub');
        if (sub) sub.after(noteFor(area));
        else el.querySelector('h2').after(noteFor(area));
      };
    });
  }

  window.LPStatic = { adopt: adopt };
})();
