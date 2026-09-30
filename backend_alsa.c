/*
 * backend_alsa.c - Linux backend, using the ALSA sequencer.
 *
 * Timestamps come from an ALSA sequencer queue, i.e. they are taken by the
 * kernel when the event arrives (nanosecond resolution), not when this
 * program gets scheduled.  If the kernel does not stamp an event, the time
 * at which we read it is used instead.
 *
 * Reconnection: when a USB device is unplugged, the kernel removes its ALSA
 * client and with it our subscription; when it is plugged in again it may get
 * a different client number.  So each device we listen to is remembered by
 * NAME.  We notice the loss (our subscription has vanished, usually announced
 * immediately by the System:Announce port), report it, and keep looking for
 * a port with the same names; when it appears we subscribe again.  Events are
 * always labelled with the address the device had when we started, so the
 * output stays consistent across a reconnect.
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

#define CHECK_INTERVAL_MS 250     /* how often to verify / look for devices */

struct want {
    mm_port orig;                 /* names, and the address shown in the output */
    snd_seq_addr_t cur;           /* where the device is right now              */
    int connected;
    char label[200];              /* "device : port", for messages              */
};
static struct want *wants;
static int nwants;

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

/* Does our subscription from 'src' to our input port still exist? */
static int subscription_exists(snd_seq_addr_t src)
{
    snd_seq_port_subscribe_t *sub;
    snd_seq_addr_t dst = { snd_seq_client_id(seq), my_port };

    snd_seq_port_subscribe_alloca(&sub);
    snd_seq_port_subscribe_set_sender(sub, &src);
    snd_seq_port_subscribe_set_dest(sub, &dst);
    return snd_seq_get_port_subscription(seq, sub) == 0;
}

static int addr_equal(snd_seq_addr_t a, snd_seq_addr_t b)
{
    return a.client == b.client && a.port == b.port;
}

/* Is 'a' already being listened to on behalf of a different wanted device? */
static int in_use_by_other(const struct want *w, snd_seq_addr_t a)
{
    for (int i = 0; i < nwants; i++)
        if (&wants[i] != w && wants[i].connected && addr_equal(wants[i].cur, a))
            return 1;
    return 0;
}

/* Look for a readable port whose client and port names match this device. */
static int find_port(const struct want *w, snd_seq_addr_t *out)
{
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t *pi;
    int own = snd_seq_client_id(seq), found = 0;

    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);

    while (snd_seq_query_next_client(seq, ci) >= 0) {
        int c = snd_seq_client_info_get_client(ci);
        if (c == SND_SEQ_CLIENT_SYSTEM || c == own)
            continue;
        if (strcmp(snd_seq_client_info_get_name(ci), w->orig.device) != 0)
            continue;
        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(seq, pi) >= 0) {
            unsigned caps = snd_seq_port_info_get_capability(pi);
            snd_seq_addr_t a = { c, snd_seq_port_info_get_port(pi) };
            if ((caps & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) !=
                (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ))
                continue;
            if (caps & SND_SEQ_PORT_CAP_NO_EXPORT)
                continue;
            if (strcmp(snd_seq_port_info_get_name(pi), w->orig.name) != 0)
                continue;
            if (in_use_by_other(w, a))
                continue;
            if (addr_equal(a, w->cur)) {      /* same place as before: best match */
                *out = a;
                return 0;
            }
            if (!found) {
                *out = a;
                found = 1;
            }
        }
    }
    return found ? 0 : -1;
}

static uint64_t now_ns(void)
{
    struct timespec now;
    int64_t d;

    clock_gettime(CLOCK_MONOTONIC, &now);
    d = (int64_t)(now.tv_sec - start_mono.tv_sec) * 1000000000LL +
        (int64_t)(now.tv_nsec - start_mono.tv_nsec);
    return d < 0 ? 0 : (uint64_t)d;
}

static void notify(mm_event_cb cb, const struct want *w, int type)
{
    mm_event e;

    memset(&e, 0, sizeof e);
    e.type = type;
    e.time_ns = now_ns();
    e.src_client = w->orig.client;
    e.src_port = w->orig.port;
    e.text = w->label;
    cb(&e);
}

