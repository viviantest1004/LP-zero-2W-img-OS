#!/usr/bin/env python3
"""Fetch a Debian base filesystem and unpack it.

Called by `apt setup`. Split out from the C because three of the four
steps here - working out which build is current, streaming a download to
disk, and unpacking xz with device nodes and hard links intact - are
things Python already does correctly and our own tools do not do at all.
Our tar reads uncompressed ustar, which is the right amount of tar for
what pkg needs and nowhere near enough for a Debian root.

  apt-setup.py <arch> <target-dir> [ca-bundle]

The archive is a Debian root filesystem built by linuxcontainers.org out
of Debian's own packages - what `debootstrap` would produce, already
made. That matters on a board where debootstrap would take half an hour
and needs tools this system does not have.
"""
import os
import re
import ssl
import sys
import tarfile
import urllib.request

BASE = "https://images.linuxcontainers.org/images/debian/bookworm"


def fail(msg, *hints):
    print(f"apt-setup: {msg}", file=sys.stderr)
    for h in hints:
        print(f"apt-setup:   {h}", file=sys.stderr)
    sys.exit(1)


def opener(ca_bundle):
    """HTTPS that actually checks the certificate.

    An unverified download of a root filesystem is a root filesystem
    chosen by whoever answers first, and everything installed into it
    afterwards inherits that choice. If there are no certificates to
    check against, stop - the alternative is trusting the network
    silently, which is the failure people find out about last.
    """
    if ca_bundle and os.path.exists(ca_bundle):
        return urllib.request.build_opener(urllib.request.HTTPSHandler(
            context=ssl.create_default_context(cafile=ca_bundle)))
    try:
        ctx = ssl.create_default_context()
        ctx.load_default_certs()
    except Exception:
        fail("no CA certificates to check the download against",
             "they normally live in /data/ssl/cert.pem")
    return urllib.request.build_opener(urllib.request.HTTPSHandler(context=ctx))


def latest_build(url_opener, arch):
    """Which dated build is newest.

    The image server keeps one directory per build, named by date, and
    offers no 'current' link. Taking the last name in sorted order works
    because the date format sorts chronologically - that is what it is
    for - and it means this does not go stale the way a pinned date
    would.
    """
    index = f"{BASE}/{arch}/default/"
    try:
        with url_opener.open(index, timeout=60) as r:
            html = r.read().decode("utf-8", "replace")
    except Exception as e:
        fail(f"cannot read the image index: {e}",
             "`net` says whether this machine can reach anything, and",
             "HTTPS fails outright when the clock is far wrong - try `ntp`")

    builds = sorted(set(re.findall(r"\d{8}_\d{2}:\d{2}", html)))
    if not builds:
        fail("the image index had no builds in it", f"looked at {index}")
    return builds[-1]


def download(url_opener, url, dest):
    print(f"apt-setup: fetching {url}")
    try:
        with url_opener.open(url, timeout=120) as r:
            total = int(r.headers.get("Content-Length") or 0)
            done = step = 0
            with open(dest, "wb") as out:
                while True:
                    chunk = r.read(256 * 1024)
                    if not chunk:
                        break
                    out.write(chunk)
                    done += len(chunk)
                    # 30MB over a Zero 2 W's WiFi takes long enough that
                    # silence reads as a hang.
                    if total and done - step >= 4 * 1024 * 1024:
                        step = done
                        print(f"apt-setup:   {done // 1048576}"
                              f" of {total // 1048576} MB")
    except Exception as e:
        if os.path.exists(dest):
            os.unlink(dest)
        fail(f"the download failed: {e}")
    return dest


def extract(archive, target):
    print(f"apt-setup: unpacking into {target}")
    os.makedirs(target, exist_ok=True)
    try:
        with tarfile.open(archive) as t:
            # fully_trusted, deliberately: a Debian root has to keep its
            # device nodes and setuid bits, and the safe filters strip
            # exactly those. The archive arrived over a checked HTTPS
            # connection from the image server, which is the same trust
            # apt itself runs on from then on.
            kw = {"filter": "fully_trusted"} if sys.version_info >= (3, 12) else {}
            t.extractall(target, **kw)
    except Exception as e:
        fail(f"could not unpack it: {e}",
             "a half-unpacked tree is worse than none -",
             f"`rm -rf {target}` and try again")


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    arch = sys.argv[1]
    target = sys.argv[2]
    ca = sys.argv[3] if len(sys.argv) > 3 else "/data/ssl/cert.pem"

    o = opener(ca)
    build = latest_build(o, arch)
    print(f"apt-setup: Debian bookworm/{arch}, build {build}")

    url = f"{BASE}/{arch}/default/{build}/rootfs.tar.xz"
    tmp = os.path.join(os.path.dirname(target.rstrip("/")) or "/data",
                       "debian-base.tar.xz")

    download(o, url, tmp)
    extract(tmp, target)
    os.unlink(tmp)

    if not os.path.exists(os.path.join(target, "etc/debian_version")):
        fail("unpacked, but there is no /etc/debian_version in it",
             "that was not a Debian root filesystem")

    print("apt-setup: done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
