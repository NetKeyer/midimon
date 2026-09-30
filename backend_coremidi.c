/*
 * backend_coremidi.c - macOS backend, using CoreMIDI.
 *
 * CoreMIDI delivers raw MIDI bytes, each packet stamped by the system with
 * the host time (mach_absolute_time units) at which it was received.  We
 * convert that to nanoseconds since start-up and run the bytes through the
 * shared byte-stream parser (mm_parser.c).
 *
 * Sources are addressed by their CoreMIDI source index, shown in the
 * listing as "index:0".
 *
 * NOTE: uses the classic MIDIInputPortCreate() API, which delivers MIDI 1.0
 * byte streams and works on every macOS version.  It is marked deprecated in
 * the macOS 11+ SDK in favour of the MIDI 2.0 event-list API; the
 * deprecation warnings are silenced below.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "backend.h"
#include "mm_parser.h"

#ifdef __clang__
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

const char *const backend_name = "CoreMIDI";
const char *const backend_port_help = "a number from the list such as 1 or 1:0, or part of a name";

struct source {
    mm_parser parser;         /* per-source byte-stream state */
    MIDIEndpointRef ep;
    int index;
};

static MIDIClientRef client;
static MIDIPortRef inport;
static struct source *sources;
static int nsources;
static mach_timebase_info_data_t timebase;
static uint64_t start_host;
static mm_event_cb user_cb;
static pthread_mutex_t cb_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- helpers ------------------------------------------------------------ */

static void get_string(MIDIObjectRef obj, CFStringRef prop, char *buf, size_t n)
{
    CFStringRef s = NULL;

    buf[0] = '\0';
    if (MIDIObjectGetStringProperty(obj, prop, &s) == noErr && s) {
        if (!CFStringGetCString(s, buf, (CFIndex)n, kCFStringEncodingUTF8))
            buf[0] = '\0';
        CFRelease(s);
    }
}

/* Fill in source number 'index'.  Returns 0 if it exists and is online. */
static int fill_port(long index, mm_port *p)
{
    MIDIEndpointRef ep;
    MIDIEntityRef entity = 0;
    MIDIDeviceRef device = 0;
    SInt32 offline = 0;

    if (index < 0 || (unsigned long)index >= (unsigned long)MIDIGetNumberOfSources())
        return -1;
    ep = MIDIGetSource((ItemCount)index);
    if (!ep)
        return -1;
    if (MIDIObjectGetIntegerProperty(ep, kMIDIPropertyOffline, &offline) == noErr && offline)
        return -1;

    memset(p, 0, sizeof *p);
    p->client = (int)index;
    p->port = 0;
    get_string(ep, kMIDIPropertyName, p->name, sizeof p->name);
    if (MIDIEndpointGetEntity(ep, &entity) == noErr && entity &&
        MIDIEntityGetDevice(entity, &device) == noErr && device)
        get_string(device, kMIDIPropertyName, p->device, sizeof p->device);
    if (!p->device[0])
        snprintf(p->device, sizeof p->device, "(virtual)");
    return 0;
}

static int contains_nocase(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);

    if (nl == 0)
        return 1;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i]) {
            char a = hay[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            i++;
        }
        if (i == nl)
            return 1;
    }
    return 0;
}

/* Host time (mach_absolute_time units) -> ns since backend_start(). */
static uint64_t host_to_ns(uint64_t host)
{
    if (host == 0)                    /* 0 means "now" in CoreMIDI */
        host = mach_absolute_time();
    if (host < start_host)
        return 0;
    return (uint64_t)(((__uint128_t)(host - start_host) * timebase.numer) / timebase.denom);
}

/* ---- backend interface -------------------------------------------------- */

int backend_open(void)
{
    /* Nothing to do until backend_start(); listing works without a client. */
    return 0;
}

int backend_list(mm_port *out, int max)
{
    int n = 0;
    unsigned long count = (unsigned long)MIDIGetNumberOfSources();

    for (unsigned long i = 0; i < count && n < max; i++)
        if (fill_port((long)i, &out[n]) == 0)
            n++;
    return n;
}

