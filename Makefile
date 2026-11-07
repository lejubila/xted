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
endif

WARN     = -std=c99 -Wall -Wextra -pedantic
DEFS     = -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
ALLFLAGS = $(WARN) $(DEFS) $(CFLAGS) -ffunction-sections -fdata-sections -MMD -MP
LINK     = $(LDFLAGS) -Wl,--gc-sections

LIBSRC   = src/util.c src/utf8.c src/term.c src/buffer.c src/render.c src/editor.c
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

build/test_core: tests/test_core.c src/util.c src/utf8.c src/term.c src/buffer.c
	@mkdir -p build
	$(CC) $(WARN) $(DEFS) $(CFLAGS) -Isrc -o $@ $^ $(LDFLAGS)

test: build/test_core
	./build/test_core

clean:
	rm -rf build ked

.PHONY: all test clean
-include $(OBJ:.o=.d)
