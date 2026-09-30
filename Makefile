CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  += -lasound
PREFIX  ?= /usr/local
 
midimon: midimon.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
 
install: midimon
	install -D -m 755 midimon $(DESTDIR)$(PREFIX)/bin/midimon
 
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/midimon
 
clean:
	rm -f midimon
 
.PHONY: install uninstall clean
 