int backend_resolve(const char *spec, mm_port *out)
{
    char *end;
    long idx = strtol(spec, &end, 10);

    /* "N" or "N:0" -> source index */
    if (end != spec && (*end == '\0' || strcmp(end, ":0") == 0))
        return fill_port(idx, out);

    /* otherwise: case-insensitive substring of "device port" */
    unsigned long count = (unsigned long)MIDIGetNumberOfSources();
    for (unsigned long i = 0; i < count; i++) {
        mm_port p;
        char full[256];
        if (fill_port((long)i, &p) < 0)
            continue;
        snprintf(full, sizeof full, "%s %s", p.device, p.name);
        if (contains_nocase(full, spec)) {
            *out = p;
            return 0;
        }
    }
    return -1;
}

void backend_describe(const mm_port *p, char *buf, size_t n)
{
    if (strcmp(p->device, p->name) == 0 || !p->name[0])
        snprintf(buf, n, "%s", p->device);
    else
        snprintf(buf, n, "%s : %s", p->device, p->name);
}

/* Called on a CoreMIDI thread for each parsed event. */
static void deliver(const mm_event *ev)
{
    pthread_mutex_lock(&cb_lock);
    if (user_cb)
        user_cb(ev);
    pthread_mutex_unlock(&cb_lock);
}

/* Called by CoreMIDI, on its own high-priority thread, with new data. */
static void read_proc(const MIDIPacketList *pl, void *readRefCon, void *srcConnRefCon)
{
    mm_parser *parser = (mm_parser *)srcConnRefCon;
    const MIDIPacket *pk = &pl->packet[0];

    (void)readRefCon;
    for (UInt32 i = 0; i < pl->numPackets; i++) {
        mm_parser_feed(parser, pk->data, pk->length, host_to_ns(pk->timeStamp), deliver);
        pk = MIDIPacketNext(pk);
    }
}

int backend_start(const mm_port *sel, int n)
{
    OSStatus st;

    st = MIDIClientCreate(CFSTR("midimon"), NULL, NULL, &client);
    if (st != noErr) {
        fprintf(stderr, "Cannot create CoreMIDI client (error %d)\n", (int)st);
        return -1;
    }
    st = MIDIInputPortCreate(client, CFSTR("input"), read_proc, NULL, &inport);
    if (st != noErr) {
        fprintf(stderr, "Cannot create CoreMIDI input port (error %d)\n", (int)st);
        return -1;
    }

    sources = calloc((size_t)n, sizeof *sources);
    if (!sources) {
        fprintf(stderr, "Out of memory\n");
        return -1;
    }
    for (int i = 0; i < n; i++) {
        int dup = 0;
        for (int j = 0; j < nsources; j++)
            if (sources[j].index == sel[i].client)
                dup = 1;
        if (dup)
            continue;                 /* same source named twice */
        MIDIEndpointRef ep = MIDIGetSource((ItemCount)sel[i].client);
        if (!ep) {
            fprintf(stderr, "MIDI source %d no longer exists\n", sel[i].client);
            return -1;
        }
        sources[nsources].ep = ep;
        sources[nsources].index = sel[i].client;
        mm_parser_init(&sources[nsources].parser, sel[i].client, sel[i].port);
        nsources++;
    }

    mach_timebase_info(&timebase);
    start_host = mach_absolute_time();
    return 0;
}

void backend_run(mm_event_cb cb, volatile sig_atomic_t *running)
{
    pthread_mutex_lock(&cb_lock);
    user_cb = cb;
    pthread_mutex_unlock(&cb_lock);

    /* Connect only now, so nothing arrives before the caller is ready. */
    for (int i = 0; i < nsources; i++) {
        OSStatus st = MIDIPortConnectSource(inport, sources[i].ep, &sources[i].parser);
        if (st != noErr)
            fprintf(stderr, "Cannot connect to MIDI source %d (error %d)\n",
                    sources[i].index, (int)st);
    }

    /* Events arrive on CoreMIDI's own thread; this thread just waits. */
    while (*running) {
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    for (int i = 0; i < nsources; i++)
        MIDIPortDisconnectSource(inport, sources[i].ep);
    pthread_mutex_lock(&cb_lock);
    user_cb = NULL;
    pthread_mutex_unlock(&cb_lock);
}

void backend_close(void)
{
    if (client)
        MIDIClientDispose(client);    /* also disposes the port */
    client = 0;
    for (int i = 0; i < nsources; i++)
        mm_parser_free(&sources[i].parser);
    free(sources);
    sources = NULL;
    nsources = 0;
}
