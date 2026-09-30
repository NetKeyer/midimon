# midimon - builds with the MIDI backend for your operating system.
#   Linux : ALSA sequencer   (needs libasound2-dev / alsa-lib-devel)
#   macOS : CoreMIDI         (needs Xcode command line tools: xcode-select --install)

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= /usr/local

UNAME := $(shell uname -s)

ifeq ($(UNAME),Darwin)
  BACKEND = backend_coremidi.c
  LDLIBS += -framework CoreMIDI -framework CoreFoundation
else ifeq ($(UNAME),Linux)
  BACKEND = backend_alsa.c
  LDLIBS += -lasound
else
  $(error Unsupported OS '$(UNAME)': only Linux and macOS have a backend so far)
endif

SRCS = midimon.c mm_parser.c $(BACKEND)
HDRS = backend.h mm_event.h mm_parser.h

midimon: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

install: midimon
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 midimon $(DESTDIR)$(PREFIX)/bin/midimon

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/midimon

clean:
	rm -f midimon

.PHONY: install uninstall clean
