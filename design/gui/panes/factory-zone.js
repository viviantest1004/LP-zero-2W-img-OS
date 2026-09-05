/* 공장 초기화 - the one thing on this machine that cannot be taken back.
 *
 * It is not a screen. It is a block at the bottom of 초기화, past six
 * resets that can all be undone by changing the setting again, and that
 * position is the first part of the design: anybody who arrives here
 * has already walked past the thing they probably wanted.
 *
 * ── Why the facts come before the password ──
 *
 * The obvious order is to ask for the administrator password first and
 * only then say what is about to happen, because that is the order the
 * machine cares about. It is the wrong order for the person. Somebody
 * who has typed their password has decided; the sentence that would
 * have changed their mind arrives after the decision, and reads as
 * something to click past. So the first dialog is a list of what goes
 * and what stays, with no field in it at all.
 *
 * ── Why 남는 것 is a list and not a footnote ──
 *
 * A machine on a shelf that is reached only over the network is one
 * `rm -rf` away from being a machine nobody can reach. The SSH
 * authorized key on the boot partition is the whole difference between
 * a factory reset and a brick, and it is the single fact most likely to
 * be missing from a person's model of what this button does. It gets a
 * row of its own in the first dialog and a paragraph of its own in the
 * last.
 *
 * ── Why it is honest about how it is done ──
 *
 * This runs on the amd64 desktop and nowhere else. Its root is a real
 * ext4 filesystem that a preinit mounts and switches into, so nothing
 * about it is pristine and there is no archive in the kernel to fall
 * back on - the system is put back from the copy kept on the boot
 * partition, which takes minutes and needs the power to stay on. A
 * reset that says "잠시만 기다려 주세요" and then takes four minutes on a
 * machine somebody unplugs at ninety seconds is a reset that bricks it.
 *
 * Nothing here changes LP. A reviewer wants to press this more than
 * once, and a mockup that empties its own state on the first press can
 * only be reviewed by reloading the file.
 */

