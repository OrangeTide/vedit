# Makefile for vedit -- a single-file text editor.

CC      ?= cc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -O2
LDFLAGS ?=
PREFIX  ?= $(HOME)/.local
BINDIR  ?= $(PREFIX)/bin

PROG = vedit
SRC  = vedit.c
HDR  = vedit.h

ifdef RELEASE
CFLAGS += -O2 -DNDEBUG
else
CFLAGS += -g
endif

TESTDIR  = tests
TESTBINS = $(TESTDIR)/test_unit $(TESTDIR)/test_render
# Tests include vedit.c as one unit, so they build with the same warnings.
TESTCFLAGS = -std=gnu11 -Wall -Wextra -g

# The torture/fuzz suite includes vedit.c directly. A smaller per-search step
# budget keeps a pathological fuzz pattern from dominating the run time while
# still exercising the regex abort path.
TORTURE_CFLAGS = -DRX_STEP_LIMIT=2000000
TORTURE_ROUNDS ?= 20000

# Shared flags for the sanitizer builds of the tests and the torture suite.
SANCFLAGS = -std=gnu11 -Wall -Wextra -g -O1 -fno-omit-frame-pointer

.PHONY: all clean install uninstall test torture asan ubsan cov

all: $(PROG)

$(PROG): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

# Static build against musl, handy for dropping the binary onto a server:
#   make static
static:
	$(MAKE) CC=musl-gcc LDFLAGS=-static RELEASE=1

# Unit and integration tests, run through the vendored taptest driver.
$(TESTDIR)/taptest: $(TESTDIR)/taptest.c $(TESTDIR)/taptest_selftest.c \
    $(TESTDIR)/taptest.h
	$(CC) $(TESTCFLAGS) -o $@ $(TESTDIR)/taptest.c \
	    $(TESTDIR)/taptest_selftest.c

$(TESTDIR)/test_%: $(TESTDIR)/test_%.c $(TESTDIR)/testmain.c $(TESTDIR)/test.h \
    $(TESTDIR)/memio.h $(SRC) $(HDR)
	$(CC) $(TESTCFLAGS) -o $@ $(TESTDIR)/test_$*.c $(TESTDIR)/testmain.c \
	    $(LDFLAGS)

test: $(TESTDIR)/taptest $(TESTBINS)
	$(TESTDIR)/taptest --self-test --exe $(TESTBINS)

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

clean:
	rm -f $(PROG) $(TESTDIR)/taptest $(TESTBINS) $(TESTDIR)/torturet \
	    $(TESTDIR)/test_unit-asan $(TESTDIR)/test_render-asan \
	    $(TESTDIR)/torture-asan $(TESTDIR)/test_unit-ubsan \
	    $(TESTDIR)/test_render-ubsan $(TESTDIR)/torture-ubsan \
	    $(TESTDIR)/test_unit-cov $(TESTDIR)/*.gcno $(TESTDIR)/*.gcda \
	    $(TESTDIR)/*.gcov

install: $(PROG)
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 0755 $(PROG) $(DESTDIR)$(BINDIR)/$(PROG)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(PROG)
