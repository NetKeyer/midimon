/*
 * backend_alsa.c - Linux backend, using the ALSA sequencer.
 *
 * Timestamps come from an ALSA sequencer queue, i.e. they are taken by the
 * kernel when the event arrives (nanosecond resolution), not when this
 * program gets scheduled.  If the kernel does not stamp an event, the time
 * at which we read it is used instead.
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "backend.h"

const char *const backend_name = "ALSA sequencer";
const char *const backend_port_help = "\"20:0\" or a client name";

static snd_seq_t *seq;
static int my_port = -1, queue = -1;
static struct timespec start_mono;
static int warned_fallback;

int backend_open(void)
{
    int err;

    /* DUPLEX (not INPUT-only): starting the timestamp queue requires sending
     * a control event to the system timer port, which needs an output buffer. */
    if ((err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0)) < 0) {
        fprintf(stderr, "Cannot open ALSA sequencer: %s\n", snd_strerror(err));
        return -1;
    }
    snd_seq_set_client_name(seq, "midimon");
    return 0;
}

int backend_list(mm_port *out, int max)
{
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t *pi;
    int own = snd_seq_client_id(seq), n = 0;

    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);

    while (snd_seq_query_next_client(seq, ci) >= 0) {
        int c = snd_seq_client_info_get_client(ci);
        if (c == SND_SEQ_CLIENT_SYSTEM || c == own)
            continue;
        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(seq, pi) >= 0) {
            unsigned caps = snd_seq_port_info_get_capability(pi);
            if ((caps & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) !=
                (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ))
                continue;
            if (caps & SND_SEQ_PORT_CAP_NO_EXPORT)
                continue;
            if (n >= max)
                return n;
            out[n].client = snd_seq_port_info_get_client(pi);
            out[n].port   = snd_seq_port_info_get_port(pi);
            snprintf(out[n].device, sizeof out[n].device, "%s", snd_seq_client_info_get_name(ci));
            snprintf(out[n].name, sizeof out[n].name, "%s", snd_seq_port_info_get_name(pi));
            out[n].loopback = (strcmp(out[n].device, "Midi Through") == 0);
            n++;
        }
    }
    return n;
}

int backend_resolve(const char *spec, mm_port *out)
{
    snd_seq_addr_t a;

    if (snd_seq_parse_address(seq, &a, spec) < 0)
        return -1;
    memset(out, 0, sizeof *out);
    out->client = a.client;
    out->port = a.port;
    return 0;
}

void backend_describe(const mm_port *p, char *buf, size_t n)
{
    snd_seq_client_info_t *ci;

    snd_seq_client_info_alloca(&ci);
    buf[0] = '\0';
    if (snd_seq_get_any_client_info(seq, p->client, ci) == 0)
        snprintf(buf, n, "%s", snd_seq_client_info_get_name(ci));
}

static int subscribe(snd_seq_addr_t src)
{
    snd_seq_port_subscribe_t *sub;
    snd_seq_addr_t dst = { snd_seq_client_id(seq), my_port };

    snd_seq_port_subscribe_alloca(&sub);
    snd_seq_port_subscribe_set_sender(sub, &src);
    snd_seq_port_subscribe_set_dest(sub, &dst);
    snd_seq_port_subscribe_set_queue(sub, queue);
    snd_seq_port_subscribe_set_time_update(sub, 1);  /* stamp events...      */
    snd_seq_port_subscribe_set_time_real(sub, 1);    /* ...in real time (ns) */
    return snd_seq_subscribe_port(seq, sub);
}

int backend_start(const mm_port *sel, int n)
{
    int err;

    queue = snd_seq_alloc_named_queue(seq, "midimon");
    if (queue < 0) {
        fprintf(stderr, "Cannot allocate queue: %s\n", snd_strerror(queue));
        return -1;
    }

    /* Create our input port with kernel timestamping enabled: every event
     * arriving here is stamped with the real time of queue 'queue'. */
    {
        snd_seq_port_info_t *pi;
        snd_seq_port_info_alloca(&pi);
        snd_seq_port_info_set_name(pi, "input");
        snd_seq_port_info_set_capability(pi, SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE);
        snd_seq_port_info_set_type(pi, SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        snd_seq_port_info_set_timestamping(pi, 1);
        snd_seq_port_info_set_timestamp_real(pi, 1);
        snd_seq_port_info_set_timestamp_queue(pi, queue);
        if ((err = snd_seq_create_port(seq, pi)) < 0) {
            fprintf(stderr, "Cannot create port: %s\n", snd_strerror(err));
            return -1;
        }
        my_port = snd_seq_port_info_get_port(pi);
    }

    for (int i = 0; i < n; i++) {
        snd_seq_addr_t src = { sel[i].client, sel[i].port };
        if ((err = subscribe(src)) < 0) {
            fprintf(stderr, "Cannot subscribe to %d:%d: %s\n",
                    sel[i].client, sel[i].port, snd_strerror(err));
            return -1;
        }
    }

    if ((err = snd_seq_start_queue(seq, queue, NULL)) < 0 ||
        (err = snd_seq_drain_output(seq)) < 0)
        fprintf(stderr, "warning: cannot start queue: %s\n", snd_strerror(err));
    clock_gettime(CLOCK_MONOTONIC, &start_mono);
    return 0;
}

/* Event time in ns since the queue started: kernel-provided if available,
 * else the time we read it. */
static uint64_t event_ns(const snd_seq_event_t *ev)
{
    if ((ev->flags & SND_SEQ_TIME_STAMP_MASK) == SND_SEQ_TIME_STAMP_REAL &&
        (ev->time.time.tv_sec || ev->time.time.tv_nsec))
        return (uint64_t)ev->time.time.tv_sec * 1000000000ULL + ev->time.time.tv_nsec;

    /* Kernel did not stamp this event: fall back to the system clock at the
     * moment we read it (less precise, but never zero). */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!warned_fallback) {
        warned_fallback = 1;
        fprintf(stderr, "note: kernel timestamps unavailable, using "
                        "read-time timestamps instead\n");
    }
    int64_t d = (int64_t)(now.tv_sec - start_mono.tv_sec) * 1000000000LL +
                (int64_t)(now.tv_nsec - start_mono.tv_nsec);
    return d < 0 ? 0 : (uint64_t)d;
}

