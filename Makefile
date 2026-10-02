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

.PHONY: all clean install uninstall

all: $(PROG)

$(PROG): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

# Static build against musl, handy for dropping the binary onto a server:
#   make static
static:
	$(MAKE) CC=musl-gcc LDFLAGS=-static RELEASE=1

clean:
	rm -f $(PROG)

install: $(PROG)
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 0755 $(PROG) $(DESTDIR)$(BINDIR)/$(PROG)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(PROG)
