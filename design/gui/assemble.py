#!/usr/bin/env python3
"""assemble.py - build settings.html out of its parts.

The settings mockup is one self-contained file, because a mockup gets
sent to people and a file that only works inside a checkout is a file
nobody looks at. But it is not written as one - it is assembled from:

    design/reference/task-manager-settings-mockup.html
        the sixteen screens the owner already designed. Taken verbatim.
        Redrawing somebody's finished work to add a section to it is how
        a design review turns into an argument about the parts that were
        not under discussion.

    design/gui/panes/*.html, *.js
        the six screens this branch adds or replaces - 키보드, 키 지정,
        초기화, 공장 초기화, 사용자, 권한.

    design/gui/lp-*.css, lp-core.js, lp-spring.js
        the design system, inlined.

Run it after changing any of those. It writes settings.html and says
what went in.
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
REF = os.path.join(ROOT, 'design', 'reference',
                   'task-manager-settings-mockup.html')
PANES = os.path.join(HERE, 'panes')
OUT = os.path.join(HERE, 'settings.html')

# The screens taken from the owner's mockup unchanged, in sidebar order.
# 키보드 and 사용자 are deliberately absent: this branch replaces both.
KEEP = ['s-gen', 's-net', 's-bt', 's-disp', 's-look', 's-snd', 's-noti',
        's-pow', 's-ws', 's-mouse', 's-a11y', 's-apps', 's-store',
        's-print', 's-priv', 's-about']

# The sidebar. Group, id, glyph, label. A pane reached only from another
# pane - 키 지정 from 키보드, 권한 from 사용자 - is not in the list; it
# is a sub-screen and a sidebar entry for it would offer two routes to
# one place that then disagree about which is selected.
NAV = [
    (None,        's-gen',   '◇', '일반'),
    ('연결',      's-net',   '◈', '네트워크'),
    (None,        's-bt',    '✳', '블루투스'),
    ('화면과 소리', 's-disp',  '▭', '화면'),
    (None,        's-look',  '◐', '모양'),
    (None,        's-snd',   '◑', '소리'),
    (None,        's-noti',  '◔', '알림'),
    ('동작',      's-pow',   '▮', '전원'),
    (None,        's-ws',    '⊞', '작업공간'),
    (None,        's-kbd',   '▤', '키보드'),
    (None,        's-mouse', '◉', '마우스'),
    (None,        's-a11y',  '◍', '접근성'),
    ('시스템',    's-apps',  '▦', '앱'),
    (None,        's-store', '▧', '저장 공간'),
    (None,        's-print', '▬', '프린터'),
    (None,        's-user',  '◎', '사용자'),
    (None,        's-priv',  '⊙', '개인 정보'),
    (None,        's-reset', '↺', '초기화'),
    (None,        's-about', '◇', '정보'),
]


def die(msg):
    print('error: ' + msg, file=sys.stderr)
    sys.exit(1)


def read(path):
    if not os.path.exists(path):
        die(path + ' 가 없습니다')
    with open(path, encoding='utf-8') as f:
        return f.read()


def extract_panes(html):
    """Pull every <div class="pane" ...> block out by brace counting.

    A regex cannot do this. The panes contain nested divs, and a
    non-greedy match to the first </div> stops at the first row while a
    greedy one swallows every pane at once. Counting opens and closes is
    the only version that is right, and it is ten lines.
    """
    out = {}
    for m in re.finditer(r'<div class="pane[^"]*" id="([^"]+)">', html):
        pid = m.group(1)
        i = m.end()
        depth = 1
        while depth and i < len(html):
            nxt_open = html.find('<div', i)
            nxt_close = html.find('</div>', i)
            if nxt_close == -1:
                break
            if nxt_open != -1 and nxt_open < nxt_close:
                depth += 1
                i = nxt_open + 4
            else:
                depth -= 1
                i = nxt_close + 6
        out[pid] = html[m.start():i]
    return out


def main():
    ref_panes = extract_panes(read(REF))
    missing = [p for p in KEEP if p not in ref_panes]
    if missing:
        die('참고 목업에 없는 화면: ' + ', '.join(missing))

    # The screens this branch adds. Each is a pair of files written by
    # the design pass; the js registers LP.panes[id].
    new_html, new_js, new_css, dialogs = {}, [], [], []
    if os.path.isdir(PANES):
        for name in sorted(os.listdir(PANES)):
            path = os.path.join(PANES, name)
            base, ext = os.path.splitext(name)
            if ext == '.html':
                new_html[base] = read(path)
            elif ext == '.js':
                new_js.append((base, read(path)))
            elif ext == '.css':
                new_css.append(read(path))
            elif ext == '.dialogs':
                dialogs.append(read(path))

    # Panes, in sidebar order, so the file reads in the order the app
    # does. The first one is the one that opens.
    body = []
    for _, pid, _, _ in NAV:
        if pid in new_html:
            body.append(new_html[pid])
        elif pid in ref_panes:
            body.append(ref_panes[pid])
        else:
            die(pid + ' 화면이 없습니다')
    # Sub-screens last: reached from a row, never from the sidebar.
    for pid in ('s-keys', 's-perm'):
        if pid in new_html:
            body.append(new_html[pid])

    side = []
    for grp, pid, glyph, label in NAV:
        if grp:
            side.append('      <div class="grp">%s</div>' % grp)
        side.append(
            '      <a data-pane="%s"><b>%s</b>%s</a>' % (pid, glyph, label))

    css = '\n'.join(read(os.path.join(HERE, f)) for f in
                    ('lp-motion.css', 'lp-type.css', 'lp-ui.css'))
    if new_css:
        css += '\n\n/* ── screens that needed a class of their own ── */\n'
        css += '\n'.join(new_css)

    js = read(os.path.join(HERE, 'lp-spring.js'))
    js += '\n\n' + read(os.path.join(HERE, 'lp-core.js'))
    js += '\n\n' + read(os.path.join(HERE, 'lp-static.js'))
    for base, src in new_js:
        js += '\n\n/* ── %s ── */\n' % base + src
    js += '\n\n' + read(os.path.join(HERE, 'lp-boot.js'))

    page = TEMPLATE.format(
        css=css,
        side='\n'.join(side),
        panes='\n\n'.join(body),
        dialogs='\n'.join(dialogs),
        js=js,
    )

    with open(OUT, 'w', encoding='utf-8') as f:
        f.write(page)

    print('설정 목업')
    print('  참고 목업에서 그대로: %d개  %s' % (len(KEEP), ' '.join(KEEP)))
    print('  이 브랜치가 추가·교체: %d개  %s'
          % (len(new_html), ' '.join(sorted(new_html))))
    print('  대화상자 묶음: %d개' % len(dialogs))
    print('  %d KB  %s' % (os.path.getsize(OUT) // 1024, OUT))


TEMPLATE = '''<meta charset="utf-8">
<title>설정 — LP 데스크탑</title>
<style>
{css}
</style>

<div class="wrap">

<!-- The account switch is not part of the OS.
     It is here so that one file can be reviewed as both kinds of user
     without logging out and back in, and so that the claim "a
     restricted setting looks restricted everywhere" can be checked
     rather than taken on trust. Flip it on any screen. -->
<div class="appswitch">
  <button class="on pressable" data-acct="admin">관리자로 보기</button>
  <button class="pressable" data-acct="std">표준 사용자로 보기</button>
  <span class="as-note">같은 화면을 두 계정으로 봅니다</span>
</div>

<div class="win" id="st">
  <div class="wintop">
    <div class="dots"><i></i><i></i><i></i></div>
    <div class="wintitle">설정</div>
    <div class="who" id="who"></div>
  </div>
  <div class="body">
    <div class="side">
{side}
    </div>
    <div class="main">

{panes}

    </div>
  </div>

{dialogs}
</div>

</div>

<script>
{js}
</script>
'''


if __name__ == '__main__':
    main()
