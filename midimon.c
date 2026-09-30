/*
 * midimon - Linux MIDI event monitor (ALSA sequencer)
 *
 * Build:  gcc -O2 -Wall -o midimon midimon.c -lasound
 * Needs:  libasound2-dev (Debian/Ubuntu)  /  alsa-lib-devel (Fedora)
 *
 * Usage:
 *   midimon -l                 list MIDI input devices/ports
 *   midimon                    list devices, then prompt for a selection
 *   midimon -p 20:0            listen to client 20, port 0
 *   midimon -p "Keystation"    listen by (part of) name
 *   midimon -a                 listen to all devices
 *   Options: -f  hide clock / active-sensing spam
 *            -w  show wall-clock time instead of time-since-start
 *            -r  also show raw bytes for each event
 *            -M  disable MoMIDI decoding (on by default)
 *            -m  show only MoMIDI decoding (hide the normal MIDI lines)
 *
 * Timestamps come from the ALSA sequencer queue, i.e. they are taken by the
 * kernel when the event arrives (nanosecond resolution, printed here to
 * microseconds), not when this program gets scheduled.
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_PORTS 128

static volatile sig_atomic_t running = 1;
static int opt_filter, opt_wall, opt_raw;
static int opt_midi = 1;     /* show normal MIDI decoding (-m turns off) */
static int opt_momidi = 1;   /* show MoMIDI decoding      (-M turns off) */
static struct timespec start_real, start_mono;
static int warned_fallback;

static void on_sig(int s) { (void)s; running = 0; }

static const char *note_names[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

static const char *cc_name(int cc)
{
    switch (cc) {
    case 0:  return "Bank Select MSB";   case 1:  return "Mod Wheel";
    case 2:  return "Breath";            case 4:  return "Foot Ctrl";
    case 5:  return "Portamento Time";   case 6:  return "Data Entry MSB";
    case 7:  return "Volume";            case 8:  return "Balance";
    case 10: return "Pan";               case 11: return "Expression";
    case 32: return "Bank Select LSB";   case 38: return "Data Entry LSB";
    case 64: return "Sustain";           case 65: return "Portamento";
    case 66: return "Sostenuto";         case 67: return "Soft Pedal";
    case 68: return "Legato";            case 69: return "Hold 2";
    case 71: return "Resonance";         case 72: return "Release Time";
    case 73: return "Attack Time";       case 74: return "Cutoff";
    case 91: return "Reverb";            case 93: return "Chorus";
    case 96: return "Data Incr";         case 97: return "Data Decr";
    case 98: return "NRPN LSB";          case 99: return "NRPN MSB";
    case 100: return "RPN LSB";          case 101: return "RPN MSB";
    case 120: return "All Sound Off";    case 121: return "Reset Controllers";
    case 122: return "Local Control";    case 123: return "All Notes Off";
    case 124: return "Omni Off";         case 125: return "Omni On";
    case 126: return "Mono On";          case 127: return "Poly On";
    default: return NULL;
    }
}

static void note_str(int n, char *buf, size_t sz)
{
    snprintf(buf, sz, "%s%d", note_names[n % 12], n / 12 - 1); /* 60 = C4 */
}

/* ---- port listing / selection ---------------------------------------- */

static int list_ports(snd_seq_t *seq, snd_seq_addr_t *addrs, int max, int verbose)
{
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t *pi;
    int own = snd_seq_client_id(seq), n = 0;

    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);

    if (verbose)
        printf("  #  Addr   Device / Port\n  -  -----  -------------\n");

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
            addrs[n].client = snd_seq_port_info_get_client(pi);
            addrs[n].port   = snd_seq_port_info_get_port(pi);
            if (verbose)
                printf(" %2d  %3d:%-2d  %s : %s\n", n, addrs[n].client, addrs[n].port,
                       snd_seq_client_info_get_name(ci), snd_seq_port_info_get_name(pi));
            n++;
        }
    }
    return n;
}

static int subscribe(snd_seq_t *seq, int my_port, int queue, snd_seq_addr_t src)
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

/* ---- output ----------------------------------------------------------- */

/* Get the event's timestamp: kernel-provided if available, else the
 * time we read it. */