/* Verify every device is still subscribed; reconnect any that went away. */
static void maintain(mm_event_cb cb)
{
    for (int i = 0; i < nwants; i++) {
        struct want *w = &wants[i];
        snd_seq_addr_t a;

        if (w->connected && !subscription_exists(w->cur)) {
            w->connected = 0;
            notify(cb, w, MM_DEVICE_LOST);
        }
        if (!w->connected && find_port(w, &a) == 0 && subscribe(a) == 0) {
            w->cur = a;
            w->connected = 1;
            notify(cb, w, MM_DEVICE_BACK);
        }
    }
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

    wants = calloc((size_t)n, sizeof *wants);
    if (!wants) {
        fprintf(stderr, "Out of memory\n");
        return -1;
    }
    nwants = n;

    for (int i = 0; i < n; i++) {
        struct want *w = &wants[i];
        snd_seq_client_info_t *ci;
        snd_seq_port_info_t *pi;

        w->orig = sel[i];
        w->cur.client = sel[i].client;
        w->cur.port = sel[i].port;

        /* Remember the device by name, so it can be found again later. */
        snd_seq_client_info_alloca(&ci);
        snd_seq_port_info_alloca(&pi);
        if (snd_seq_get_any_client_info(seq, w->cur.client, ci) == 0)
            snprintf(w->orig.device, sizeof w->orig.device, "%s", snd_seq_client_info_get_name(ci));
        if (snd_seq_get_any_port_info(seq, w->cur.client, w->cur.port, pi) == 0)
            snprintf(w->orig.name, sizeof w->orig.name, "%s", snd_seq_port_info_get_name(pi));
        if (strcmp(w->orig.device, w->orig.name) == 0)
            snprintf(w->label, sizeof w->label, "%s", w->orig.name);
        else
            snprintf(w->label, sizeof w->label, "%s : %s", w->orig.device, w->orig.name);

        if ((err = subscribe(w->cur)) < 0) {
            fprintf(stderr, "Cannot subscribe to %d:%d: %s\n",
                    sel[i].client, sel[i].port, snd_strerror(err));
            return -1;
        }
        w->connected = 1;
    }

    /* Ask to be told when devices come and go (System:Announce). */
    if ((err = snd_seq_connect_from(seq, my_port, SND_SEQ_CLIENT_SYSTEM,
                                    SND_SEQ_PORT_SYSTEM_ANNOUNCE)) < 0)
        fprintf(stderr, "warning: cannot watch for device changes (%s); "
                        "reconnection will be slightly delayed\n", snd_strerror(err));

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
    if (!warned_fallback) {
        warned_fallback = 1;
        fprintf(stderr, "note: kernel timestamps unavailable, using "
                        "read-time timestamps instead\n");
    }
    return now_ns();
}

/* Translate an ALSA event into a neutral one. */
static void convert(const snd_seq_event_t *ev, mm_event *out)
{
    memset(out, 0, sizeof *out);
    out->time_ns = event_ns(ev);
    out->src_client = ev->source.client;
    out->src_port = ev->source.port;
    for (int i = 0; i < nwants; i++)          /* keep the label we started with */
        if (wants[i].connected && wants[i].cur.client == ev->source.client &&
            wants[i].cur.port == ev->source.port) {
            out->src_client = wants[i].orig.client;
            out->src_port = wants[i].orig.port;
            break;
        }

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
    struct timespec last, now;

    snd_seq_poll_descriptors(seq, pfd, npfd, POLLIN);
    clock_gettime(CLOCK_MONOTONIC, &last);

    while (*running) {
        int check = 0;

        if (poll(pfd, npfd, CHECK_INTERVAL_MS) > 0) {
            snd_seq_event_t *ev;
            do {
                err = snd_seq_event_input(seq, &ev);
                if (err == -ENOSPC) { fprintf(stderr, "*** input overrun, events lost ***\n"); continue; }
                if (err < 0) break;
                if (!ev)
                    continue;
                if (ev->source.client == SND_SEQ_CLIENT_SYSTEM) {
                    check = 1;               /* a client or port came or went */
                } else {
                    mm_event e;
                    convert(ev, &e);
                    cb(&e);
                }
            } while (snd_seq_event_input_pending(seq, 0) > 0);
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        if (check || (now.tv_sec - last.tv_sec) * 1000L +
                     (now.tv_nsec - last.tv_nsec) / 1000000L >= CHECK_INTERVAL_MS) {
            maintain(cb);
            last = now;
        }
    }
}

void backend_close(void)
{
    if (seq)
        snd_seq_close(seq);
    seq = NULL;
    free(wants);
    wants = NULL;
    nwants = 0;
}
