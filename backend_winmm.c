/*
 * backend_winmm.c - Windows backend, using the classic WinMM midiIn API.
 *
 * WinMM calls our callback on a system thread whenever a device delivers
 * data.  Microsoft's rules for that callback forbid calling almost any
 * system function (including printf), so the callback only records the raw
 * bytes plus a timestamp in a queue.  The main thread (backend_run) takes
 * items off the queue, runs the bytes through the shared byte-stream parser
 * (mm_parser.c) and hands the resulting events to the core.
 *
 * Timestamps: WinMM's own timestamp has only 1 ms resolution, so instead we
 * read QueryPerformanceCounter (sub-microsecond) as the first thing the
 * callback does.  That includes whatever delay there is between the driver
 * receiving the data and WinMM calling us, which is normally well under a
 * millisecond but is not zero.
 *
 * Devices are addressed by their WinMM device number, shown in the listing
 * as "number:0".
 *
 * Limitation of WinMM: most drivers let only ONE program open a MIDI input
 * device at a time.  If another program (a logger, radio software, ...) has
 * the device open, opening it here fails.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend.h"
#include "mm_parser.h"

const char *const backend_name = "Windows WinMM";
const char *const backend_port_help = "a number from the list such as 1 or 1:0, or part of a name";

#define RING_N     1024          /* queue slots between callback and main thread */
#define SYSEX_BUF  1024          /* bytes per SysEx input buffer                 */
#define NBUF       4             /* SysEx input buffers per device               */

struct item {
    int src;                     /* index into sources[]                          */
    LONGLONG qpc;                /* QueryPerformanceCounter when data arrived     */
    unsigned len;                /* bytes in data[]                               */
    MIDIHDR *hdr;                /* SysEx buffer to give back, or NULL            */
    unsigned char data[SYSEX_BUF];
};

struct source {
    int index;                   /* WinMM device number                           */
    int id;                      /* position in sources[]                         */
    HMIDIIN h;
    int prepared;
    mm_parser parser;
    MIDIHDR hdr[NBUF];
    unsigned char buf[NBUF][SYSEX_BUF];
};

static struct source *sources;
static int nsources;

static struct item ring[RING_N];
static int ring_head, ring_tail;             /* guarded by ring_lock */
static int ring_overrun;
static CRITICAL_SECTION ring_lock;
static HANDLE ring_event;

static LONGLONG qpc_freq, start_qpc;

/* ---- helpers ------------------------------------------------------------ */

static void wide_to_utf8(const WCHAR *w, char *out, size_t n)
{
    out[0] = '\0';
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL) <= 0)
        out[0] = '\0';
}

static int fill_port(long index, mm_port *p)
{
    MIDIINCAPSW caps;

    if (index < 0 || (UINT)index >= midiInGetNumDevs())
        return -1;
    if (midiInGetDevCapsW((UINT_PTR)index, &caps, sizeof caps) != MMSYSERR_NOERROR)
        return -1;

    memset(p, 0, sizeof *p);
    p->client = (int)index;
    p->port = 0;
    wide_to_utf8(caps.szPname, p->name, sizeof p->name);
    snprintf(p->device, sizeof p->device, "%s", p->name);   /* WinMM has one name per device */
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

static uint64_t qpc_to_ns(LONGLONG q)
{
    uint64_t d, f = (uint64_t)qpc_freq;

    if (q <= start_qpc)
        return 0;
    d = (uint64_t)(q - start_qpc);
    return (d / f) * 1000000000ULL + (d % f) * 1000000000ULL / f;
}

/* Number of bytes in the short message packed into a MIM_DATA parameter. */
static unsigned short_len(unsigned status)
{
    if (status >= 0xF8) return 1;
    if (status >= 0xF0) return status == 0xF2 ? 3 : (status == 0xF1 || status == 0xF3) ? 2 : 1;
    if (status >= 0x80) return ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) ? 2 : 3;
    return 0;
}

/* ---- the WinMM callback (runs on a system thread; keep it minimal) ------- */

static void CALLBACK midi_proc(HMIDIIN h, UINT msg, DWORD_PTR instance,
                               DWORD_PTR p1, DWORD_PTR p2)
{
    struct source *s = (struct source *)instance;
    struct item *it;
    LARGE_INTEGER now;
    unsigned len = 0, status;
    MIDIHDR *hdr = NULL;
    unsigned char tmp[3];

    (void)h; (void)p2;

    if (msg == MIM_DATA || msg == MIM_MOREDATA) {
        DWORD m = (DWORD)p1;
        status = m & 0xFF;
        len = short_len(status);
        if (!len)
            return;
        tmp[0] = (unsigned char)status;
        tmp[1] = (unsigned char)((m >> 8) & 0xFF);
        tmp[2] = (unsigned char)((m >> 16) & 0xFF);
    } else if (msg == MIM_LONGDATA) {
        hdr = (MIDIHDR *)p1;
        if (hdr->dwBytesRecorded == 0)      /* buffer returned by reset/close */
            return;
    } else {
        return;
    }

    QueryPerformanceCounter(&now);

    EnterCriticalSection(&ring_lock);
    if ((ring_head + 1) % RING_N == ring_tail) {
        ring_overrun = 1;                   /* queue full: drop this event */
        LeaveCriticalSection(&ring_lock);
        return;
    }
    it = &ring[ring_head];
    it->src = s->id;
    it->qpc = now.QuadPart;
    it->hdr = hdr;
    if (hdr) {
        len = hdr->dwBytesRecorded > SYSEX_BUF ? SYSEX_BUF : (unsigned)hdr->dwBytesRecorded;
        memcpy(it->data, hdr->lpData, len);
    } else {
        memcpy(it->data, tmp, len);
    }
    it->len = len;
    ring_head = (ring_head + 1) % RING_N;
    LeaveCriticalSection(&ring_lock);
    SetEvent(ring_event);
}

