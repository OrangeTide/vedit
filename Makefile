# Makefile for vedit -- a single-file text editor.

CC      ?= cc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -O2
LDFLAGS ?=
PREFIX  ?= $(HOME)/.local
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/share/man

PROG = vedit
SRC  = vedit.c
HDR  = vedit.h

# Every build lands in its own directory under OUT, keyed by the target
# triple, so a native, a static musl, and a cross build never overwrite each
# other and each stays up to date on its own. TARGET defaults to what the
# compiler reports (the static mode below sets its own). A `vedit` symlink at
# the top level always points at the most recent build, for the scripts and
# docs that run ./vedit.
OUT    ?= _out
TARGET ?= $(shell $(CC) -dumpmachine)
OUTBIN  = $(OUT)/$(TARGET)/bin
# A Windows cross build (make CC=x86_64-w64-mingw32-gcc) gets the .exe suffix.
# gvedit needs the SDL3 mingw package as well: make gvedit CC=... with
# SDL_CFLAGS=-I<pkg>/include and SDL_LIBS="-L<pkg>/lib -lSDL3 -lmingw32
# -mwindows", as the gvedit-windows CI job does.
EXE     = $(if $(findstring mingw,$(TARGET)),.exe,)
BIN     = $(OUTBIN)/$(PROG)$(EXE)

# The graphical editor: the same source with the guterm window binding
# (VEDIT_GUI) in place of the tty one. Needs SDL3; built only on request
# with `make gvedit`, so a plain build never needs it.
GPROG   = gvedit
GBIN    = $(OUTBIN)/$(GPROG)$(EXE)
# Asked of pkg-config only when the gvedit recipe runs (recursive
# assignment), so a plain make never calls it.
SDL_CFLAGS = $(shell pkg-config --cflags sdl3 2>/dev/null)
SDL_LIBS   = $(shell pkg-config --libs sdl3 2>/dev/null)
# Where the vendored guterm.h is refreshed from with `make guterm-sync`.
GUTERM_DIR ?= ../guterm

# Static build against musl, handy for dropping the binary onto a server:
#   make static            (or: make STATIC=1)
#   make static install    installs that binary
# Naming `static` among the goals turns the mode on for the whole run, so the
# other goals on the same line (install, screenshots) see the same binary.
# The target is named explicitly because musl-gcc reports the host triple.
ifneq ($(filter static,$(MAKECMDGOALS)),)
STATIC = 1
endif
ifdef STATIC
CC      = musl-gcc
TARGET  = $(shell $(CC) -dumpmachine | cut -d- -f1)-linux-musl
LDFLAGS += -static
RELEASE = 1
endif

ifdef RELEASE
CFLAGS += -O2 -DNDEBUG
else
CFLAGS += -g
endif

# Embedded VT terminal panel (shell / build output in a buffer) is built by
# default (vedit.c defines VEDIT_TERM unless opted out). A primitive embedding
# host that wants none of the PTY/emulator code opts out with
# `make VEDIT_NO_TERM=1`.
ifdef VEDIT_NO_TERM
CFLAGS += -DVEDIT_NO_TERM
endif
# The art view (.ans files as a cell grid) is likewise built by default;
# `make VEDIT_NO_ART=1` drops it. The VT emulator stays as long as either
# feature is in.
ifdef VEDIT_NO_ART
CFLAGS += -DVEDIT_NO_ART
endif

TESTDIR  = tests
TESTBINS = $(TESTDIR)/test_unit $(TESTDIR)/test_render
# Tests include vedit.c as one unit, so they build with the same warnings.
TESTCFLAGS = -std=gnu11 -Wall -Wextra -g

# The terminal-buffer tests build vedit.c with the terminal panel enabled, plus
# the test-only term_attach() injection point that stands a pipe in for a PTY.
TERMTESTBIN = $(TESTDIR)/test_term
TERMTESTCFLAGS = $(TESTCFLAGS) -DVEDIT_TERM -DVEDIT_TEST

