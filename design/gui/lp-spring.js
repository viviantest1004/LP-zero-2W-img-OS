/* lp-spring.js - springs that can be interrupted.
 *
 * lp-motion.css already carries every spring in the system, solved and
 * sampled into a CSS linear() easing. For most things that is the right
 * answer and this file should not be used: a CSS transition runs on the
 * compositor thread, survives a busy main thread, and costs nothing.
 *
 * What a CSS transition cannot do is be redirected. Change the target
 * mid-flight and the browser restarts the easing from wherever the
 * property currently is, at zero velocity - so an object that was
 * moving fast stops dead and sets off again. On a small movement nobody
 * notices. On a window being pulled closed while it is still opening,
 * or a panel re-collapsed halfway through expanding, it is the whole
 * difference between the interface having weight and having none, and
 * it is the reason shell-design-spec.md §1-5 chose springs at all:
 *
 *     베지어는 "0.4초 동안 A→B"라는 시간표라 중간에 끊기지만,
 *     스프링은 현재 위치와 속도만 있으면 목표만 바꿔도 자연스럽게 휘어진다.
 *
 * This is that sentence, implemented. A Spring holds a position and a
 * velocity and integrates toward whatever its target currently is. Move
 * the target and nothing restarts - the acceleration simply points
 * somewhere else from the next frame on, and the velocity it already
 * had carries through the turn.
 *
 * ── One loop, not one per spring ──
 *
 * Every live spring is stepped from a single requestAnimationFrame
 * callback. A dozen independent rAF loops would each read the clock,
 * each schedule the next frame, and each force their own style
 * recalculation; the browser would collapse them into one frame anyway,
 * but the work would be done a dozen times. The loop stops itself when
 * the last spring settles, so an idle window costs nothing.
 *
 * ── The step ──
 *
 * Semi-implicit Euler, clamped to a maximum step. The clamp is what
 * keeps a spring stable when the tab has been in the background: the
 * first frame back can be seconds later, and integrating one step of
 * that size sends a stiff spring to infinity. Clamping loses accuracy
 * for that one frame and keeps the object on screen, which is the right
 * trade every time.
 */

