/*
 * backend.h - what midimon needs from an operating-system MIDI backend.
 * Exactly one backend is linked in:
 *      backend_alsa.c      Linux  (ALSA sequencer)
 *      backend_coremidi.c  macOS  (CoreMIDI)
 */
#ifndef BACKEND_H
#define BACKEND_H

#include <signal.h>
#include <stddef.h>
#include "mm_event.h"

/* One MIDI input port ("source") that can be listened to. */
typedef struct mm_port {
    int client, port;         /* backend-specific address, shown as client:port */
    char device[96];          /* device / client name */
    char name[96];            /* port name            */
} mm_port;

extern const char *const backend_name;        /* e.g. "ALSA sequencer" */
extern const char *const backend_port_help;   /* how -p may name a port */

/* All return 0 on success, -1 on failure (after printing a message),
 * except backend_list which returns the number of ports found. */
int  backend_open(void);
int  backend_list(mm_port *out, int max);
int  backend_resolve(const char *spec, mm_port *out);
void backend_describe(const mm_port *p, char *buf, size_t n);
int  backend_start(const mm_port *sel, int n);   /* prepare; start the clock */
void backend_run(mm_event_cb cb, volatile sig_atomic_t *running);
void backend_close(void);

#endif
