#!/usr/bin/env python3
"""gen-sounds.py - synthesise the system sounds.

The design documents that came before this one cover type, colour,
motion and layout, and say nothing at all about sound. That is the usual
order, and it is why so many desktops end up with sounds that were
picked rather than designed: someone reaches the end of the project,
needs a notification noise, and takes one.

── What earns a sound ──

The rule that decides every row of the table below:

    A sound is for something you might not be looking at, or something
    you cannot take back.

Everything else stays silent. That single line rules out most of what
other systems make noise about:

    타이핑              보고 있습니다
    창 열기·닫기        보고 있고, 되돌릴 수 있습니다
    앱·작업공간 전환    보고 있습니다
    버튼 누르기 성공    성공의 신호는 침묵입니다
    스크롤·호버         분당 수백 번 일어납니다

and it lets in the ones that matter: a refusal (you may not have seen
why), a finished download (you were doing something else), a
notification (that is the entire point of one), a battery warning, and
the irreversible ones - emptying the trash, taking a screenshot.

A sound that fires more than a few times a minute is a sound that will
be turned off, and once a person turns off system sounds they lose the
battery warning too. Restraint here is not taste, it is what keeps the
important ones audible.

── Why every note comes from one pentatonic scale ──

C major pentatonic: C D E G A. It has no semitone steps and no tritone,
which means *any two notes from it sound consonant together*.

That is not a musical nicety, it is an engineering requirement. System
sounds overlap. A notification arrives while a download finishes; a
device is unplugged during an error. With an arbitrary set of tones some
of those pairs land a semitone or a tritone apart and the machine
sounds, for a moment, broken. Restricted to a pentatonic set, every
accidental pair is a chord.

Direction carries meaning and is used consistently:

    올라감    좋은 일 (알림, 연결, 완료)
    내려감    나쁜 일이거나 끝난 일 (오류, 분리, 삭제)

── Why they are quiet, short, and have no low end ──

Peak levels are set well below full scale, and lower still for the ones
that repeat. A system sound competing with music is a system sound
somebody mutes.

Every sound is high-passed at 180Hz. Laptop and monitor speakers cannot
reproduce much below that, so the energy there is not heard - it is
turned into cone excursion, which makes small speakers sound muddy and
distorted at exactly the moment the sound needs to be intelligible.

Attacks are 8-12ms: fast enough to feel immediate, slow enough that
there is no click. Releases are exponential and every file ends at
exact silence, because a waveform cut off mid-cycle is a click, and a
click is the one thing that makes a designed sound feel cheap.

Writes 48kHz 16-bit mono wav into sounds/ beside this file.
"""

import math
import os
import random
import struct
import wave

RATE = 48000

# C major pentatonic. Nothing outside this set appears in any sound.
N = {
    'C5': 523.25, 'D5': 587.33, 'E5': 659.25, 'G5': 783.99, 'A5': 880.00,
    'C6': 1046.50, 'D6': 1174.66, 'E6': 1318.51, 'G6': 1567.98,
    'G4': 392.00, 'C4': 261.63, 'A4': 440.00,
}


def silence(ms):
    return [0.0] * int(RATE * ms / 1000)


def tone(freq, ms, at=0.0, amp=1.0, attack=10.0, decay=None, into=None):
    """One note, mixed into `into` at `at` milliseconds.

    Three partials rather than a bare sine. A pure sine is soft but it
    is also characterless, and at these lengths it reads as a test tone.
    The octave and the twelfth at low level give it a body that survives
    a small speaker without making it bright or bell-like.
    """
    n = int(RATE * ms / 1000)
    start = int(RATE * at / 1000)
    if into is None:
        into = [0.0] * (start + n)
    while len(into) < start + n:
        into.append(0.0)

    tau = (decay if decay is not None else ms * 0.42) / 1000.0
    att = attack / 1000.0

    for i in range(n):
        t = i / RATE
        # Raised cosine attack. Linear would still click faintly at the
        # corner where it meets the decay.
        a = 0.5 - 0.5 * math.cos(math.pi * min(1.0, t / att)) if att > 0 else 1.0
        e = a * math.exp(-t / tau)
        w = (math.sin(2 * math.pi * freq * t)
             + 0.20 * math.sin(2 * math.pi * freq * 2 * t)
             + 0.08 * math.sin(2 * math.pi * freq * 3 * t))
        into[start + i] += amp * e * w / 1.28
    return into