static void event_time(const snd_seq_event_t *ev, struct timespec *t)
{
    if ((ev->flags & SND_SEQ_TIME_STAMP_MASK) == SND_SEQ_TIME_STAMP_REAL &&
        (ev->time.time.tv_sec || ev->time.time.tv_nsec)) {
        t->tv_sec  = ev->time.time.tv_sec;
        t->tv_nsec = ev->time.time.tv_nsec;
    } else {
        /* Kernel did not stamp this event: fall back to the system clock
         * at the moment we read it (less precise, but never zero). */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        t->tv_sec  = now.tv_sec - start_mono.tv_sec;
        t->tv_nsec = now.tv_nsec - start_mono.tv_nsec;
        if (t->tv_nsec < 0) { t->tv_sec--; t->tv_nsec += 1000000000L; }
        if (!warned_fallback) {
            warned_fallback = 1;
            fprintf(stderr, "note: kernel timestamps unavailable, using "
                            "read-time timestamps instead\n");
        }
    }
}

static void print_time(const struct timespec *t)
{
    if (opt_wall) {
        long long ns = (long long)start_real.tv_nsec + t->tv_nsec;
        time_t wt = start_real.tv_sec + t->tv_sec + ns / 1000000000LL;
        long us = (ns % 1000000000LL) / 1000;
        struct tm tm;
        localtime_r(&wt, &tm);
        printf("%02d:%02d:%02d.%06ld", tm.tm_hour, tm.tm_min, tm.tm_sec, us);
    } else {
        printf("%6ld.%06ld", (long)t->tv_sec, (long)(t->tv_nsec / 1000));
    }
}

/* ---- MoMIDI (Morse over MIDI) decoding ---------------------------------
 * Spec: https://github.com/NetKeyer/MoMIDI-Spec   (implements v0.1)
 *
 * Keying notes: 20 = left paddle (or straight key), 21 = right paddle,
 * 30 = straight key, 31 = PTT.  Note On = key down, Note Off = key up.
 *
 * Timing: the Note velocity carries the low part of the time (ms) since the
 * previous keying event.  Velocity 0 or 127 means "no timing information";
 * such an event also becomes the epoch ("zero ms") for later timed events.
 * An optional Polyphonic Aftertouch for the same note, sent just before the
 * Note On/Off, carries the high part:
 *
 *      time_ms = velocity + pressure * 126        (no aftertouch = 0)
 *
 * Version: Song Select 0 is a version query; Song Select N (N != 0) reports
 * MoMIDI version N (bits 6..4 major, 3..0 minor).
 *
 * SysEx: MoMIDI assigns its own manufacturer IDs (0x01..0x03 so far).
 *
 * This is a passive monitor: it never answers version queries.
 * Note On with velocity 0 is treated as Note Off (standard MIDI convention).
 * State is tracked per source port.
 */
#define MOMIDI_NKEYS     4
#define MOMIDI_MAX_SRC   32
#define MOMIDI_NOTE_LEFT 20

static const int momidi_notes[MOMIDI_NKEYS] = { 20, 21, 30, 31 };
static const char *const momidi_names[MOMIDI_NKEYS] =
    { "left paddle/straight", "right paddle", "straight key", "PTT" };

static int momidi_key(int note)
{
    for (int i = 0; i < MOMIDI_NKEYS; i++)
        if (momidi_notes[i] == note) return i;
    return -1;
}

struct momidi_state {
    int used, client, port;
    int at_valid, at_note, at_val;            /* pending aftertouch          */
    long long round_ms;                       /* device ms since epoch       */
    int round_valid;                          /* epoch known?                */
    int down_valid[MOMIDI_NKEYS];             /* key-down time known?        */
    long long down_at[MOMIDI_NKEYS];          /* round_ms when key went down */
    int have_prev;
    long long prev_ns;                        /* host time of previous event */
};
static struct momidi_state momidi_src[MOMIDI_MAX_SRC];

static struct momidi_state *momidi_lookup(const snd_seq_addr_t *a)
{
    int i;
    for (i = 0; i < MOMIDI_MAX_SRC; i++)
        if (momidi_src[i].used && momidi_src[i].client == a->client &&
            momidi_src[i].port == a->port)
            return &momidi_src[i];
    for (i = 0; i < MOMIDI_MAX_SRC; i++)
        if (!momidi_src[i].used) {
            memset(&momidi_src[i], 0, sizeof momidi_src[i]);
            momidi_src[i].used = 1;
            momidi_src[i].client = a->client;
            momidi_src[i].port = a->port;
            return &momidi_src[i];
        }
    return NULL;
}

