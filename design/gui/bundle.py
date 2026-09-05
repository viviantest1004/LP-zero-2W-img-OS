#!/usr/bin/env python3
"""bundle.py - fold a page and everything it loads into one file.

`feel.html` pulls in three stylesheets, a script and ten wav files. That
is the right shape to work on: a change to a spring constant is one
edit to one file and every page picks it up.

It is the wrong shape to send. A design review happens by somebody
opening the thing on their own machine, or on a phone, or by forwarding
it to somebody else - and a page that only works from inside a checkout
is a page that gets one look from one person. Six files in a zip is not
better; it is the same problem with an extra step and a folder that has
to stay together.

So: one file. Stylesheets inlined, scripts inlined, sounds turned into
data: URIs. It opens by double-clicking, it survives being attached to
a message, and it has no way to arrive half-broken because there is
nothing else for it to arrive without.

The cost is size - base64 is a third larger than the bytes it carries,
so 366KB of audio becomes about 490KB. For a page that is opened once
and looked at, that is not a cost worth avoiding.

    python3 bundle.py feel.html            → dist/feel.html
    python3 bundle.py                      → every page that has parts
"""

import base64
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DIST = os.path.join(HERE, 'dist')

MIME = {
    '.wav': 'audio/wav', '.mp3': 'audio/mpeg', '.ogg': 'audio/ogg',
    '.png': 'image/png', '.jpg': 'image/jpeg', '.svg': 'image/svg+xml',
    '.woff2': 'font/woff2', '.woff': 'font/woff',
}


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()


def data_uri(path):
    ext = os.path.splitext(path)[1].lower()
    mime = MIME.get(ext, 'application/octet-stream')
    with open(path, 'rb') as f:
        return 'data:%s;base64,%s' % (mime, base64.b64encode(f.read()).decode())


def bundle(name):
    src = os.path.join(HERE, name)
    if not os.path.exists(src):
        print('건너뜀: %s 가 없습니다' % name)
        return None
    html = read(src)
    took = []

    def css(m):
        href = m.group(1)
        p = os.path.join(HERE, href)
        if not os.path.exists(p):
            return m.group(0)
        took.append(href)
        return '<style>\n/* ' + href + ' */\n' + read(p) + '\n</style>'

    html = re.sub(r'<link rel="stylesheet" href="([^"]+)">', css, html)

    def js(m):
        srcp = m.group(1)
        p = os.path.join(HERE, srcp)
        if not os.path.exists(p):
            return m.group(0)
        took.append(srcp)
        return '<script>\n/* ' + srcp + ' */\n' + read(p) + '\n</script>'

    html = re.sub(r'<script src="([^"]+)"></script>', js, html)

    # Media referenced from JavaScript. The page builds its audio paths
    # by hand - 'sounds/' + name + '.wav' - so there is no tag to
    # rewrite. A table of every file in sounds/, injected before the
    # script that uses it, is the version that does not require the page
    # to be written differently for the sake of this script.
    sdir = os.path.join(HERE, 'sounds')
    if os.path.isdir(sdir) and "sounds/" in html:
        table = {}
        for f in sorted(os.listdir(sdir)):
            if f.endswith('.wav'):
                table['sounds/' + f] = data_uri(os.path.join(sdir, f))
                took.append('sounds/' + f)
        inject = ('<script>\n/* sounds/, inlined so this file travels '
                  'alone */\nwindow.LP_ASSETS = {\n' +
                  ',\n'.join('  "%s": "%s"' % (k, v) for k, v in table.items()) +
                  '\n};\n</script>\n')
        html = html.replace('<script>', inject + '<script>', 1)
        # and make new Audio() look there first
        html = html.replace(
            "const a = new Audio('sounds/' + btn.dataset.s + '.wav');",
            "const key = 'sounds/' + btn.dataset.s + '.wav';\n"
            "  const a = new Audio((window.LP_ASSETS && window.LP_ASSETS[key]) || key);")

    os.makedirs(DIST, exist_ok=True)
    out = os.path.join(DIST, name)
    with open(out, 'w', encoding='utf-8') as f:
        f.write(html)

    print('%-16s %4d KB   %d개 인라인' %
          (name, os.path.getsize(out) // 1024, len(took)))
    left = re.findall(r'(?:href|src)="(?!data:|https?:|#)([^"]+)"', html)
    if left:
        print('  아직 바깥을 가리킴: ' + ', '.join(sorted(set(left))))
    return out


if __name__ == '__main__':
    names = sys.argv[1:] or ['feel.html', 'settings.html']
    for n in names:
        bundle(n)
    print('\n' + DIST)
