/*
 * midimon - MIDI event monitor for Linux (ALSA) and macOS (CoreMIDI)
 *
 * Build:  make            (Linux/macOS: picks the backend for your OS)
 *          make windows    (cross-compiles midimon.exe; e.g. from WSL)
 *          See the Makefile and README.md.
 *
 * Usage:
 *   midimon -l                 list MIDI input devices/ports
 *   midimon                    list devices; use the only one automatically,
 *                              or prompt for a selection if there are several
 *   midimon -p PORT            listen to one port (address or name; see -h)
 *   midimon -a                 listen to all devices
 *   Options: -f  hide clock / active-sensing spam
 *            -w  show wall-clock time instead of time-since-start
 *            -r  also show raw bytes for each event
 *            -M  disable MoMIDI decoding (on by default)
 *            -m  show only MoMIDI decoding (hide the normal MIDI lines)
 *
 * This file is the platform-independent part: option handling, event
 * printing and MoMIDI decoding.  Talking to the operating system's MIDI
 * layer, and timestamping events as they arrive, is done by a backend
 * (backend_alsa.c or backend_coremidi.c); see backend.h.
 */
#include "platform.h"      /* keep first: sets up the Windows CRT */

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "backend.h"

#define MAX_PORTS 128

static volatile sig_atomic_t running = 1;
static int opt_filter, opt_wall, opt_raw;
static int opt_midi = 1;     /* show normal MIDI decoding (-m turns off) */
static int opt_momidi = 1;   /* show MoMIDI decoding      (-M turns off) */
static struct timespec start_real;   /* wall-clock time at time_ns == 0 */

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

/* ---- output ----------------------------------------------------------- */