(function (global) {
  'use strict';

  /* The same constants gen-springs.py solved, so the JS springs and the
   * CSS easings are the same springs rather than two sets that drift.
   * If the table there changes, these change with it. */
  const PRESETS = {
    window: { k: 602,  c: 49.1 },
    sheet:  { k: 856,  c: 55.6 },
    menu:   { k: 1599, c: 73.6 },
    press:  { k: 5005, c: 120.3 },
    insert: { k: 1435, c: 53.0 },
    expand: { k: 1355, c: 73.6 },
    knob:   { k: 1614, c: 52.2 },
    slide:  { k: 602,  c: 49.1 },
    rubber: { k: 368,  c: 38.4 },
  };

  /* Same thresholds as the generator, for the same reason: a spring is
   * over when there is nothing left to see, not when the arithmetic
   * stops changing. Scaled by the distance the spring is covering, so a
   * spring travelling 600 pixels and one travelling 15 are both judged
   * on what a person could notice rather than on the raw number. */
  const REST_X = 0.005;
  const REST_V = 0.05;

  const MAX_STEP = 1 / 30;      /* a backgrounded tab must not explode */
  const SUB      = 1 / 240;     /* integrate finer than the display */

  const live = new Set();
  let running = false;
  let last = 0;

  function frame(now) {
    const dt = last ? Math.min((now - last) / 1000, MAX_STEP) : SUB;
    last = now;

    for (const s of live) s._step(dt);

    if (live.size) {
      requestAnimationFrame(frame);
    } else {
      running = false;
      last = 0;
    }
  }

  function start() {
    if (running) return;
    running = true;
    last = 0;
    requestAnimationFrame(frame);
  }

  class Spring {
    /* preset is a name from the table above, or {k, c}. */
    constructor(preset, value) {
      const p = typeof preset === 'string' ? PRESETS[preset] : preset;
      if (!p) throw new Error('lp-spring: 알 수 없는 스프링 ' + preset);
      this.k = p.k;
      this.c = p.c;
      this.x = value || 0;
      this.v = 0;
      this.target = this.x;
      this.scale = 1;            /* the distance this spring covers */
      this._on = [];
      this._done = [];
    }

    /* Retarget. The point of the whole file: position and velocity are
     * left exactly as they are, so a spring already moving carries its
     * momentum through the change of mind. */
    to(target) {
      this.target = target;
      const d = Math.abs(target - this.x);
      if (d > this.scale) this.scale = d;
      if (!this.settled()) { live.add(this); start(); }
      return this;
    }

    /* Put it somewhere with no motion at all - initial state, or a
     * cancel that should not animate. */
    set(value) {
      this.x = this.target = value;
      this.v = 0;
      live.delete(this);
      this._emit();
      return this;
    }

    onChange(fn) { this._on.push(fn); return this; }
    onRest(fn)   { this._done.push(fn); return this; }

    settled() {
      const s = this.scale || 1;
      return Math.abs(this.x - this.target) < REST_X * s &&
             Math.abs(this.v) < REST_V * s;
    }

    _emit() { for (const fn of this._on) fn(this.x, this); }

    _step(dt) {
      /* Substepped so that a stiff spring is integrated at a rate its
       * own frequency can stand, whatever the display is doing. A 60Hz
       * frame with k=5005 is barely stable in one step and visibly wrong
       * in two; four substeps costs nothing and is always right. */
      let left = dt;
      while (left > 0) {
        const h = Math.min(SUB, left);
        const a = -this.k * (this.x - this.target) - this.c * this.v;
        this.v += a * h;
        this.x += this.v * h;
        left -= h;
      }

      if (this.settled()) {
        this.x = this.target;
        this.v = 0;
        live.delete(this);
        this._emit();
        for (const fn of this._done) fn(this);
        return;
      }
      this._emit();
    }
  }

  /* ── the two things this actually gets used for ─────────────────
   *
   * Height, and a value that drives a transform. Both are here rather
   * than in the screens because both have a trap in them that is easy
   * to get wrong once per screen.
   */

  /* Expand or collapse a block whose height is not known in advance.
   *
   * The trap: height: auto cannot be animated, so the natural height
   * has to be measured first - and measuring it means the element must
   * be laid out at that height, which means it flashes open unless the
   * measurement and the first frame happen before paint. scrollHeight
   * on a hidden-overflow element gives it without that.
   *
   * The second trap: the height has to be released back to auto when it
   * settles open, or the panel stops growing when its content changes. */
  function height(el, open, onRest) {
    if (!el._spring) {
      el._spring = new Spring('expand', open ? 1 : 0);
      el._spring.onChange(function (t) {
        const h = el._targetH * t;
        el.style.height = h + 'px';
        el.style.opacity = Math.min(1, t * 1.6);
        el.style.overflow = 'hidden';
      });
      el._spring.onRest(function () {
        if (el._spring.target === 1) {
          el.style.height = 'auto';
          el.style.overflow = '';
        } else {
          el.style.display = 'none';
        }
        if (onRest) onRest();
      });
    }
    if (open) el.style.display = '';
    /* Measured every time, not cached: the content can have changed
     * since the last expansion, and a cached height clips it. */
    el.style.height = 'auto';
    el._targetH = el.scrollHeight;
    if (!open) el.style.height = el._targetH + 'px';
    el._spring.to(open ? 1 : 0);
    return el._spring;
  }

  /* A named spring per element, so repeated calls retarget the one that
   * is already running instead of starting a second one on top of it -
   * which is the usual way an interruptible animation ends up with two
   * springs fighting over the same property. */
  function on(el, name, preset, from) {
    el._springs = el._springs || {};
    if (!el._springs[name]) el._springs[name] = new Spring(preset, from);
    return el._springs[name];
  }

  global.LPSpring = { Spring, PRESETS, height, on };

})(window);