/* ---- backend interface -------------------------------------------------- */

int backend_open(void)
{
    LARGE_INTEGER f;

    SetConsoleOutputCP(CP_UTF8);            /* device names are printed as UTF-8 */
    InitializeCriticalSection(&ring_lock);
    ring_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!ring_event) {
        fprintf(stderr, "Cannot create event object\n");
        return -1;
    }
    QueryPerformanceFrequency(&f);
    qpc_freq = f.QuadPart;
    return 0;
}

int backend_list(mm_port *out, int max)
{
    int n = 0;
    UINT count = midiInGetNumDevs();

    for (UINT i = 0; i < count && n < max; i++)
        if (fill_port((long)i, &out[n]) == 0)
            n++;
    return n;
}

int backend_resolve(const char *spec, mm_port *out)
{
    char *end;
    long idx = strtol(spec, &end, 10);
    UINT count;

    /* "N" or "N:0" -> device number */
    if (end != spec && (*end == '\0' || strcmp(end, ":0") == 0))
        return fill_port(idx, out);

    /* otherwise: case-insensitive substring of the device name */
    count = midiInGetNumDevs();
    for (UINT i = 0; i < count; i++) {
        mm_port p;
        if (fill_port((long)i, &p) < 0)
            continue;
        if (contains_nocase(p.name, spec)) {
            *out = p;
            return 0;
        }
    }
    return -1;
}

void backend_describe(const mm_port *p, char *buf, size_t n)
{
    snprintf(buf, n, "%s", p->name);
}

static void report_open_error(int index, MMRESULT r)
{
    char text[MAXERRORLENGTH] = "";

    midiInGetErrorTextA(r, text, sizeof text);
    fprintf(stderr, "Cannot open MIDI device %d: %s\n", index, text);
    if (r == MMSYSERR_ALLOCATED)
        fprintf(stderr, "It is probably in use by another program.  Most Windows MIDI "
                        "drivers allow only one program at a time to open a device.\n");
}

int backend_start(const mm_port *sel, int n)
{
    sources = calloc((size_t)n, sizeof *sources);
    if (!sources) {
        fprintf(stderr, "Out of memory\n");
        return -1;
    }

    for (int i = 0; i < n; i++) {
        struct source *s;
        MMRESULT r;
        int dup = 0;

        for (int j = 0; j < nsources; j++)
            if (sources[j].index == sel[i].client)
                dup = 1;
        if (dup)
            continue;                       /* same device named twice */

        s = &sources[nsources];
        s->index = sel[i].client;
        s->id = nsources;
        mm_parser_init(&s->parser, sel[i].client, sel[i].port);
        nsources++;                         /* counted now so backend_close cleans up */

        r = midiInOpen(&s->h, (UINT)s->index, (DWORD_PTR)midi_proc,
                       (DWORD_PTR)s, CALLBACK_FUNCTION);
        if (r != MMSYSERR_NOERROR) {
            s->h = NULL;
            report_open_error(s->index, r);
            return -1;
        }

        for (int b = 0; b < NBUF; b++) {
            s->hdr[b].lpData = (LPSTR)s->buf[b];
            s->hdr[b].dwBufferLength = SYSEX_BUF;
            if (midiInPrepareHeader(s->h, &s->hdr[b], sizeof s->hdr[b]) == MMSYSERR_NOERROR) {
                s->prepared |= 1 << b;
                midiInAddBuffer(s->h, &s->hdr[b], sizeof s->hdr[b]);
            }
        }
    }

    {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        start_qpc = q.QuadPart;
    }
    return 0;
}

void backend_run(mm_event_cb cb, volatile sig_atomic_t *running)
{
    static struct item it;                  /* working copy (too big for the stack) */

    for (int i = 0; i < nsources; i++)
        midiInStart(sources[i].h);

    while (*running) {
        WaitForSingleObject(ring_event, 100);

        for (;;) {
            struct source *s;
            int overrun;

            EnterCriticalSection(&ring_lock);
            overrun = ring_overrun;
            ring_overrun = 0;
            if (ring_tail == ring_head) {
                LeaveCriticalSection(&ring_lock);
                if (overrun)
                    fprintf(stderr, "*** input overrun, events lost ***\n");
                break;
            }
            it = ring[ring_tail];
            ring_tail = (ring_tail + 1) % RING_N;
            LeaveCriticalSection(&ring_lock);

            if (overrun)
                fprintf(stderr, "*** input overrun, events lost ***\n");

            s = &sources[it.src];
            mm_parser_feed(&s->parser, it.data, it.len, qpc_to_ns(it.qpc), cb);

            if (it.hdr && *running)         /* give the SysEx buffer back to WinMM */
                midiInAddBuffer(s->h, it.hdr, sizeof *it.hdr);
        }
    }

    for (int i = 0; i < nsources; i++)
        if (sources[i].h)
            midiInStop(sources[i].h);
}

void backend_close(void)
{
    for (int i = 0; i < nsources; i++) {
        struct source *s = &sources[i];
        if (s->h) {
            midiInReset(s->h);              /* returns any queued SysEx buffers */
            for (int b = 0; b < NBUF; b++)
                if (s->prepared & (1 << b))
                    midiInUnprepareHeader(s->h, &s->hdr[b], sizeof s->hdr[b]);
            midiInClose(s->h);
            s->h = NULL;
        }
        mm_parser_free(&s->parser);
    }
    free(sources);
    sources = NULL;
    nsources = 0;
    if (ring_event) {
        CloseHandle(ring_event);
        ring_event = NULL;
        DeleteCriticalSection(&ring_lock);
    }
}