static const char *momidi_mfg(int id)
{
    switch (id) {
    case 0x01: return "Lynovations";
    case 0x02: return "Halibut Electronics";
    case 0x03: return "Remote Ham Radio";
    default:   return NULL;
    }
}

/* Start of a MoMIDI line.  Normally it sits under the MIDI line it decodes;
 * in MoMIDI-only mode there is no MIDI line, so it carries its own
 * timestamp and source. */
static void momidi_prefix(const snd_seq_event_t *ev, const struct timespec *ts)
{
    if (opt_midi) {
        printf("%*s", 15, "");
    } else {
        print_time(ts);
        printf("  %3d:%-2d  ", ev->source.client, ev->source.port);
    }
}

static void momidi_event(const snd_seq_event_t *ev, const struct timespec *ts)
{
    struct momidi_state *st;
    int note, vel, k, down, at = 0, at_used = 0;
    long long now_ns, time_ms = 0;

    /* ---- Song Select: version query / version report ---- */
    if (ev->type == SND_SEQ_EVENT_SONGSEL) {
        int v = ev->data.control.value & 0x7F;
        momidi_prefix(ev, ts);
        printf(">> MoMIDI  ");
        if (v == 0)
            printf("version query (Song Select 0)\n");
        else
            printf("version v%d.%d (0x%02X)\n", (v >> 4) & 7, v & 0xF, v);
        fflush(stdout);
        return;
    }

    /* ---- SysEx: MoMIDI manufacturer IDs ---- */
    if (ev->type == SND_SEQ_EVENT_SYSEX) {
        const unsigned char *d = ev->data.ext.ptr;
        unsigned n = ev->data.ext.len;
        const char *m;
        if (n >= 2 && d[0] == 0xF0 && (m = momidi_mfg(d[1])) != NULL) {
            unsigned payload = n - 2 - (d[n - 1] == 0xF7 ? 1 : 0);
            momidi_prefix(ev, ts);
            printf(">> MoMIDI  SysEx, manufacturer ID 0x%02X (%s), "
                   "%u data bytes\n", d[1], m, payload);
            fflush(stdout);
        }
        return;
    }

    if (ev->type != SND_SEQ_EVENT_KEYPRESS &&
        ev->type != SND_SEQ_EVENT_NOTEON &&
        ev->type != SND_SEQ_EVENT_NOTEOFF)
        return;
    if (!(st = momidi_lookup(&ev->source)))
        return;

    note = ev->data.note.note;
    vel  = ev->data.note.velocity;      /* pressure, for KEYPRESS */

    /* Aftertouch: remember it for the next Note On/Off of the same note. */
    if (ev->type == SND_SEQ_EVENT_KEYPRESS) {
        if (momidi_key(note) >= 0) {
            st->at_valid = 1;
            st->at_note = note;
            st->at_val = vel;
        }
        return;
    }

    /* Any Note On/Off consumes whatever aftertouch was pending. */
    if (st->at_valid && st->at_note == note) {
        at = st->at_val;
        at_used = 1;
    }
    st->at_valid = 0;

    if ((k = momidi_key(note)) < 0)
        return;

    down = (ev->type == SND_SEQ_EVENT_NOTEON && vel != 0);
    now_ns = (long long)ts->tv_sec * 1000000000LL + ts->tv_nsec;

    momidi_prefix(ev, ts);
    printf(">> MoMIDI  %-22s %-4s  ", momidi_names[k], down ? "DOWN" : "UP");

    if (vel == 0 || vel == 127) {
        /* No timing information: this event is the new epoch. */
        st->round_valid = 1;
        st->round_ms = 0;
        for (int i = 0; i < MOMIDI_NKEYS; i++) st->down_valid[i] = 0;
        printf("no timing info (vel %d), new epoch", vel);
        if (at_used && at != 0) printf(" (aftertouch %d ignored)", at);
        if (down) { st->down_valid[k] = 1; st->down_at[k] = 0; }
    } else {
        time_ms = vel + (long long)at * 126;
        if (!st->round_valid) {          /* joined mid-stream: epoch unseen */
            st->round_valid = 1;
            st->round_ms = 0;
            for (int i = 0; i < MOMIDI_NKEYS; i++) st->down_valid[i] = 0;
        }
        st->round_ms += time_ms;
        printf("+%lld ms", time_ms);
        if (at_used) printf(" (%d*126 + %d)", at, vel);
        printf("  epoch+%lld ms", st->round_ms);
        if (down) {
            st->down_valid[k] = 1;
            st->down_at[k] = st->round_ms;
        } else {
            if (st->down_valid[k])
                printf("  held %lld ms", st->round_ms - st->down_at[k]);
            st->down_valid[k] = 0;
        }
    }

    if (st->have_prev)
        printf("  [host +%.3f ms]", (now_ns - st->prev_ns) / 1e6);
    st->have_prev = 1;
    st->prev_ns = now_ns;
    putchar('\n');
    fflush(stdout);
}

