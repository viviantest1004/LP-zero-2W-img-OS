/* lp-boot.js - wire the window up. Runs last, after every screen has
 * registered itself.
 *
 * Three things happen here and nothing else: the sidebar is made to
 * navigate, the account switch is made to switch, and the screens that
 * never asked to be locked are adopted by the mechanism that locks
 * them. Anything with more substance than that belongs in the screen
 * that owns it.
 */

(function () {
  'use strict';

  /* Adopt first. LPStatic skips any pane that already has a renderer, so
   * this has to run after every screen module and before the first
   * render - otherwise the screens from the reference mockup would
   * render once with no lock applied, which is the one frame a
   * screenshot is most likely to catch. */
  LPStatic.adopt();

  /* Delegated, not one listener per row. The sidebar is static here, but
   * the panes are not - most of them replace their own contents on every
   * render, and a handler bound to a row that a render has since
   * replaced is a handler on an element nobody can click. Listening at
   * the window is the version that cannot go stale. */
  document.getElementById('st').addEventListener('click', function (e) {
    const nav = e.target.closest('.side a[data-pane]');
    if (nav) { LP.go(nav.dataset.pane); return; }

    /* Every dialog closes on its backdrop and on anything marked to
     * close it, so no screen has to remember to wire its own cancel. */
    const back = e.target.closest('.backdrop');
    if (back && e.target === back) { back.classList.remove('on'); return; }
    const shut = e.target.closest('[data-close]');
    if (shut) { LP.close(shut.dataset.close); }
  });

  document.querySelectorAll('.appswitch button[data-acct]').forEach(function (b) {
    b.addEventListener('click', function () {
      LP.account = b.dataset.acct;
      document.querySelectorAll('.appswitch button').forEach(function (o) {
        o.classList.toggle('on', o === b);
      });
      /* Every pane re-renders, not only the one on screen. A pane that
       * re-renders lazily when it is next opened would be correct, and
       * would also mean that whether the lock is right depends on
       * whether the pane happened to be visible when the account
       * changed - which is exactly the kind of "usually right" this
       * model cannot afford. */
      Object.keys(LP.panes).forEach(function (id) {
        const el = document.getElementById(id);
        if (el) LP.panes[id](el);
      });
      LP.renderSidebar();
      LP.closeAll();
    });
  });

  /* Open on 키보드. It is the screen this branch exists for, and a
   * mockup that opens on 일반 makes a reviewer hunt for the thing they
   * were sent it to look at. */
  LP.go('s-kbd');
})();