(function () {
  'use strict';

  const SLOT = 'factory-zone';

  /* Never LP.account. LP.can('factory') is false for a standard account
   * whatever 사용자 › 권한 says - the administrator cannot delegate this
   * one - and asking the helper is how that stays true in one place. */
  const AREA = 'factory';

  const D_WHAT = 'dlg-factory-what';
  const D_WHO  = 'dlg-factory-who';
  const D_SURE = 'dlg-factory-sure';
  const D_RUN  = 'dlg-factory-run';
  const D_DONE = 'dlg-factory-done';

  const PHRASE = '초기화';

  /* What the machine is doing, in the order it does it. The bar reports
   * these rather than a percentage of nothing: "설정을 처음 값으로
   * 되돌립니다" at 84% tells somebody how much longer to leave the power
   * alone, and a number on its own does not. */
  const STEPS = [
    { at: 18,  say: '부팅 파티션의 사본을 확인합니다' },
    { at: 38,  say: '계정과 홈 폴더를 지웁니다' },
    { at: 62,  say: '설치한 앱과 패키지를 지웁니다' },
    { at: 84,  say: '부팅 파티션의 사본에서 시스템을 다시 씁니다' },
    { at: 100, say: '설정을 처음 값으로 되돌립니다' },
  ];

  function dlg(id) { return document.getElementById(id); }

  /* ── the zone ───────────────────────────────────────────────────
   *
   * Called again on every render, so everything that depends on the
   * account is removed and re-made rather than toggled - the shape
   * lp-core uses for the sidebar lock, and for the same reason:
   * applying the lock twice leaves two glyphs, and undoing it would
   * have to reverse every step exactly. */
  function render(el) {
    const zone = el.querySelector('.zone');
    const act  = zone.querySelector('[data-act]');
    const may  = LP.can(AREA);

    zone.classList.toggle('locked', !may);

    const oldLock = zone.querySelector('.lock');
    if (oldLock) oldLock.remove();
    const oldWhy = zone.querySelector('[data-why]');
    if (oldWhy) oldWhy.remove();

    if (!may) {
      const glyph = document.createElement('span');
      glyph.className = 'lock';
      glyph.textContent = '자물쇠';
      zone.querySelector('h3').appendChild(glyph);

      /* Never a bare 권한이 없습니다. whyLocked names who can, which is
       * the only version of the sentence somebody can act on - and it
       * is a paragraph of the zone rather than the dimmer .why used on
       * rows, because in a block this size it is the line that has to
       * be read. */
      const why = document.createElement('p');
      why.setAttribute('data-why', '');
      why.textContent = LP.whyLocked(AREA);
      zone.insertBefore(why, act);
    }

    /* Rebuilt rather than enabled in place. A listener added to a node
     * that survives the render is added again on the next one, and the
     * count climbs with every account switch; a node this render made
     * cannot be carrying the handler from the drawing before it.
     *
     * The disabled button keeps `danger` and loses `pressable`:
     * .btn[disabled] comes after .btn.danger in lp-ui.css and wins, so
     * it greys out as the stylesheet intends, and a control that cannot
     * be pressed should not answer to a press. */
    act.innerHTML = '<button class="btn danger' + (may ? ' pressable' : '') +
                    '"' + (may ? '' : ' disabled') + '>공장 초기화</button>';

    if (may)
      act.querySelector('button')
         .addEventListener('click', function () { LP.open(D_WHAT); });
  }

  /* ── the three questions ────────────────────────────────────────── */

  /* Cleared on the way in, not on the way out. A dialog that was left
   * by Escape keeps whatever was typed in it, and the next person to
   * open it would find a password box that is already full. */
  function openWho() {
    const el    = dlg(D_WHO);
    const field = el.querySelector('[data-pw]');
    field.value = '';
    field.classList.remove('bad');
    el.querySelector('.inline').className = 'inline';
    el.querySelector('[data-go]').disabled = true;
    LP.open(D_WHO);
    field.focus();
  }

  function openSure() {
    const el    = dlg(D_SURE);
    const field = el.querySelector('[data-phrase]');
    field.value = '';
    el.querySelector('[data-go]').disabled = true;
    LP.open(D_SURE);
    field.focus();
  }

  function checkPassword() {
    const el    = dlg(D_WHO);
    const field = el.querySelector('[data-pw]');

    /* Any password is taken, because there is no account behind this
     * screen to ask. One literal is refused so that the wrong-password
     * state is reachable: a branch a reviewer cannot produce is a
     * branch that gets shipped without ever being looked at. */
    if (field.value === 'wrong') {
      /* Emptied rather than selected. A rejected password is retyped
       * from the start every time, and leaving the old one in the box
       * under a selection highlight only invites a second press of the
       * same wrong thing. */
      field.value = '';
      field.classList.add('bad');
      el.querySelector('[data-go]').disabled = true;
      field.focus();
      LP.say(el.querySelector('.inline'),
             '비밀번호가 맞지 않습니다. 다시 입력해 주세요', 'bad');
      return;
    }

    LP.close(D_WHO);
    openSure();
  }

  /* ── doing it ───────────────────────────────────────────────────── */
  function start() {
    /* The disabled button in the zone is the drawing of the rule. This
     * is the rule. The dialogs are wired once and outlive every render,
     * so this is the check that actually holds when a click arrives
     * some other way - a focused button and a stray Enter, or a console
     * that opened the dialog directly. */
    if (!LP.can(AREA)) return;

    const el   = dlg(D_RUN);
    const box  = el.querySelector('.prog');
    const line = el.querySelector('[data-step]');

    /* A new fill every run. The one from the press before is already at
     * 100% and has nowhere to travel, so it would arrive with no
     * transition and no transitionend, and the rest of this hangs on
     * that event. */
    box.innerHTML = '<div></div>';
    const fill = box.firstElementChild;

    LP.close(D_SURE);
    LP.open(D_RUN);

    /* Reading the layout is what gives the width below something to
     * travel from: a node inserted and moved inside one tick has no
     * previous computed style and the browser has nothing to
     * interpolate, so the bar would snap to 18% in silence. */
    void fill.offsetWidth;

    let i = 0;
    function step() {
      line.textContent = STEPS[i].say;
      fill.style.width = STEPS[i].at + '%';
    }

    /* Nothing here picks a duration. The bar is a readout rather than an
     * object arriving somewhere, which is why lp-motion.css keeps it
     * linear, and each step is timed by the width transition that
     * lp-ui.css already gives .prog - the steps are chained off it
     * rather than off a clock this file invented. */
    fill.addEventListener('transitionend', function () {
      if (++i < STEPS.length) { step(); return; }

      /* Escape closes any dialog in this mockup, this one included.
       * Somebody who left the progress behind was not asking for a
       * second dialog to land on top of the screen they went back to. */
      if (el.classList.contains('on')) {
        LP.close(D_RUN);
        LP.open(D_DONE);
      }
    });

    step();
  }

  /* ── wiring ─────────────────────────────────────────────────────
   *
   * Once, at load. The dialogs are static markup outside the pane, so
   * no render ever throws their nodes away - and wiring them from the
   * renderer would therefore stack a second handler on every account
   * switch. Everything they decide is read from LP at the moment of the
   * click, so nothing is frozen by being bound early. */
  function wire() {
    dlg(D_WHAT).querySelector('[data-go]')
      .addEventListener('click', function () {
        LP.close(D_WHAT);
        openWho();
      });

    const who   = dlg(D_WHO);
    const pw    = who.querySelector('[data-pw]');
    const pwGo  = who.querySelector('[data-go]');

    pw.addEventListener('input', function () {
      /* The refusal clears itself after four seconds; the red border on
       * the field has to go the moment the person acts on it, or it
       * outlives the thing it was about. */
      pw.classList.remove('bad');
      pwGo.disabled = !pw.value;
    });
    pwGo.addEventListener('click', checkPassword);

    const sure   = dlg(D_SURE);
    const phrase = sure.querySelector('[data-phrase]');
    const goBtn  = sure.querySelector('[data-go]');

    /* Compared as typed. The point of the phrase is that it is typed
     * exactly, and trimming it would quietly accept something the
     * person did not write. */
    phrase.addEventListener('input', function () {
      goBtn.disabled = phrase.value !== PHRASE;
    });
    goBtn.addEventListener('click', start);

    /* Enter, on both fields, so the flow can be finished without
     * reaching for the mouse. isComposing is not optional here: the
     * phrase is Hangul, and the Enter that commits an IME composition
     * arrives at this handler as well - without the guard the dialog
     * would advance on the keystroke that finished typing 초기화 rather
     * than on a deliberate second press. */
    [[pw, pwGo], [phrase, goBtn]].forEach(function (pair) {
      pair[0].addEventListener('keydown', function (e) {
        if (e.key !== 'Enter' || e.isComposing) return;
        if (!pair[1].disabled) pair[1].click();
      });
    });
  }

  wire();
  LP.panes[SLOT] = render;

})();