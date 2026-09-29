#!/bin/sh
# get.sh - download the LP installer image for an 8GB USB stick.
#
#   curl -fsSL https://raw.githubusercontent.com/viviantest1004/LP-zero-2W-img-OS/refs/heads/claude/hohho-xvzof5/dist/desktop-8g/get.sh | sh
#
# The same installer as dist/desktop - every install option, the same
# system - in about 6GB instead of 9: 0.5GB free on the stick instead of
# 2GB, and a recovery partition with the recovery menu and its shell but
# without the reinstall payload. The download itself is dist/desktop's
# get.sh, run for this edition.
set -eu
curl -fsSL https://raw.githubusercontent.com/viviantest1004/LP-zero-2W-img-OS/refs/heads/claude/hohho-xvzof5/dist/desktop/get.sh |
    LP_EDITION=8g sh
