# midimon - builds with the MIDI backend for your operating system.
#   Linux : ALSA sequencer   (needs libasound2-dev / alsa-lib-devel)
#   macOS : CoreMIDI         (needs Xcode command line tools: xcode-select --install)
#
# The Windows program (midimon.exe, which uses WinMM) is cross-compiled from a
# Linux shell with the MinGW-w64 compiler.  On Windows, use WSL for this:
#   sudo apt install make gcc-mingw-w64-x86-64
#   make windows

CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra
PREFIX   ?= /usr/local
MINGW_CC ?= x86_64-w64-mingw32-gcc

UNAME := $(shell uname -s)

ifeq ($(UNAME),Darwin)
  BACKEND = backend_coremidi.c
  LDLIBS += -framework CoreMIDI -framework CoreFoundation
else ifeq ($(UNAME),Linux)
  BACKEND = backend_alsa.c
  LDLIBS += -lasound
else
  BACKEND =
endif

COMMON = midimon.c mm_parser.c
HDRS   = backend.h mm_event.h mm_parser.h platform.h

all: midimon

midimon: $(COMMON) $(BACKEND) $(HDRS)
ifeq ($(BACKEND),)
	@echo "No native MIDI backend for '$(UNAME)' (only Linux and macOS)." >&2
	@echo "To build the Windows program, use: make windows" >&2
	@exit 1
else
	$(CC) $(CFLAGS) -o $@ $(COMMON) $(BACKEND) $(LDLIBS)
endif

windows: midimon.exe

midimon.exe: $(COMMON) backend_winmm.c $(HDRS)
	@command -v $(MINGW_CC) >/dev/null 2>&1 || { \
	    echo "Cannot find the MinGW-w64 compiler '$(MINGW_CC)'." >&2; \
	    echo "On Debian/Ubuntu (including WSL): sudo apt install make gcc-mingw-w64-x86-64" >&2; \
	    exit 1; }
	$(MINGW_CC) $(CFLAGS) -o $@ $(COMMON) backend_winmm.c -lwinmm

install: midimon
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 midimon $(DESTDIR)$(PREFIX)/bin/midimon

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/midimon

clean:
	rm -f midimon midimon.exe

.PHONY: all windows install uninstall clean
