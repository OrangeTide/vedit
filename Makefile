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

.PHONY: all clean install uninstall test

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

clean:
	rm -f $(PROG) $(TESTDIR)/taptest $(TESTBINS)

install: $(PROG)
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 0755 $(PROG) $(DESTDIR)$(BINDIR)/$(PROG)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(PROG)
