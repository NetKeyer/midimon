/*
 * platform.h - the few POSIX-only things the platform-independent core
 * (midimon.c) uses, with Windows equivalents.  Include this first.
 *
 *   plat_getopt / plat_optarg   command-line option parsing
 *   plat_realtime()             current wall-clock time
 *   plat_localtime()            break a time_t into local time
 *
 * On Windows there is no getopt(), so a small one is provided here.  It can
 * be tested on any system by compiling with -DMM_TEST_GETOPT.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- option parsing ------------------------------------------------------- */

#if defined(_WIN32) || defined(MM_TEST_GETOPT)

static char *plat_optarg;
static int plat_optind = 1;
static int plat_optpos;                 /* position inside a "-abc" group */

/* Minimal POSIX-style getopt: single-letter options, "x:" for an argument,
 * grouped flags ("-fw"), attached ("-p3") or separate ("-p 3") arguments,
 * stops at the first non-option or at "--". */
static int plat_getopt(int argc, char *const argv[], const char *optstring)
{
    const char *spec;
    char c;
    int last;

    plat_optarg = NULL;
    if (plat_optpos == 0) {
        if (plat_optind >= argc || argv[plat_optind][0] != '-' || argv[plat_optind][1] == '\0')
            return -1;
        if (strcmp(argv[plat_optind], "--") == 0) {
            plat_optind++;
            return -1;
        }
        plat_optpos = 1;
    }

    c = argv[plat_optind][plat_optpos++];
    last = (argv[plat_optind][plat_optpos] == '\0');
    spec = (c == ':') ? NULL : strchr(optstring, c);

    if (!spec) {
        fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], c);
        if (last) { plat_optind++; plat_optpos = 0; }
        return '?';
    }
    if (spec[1] == ':') {                       /* option takes an argument */
        if (!last) {
            plat_optarg = &argv[plat_optind][plat_optpos];
        } else if (plat_optind + 1 < argc) {
            plat_optarg = argv[++plat_optind];
        } else {
            fprintf(stderr, "%s: option requires an argument -- '%c'\n", argv[0], c);
            plat_optind++; plat_optpos = 0;
            return '?';
        }
        plat_optind++; plat_optpos = 0;
    } else if (last) {
        plat_optind++; plat_optpos = 0;
    }
    return (unsigned char)c;
}

#else  /* POSIX */

#include <unistd.h>
#define plat_getopt getopt
#define plat_optarg optarg
#define plat_optind optind

#endif

/* ---- time ----------------------------------------------------------------- */

#ifdef _WIN32

#include <sys/timeb.h>

/* Wall-clock time; millisecond resolution is plenty for the -w start time. */
static void plat_realtime(struct timespec *ts)
{
    struct _timeb tb;

    _ftime(&tb);
    ts->tv_sec = tb.time;
    ts->tv_nsec = (long)tb.millitm * 1000000L;
}

/* Only ever called from the main thread, so localtime()'s static buffer is fine. */
static void plat_localtime(const time_t *t, struct tm *out)
{
    struct tm *r = localtime(t);

    if (r)
        *out = *r;
    else
        memset(out, 0, sizeof *out);
}

#else

static void plat_realtime(struct timespec *ts)
{
    clock_gettime(CLOCK_REALTIME, ts);
}

static void plat_localtime(const time_t *t, struct tm *out)
{
    localtime_r(t, out);
}

#endif

#endif /* PLATFORM_H */
