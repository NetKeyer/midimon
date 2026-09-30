@echo off
rem Build midimon with Microsoft Visual C++ (run from a "Developer Command Prompt").
rem NOTE: this script has not been tested; the MinGW-w64 build via "make" has.
cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS midimon.c mm_parser.c backend_winmm.c winmm.lib /Fe:midimon.exe
