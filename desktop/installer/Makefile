# desktop/installer/Makefile - the installer window (lp-installer).
#
# Built inside the Debian base, like every GTK program here: it links the
# base's glibc and GTK 4.8, and pkg-config in the chroot is the only way
# to find those (a binary built against the host's GTK 4.14 does not
# start on the image). lp-install, the backend, is Python and needs no
# build.
#
# setup-ui.c is shared with desktop/firstboot, which compiles it from
# here. lp-motion.c and lp-json.c are desktop/common's, compiled in
# rather than linked from liblpshell.a, which is a GTK 3 library.
#
#   make                        (inside the base chroot)
#   make install DESTDIR=/root  (what tools/mkdesktop.sh does)

CC      ?= cc
PKG     ?= pkg-config
PKGS    := gtk4

BIN     := lp-installer
SRC     := lp-installer.c setup-ui.c ../common/lp-motion.c ../common/lp-json.c
HDRS    := setup-ui.h ../common/lp-motion.h ../common/lp-json.h ../theme/motion.css

CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -I. -I../common
CFLAGS  += $(shell $(PKG) --cflags $(PKGS))
LDLIBS  += $(shell $(PKG) --libs $(PKGS)) -lm

PREFIX  ?= /usr/local
BINDIR  := $(DESTDIR)$(PREFIX)/bin

all: $(BIN)

$(BIN): $(SRC) $(HDRS)
	@$(PKG) --exists $(PKGS) || \
	    { echo "gtk4 development files are missing: apt install libgtk-4-dev"; exit 1; }
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

install: $(BIN)
	install -D -m 755 $(BIN) $(BINDIR)/$(BIN)
	install -D -m 755 lp-install $(BINDIR)/lp-install

clean:
	rm -f $(BIN)

.PHONY: all install clean
