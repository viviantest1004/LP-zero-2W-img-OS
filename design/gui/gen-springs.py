#!/usr/bin/env python3
"""gen-springs.py - turn the motion table into CSS easing curves.

shell-design-spec.md §1-5 settles the argument between springs and
beziers in favour of springs, and gives one pair of constants. This
generates the rest, and it generates them as something a browser and a
compositor can both use.

── Why this file exists at all ──

A spring is not a curve, it is a differential equation:

    a = -k(x - target)/m - c·v

which means it has no duration - it has constants, and the duration
falls out of them. That is exactly the property the spec wants, because
a spring can be retargeted mid-flight from wherever it happens to be,
while a bezier is a timetable that has to be restarted.

CSS has no spring. What it has, since Easing Level 2, is linear() -
which takes a list of sampled points and interpolates between them. So
the honest way to get a spring into CSS is to solve the equation once,
here, at build time, and emit the samples. The result is not an
approximation of a spring; it IS the spring, read at 60 points instead
of continuously.

The mockup then gets the real thing in two forms:

    CSS   linear() easings, for everything that is fire-and-forget
    JS    lp-spring.js, a live integrator, for anything that can be
          interrupted - a window being dragged closed while it is still
          opening, a panel re-collapsed mid-expand

Interruption is the whole reason the spec chose springs, so the second
one is not a nicety. But most transitions in a settings window are never
interrupted, and paying for a rAF loop on each of them would be worse
than the problem it solves.

── How the numbers were chosen ──

Backwards from what they have to feel like, not forwards from a pair of
constants somebody liked.

The spec gives two rules that turn out to disagree. §1-5 names stiffness
170 and damping 26 as the window default, and the same section says most
motion should take 150 to 250ms and only big movements 350. Solved, 170
and 26 take 596ms. Both cannot hold, and the one to keep is the budget:
a duration is something a person feels every time a window opens, while
a stiffness is a number in a document. So the table below states what
each motion should feel like - how long, and how hard it lands - and the
constants are solved for.

    settle    how long until there is nothing left to see
    ζ         damping ratio, which is what the eye actually reads:

        ζ = 1.00   arrives and stops. Reads as deliberate. Anything
                   large or authoritative - a window, a workspace -
                   wants this, because overshoot on a big object reads
                   as sloppy rather than lively.
        ζ ≈ 0.80   a small overshoot, one that lands rather than
                   wobbles. Small things that appear and disappear want
                   it: a toggle knob, a row being inserted. Without it a
                   switch feels like it was moved by a machine.
        ζ < 0.60   visible bounce. Not used anywhere here. Charming
                   once, irritating on the four hundredth time.

The rule holding the table together is that distance travelled and speed
go in opposite directions. A workspace crossing the whole screen gets
the longest settle; a switch knob crossing fifteen pixels gets one of
the shortest. Give them the same and the workspace looks frantic while
the switch looks broken.

Run it with no arguments; it writes lp-motion.css beside itself.
"""

import math
import os