static void raw3(const char *fmt, int a, int b, int c)
{
    if (opt_raw) printf("   [");
    if (opt_raw) { printf(fmt, a, b, c); printf("]"); }
}

/* The normal (non-MoMIDI) one-line decode of an event. */
static void print_midi_line(const snd_seq_event_t *ev, const struct timespec *ts)
{
    char nb[8];
    const char *nm;
    int ch = ev->data.note.channel + 1;
    int t = ev->type;

    /* time + source address */
    print_time(ts);
    printf("  %3d:%-2d  ", ev->source.client, ev->source.port);

    switch (t) {
    case SND_SEQ_EVENT_NOTEON:
        note_str(ev->data.note.note, nb, sizeof nb);
        if (ev->data.note.velocity == 0)
            printf("Note Off   ch %2d  %-4s (%3d)  vel 0 (via Note On)",
                   ch, nb, ev->data.note.note);
        else
            printf("Note On    ch %2d  %-4s (%3d)  vel %d",
                   ch, nb, ev->data.note.note, ev->data.note.velocity);
        raw3("%02X %02X %02X", 0x90 | (ch - 1), ev->data.note.note, ev->data.note.velocity);
        break;
    case SND_SEQ_EVENT_NOTEOFF:
        note_str(ev->data.note.note, nb, sizeof nb);
        printf("Note Off   ch %2d  %-4s (%3d)  vel %d",
               ch, nb, ev->data.note.note, ev->data.note.velocity);
        raw3("%02X %02X %02X", 0x80 | (ch - 1), ev->data.note.note, ev->data.note.velocity);
        break;
    case SND_SEQ_EVENT_KEYPRESS:
        note_str(ev->data.note.note, nb, sizeof nb);
        printf("Poly Press ch %2d  %-4s (%3d)  pressure %d",
               ch, nb, ev->data.note.note, ev->data.note.velocity);
        raw3("%02X %02X %02X", 0xA0 | (ch - 1), ev->data.note.note, ev->data.note.velocity);
        break;
    case SND_SEQ_EVENT_CONTROLLER:
        ch = ev->data.control.channel + 1;
        nm = cc_name(ev->data.control.param);
        printf("Control    ch %2d  CC %3d = %3d  %s%s%s", ch,
               ev->data.control.param, ev->data.control.value,
               nm ? "(" : "", nm ? nm : "", nm ? ")" : "");
        raw3("%02X %02X %02X", 0xB0 | (ch - 1), ev->data.control.param, ev->data.control.value);
        break;
    case SND_SEQ_EVENT_PGMCHANGE:
        ch = ev->data.control.channel + 1;
        printf("Prog Chg   ch %2d  program %d", ch, ev->data.control.value);
        raw3("%02X %02X %02X", 0xC0 | (ch - 1), ev->data.control.value, 0);
        break;
    case SND_SEQ_EVENT_CHANPRESS:
        ch = ev->data.control.channel + 1;
        printf("Chan Press ch %2d  pressure %d", ch, ev->data.control.value);
        raw3("%02X %02X %02X", 0xD0 | (ch - 1), ev->data.control.value, 0);
        break;
    case SND_SEQ_EVENT_PITCHBEND:
        ch = ev->data.control.channel + 1;
        printf("Pitch Bend ch %2d  %+6d  (raw %5d)", ch,
               ev->data.control.value, ev->data.control.value + 8192);
        raw3("%02X %02X %02X", 0xE0 | (ch - 1),
             (ev->data.control.value + 8192) & 0x7F, ((ev->data.control.value + 8192) >> 7) & 0x7F);
        break;
    case SND_SEQ_EVENT_CONTROL14:
        ch = ev->data.control.channel + 1;
        printf("Control14  ch %2d  CC %3d/%3d = %5d", ch, ev->data.control.param,
               ev->data.control.param + 32, ev->data.control.value);
        break;
    case SND_SEQ_EVENT_NONREGPARAM:
        ch = ev->data.control.channel + 1;
        printf("NRPN       ch %2d  param %5d = %5d", ch, ev->data.control.param, ev->data.control.value);
        break;
    case SND_SEQ_EVENT_REGPARAM:
        ch = ev->data.control.channel + 1;
        printf("RPN        ch %2d  param %5d = %5d", ch, ev->data.control.param, ev->data.control.value);
        break;
    case SND_SEQ_EVENT_SONGPOS:
        printf("Song Position  %d", ev->data.control.value);
        break;
    case SND_SEQ_EVENT_SONGSEL:
        printf("Song Select    %d", ev->data.control.value);
        break;
    case SND_SEQ_EVENT_QFRAME:
        printf("MTC Quarter Frame  type %d  value %d",
               (ev->data.control.value >> 4) & 7, ev->data.control.value & 0xF);
        break;
    case SND_SEQ_EVENT_TUNE_REQUEST: printf("Tune Request"); break;
    case SND_SEQ_EVENT_CLOCK:        printf("Timing Clock"); break;
    case SND_SEQ_EVENT_START:        printf("Start"); break;
    case SND_SEQ_EVENT_CONTINUE:     printf("Continue"); break;
    case SND_SEQ_EVENT_STOP:         printf("Stop"); break;
    case SND_SEQ_EVENT_SENSING:      printf("Active Sensing"); break;
    case SND_SEQ_EVENT_RESET:        printf("System Reset"); break;
    case SND_SEQ_EVENT_SYSEX: {
        const unsigned char *d = ev->data.ext.ptr;
        unsigned i, n = ev->data.ext.len;
        printf("SysEx      %u bytes:", n);
        for (i = 0; i < n; i++) {
            if (i && i % 24 == 0) printf("\n%*s", 28, "");
            printf(" %02X", d[i]);
        }
        break;
    }
    default:
        printf("Other event (ALSA type %d)", t);
        break;
    }
    putchar('\n');
    fflush(stdout);
}

