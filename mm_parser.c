#include <stdlib.h>
#include <string.h>
#include "mm_parser.h"

#define SYSEX_MAX (1u << 20)

void mm_parser_init(mm_parser *p, int src_client, int src_port)
{
    memset(p, 0, sizeof *p);
    p->src_client = src_client;
    p->src_port = src_port;
}

void mm_parser_free(mm_parser *p)
{
    free(p->sx);
    p->sx = NULL;
    p->sx_len = p->sx_cap = 0;
}

static void sx_append(mm_parser *p, unsigned char b)
{
    if (p->sx_len >= SYSEX_MAX) { p->sx_overflow = 1; return; }
    if (p->sx_len == p->sx_cap) {
        size_t cap = p->sx_cap ? p->sx_cap * 2 : 256;
        unsigned char *n = realloc(p->sx, cap);
        if (!n) { p->sx_overflow = 1; return; }
        p->sx = n;
        p->sx_cap = cap;
    }
    p->sx[p->sx_len++] = b;
}

static void emit(mm_parser *p, mm_event *e, int type, uint64_t t, mm_event_cb cb)
{
    e->type = type;
    e->time_ns = t;
    e->src_client = p->src_client;
    e->src_port = p->src_port;
    cb(e);
}

static void dispatch(mm_parser *p, uint64_t t, mm_event_cb cb)
{
    mm_event e;
    int d0 = p->data[0], d1 = p->data[1];
    memset(&e, 0, sizeof e);

    if (p->status < 0xF0) {
        e.channel = p->status & 0x0F;
        switch (p->status & 0xF0) {
        case 0x80: e.note = d0; e.velocity = d1; emit(p, &e, MM_NOTE_OFF, t, cb); break;
        case 0x90: e.note = d0; e.velocity = d1; emit(p, &e, MM_NOTE_ON, t, cb); break;
        case 0xA0: e.note = d0; e.velocity = d1; emit(p, &e, MM_KEY_PRESSURE, t, cb); break;
        case 0xB0: e.param = d0; e.value = d1; emit(p, &e, MM_CONTROL, t, cb); break;
        case 0xC0: e.value = d0; emit(p, &e, MM_PROGRAM, t, cb); break;
        case 0xD0: e.value = d0; emit(p, &e, MM_CHAN_PRESSURE, t, cb); break;
        case 0xE0: e.value = (d0 | (d1 << 7)) - 8192; emit(p, &e, MM_PITCH_BEND, t, cb); break;
        }
    } else {
        switch (p->status) {
        case 0xF1: e.value = d0; emit(p, &e, MM_QFRAME, t, cb); break;
        case 0xF2: e.value = d0 | (d1 << 7); emit(p, &e, MM_SONG_POS, t, cb); break;
        case 0xF3: e.value = d0; emit(p, &e, MM_SONG_SEL, t, cb); break;
        }
    }
}

void mm_parser_feed(mm_parser *p, const unsigned char *buf, size_t n,
                    uint64_t t, mm_event_cb cb)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char b = buf[i];
        mm_event e;

        if (b >= 0xF8) {                 /* realtime: may appear anywhere */
            int type;
            switch (b) {
            case 0xF8: type = MM_CLOCK; break;
            case 0xFA: type = MM_START; break;
            case 0xFB: type = MM_CONTINUE; break;
            case 0xFC: type = MM_STOP; break;
            case 0xFE: type = MM_SENSING; break;
            case 0xFF: type = MM_RESET; break;
            default:   continue;         /* 0xF9, 0xFD undefined */
            }
            memset(&e, 0, sizeof e);
            emit(p, &e, type, t, cb);
            continue;
        }

        if (b & 0x80) {                  /* status byte */
            if (p->in_sysex) {
                if (b == 0xF7) {
                    sx_append(p, b);
                    if (!p->sx_overflow) {
                        memset(&e, 0, sizeof e);
                        e.sysex = p->sx;
                        e.sysex_len = (unsigned)p->sx_len;
                        emit(p, &e, MM_SYSEX, t, cb);
                    }
                    p->in_sysex = 0;
                    p->status = 0;
                    continue;
                }
                p->in_sysex = 0;         /* any other status aborts the SysEx */
            }
            if (b == 0xF0) {
                p->in_sysex = 1;
                p->sx_overflow = 0;
                p->sx_len = 0;
                sx_append(p, b);
                p->status = 0;
                continue;
            }
            if (b >= 0xF4) {             /* F4/F5 undefined, F6 tune, F7 stray */
                p->status = 0;
                if (b == 0xF6) {
                    memset(&e, 0, sizeof e);
                    emit(p, &e, MM_TUNE_REQUEST, t, cb);
                }
                continue;
            }
            p->status = b;
            p->have = 0;
            if (b < 0xF0) {
                p->need = ((b & 0xF0) == 0xC0 || (b & 0xF0) == 0xD0) ? 1 : 2;
                p->running_ok = 1;
            } else {
                p->need = (b == 0xF2) ? 2 : 1;   /* F1, F3: 1;  F2: 2 */
                p->running_ok = 0;
            }
            continue;
        }

        /* data byte */
        if (p->in_sysex) { sx_append(p, b); continue; }
        if (!p->status) continue;        /* orphan data byte */
        p->data[p->have++] = b;
        if (p->have == p->need) {
            dispatch(p, t, cb);
            p->have = 0;
            if (!p->running_ok) p->status = 0;
        }
    }
}