# name, settle time in ms, damping ratio, what it is for
#
# Mass is 1 throughout. It is a free parameter - doubling the mass and
# the damping together gives the same motion - so fixing it removes one
# number from every row without removing anything from the result.
SPRINGS = [
    ("window", 340, 1.00,
     "창이 열리고 닫힙니다. 이 표에서 가장 크고, 명세서가 '큰 동작만 "
     "350ms' 라고 한 바로 그 자리입니다. 오버슛 없음 - 큰 것이 튀면 "
     "경쾌한 게 아니라 조잡해 보입니다.\n"
     "명세서의 강성 170·감쇠 26 을 그대로 풀면 596ms 라서 같은 "
     "명세서의 시간 예산을 넘깁니다. 예산을 지켰습니다."),

    ("sheet", 260, 0.95,
     "대화상자와 시트. 창보다 빠릅니다. 이미 무언가를 누른 뒤에 나오는 "
     "것이라, 창처럼 등장을 기다릴 이유가 없습니다."),

    ("menu", 180, 0.92,
     "메뉴와 팝오버. 누른 자리에서 자라나므로 이동 거리가 짧고, 짧은 "
     "거리는 빠른 스프링을 견딥니다. 메뉴가 느리면 그 메뉴는 두 번째부터 "
     "쓰이지 않습니다."),

    ("press", 130, 0.85,
     "눌린 컨트롤이 0.97 로 줄었다 돌아옵니다. 사람이 손가락을 떼기 전에 "
     "끝나야 하므로 이 표에서 가장 빠릅니다. 여기서 20ms 를 더 쓰면 "
     "버튼이 끈적해집니다."),

    ("insert", 220, 0.70,
     "목록에 항목이 들어오고 나갑니다. 작은 오버슛이 있어서 항목이 "
     "'놓입니다'. 입력 소스를 추가할 때 쓰는 것이 이것입니다."),

    ("expand", 240, 1.00,
     "아코디언이 펴집니다. 높이를 움직이는 것이라 오버슛이 있으면 안 "
     "됩니다 - 글자가 눈에 보이게 다시 흐르기 때문입니다. 이 표에서 "
     "임계 감쇠가 취향이 아니라 요구사항인 유일한 항목입니다."),

    ("knob", 200, 0.65,
     "스위치 손잡이. 이 표에서 가장 많이 튑니다. 그래야 합니다 - 스위치가 "
     "딱 붙지 않으면 사람이 켠 게 아니라 기계가 옮긴 것처럼 보입니다.\n"
     "오버슛이 6% 인데 손잡이가 움직이는 거리가 15px 이므로 실제로는 "
     "1px 입니다. 눈에 보이는 튐이 아니라 손끝에 남는 또렷함입니다."),

    ("slide", 340, 1.00,
     "작업공간 전환. 이동 거리가 가장 멀어서 창과 같은 예산 끝을 씁니다."),

    ("rubber", 420, 1.00,
     "스크롤 끝에서 되돌아옵니다. 느리게 돌아오는 것이 핵심이라 이것만 "
     "예산을 넘깁니다 - 손가락을 뗀 뒤에 일어나는 일이라 기다리는 사람이 "
     "없습니다."),
]

# Integration step. Far finer than any display, because the peak
# overshoot is read off this curve and semi-implicit Euler adds damping
# of its own when the step is coarse relative to the spring - which
# would report a springy control as a dead one and get it "fixed".
DT = 1.0 / 4000.0
# When is a spring over?
#
# Mathematically it never is - it approaches the target forever. So a
# threshold has to be picked, and picking it too tight is how a 350ms
# animation gets published as an 800ms one: the last 450ms move the
# object by less than a pixel, nobody can see them, but the number says
# the animation is still running and anything sequenced after it waits.
#
# 0.5% of the travel. On a 400px window that is two pixels, arrived at
# with the velocity already near zero. Below that there is nothing left
# to watch, and the budget in shell-design-spec.md §1-5 - most things
# 150 to 250ms, big ones 350 - is measured against what can be seen.
SETTLE_X = 0.005
SETTLE_V = 0.05
MAX_S = 3.0
SAMPLES = 60              # points emitted into linear()


def solve(k, c, m=1.0):
    """Integrate x'' = -k(x-1)/m - c·x'/m from rest at 0.

    Semi-implicit Euler at 240Hz. Not because the accuracy is needed -
    a display samples this 60 times a second at most - but because the
    settle time is read off the result, and an under-resolved integration
    reports the wrong one.
    """
    x, v, t = 0.0, 0.0, 0.0
    out = [(0.0, 0.0)]
    while t < MAX_S:
        a = (-k * (x - 1.0) - c * v) / m
        v += a * DT
        x += v * DT
        t += DT
        out.append((t, x))
        if abs(x - 1.0) < SETTLE_X and abs(v) < SETTLE_V:
            break
    return out, t


def constants_for(settle_ms, zeta):
    """Find the stiffness that settles in settle_ms at this damping.

    Settle time has no closed form once the 0.5% threshold is involved,
    so it is bisected. Stiffness and settle time are monotonic - stiffer
    is always sooner - which is what makes bisection valid here and is
    worth stating, because it is the assumption the loop rests on.
    """
    want = settle_ms / 1000.0
    lo, hi = 1.0, 20000.0
    for _ in range(60):
        k = (lo + hi) / 2.0
        c = 2.0 * zeta * math.sqrt(k)
        _, t = solve(k, c)
        if t > want:
            lo = k                 # too slow, stiffen it
        else:
            hi = k
    k = round((lo + hi) / 2.0)
    return k, round(2.0 * zeta * math.sqrt(k), 1)