/* Translate an ALSA event into a neutral one. */
static void convert(const snd_seq_event_t *ev, mm_event *out)
{
    memset(out, 0, sizeof *out);
    out->time_ns = event_ns(ev);
    out->src_client = ev->source.client;
    out->src_port = ev->source.port;

    switch (ev->type) {
    case SND_SEQ_EVENT_NOTEON:
    case SND_SEQ_EVENT_NOTEOFF:
    case SND_SEQ_EVENT_KEYPRESS:
        out->type = ev->type == SND_SEQ_EVENT_NOTEON  ? MM_NOTE_ON :
                    ev->type == SND_SEQ_EVENT_NOTEOFF ? MM_NOTE_OFF : MM_KEY_PRESSURE;
        out->channel = ev->data.note.channel;
        out->note = ev->data.note.note;
        out->velocity = ev->data.note.velocity;
        break;
    case SND_SEQ_EVENT_CONTROLLER:
        out->type = MM_CONTROL;
        out->channel = ev->data.control.channel;
        out->param = ev->data.control.param;
        out->value = ev->data.control.value;
        break;
    case SND_SEQ_EVENT_PGMCHANGE:
    case SND_SEQ_EVENT_CHANPRESS:
    case SND_SEQ_EVENT_PITCHBEND:
        out->type = ev->type == SND_SEQ_EVENT_PGMCHANGE ? MM_PROGRAM :
                    ev->type == SND_SEQ_EVENT_CHANPRESS ? MM_CHAN_PRESSURE : MM_PITCH_BEND;
        out->channel = ev->data.control.channel;
        out->value = ev->data.control.value;
        break;
    case SND_SEQ_EVENT_SONGPOS:
    case SND_SEQ_EVENT_SONGSEL:
    case SND_SEQ_EVENT_QFRAME:
        out->type = ev->type == SND_SEQ_EVENT_SONGPOS ? MM_SONG_POS :
                    ev->type == SND_SEQ_EVENT_SONGSEL ? MM_SONG_SEL : MM_QFRAME;
        out->value = ev->data.control.value;
        break;
    case SND_SEQ_EVENT_TUNE_REQUEST: out->type = MM_TUNE_REQUEST; break;
    case SND_SEQ_EVENT_CLOCK:        out->type = MM_CLOCK; break;
    case SND_SEQ_EVENT_START:        out->type = MM_START; break;
    case SND_SEQ_EVENT_CONTINUE:     out->type = MM_CONTINUE; break;
    case SND_SEQ_EVENT_STOP:         out->type = MM_STOP; break;
    case SND_SEQ_EVENT_SENSING:      out->type = MM_SENSING; break;
    case SND_SEQ_EVENT_RESET:        out->type = MM_RESET; break;
    case SND_SEQ_EVENT_SYSEX:
        out->type = MM_SYSEX;
        out->sysex = ev->data.ext.ptr;
        out->sysex_len = ev->data.ext.len;
        break;
    default:
        out->type = MM_OTHER;
        out->param = ev->type;
        break;
    }
}

void backend_run(mm_event_cb cb, volatile sig_atomic_t *running)
{
    int err;
    int npfd = snd_seq_poll_descriptors_count(seq, POLLIN);
    struct pollfd *pfd = alloca(npfd * sizeof *pfd);
    snd_seq_poll_descriptors(seq, pfd, npfd, POLLIN);

    while (*running) {
        if (poll(pfd, npfd, 500) <= 0)
            continue;
        snd_seq_event_t *ev;
        do {
            err = snd_seq_event_input(seq, &ev);
            if (err == -ENOSPC) { fprintf(stderr, "*** input overrun, events lost ***\n"); continue; }
            if (err < 0) break;
            if (ev) {
                mm_event e;
                convert(ev, &e);
                cb(&e);
            }
        } while (snd_seq_event_input_pending(seq, 0) > 0);
    }
}

void backend_close(void)
{
    if (seq)
        snd_seq_close(seq);
    seq = NULL;
}
