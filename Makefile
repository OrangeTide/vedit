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
BIN     = $(OUTBIN)/$(PROG)

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
    screenshots static

all: $(BIN) link

$(BIN): $(SRC) $(HDR) | $(OUTBIN)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

$(OUTBIN):
	mkdir -p $@

# Point the top-level symlink at this target's binary. Always rerun, so
# `make static` and a plain `make` each leave ./vedit on the build just made.
link: $(BIN)
	ln -sfn $(BIN) $(PROG)

static: all

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

test: $(TESTDIR)/taptest $(TESTBINS) $(TERMTESTBIN) $(TERMFAULTBIN)
	$(TESTDIR)/taptest --self-test --exe $(TESTBINS) $(TERMTESTBIN) \
	    $(TERMFAULTBIN)

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
# block runs from `struct term {` to its closing VEDIT_TERM #endif, found in
# the source so the bounds track edits. Informational.
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
	lo=`grep -n '^struct term {' ../vedit.c | head -1 | cut -d: -f1` && \
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
	rm -f $(PROG) $(TESTDIR)/taptest $(TESTBINS) $(TERMTESTBIN) \
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