static void print_time(uint64_t ns)
{
    long sec = (long)(ns / 1000000000ULL);
    long nsec = (long)(ns % 1000000000ULL);

    if (opt_wall) {
        long long tot = (long long)start_real.tv_nsec + nsec;
        time_t wt = start_real.tv_sec + sec + tot / 1000000000LL;
        long us = (tot % 1000000000LL) / 1000;
        struct tm tm;
        plat_localtime(&wt, &tm);
        printf("%02d:%02d:%02d.%06ld", tm.tm_hour, tm.tm_min, tm.tm_sec, us);
    } else {
        printf("%6ld.%06ld", sec, nsec / 1000);
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

static struct momidi_state *momidi_lookup(int client, int port)
{
    int i;
    for (i = 0; i < MOMIDI_MAX_SRC; i++)
        if (momidi_src[i].used && momidi_src[i].client == client &&
            momidi_src[i].port == port)
            return &momidi_src[i];
    for (i = 0; i < MOMIDI_MAX_SRC; i++)
        if (!momidi_src[i].used) {
            memset(&momidi_src[i], 0, sizeof momidi_src[i]);
            momidi_src[i].used = 1;
            momidi_src[i].client = client;
            momidi_src[i].port = port;
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
static void momidi_prefix(const mm_event *ev)
{
    if (opt_midi) {
        printf("%*s", 15, "");
    } else {
        print_time(ev->time_ns);
        printf("  %3d:%-2d  ", ev->src_client, ev->src_port);
    }
}

/* Forget everything known about a source's timing (its device went away). */
static void momidi_reset(int client, int port)
{
    struct momidi_state *st = momidi_lookup(client, port);

    if (st) {
        memset(st, 0, sizeof *st);
        st->used = 1;
        st->client = client;
        st->port = port;
    }
}

static void momidi_event(const mm_event *ev)
{
    struct momidi_state *st;
    int note, vel, k, down, at = 0, at_used = 0;
    long long now_ns, time_ms = 0;

    /* ---- Song Select: version query / version report ---- */
    if (ev->type == MM_SONG_SEL) {
        int v = ev->value & 0x7F;
        momidi_prefix(ev);
        printf(">> MoMIDI  ");
        if (v == 0)
            printf("version query (Song Select 0)\n");
        else
            printf("version v%d.%d (0x%02X)\n", (v >> 4) & 7, v & 0xF, v);
        fflush(stdout);
        return;
    }

    /* ---- SysEx: MoMIDI manufacturer IDs ---- */
    if (ev->type == MM_SYSEX) {
        const unsigned char *d = ev->sysex;
        unsigned n = ev->sysex_len;
        const char *m;
        if (n >= 2 && d[0] == 0xF0 && (m = momidi_mfg(d[1])) != NULL) {
            unsigned payload = n - 2 - (d[n - 1] == 0xF7 ? 1 : 0);
            momidi_prefix(ev);
            printf(">> MoMIDI  SysEx, manufacturer ID 0x%02X (%s), "
                   "%u data bytes\n", d[1], m, payload);
            fflush(stdout);
        }
        return;
    }

    if (ev->type != MM_KEY_PRESSURE &&
        ev->type != MM_NOTE_ON &&
        ev->type != MM_NOTE_OFF)
        return;
    if (!(st = momidi_lookup(ev->src_client, ev->src_port)))
        return;

    note = ev->note;
    vel  = ev->velocity;      /* pressure, for KEYPRESS */

    /* Aftertouch: remember it for the next Note On/Off of the same note. */
    if (ev->type == MM_KEY_PRESSURE) {
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

    down = (ev->type == MM_NOTE_ON && vel != 0);
    now_ns = (long long)ev->time_ns;

    momidi_prefix(ev);
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
static void print_midi_line(const mm_event *ev)
{
    char nb[16];
    const char *nm;
    int ch = ev->channel + 1;
    int t = ev->type;

    /* time + source address */
    print_time(ev->time_ns);
    printf("  %3d:%-2d  ", ev->src_client, ev->src_port);

    switch (t) {
    case MM_NOTE_ON:
        note_str(ev->note, nb, sizeof nb);
        if (ev->velocity == 0)
            printf("Note Off   ch %2d  %-4s (%3d)  vel 0 (via Note On)",
                   ch, nb, ev->note);
        else
            printf("Note On    ch %2d  %-4s (%3d)  vel %d",
                   ch, nb, ev->note, ev->velocity);
        raw3("%02X %02X %02X", 0x90 | (ch - 1), ev->note, ev->velocity);
        break;
    case MM_NOTE_OFF:
        note_str(ev->note, nb, sizeof nb);
        printf("Note Off   ch %2d  %-4s (%3d)  vel %d",
               ch, nb, ev->note, ev->velocity);
        raw3("%02X %02X %02X", 0x80 | (ch - 1), ev->note, ev->velocity);
        break;
    case MM_KEY_PRESSURE:
        note_str(ev->note, nb, sizeof nb);
        printf("Poly Press ch %2d  %-4s (%3d)  pressure %d",
               ch, nb, ev->note, ev->velocity);
        raw3("%02X %02X %02X", 0xA0 | (ch - 1), ev->note, ev->velocity);
        break;
    case MM_CONTROL:
        ch = ev->channel + 1;
        nm = cc_name(ev->param);
        printf("Control    ch %2d  CC %3d = %3d  %s%s%s", ch,
               ev->param, ev->value,
               nm ? "(" : "", nm ? nm : "", nm ? ")" : "");
        raw3("%02X %02X %02X", 0xB0 | (ch - 1), ev->param, ev->value);
        break;
    case MM_PROGRAM:
        ch = ev->channel + 1;
        printf("Prog Chg   ch %2d  program %d", ch, ev->value);
        raw3("%02X %02X %02X", 0xC0 | (ch - 1), ev->value, 0);
        break;
    case MM_CHAN_PRESSURE:
        ch = ev->channel + 1;
        printf("Chan Press ch %2d  pressure %d", ch, ev->value);
        raw3("%02X %02X %02X", 0xD0 | (ch - 1), ev->value, 0);
        break;
    case MM_PITCH_BEND:
        ch = ev->channel + 1;
        printf("Pitch Bend ch %2d  %+6d  (raw %5d)", ch,
               ev->value, ev->value + 8192);
        raw3("%02X %02X %02X", 0xE0 | (ch - 1),
             (ev->value + 8192) & 0x7F, ((ev->value + 8192) >> 7) & 0x7F);
        break;
    case MM_SONG_POS:
        printf("Song Position  %d", ev->value);
        break;
    case MM_SONG_SEL:
        printf("Song Select    %d", ev->value);
        break;
    case MM_QFRAME:
        printf("MTC Quarter Frame  type %d  value %d",
               (ev->value >> 4) & 7, ev->value & 0xF);
        break;
    case MM_TUNE_REQUEST: printf("Tune Request"); break;
    case MM_CLOCK:               printf("Timing Clock"); break;
    case MM_START:               printf("Start"); break;
    case MM_CONTINUE:            printf("Continue"); break;
    case MM_STOP:                printf("Stop"); break;
    case MM_SENSING:             printf("Active Sensing"); break;
    case MM_RESET:               printf("System Reset"); break;
    case MM_SYSEX: {
        const unsigned char *d = ev->sysex;
        unsigned i, n = ev->sysex_len;
        printf("SysEx      %u bytes:", n);
        for (i = 0; i < n; i++) {
            if (i && i % 24 == 0) printf("\n%*s", 28, "");
            printf(" %02X", d[i]);
        }
        break;
    }
    default:
        printf("Other event (native type %d)", ev->param);
        break;
    }
    putchar('\n');
    fflush(stdout);
}


/* A device went away or came back; the backend is handling reconnection. */
static void print_device_event(const mm_event *ev)
{
    print_time(ev->time_ns);
    printf("  %3d:%-2d  ** Device %s: %s%s\n", ev->src_client, ev->src_port,
           ev->type == MM_DEVICE_LOST ? "disconnected" : "reconnected",
           ev->text ? ev->text : "?",
           ev->type == MM_DEVICE_LOST ? " - waiting for it to return" : "");
    fflush(stdout);
}

static void on_event(const mm_event *ev)
{
    int t = ev->type;

    if (t == MM_DEVICE_LOST || t == MM_DEVICE_BACK) {
        /* Always shown, whichever decoding is selected.  The device's own
         * timer restarts when it reconnects, so MoMIDI timing starts over. */
        print_device_event(ev);
        momidi_reset(ev->src_client, ev->src_port);
        return;
    }

    if (opt_filter && (t == MM_CLOCK || t == MM_SENSING))
        return;

    if (opt_midi)
        print_midi_line(ev);
    if (opt_momidi)
        momidi_event(ev);
}

/* ---- main --------------------------------------------------------------- */

static void usage(const char *p)
{
    fprintf(stderr,
        "Usage: %s [-l] [-p PORT] [-a] [-f] [-w] [-r] [-m | -M]\n"
        "  MIDI backend: %s\n"
        "  -l  list MIDI input ports and exit\n"
        "  -p  port to monitor (%s; repeatable)\n"
        "  -a  monitor all ports\n"
        "  -f  hide Timing Clock / Active Sensing\n"
        "  -w  wall-clock timestamps (default: seconds since start)\n"
        "  -r  show raw MIDI bytes too\n"
        "  -M  disable MoMIDI (Morse over MIDI) decoding\n"
        "  -m  MoMIDI only: hide the normal MIDI decoding (cannot combine with -M)\n",
        p, backend_name, backend_port_help);
}

static void print_port_list(const mm_port *ports, int n)
{
    printf("  #  Addr   Device / Port\n  -  -----  -------------\n");
    for (int i = 0; i < n; i++)
        printf(" %2d  %3d:%-2d  %s : %s\n", i, ports[i].client, ports[i].port,
               ports[i].device, ports[i].name);
}

/* Number of ports that are real devices (not software loopbacks). */
static int real_count(const mm_port *ports, int n)
{
    int c = 0;

    for (int i = 0; i < n; i++)
        if (!ports[i].loopback)
            c++;
    return c;
}

int main(int argc, char **argv)
{
    mm_port avail[MAX_PORTS], chosen[MAX_PORTS];
    const char *specs[MAX_PORTS];
    int nspecs = 0, nchosen = 0, do_list = 0, do_all = 0, opt;

    while ((opt = plat_getopt(argc, argv, "lp:afwrmMh")) != -1) {
        switch (opt) {
        case 'l': do_list = 1; break;
        case 'p': if (nspecs < MAX_PORTS) specs[nspecs++] = plat_optarg; break;
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

    if (backend_open() < 0)
        return 1;

    int navail = backend_list(avail, MAX_PORTS);
    if (do_list || (!nspecs && !do_all))
        print_port_list(avail, navail);
    if (do_list)
        return 0;

    if (navail == 0 && !nspecs) {
        fprintf(stderr, "No MIDI input devices found.\n");
        return 1;
    }

    if (do_all) {
        for (int i = 0; i < navail; i++) chosen[nchosen++] = avail[i];
    } else if (nspecs) {
        for (int i = 0; i < nspecs; i++) {
            if (backend_resolve(specs[i], &chosen[nchosen]) < 0) {
                fprintf(stderr, "Cannot find MIDI port '%s'\n", specs[i]);
                return 1;
            }
            nchosen++;
        }
    } else if (real_count(avail, navail) == 1) {
        /* Exactly one real device: no need to ask. */
        for (int i = 0; i < navail; i++)
            if (!avail[i].loopback)
                chosen[nchosen++] = avail[i];
        printf("\nOnly one MIDI device found, using it.\n");
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

    if (backend_start(chosen, nchosen) < 0)
        return 1;
    plat_realtime(&start_real);   /* wall-clock at time_ns == 0 */

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    printf("\nListening on:");
    for (int i = 0; i < nchosen; i++) {
        char desc[200];
        backend_describe(&chosen[i], desc, sizeof desc);
        printf(" [%d:%d %s]", chosen[i].client, chosen[i].port, desc);
    }
    printf("\nCtrl-C to quit.\n\n%s   Source  Event\n",
           opt_wall ? "  Wall clock     " : "  Seconds     ");
    fflush(stdout);

    backend_run(on_event, &running);

    backend_close();
    putchar('\n');
    return 0;
}