# Allocation-failure tests for the terminal code. The linker redirects calloc
# and realloc to the __wrap_ versions in the test so a case can fail a single
# allocation and drive the cleanup paths, with no change to vedit.c.
TERMFAULTBIN = $(TESTDIR)/test_termfault
TERMFAULTLDFLAGS = -Wl,--wrap=calloc,--wrap=realloc

# The torture/fuzz suite includes vedit.c directly. A smaller per-search step
# budget keeps a pathological fuzz pattern from dominating the run time while
# still exercising the regex abort path.
TORTURE_CFLAGS = -DRX_STEP_LIMIT=2000000
TORTURE_ROUNDS ?= 20000

# Shared flags for the sanitizer builds of the tests and the torture suite.
SANCFLAGS = -std=gnu11 -Wall -Wextra -g -O1 -fno-omit-frame-pointer

.PHONY: all link clean install uninstall test torture asan ubsan cov cov-term \
    screenshots static gvedit veditmin wasm web guterm-sync

all: $(BIN) link

$(BIN): $(SRC) $(HDR) | $(OUTBIN)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

$(OUTBIN):
	mkdir -p $@

# veditmin: a reduced native build for embedding vedit in a host (such as a
# MUD) that does not need the full feature set. It keeps the text editor, the
# vi keys, and search, and drops features a host does not need. The dropped
# set grows as the VEDIT_NO_* gates are wired.
VEDITMIN_DEFS = -DVEDIT_NO_TERM -DVEDIT_NO_ART -DVEDIT_NO_TOOLS \
    -DVEDIT_NO_MAIL -DVEDIT_NO_MOUSE -DVEDIT_NO_XDG -DVEDIT_NO_HEX \
    -DVEDIT_NO_SWAP
MINBIN = $(OUTBIN)/veditmin$(EXE)

veditmin: $(MINBIN)
	$(if $(EXE),,ln -sfn $(MINBIN) veditmin)

$(MINBIN): $(SRC) $(HDR) | $(OUTBIN)
	$(CC) $(CFLAGS) $(VEDITMIN_DEFS) -o $@ $(SRC) $(LDFLAGS)

# Point the top-level symlink at this target's binary. Always rerun, so
# `make static` and a plain `make` each leave ./vedit on the build just made.
link: $(BIN)
	$(if $(EXE),,ln -sfn $(BIN) $(PROG))

static: all

gvedit: $(GBIN)
	$(if $(EXE),,ln -sfn $(GBIN) $(GPROG))

$(GBIN): $(SRC) $(HDR) guterm.h | $(OUTBIN)
	$(CC) $(CFLAGS) -DVEDIT_GUI $(if $(EXE),,-pthread) $(SDL_CFLAGS) -o $@ \
	    $(SRC) $(SDL_LIBS) -lm \
	    $(LDFLAGS)

# A headless CLI vedit for wasm32-wasi, run under a WASI runtime such as
# wasmtime: stdin and stdout carry the key bytes and the cell output, and a
# preopened directory (wasmtime --dir) is the only file system it sees. There
# is no tty, no processes, and no PTY in the sandbox, so the terminal panel,
# tools, art, and mail are compiled out. Built on request with `make wasm`:
#   make wasm
#   wasmtime run --dir . --env COLUMNS=80 --env LINES=24 \
#       _out/wasm32-wasi/bin/vedit.wasm note.txt
# Defaults target Debian's wasi-libc (headers under /usr, builtins from the
# clang resource dir). For a wasi-sdk install, override the toolchain vars:
#   make wasm WASI_CC=$$WASI_SDK/bin/clang \
#       WASI_INC="--sysroot=$$WASI_SDK/share/wasi-sysroot" \
#       WASI_LIBDIR=$$WASI_SDK/share/wasi-sysroot/lib/wasm32-wasi
WASI_CC     ?= clang
WASI_RESDIR  = $(shell $(WASI_CC) -print-resource-dir)
WASI_INC    ?= -nostdinc -isystem /usr/include/wasm32-wasi \
               -isystem $(WASI_RESDIR)/include