static void print_event(snd_seq_t *seq, const snd_seq_event_t *ev)
{
    struct timespec ts;
    int t = ev->type;
    (void)seq;

    if (opt_filter && (t == SND_SEQ_EVENT_CLOCK || t == SND_SEQ_EVENT_SENSING))
        return;

    event_time(ev, &ts);
    if (opt_midi)
        print_midi_line(ev, &ts);
    if (opt_momidi)
        momidi_event(ev, &ts);
}

/* ---- main --------------------------------------------------------------- */

static void usage(const char *p)
{
    fprintf(stderr,
        "Usage: %s [-l] [-p client:port | -p name] [-a] [-f] [-w] [-r]\n"
        "  -l  list MIDI input ports and exit\n"
        "  -p  port to monitor (\"20:0\" or a name; repeatable)\n"
        "  -a  monitor all ports\n"
        "  -f  hide Timing Clock / Active Sensing\n"
        "  -w  wall-clock timestamps (default: seconds since start)\n"
        "  -r  show raw MIDI bytes too\n"
        "  -M  disable MoMIDI (Morse over MIDI) decoding\n"
        "  -m  MoMIDI only: hide the normal MIDI decoding (cannot combine with -M)\n", p);
}

int main(int argc, char **argv)
{
    snd_seq_t *seq;
    snd_seq_addr_t avail[MAX_PORTS], chosen[MAX_PORTS];
    const char *specs[MAX_PORTS];
    int nspecs = 0, nchosen = 0, do_list = 0, do_all = 0, opt, err;

    while ((opt = getopt(argc, argv, "lp:afwrmMh")) != -1) {
        switch (opt) {
        case 'l': do_list = 1; break;
        case 'p': if (nspecs < MAX_PORTS) specs[nspecs++] = optarg; break;
        case 'a': do_all = 1; break;
        case 'f': opt_filter = 1; break;
        case 'w': opt_wall = 1; break;
        case 'r': opt_raw = 1; break;
        case 'm': opt_midi = 0; break;
        case 'M': opt_momidi = 0; break;
        default:  usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }

    if (!opt_midi && !opt_momidi) {
        fprintf(stderr, "-m and -M together would display nothing.\n");
        return 1;
    }

    /* DUPLEX (not INPUT-only): starting the timestamp queue requires sending
     * a control event to the system timer port, which needs an output buffer. */
    if ((err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0)) < 0) {
        fprintf(stderr, "Cannot open ALSA sequencer: %s\n", snd_strerror(err));
        return 1;
    }
    snd_seq_set_client_name(seq, "midimon");

    int navail = list_ports(seq, avail, MAX_PORTS, do_list || (!nspecs && !do_all));
    if (do_list) return 0;

    if (navail == 0 && !nspecs) {
        fprintf(stderr, "No MIDI input devices found.\n");
        return 1;
    }

    if (do_all) {
        for (int i = 0; i < navail; i++) chosen[nchosen++] = avail[i];
    } else if (nspecs) {
        for (int i = 0; i < nspecs; i++) {
            snd_seq_addr_t a;
            if (snd_seq_parse_address(seq, &a, specs[i]) < 0) {
                fprintf(stderr, "Cannot find MIDI port '%s'\n", specs[i]);
                return 1;
            }
            chosen[nchosen++] = a;
        }
    } else {
        char line[256];
        printf("\nSelect device number(s) (comma-separated, or 'a' for all): ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) return 1;
        if (line[0] == 'a' || line[0] == 'A') {
            for (int i = 0; i < navail; i++) chosen[nchosen++] = avail[i];
        } else {
            for (char *tok = strtok(line, ", \t\n"); tok; tok = strtok(NULL, ", \t\n")) {
                int idx = atoi(tok);
                if (idx < 0 || idx >= navail || (idx == 0 && tok[0] != '0')) {
                    fprintf(stderr, "Invalid selection '%s'\n", tok);
                    return 1;
                }
                chosen[nchosen++] = avail[idx];
            }
        }
        if (!nchosen) return 1;
    }

    int queue = snd_seq_alloc_named_queue(seq, "midimon");
    if (queue < 0) { fprintf(stderr, "Cannot allocate queue: %s\n", snd_strerror(queue)); return 1; }

    /* Create our input port with kernel timestamping enabled: every event
     * arriving here is stamped with the real time of queue 'queue'. */
    int my_port;
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
            return 1;
        }
        my_port = snd_seq_port_info_get_port(pi);
    }

    for (int i = 0; i < nchosen; i++) {
        if ((err = subscribe(seq, my_port, queue, chosen[i])) < 0) {
            fprintf(stderr, "Cannot subscribe to %d:%d: %s\n",
                    chosen[i].client, chosen[i].port, snd_strerror(err));
            return 1;
        }
    }

    if ((err = snd_seq_start_queue(seq, queue, NULL)) < 0 ||
        (err = snd_seq_drain_output(seq)) < 0)
        fprintf(stderr, "warning: cannot start queue: %s\n", snd_strerror(err));
    clock_gettime(CLOCK_REALTIME, &start_real);
    clock_gettime(CLOCK_MONOTONIC, &start_mono);

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    printf("\nListening on:");
    for (int i = 0; i < nchosen; i++) {
        snd_seq_client_info_t *ci;
        snd_seq_client_info_alloca(&ci);
        if (snd_seq_get_any_client_info(seq, chosen[i].client, ci) == 0)
            printf(" [%d:%d %s]", chosen[i].client, chosen[i].port, snd_seq_client_info_get_name(ci));
    }
    printf("\nCtrl-C to quit.\n\n%s   Source  Event\n",
           opt_wall ? "  Wall clock     " : "  Seconds     ");

    int npfd = snd_seq_poll_descriptors_count(seq, POLLIN);
    struct pollfd *pfd = alloca(npfd * sizeof *pfd);
    snd_seq_poll_descriptors(seq, pfd, npfd, POLLIN);

    while (running) {
        if (poll(pfd, npfd, 500) <= 0)
            continue;
        snd_seq_event_t *ev;
        do {
            err = snd_seq_event_input(seq, &ev);
            if (err == -ENOSPC) { fprintf(stderr, "*** input overrun, events lost ***\n"); continue; }
            if (err < 0) break;
            if (ev) print_event(seq, ev);
        } while (snd_seq_event_input_pending(seq, 0) > 0);
    }

    snd_seq_close(seq);
    putchar('\n');
    return 0;
}