def noise(ms, at=0.0, amp=1.0, lo=900.0, hi=6000.0, attack=1.0,
          decay=None, sweep=None, into=None, seed=7):
    """A band of noise - the shutter and the trash sweep.

    Two one-pole filters make the band. It is a gentle shape rather than
    a surgical one, which is what is wanted: a steep filter on noise
    sounds like a filter, and the point is to sound like a mechanism.

    `sweep` moves the centre over the length of the sound, which is the
    whole of what makes the trash sweep read as something falling away.
    """
    n = int(RATE * ms / 1000)
    start = int(RATE * at / 1000)
    if into is None:
        into = [0.0] * (start + n)
    while len(into) < start + n:
        into.append(0.0)

    rng = random.Random(seed)
    tau = (decay if decay is not None else ms * 0.35) / 1000.0
    att = max(attack, 0.2) / 1000.0
    hp, lp = 0.0, 0.0

    for i in range(n):
        t = i / RATE
        f = 1.0 if sweep is None else (1.0 - i / n) ** 1.5
        klo = lo * (sweep[0] + (sweep[1] - sweep[0]) * (1 - f)) if sweep else 1.0
        centre_lo = lo * (klo if sweep else 1.0)
        centre_hi = hi * (klo if sweep else 1.0)

        x = rng.uniform(-1.0, 1.0)
        a_lp = 1.0 - math.exp(-2 * math.pi * centre_hi / RATE)
        lp += a_lp * (x - lp)
        a_hp = 1.0 - math.exp(-2 * math.pi * centre_lo / RATE)
        hp += a_hp * (lp - hp)
        band = lp - hp

        a = 0.5 - 0.5 * math.cos(math.pi * min(1.0, t / att))
        into[start + i] += amp * a * math.exp(-t / tau) * band * 2.2
    return into


def highpass(buf, hz=180.0):
    """Roll off what a small speaker turns into distortion instead of sound."""
    out, prev_x, prev_y = [], 0.0, 0.0
    rc = 1.0 / (2 * math.pi * hz)
    a = rc / (rc + 1.0 / RATE)
    for x in buf:
        y = a * (prev_y + x - prev_x)
        out.append(y)
        prev_x, prev_y = x, y
    return out


def finish(buf, peak_db):
    """Normalise to a peak, then guarantee the file ends at true zero.

    The tail fade is 3ms and it is not optional. An exponential decay
    reaches an inaudible level but never reaches zero, and a wav that
    stops at -60dBFS mid-cycle still produces a step in the DAC - which
    is a click, at the end of every single sound.
    """
    buf = highpass(buf)
    peak = max(abs(v) for v in buf) or 1.0
    target = 10 ** (peak_db / 20.0)
    g = target / peak
    buf = [v * g for v in buf]

    tail = int(RATE * 0.003)
    for i in range(min(tail, len(buf))):
        buf[len(buf) - 1 - i] *= i / tail
    return buf


def write(name, buf, note):
    """Mono, deliberately.

    A system sound has no stereo image - there is nowhere for it to come
    from. Writing the same samples to two channels would double every
    file to carry no extra information, and any mixer worth using pans a
    mono source to centre for free.
    """
    path = os.path.join(OUT, name + '.wav')
    with wave.open(path, 'w') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        frames = bytearray()
        for v in buf:
            s = int(max(-1.0, min(1.0, v)) * 32767)
            frames += struct.pack('<h', s)
        w.writeframes(bytes(frames))
    ms = int(len(buf) / RATE * 1000)
    print('  %-10s %5dms  %s' % (name + '.wav', ms, note))


# ── the sounds ───────────────────────────────────────────────────
#
# Peak levels, and why they differ:
#
#   -14 dBFS   경고. 다른 소리에 묻히면 안 되는 것.
#   -18 dBFS   보통. 알림, 완료, 오류.
#   -22 dBFS   자주 나는 것. 연결·분리.
#   -26 dBFS   눈금. 볼륨을 한 칸 올릴 때마다 납니다.