WASI_LIBDIR ?= /usr/lib/wasm32-wasi
WASI_BIN     = $(OUT)/wasm32-wasi/bin/vedit.wasm
# WASI has no signals, mmap, or process ids; wasi-libc ships emulated shims.
# -nodefaultlibs stops clang pulling the wasm32 compiler-rt archive (absent in
# this layout; vedit provides the one 128-bit-multiply builtin libc needs).
WASI_DEFS    = -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL \
               -D_WASI_EMULATED_GETPID \
               -DVEDIT_NO_TERM -DVEDIT_NO_ART -DVEDIT_NO_TOOLS -DVEDIT_NO_MAIL
WASI_LIBS    = -lc -lwasi-emulated-mman -lwasi-emulated-signal \
               -lwasi-emulated-getpid

wasm: $(WASI_BIN)

$(WASI_BIN): $(SRC) $(HDR) | $(OUT)/wasm32-wasi/bin
	$(WASI_CC) --target=wasm32-wasi $(WASI_INC) -std=gnu11 -Wall -Wextra \
	    -O2 $(WASI_DEFS) -nodefaultlibs -L$(WASI_LIBDIR) \
	    $(SRC) $(WASI_LIBS) -o $@

$(OUT)/wasm32-wasi/bin:
	mkdir -p $@

# Browser build: vedit compiled with emscripten and wired to an xterm.js
# terminal, editing a file in an in-memory filesystem (MEMFS). ASYNCIFY lets
# the blocking editor loop yield to the browser event loop while it waits for a
# key, so no worker or SharedArrayBuffer (and no COOP/COEP headers) are needed.
# Needs the emscripten SDK on PATH (`emcc`); `make web` then serve web/ with
# any static file server (for example `npx serve web`) and open the page it
# prints. Build artifacts land in web/ beside the committed page and are
# git-ignored.
EMCC        ?= emcc
WEB_SAMPLE  ?= web/sample.txt
WEB_DEFS     = -DVEDIT_NO_TERM -DVEDIT_NO_ART -DVEDIT_NO_TOOLS -DVEDIT_NO_MAIL
WEB_EMFLAGS  = -sASYNCIFY -sINVOKE_RUN=0 -sEXIT_RUNTIME=1 \
               -sEXPORTED_RUNTIME_METHODS=callMain,FS

web: web/vedit.js

web/vedit.js: $(SRC) $(HDR) web/index.html $(WEB_SAMPLE)
	$(EMCC) -std=gnu11 -O2 $(WEB_DEFS) $(WEB_EMFLAGS) \
	    --embed-file $(WEB_SAMPLE)@/note.txt \
	    $(SRC) -o $@

# Refresh the vendored header from a guterm checkout and stamp its commit.
guterm-sync:
	cp $(GUTERM_DIR)/guterm.h guterm.h
	sed -i "1a\\/* vendored from guterm $$(git -C $(GUTERM_DIR) describe --always --dirty) */" guterm.h

# Unit and integration tests, run through the vendored taptest driver.
$(TESTDIR)/taptest: $(TESTDIR)/taptest.c $(TESTDIR)/taptest_selftest.c \
    $(TESTDIR)/taptest.h
	$(CC) $(TESTCFLAGS) -o $@ $(TESTDIR)/taptest.c \
	    $(TESTDIR)/taptest_selftest.c

$(TESTDIR)/test_%: $(TESTDIR)/test_%.c $(TESTDIR)/testmain.c $(TESTDIR)/test.h \
    $(TESTDIR)/memio.h $(SRC) $(HDR)
	$(CC) $(TESTCFLAGS) -o $@ $(TESTDIR)/test_$*.c $(TESTDIR)/testmain.c \
	    $(LDFLAGS)