def resample(curve, n):
    """n evenly spaced points in time, which is what linear() wants."""
    total = curve[-1][0]
    pts, j = [], 0
    for i in range(n + 1):
        want = total * i / n
        while j + 1 < len(curve) and curve[j + 1][0] < want:
            j += 1
        if j + 1 >= len(curve):
            pts.append(curve[-1][1])
            continue
        (t0, x0), (t1, x1) = curve[j], curve[j + 1]
        f = 0.0 if t1 == t0 else (want - t0) / (t1 - t0)
        pts.append(x0 + (x1 - x0) * f)
    return pts


def fmt(v):
    s = ("%.4f" % v).rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    lines = [
        "/* lp-motion.css - generated by gen-springs.py. Do not edit.",
        " *",
        " * Every easing here is a real spring, solved and sampled rather",
        " * than a bezier drawn to look like one. The comment on each says",
        " * what it moves, how long it takes and how hard it lands.",
        " *",
        " * Two numbers are published for each spring: the easing and the",
        " * duration it settles in. They belong together - using one",
        " * spring's curve with another's duration gives motion that is",
        " * neither, and it is the usual way this goes wrong.",
        " *",
        " *   transition: transform var(--t-window) var(--e-window);",
        " *",
        " * Exits are not entrances played backwards. Leaving is faster:",
        " * the decision is already made and the object is on its way out,",
        " * so --t-*-out is 0.7 of the entrance throughout.",
        " */",
        "",
        ":root{",
    ]

    table = []
    for name, want_ms, zeta, why in SPRINGS:
        k, c = constants_for(want_ms, zeta)
        curve, settle = solve(k, c)
        pts = resample(curve, SAMPLES)
        ms = int(round(settle * 1000))
        out_ms = int(round(settle * 1000 * 0.7))

        for para in why.split("\n"):
            lines.append("  /* " + para.strip() + " */" if para.strip() else "  /*")
        lines.append(
            "  /* k=%d  c=%.1f  ζ=%.2f  %dms  %s */"
            % (k, c, zeta, ms,
               "임계 감쇠" if zeta >= 0.99
               else "오버슛 %.1f%%" % ((max(pts) - 1) * 100))
        )
        lines.append("  --e-%s:linear(%s);" % (name, ",".join(fmt(p) for p in pts)))
        lines.append("  --t-%s:%dms;" % (name, ms))
        lines.append("  --t-%s-out:%dms;" % (name, out_ms))
        lines.append("")
        table.append((name, k, c, zeta, ms, max(pts)))

    lines += [
        "  /* The one non-spring in the file.",
        "   * A progress bar filling at a known rate is not a physical",
        "   * object arriving somewhere - it is a readout, and easing it",
        "   * would make it lie about the rate. */",
        "  --e-linear:linear;",
        "}",
        "",
        "/* Somebody who has asked the system to stop moving things has",
        " * asked for that everywhere, and a design system that honours it",
        " * on eight transitions and forgets the ninth has not honoured it.",
        " * The transitions are not removed - state still has to change",
        " * visibly - they are collapsed to a length below what reads as",
        " * motion. */",
        "@media (prefers-reduced-motion: reduce){",
        "  :root{",
    ]
    for name, *_ in table:
        lines.append("    --t-%s:1ms;--t-%s-out:1ms;--e-%s:linear;" % (name, name, name))
    lines += ["  }", "}", ""]

    path = os.path.join(here, "lp-motion.css")
    with open(path, "w") as f:
        f.write("\n".join(lines))

    print("%-9s %6s %6s %6s %8s %9s" % ("이름", "k", "c", "ζ", "시간", "오버슛"))
    for name, k, c, z, ms, peak in table:
        print("%-9s %6d %6.1f %6.2f %7dms %8.1f%%"
              % (name, k, c, z, ms, (peak - 1) * 100))
    print("")
    for name, k, c, z, ms, peak in table:
        if ms > 400 and name != "rubber":
            print("  경고: %s 가 %dms 입니다. 명세서 상한은 큰 동작 350ms."
                  % (name, ms))
        if 0 < ms < 100:
            print("  경고: %s 가 %dms 입니다. 100ms 아래는 움직임이 아니라"
                  " 튀는 것으로 읽힙니다." % (name, ms))
    print("\n" + path)


if __name__ == "__main__":
    main()
