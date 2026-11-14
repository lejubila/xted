CC       ?= gcc
CFLAGS   ?= -O2
LDFLAGS  ?=

# make STATIC=1  -> statically linked binary (e.g. CC=musl-gcc)
# make SAN=1     -> AddressSanitizer + UBSan build, for development
ifdef SAN
CFLAGS   = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif
ifdef STATIC
LDFLAGS += -static
# a static binary is meant to be small (SPEC: under 400 KB): -Os unless CFLAGS is given
ifeq ($(origin CFLAGS),file)
CFLAGS = -Os
endif
endif

WARN     = -std=c99 -Wall -Wextra -pedantic
DEFS     = -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
ALLFLAGS = $(WARN) $(DEFS) $(CFLAGS) -ffunction-sections -fdata-sections -MMD -MP
LINK     = $(LDFLAGS) -Wl,--gc-sections

LIBSRC   = src/util.c src/utf8.c src/term.c src/undo.c src/clip.c src/buffer.c \
           src/syntax.c src/langs.c src/theme.c src/doc.c src/layout.c src/fuzzy.c src/picker.c src/files.c src/session.c src/search.c src/find.c src/commands.c src/complete.c src/snippet.c src/cursors.c src/fold.c src/shell.c src/run.c src/errors.c src/keymap.c src/config.c src/git.c src/tags.c src/mouse.c src/symbols.c src/stdsym.c \
           src/render.c src/edit.c src/editor.c
OBJ      = $(LIBSRC:src/%.c=build/%.o) build/main.o

all: ked

ked: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LINK)
ifndef SAN
	strip -s $@
endif

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(ALLFLAGS) -c -o $@ $<

build/test_core: tests/test_core.c $(LIBSRC)
	@mkdir -p build
	$(CC) $(WARN) $(DEFS) $(CFLAGS) -Isrc -o $@ $^ $(LDFLAGS)

test: build/test_core
	./build/test_core

clean:
	rm -rf build ked

.PHONY: all test clean
-include $(OBJ:.o=.d)