# Explicit rule (overrides the pattern above) so the terminal tests get the
# VEDIT_TERM/VEDIT_TEST flags.
$(TERMTESTBIN): $(TESTDIR)/test_term.c $(TESTDIR)/testmain.c $(TESTDIR)/test.h \
    $(TESTDIR)/memio.h $(SRC) $(HDR)
	$(CC) $(TERMTESTCFLAGS) -o $@ $(TESTDIR)/test_term.c $(TESTDIR)/testmain.c \
	    $(LDFLAGS)

$(TERMFAULTBIN): $(TESTDIR)/test_termfault.c $(TESTDIR)/testmain.c \
    $(TESTDIR)/test.h $(TESTDIR)/memio.h $(SRC) $(HDR)
	$(CC) $(TERMTESTCFLAGS) -o $@ $(TESTDIR)/test_termfault.c \
	    $(TESTDIR)/testmain.c $(TERMFAULTLDFLAGS) $(LDFLAGS)

# The tests never read a config file, but point the lookup at an empty
# directory anyway so a developer's own settings can never leak in.
# TAPFLAGS passes options to the driver: -v lists every check, so a failing
# one is named in a CI log.
TAPFLAGS ?=
test: $(TESTDIR)/taptest $(TESTBINS) $(TERMTESTBIN) $(TERMFAULTBIN)
	XDG_CONFIG_HOME=$(abspath $(TESTDIR))/no-config VEDIT_CONFIG= \
	    $(TESTDIR)/taptest $(TAPFLAGS) --self-test --exe $(TESTBINS) \
	    $(TERMTESTBIN) $(TERMFAULTBIN)

# Build and run the torture/fuzz suite.
$(TESTDIR)/torturet: $(TESTDIR)/torture.c $(SRC) $(HDR)
	$(CC) $(TESTCFLAGS) $(TORTURE_CFLAGS) -o $@ $(TESTDIR)/torture.c $(LDFLAGS)

torture: $(TESTDIR)/torturet
	$(TESTDIR)/torturet $(TORTURE_ROUNDS)

# Unit, render, and torture suites under AddressSanitizer (with leak
# detection). The tests exit non-zero on failure; the sanitizer aborts on a
# memory fault or leak.
asan:
	$(CC) $(SANCFLAGS) -fsanitize=address $(TESTDIR)/test_unit.c \
	    $(TESTDIR)/testmain.c -o $(TESTDIR)/test_unit-asan
	$(CC) $(SANCFLAGS) -fsanitize=address $(TESTDIR)/test_render.c \
	    $(TESTDIR)/testmain.c -o $(TESTDIR)/test_render-asan
	$(CC) $(SANCFLAGS) -fsanitize=address $(TORTURE_CFLAGS) \
	    $(TESTDIR)/torture.c -o $(TESTDIR)/torture-asan
	$(TESTDIR)/test_unit-asan
	$(TESTDIR)/test_render-asan
	$(TESTDIR)/torture-asan $(TORTURE_ROUNDS)

# The same suites under UndefinedBehaviorSanitizer, aborting on any UB.
ubsan:
	$(CC) $(SANCFLAGS) -fsanitize=undefined -fno-sanitize-recover=all \
	    $(TESTDIR)/test_unit.c $(TESTDIR)/testmain.c \
	    -o $(TESTDIR)/test_unit-ubsan
	$(CC) $(SANCFLAGS) -fsanitize=undefined -fno-sanitize-recover=all \
	    $(TESTDIR)/test_render.c $(TESTDIR)/testmain.c \
	    -o $(TESTDIR)/test_render-ubsan
	$(CC) $(SANCFLAGS) -fsanitize=undefined -fno-sanitize-recover=all \
	    $(TORTURE_CFLAGS) $(TESTDIR)/torture.c -o $(TESTDIR)/torture-ubsan
	$(TESTDIR)/test_unit-ubsan
	$(TESTDIR)/test_render-ubsan
	$(TESTDIR)/torture-ubsan $(TORTURE_ROUNDS)

