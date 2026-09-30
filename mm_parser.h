/*
 * mm_parser.h - turns a raw MIDI 1.0 byte stream into mm_events.
 * Used by backends that deliver bytes rather than decoded events
 * (CoreMIDI, and later Windows).  Handles running status, realtime bytes
 * interleaved with other messages, and SysEx split across buffers.
 */
#ifndef MM_PARSER_H
#define MM_PARSER_H

#include <stddef.h>
#include "mm_event.h"

typedef struct mm_parser {
    int src_client, src_port;
    unsigned char status;         /* current (running) status byte, or 0 */
    unsigned char data[2];
    int need, have;
    int running_ok;               /* status may be reused (channel messages) */
    int in_sysex, sx_overflow;
    unsigned char *sx;
    size_t sx_len, sx_cap;
} mm_parser;

void mm_parser_init(mm_parser *p, int src_client, int src_port);
void mm_parser_free(mm_parser *p);
/* Forget any partly received message (e.g. after a device reconnects). */
void mm_parser_reset(mm_parser *p);
void mm_parser_feed(mm_parser *p, const unsigned char *buf, size_t n,
                    uint64_t time_ns, mm_event_cb cb);

#endif