def build():
    # 오류 - 거부당했을 때. 내려가는 3도. 짧고, 뒤에 남지 않습니다.
    # 두 번째 음이 첫 번째보다 조금 큽니다: 사람은 소리의 끝을 듣고
    # 판단하고, 끝이 약하면 미안해하는 것처럼 들립니다.
    b = tone(N['E5'], 90, at=0, amp=0.55, decay=40)
    b = tone(N['C5'], 150, at=70, amp=0.75, decay=70, into=b)
    write('error', finish(b, -18), '거부·실패. 내려가는 3도')

    # 경고 - 배터리 부족, 디스크 가득. 방해 금지 중에도 통과하는 것들.
    # 두 번 반복합니다. 한 번은 알림처럼 들리고, 두 번은 무언가
    # 하라는 뜻으로 들립니다.
    b = tone(N['A5'], 150, at=0,   amp=0.8, decay=70)
    b = tone(N['E5'], 260, at=170, amp=0.9, decay=110, into=b)
    write('alert', finish(b, -14), '주의. 두 번 울립니다')

    # 알림 - 앱이 보내는 소식. 올라가는 4도, 가장 자주 나는 소리이므로
    # 가장 눈에 띄지 않아야 합니다.
    b = tone(N['G5'], 110, at=0,  amp=0.6, decay=55)
    b = tone(N['C6'], 200, at=85, amp=0.7, decay=95, into=b)
    write('notify', finish(b, -18), '알림. 올라가는 4도')

    # 완료 - 내려받기나 설치가 끝났습니다. 세 음이 올라갑니다.
    # 기다리고 있지 않았을 때 듣는 소리라 조금 깁니다.
    b = tone(N['C5'], 110, at=0,   amp=0.5, decay=60)
    b = tone(N['E5'], 110, at=80,  amp=0.6, decay=60, into=b)
    b = tone(N['G5'], 240, at=160, amp=0.75, decay=120, into=b)
    write('complete', finish(b, -18), '완료. 올라가는 3음')

    # 볼륨 눈금 - 한 칸에 한 번. 이 표에서 가장 짧고 가장 조용합니다.
    # 소리 자체가 목적이 아니라 새 볼륨을 들려주는 것이 목적이라
    # 거의 딸깍 소리에 가깝습니다.
    b = tone(N['D6'], 55, amp=0.7, attack=4, decay=18)
    write('volume', finish(b, -26), '볼륨 한 칸')

    # 스크린샷 - 셔터. 두 번의 짧은 잡음, 거울이 올라갔다 내려옵니다.
    # 음이 아니라 기계 소리여야 합니다. 사진을 찍었다는 뜻이지
    # 무언가를 알린다는 뜻이 아니기 때문입니다.
    b = noise(45, at=0,  amp=0.9, lo=1200, hi=7000, decay=14, seed=3)
    b = noise(70, at=75, amp=0.6, lo=800,  hi=4500, decay=26, seed=11, into=b)
    write('shutter', finish(b, -18), '스크린샷. 기계 소리')

    # 휴지통 비우기 - 되돌릴 수 없는 것. 잡음이 아래로 쓸려 내려가고
    # 낮은 음이 받칩니다. 완료가 아니라 사라짐으로 들려야 합니다.
    b = noise(260, amp=0.8, lo=700, hi=5000, decay=90,
              sweep=(1.0, 0.18), attack=3, seed=5)
    b = tone(N['C4'], 240, at=30, amp=0.35, decay=100, into=b)
    write('trash', finish(b, -18), '휴지통 비우기. 쓸려 내려감')

    # 장치 연결·분리. 같은 두 음을 순서만 바꿔 씁니다 - 한 쌍으로
    # 들려야 하고, 그러려면 정말로 한 쌍이어야 합니다.
    b = tone(N['C5'], 90,  at=0,  amp=0.55, decay=42)
    b = tone(N['G5'], 150, at=70, amp=0.65, decay=70, into=b)
    write('plug', finish(b, -22), '장치 연결. 올라감')

    b = tone(N['G5'], 90,  at=0,  amp=0.55, decay=42)
    b = tone(N['C5'], 150, at=70, amp=0.65, decay=70, into=b)
    write('unplug', finish(b, -22), '장치 분리. 내려감')

    # 부팅.
    #
    # 하루에 한 번 나는 소리이고, 이 기계가 무엇인지 말하는 유일한
    # 소리입니다. 그래서 이 표에서 유일하게 긴 것이 허용됩니다.
    # 네 음이 겹치며 쌓여 화음이 되고, 마지막 음만 길게 남습니다.
    # 펜타토닉이라 넷이 동시에 울려도 부딪히지 않습니다.
    b = tone(N['C5'], 900,  at=0,   amp=0.40, attack=25, decay=420)
    b = tone(N['E5'], 850,  at=120, amp=0.34, attack=25, decay=400, into=b)
    b = tone(N['G5'], 900,  at=240, amp=0.32, attack=25, decay=430, into=b)
    b = tone(N['C6'], 1150, at=360, amp=0.40, attack=30, decay=620, into=b)
    b = tone(N['G4'], 1300, at=360, amp=0.16, attack=40, decay=700, into=b)
    write('boot', finish(b, -16), '부팅. 하루에 한 번')


OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'sounds')

if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    print('48kHz 16bit mono, 180Hz 하이패스, 끝은 항상 정확히 0\n')
    build()
    total = sum(os.path.getsize(os.path.join(OUT, f))
                for f in os.listdir(OUT) if f.endswith('.wav'))
    print('\n  %d개, 합계 %dKB' % (
        len([f for f in os.listdir(OUT) if f.endswith('.wav')]),
        total // 1024))
    print('  ' + OUT)