# Line coverage of the editor from the unit tests, written inside tests/ so the
# coverage artifacts stay out of the top level. Informational, not gated.
cov:
	cd $(TESTDIR) && $(CC) -std=gnu11 -O0 -g --coverage test_unit.c \
	    testmain.c -o test_unit-cov && ./test_unit-cov >/dev/null && \
	    gcov test_unit-cov-test_unit.gcda >/dev/null 2>&1 || true
	@echo "see $(TESTDIR)/vedit.c.gcov for per-line counts"

# Line coverage of the terminal-buffer code: the union of the terminal tests
# and the allocation-failure tests, both built with the feature on. The gate
# for making VEDIT_TERM the default is >90% of the terminal block covered. The
# block runs from the "Terminal buffers" banner to its closing VEDIT_TERM
# #endif, found in the source so the bounds track edits. Informational.
cov-term:
	@cd $(TESTDIR) && \
	$(CC) $(TERMTESTCFLAGS) -O0 --coverage test_term.c testmain.c \
	    -o test_term-cov && ./test_term-cov >/dev/null && \
	    gcov test_term-cov-test_term.gcda >/dev/null 2>&1 && \
	    mv vedit.c.gcov vedit.c.gcov.term && \
	$(CC) $(TERMTESTCFLAGS) -O0 --coverage test_termfault.c testmain.c \
	    $(TERMFAULTLDFLAGS) -o test_termfault-cov && \
	    ./test_termfault-cov >/dev/null && \
	    gcov test_termfault-cov-test_termfault.gcda >/dev/null 2>&1 && \
	    mv vedit.c.gcov vedit.c.gcov.fault && \
	lo=`grep -n '^ \* Terminal buffers$' ../vedit.c | head -1 | cut -d: -f1` && \
	hi=`awk -v lo=$$lo 'NR>lo && /#endif \/\* VEDIT_TERM/{print NR; exit}' \
	    ../vedit.c` && \
	awk -v lo=$$lo -v hi=$$hi 'function load(f){ \
	    while((getline l<f)>0){n=split(l,a,":");c=a[1];ln=a[2]+0; \
	    gsub(/ /,"",c);if(c=="-"||ln==0)continue;e[ln]=1; \
	    if(c!="#####"&&c!="====="&&c+0>0)v[ln]=1}close(f)} \
	    BEGIN{load("vedit.c.gcov.term");load("vedit.c.gcov.fault"); \
	    for(ln=lo;ln<=hi;ln++)if(e[ln]){t++;if(v[ln])c++}; \
	    printf "terminal block (lines %d-%d): %d/%d = %.1f%% covered\n", \
	    lo,hi,c,t,t?100.0*c/t:0}'
	@echo "per-line counts: $(TESTDIR)/vedit.c.gcov.term and .fault"

# Regenerate the README screenshots (docs/shot-*.png) from the built binary.
# Needs Xvfb, xterm, xdotool, and ImageMagick's import on PATH.
screenshots: all
	docs/screenshots.sh ./$(PROG)

clean:
	rm -rf $(OUT)
	rm -f $(PROG) $(GPROG) $(TESTDIR)/taptest $(TESTBINS) $(TERMTESTBIN) \
	    $(TERMFAULTBIN) $(TESTDIR)/test_termfault-cov \
	    $(TESTDIR)/torturet \
	    $(TESTDIR)/test_unit-asan $(TESTDIR)/test_render-asan \
	    $(TESTDIR)/torture-asan $(TESTDIR)/test_unit-ubsan \
	    $(TESTDIR)/test_render-ubsan $(TESTDIR)/torture-ubsan \
	    $(TESTDIR)/test_unit-cov $(TESTDIR)/test_term-cov \
	    $(TESTDIR)/*.gcno $(TESTDIR)/*.gcda $(TESTDIR)/*.gcov \
	    $(TESTDIR)/vedit.c.gcov.term $(TESTDIR)/vedit.c.gcov.fault

install: $(BIN)
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(PROG)
	mkdir -p $(DESTDIR)$(MANDIR)/man1
	install -m 0644 man/vedit.1 $(DESTDIR)$(MANDIR)/man1/vedit.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(PROG)
	rm -f $(DESTDIR)$(MANDIR)/man1/vedit.1
