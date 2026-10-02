# desktop/calc/Makefile - lp-calc, LP's own calculator (calc.c says why
# there is one). Built inside the Debian base chroot like every desktop
# program here, against its GTK 4.8. Also the terminal's `calc`: the same
# binary under that name (tools/mkdesktop.sh links it).

CC      ?= cc
PKG     ?= pkg-config
PKGS    := gtk4

BIN     ?= lp-calc
SRC     := calc.c
DESKTOP := lp-calc.desktop

CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Werror
CFLAGS  += $(shell $(PKG) --cflags $(PKGS))
LDLIBS  += $(shell $(PKG) --libs $(PKGS)) -lm

PREFIX  ?= /usr/local
BINDIR  := $(DESTDIR)$(PREFIX)/bin
APPDIR  := $(DESTDIR)$(PREFIX)/share/applications

all: $(BIN)

$(BIN): $(SRC)
	@$(PKG) --exists $(PKGS) || \
	    { echo "gtk4 development files are missing: apt install libgtk-4-dev"; exit 1; }
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

install: $(BIN)
	install -D -m 755 $(BIN) $(BINDIR)/lp-calc
	ln -sf lp-calc $(BINDIR)/calc
	install -D -m 644 $(DESKTOP) $(APPDIR)/$(DESKTOP)

clean:
	rm -f lp-calc

.PHONY: all install clean
