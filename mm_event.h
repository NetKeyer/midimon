/*
 * mm_event.h - platform-neutral MIDI event, produced by the backends
 * (ALSA, CoreMIDI, ...) and consumed by the printing/MoMIDI code.
 */
#ifndef MM_EVENT_H
#define MM_EVENT_H

#include <stdint.h>

enum mm_type {
    MM_NOTE_ON, MM_NOTE_OFF, MM_KEY_PRESSURE,      /* note, velocity (pressure) */
    MM_CONTROL,                                    /* param, value              */
    MM_PROGRAM, MM_CHAN_PRESSURE,                  /* value                     */
    MM_PITCH_BEND,                                 /* value: -8192..8191        */
    MM_SONG_POS, MM_SONG_SEL, MM_QFRAME,           /* value                     */
    MM_TUNE_REQUEST,
    MM_CLOCK, MM_START, MM_CONTINUE, MM_STOP, MM_SENSING, MM_RESET,
    MM_SYSEX,                                      /* sysex, sysex_len          */
    MM_DEVICE_LOST, MM_DEVICE_BACK,                /* text = device name; the backend
                                                      is reconnecting by itself  */
    MM_OTHER                                       /* param = native type code  */
};

typedef struct mm_event {
    int type;                 /* enum mm_type                                    */
    uint64_t time_ns;         /* arrival time, ns since the backend was started  */
    int src_client;           /* identifies the source; printed as client:port   */
    int src_port;
    int channel;              /* 0..15 (channel messages only)                   */
    int note, velocity;       /* note messages; velocity = pressure for KEY_PRESSURE */
    int param, value;         /* controller number/value, or single data value   */
    const unsigned char *sysex;   /* full message including F0 ... F7            */
    unsigned sysex_len;
    const char *text;         /* MM_DEVICE_LOST / MM_DEVICE_BACK: device name */
} mm_event;

/* Called by a backend for every event.  The event (and any sysex buffer it
 * points to) is only valid during the call. */
typedef void (*mm_event_cb)(const mm_event *ev);

#endif
