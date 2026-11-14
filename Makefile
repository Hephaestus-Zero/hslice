CC      ?= cc
CFLAGS  ?= -std=c11 -O3 -Wall -Wextra -Werror
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

.PHONY: all install uninstall test clean

all: hslice hfind hsearch

hslice: src/hslice.c src/hslice_common.h
	$(CC) $(CFLAGS) -o $@ src/hslice.c

hfind: src/hfind.c src/hslice_common.h
	$(CC) $(CFLAGS) -o $@ src/hfind.c

hsearch: hfind
	ln -sf hfind $@

install: all
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 0755 hslice "$(DESTDIR)$(BINDIR)/hslice"
	install -m 0755 hfind "$(DESTDIR)$(BINDIR)/hfind"
	ln -sf hfind "$(DESTDIR)$(BINDIR)/hsearch"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/hslice" "$(DESTDIR)$(BINDIR)/hfind" "$(DESTDIR)$(BINDIR)/hsearch"

test: all
	python3 -m unittest discover -s tests -v

clean:
	rm -f hslice hfind hsearch *.o
