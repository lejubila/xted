# The modes (each compiles into its own folder, so that objects of different flags never mix):
#   make            static with musl-gcc, when it is installed: one binary that runs on any Linux
#                   (SPEC: under 600 KB); without musl, dynamic with the system's cc, with a note
#   make DYNAMIC=1  dynamic, with the system's cc (or the CC given): for packages, for development
#   make STATIC=1   static even with another CC (CC=musl-gcc is the default when installed)
#   make SAN=1      AddressSanitizer + UBSan, dynamic with gcc (the sanitizers need glibc)
# A CC given on the command line is used as it is (dynamic, unless STATIC=1).
LDFLAGS ?=
MUSL    := $(shell command -v musl-gcc 2>/dev/null)

ifdef SAN
MODE     = san
else ifdef DYNAMIC
MODE     = dynamic
else ifdef STATIC
MODE     = static
else ifneq ($(origin CC),default)
MODE     = dynamic
else ifneq ($(MUSL),)
MODE     = static
else
MODE     = dynamic
NO_MUSL  = 1
endif

ifeq ($(MODE),san)
ifeq ($(origin CC),default)
CC       = gcc
endif
CFLAGS   = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
BUILD    = build/san
else ifeq ($(MODE),static)
ifeq ($(origin CC),default)
CC       = $(if $(MUSL),musl-gcc,cc)
endif
CFLAGS  ?= -Os
LDFLAGS += -static
BUILD    = build/static
else
CFLAGS  ?= -O2
BUILD    = build
endif

PROG     = xted
PREFIX  ?= /usr/local
MANDIR  ?= $(PREFIX)/share/man

WARN     = -std=c99 -Wall -Wextra -pedantic
DEFS     = -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -DXTED_DATADIR='"$(PREFIX)/share/xted"'
ALLFLAGS = $(WARN) $(DEFS) $(CFLAGS) -ffunction-sections -fdata-sections -MMD -MP
LINK     = $(LDFLAGS) -Wl,--gc-sections

LIBSRC   = src/util.c src/utf8.c src/term.c src/undo.c src/clip.c src/buffer.c \
           src/syntax.c src/langs.c src/theme.c src/doc.c src/layout.c src/fuzzy.c src/picker.c src/files.c src/session.c src/search.c src/find.c src/commands.c src/complete.c src/snippet.c src/cursors.c src/fold.c src/shell.c src/run.c src/errors.c src/keymap.c src/config.c src/git.c src/tags.c src/mouse.c src/symbols.c src/stdsym.c src/indent.c src/editorconfig.c src/cfgsave.c src/i18n.c src/imports.c src/json.c \
           src/render.c src/edit.c src/editor.c
OBJ      = $(LIBSRC:src/%.c=$(BUILD)/%.o) $(BUILD)/main.o


all: $(PROG)

# ./xted is the program of the mode just built: copied each time, so that switching mode (make,
# make DYNAMIC=1, make SAN=1) never leaves the binary of another one
$(PROG): $(BUILD)/$(PROG)
	cp $< $@

$(BUILD)/$(PROG): $(OBJ)
	$(CC) -o $@ $(OBJ) $(LINK)
ifndef SAN
	strip -s $@
endif
ifdef NO_MUSL
	@echo "xted: built dynamic, musl-gcc was not found. For the static binary install musl"
	@echo "      (apt install musl-tools, dnf install musl-gcc, pacman -S musl) and run make again."
endif

$(BUILD)/%.o: src/%.c
	@mkdir -p $(BUILD)
	$(CC) $(ALLFLAGS) -c -o $@ $<

$(BUILD)/test_core: tests/test_core.c $(LIBSRC)
	@mkdir -p $(BUILD)
	$(CC) $(WARN) $(DEFS) $(CFLAGS) -Isrc -o $@ $^ $(LDFLAGS)

test: $(BUILD)/test_core
	./$(BUILD)/test_core

# the editor on a real pseudo-terminal: what is drawn and the files written (needs python3)
smoke: $(PROG)
	python3 tests/smoke/run.py ./$(PROG)

clean:
	rm -rf build $(PROG)

# xt is a short name for the same program: a link, never a second binary. An xt that is not
# ours (another program with that name) is left alone.
install: $(PROG)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(PROG) $(DESTDIR)$(PREFIX)/bin/$(PROG)
	install -d $(DESTDIR)$(PREFIX)/share/xted/lang
	install -m 644 lang/* $(DESTDIR)$(PREFIX)/share/xted/lang/
	install -d $(DESTDIR)$(MANDIR)/man1 $(DESTDIR)$(MANDIR)/it/man1
	install -m 644 man/xted.1 $(DESTDIR)$(MANDIR)/man1/xted.1
	install -m 644 man/it/xted.1 $(DESTDIR)$(MANDIR)/it/man1/xted.1
	@if [ -e $(DESTDIR)$(PREFIX)/bin/xt ] && [ "$$(readlink $(DESTDIR)$(PREFIX)/bin/xt)" != "$(PROG)" ]; then \
	    echo "xt esiste già in $(DESTDIR)$(PREFIX)/bin e non è un collegamento a $(PROG): non lo tocco"; \
	else \
	    ln -sf $(PROG) $(DESTDIR)$(PREFIX)/bin/xt; \
	fi

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(PROG)
	rm -f $(addprefix $(DESTDIR)$(PREFIX)/share/xted/,$(wildcard lang/*))
	-rmdir $(DESTDIR)$(PREFIX)/share/xted/lang $(DESTDIR)$(PREFIX)/share/xted
	rm -f $(DESTDIR)$(MANDIR)/man1/xted.1 $(DESTDIR)$(MANDIR)/it/man1/xted.1
	@if [ "$$(readlink $(DESTDIR)$(PREFIX)/bin/xt)" = "$(PROG)" ]; then rm -f $(DESTDIR)$(PREFIX)/bin/xt; fi

.PHONY: all test smoke clean install uninstall $(PROG)
-include $(OBJ:.o=.d)
