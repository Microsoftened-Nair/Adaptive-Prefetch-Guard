/*
 * apg.c - Adaptive Prefetch Guard Daemon
 *
 * Detects and remediates Linux page-cache I/O thrashing caused by oversized
 * block-layer read-ahead (e.g. the kernel >=6.x default of 4096 KB on certain
 * platforms). For high-load, memory-mapped random-I/O workloads (RavenDB,
 * RocksDB, etc.) each major page fault prefetches an entire read-ahead block,
 * flooding the page cache with pages that are never consumed. The result is
 * CPU iowait pinning near 100% and throughput regressions of 2.6x-10x.
 *
 * This daemon polls three kernel-exported interfaces:
 *
 *   /proc/stat                              - CPU jiffies, used for %iowait
 *   /proc/diskstats                         - per-device sectors read / I/O time
 *   /sys/block/<dev>/queue/read_ahead_kb    - runtime prefetch threshold
 *
 * When pathological prefetch is detected (sustained high iowait, low per-fault
 * data utilization, oversized read-ahead), it steps the read-ahead down in
 * powers of two via direct POSIX writes to the sysfs attribute -- no shelling
 * out to blockdev(8). Once iowait stabilizes below threshold for a recovery
 * window, it gradually steps read-ahead back up to test for returning spatial
 * locality. All transitions are emitted as one-line JSON records on stderr (or
 * syslog) for downstream observability.
 *
 * Build:  see Makefile, or:
 *         cc -std=gnu11 -O2 -Wall -Wextra -Wpedantic -o apg apg.c
 *
 * Run:    sudo ./apg --device sda
 *         sudo ./apg --device nvme0n1 --interval-ms 250 --syslog
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/* ----------------------------------------------------------------------
 * Compile-time tunables. All overridable from the command line at runtime.
 * -------------------------------------------------------------------- */
#define DEFAULT_LOOP_INTERVAL_MS    500     /* FR-01 default sample period */
#define DEFAULT_SAMPLING_WINDOW_S     5     /* FR-02 iowait sustain window */
#define DEFAULT_RECOVERY_WINDOW_S    60     /* FR-03 stabilization window  */
#define DEFAULT_IOWAIT_HIGH_PCT    50.0     /* FR-02 trigger threshold     */
#define DEFAULT_IOWAIT_LOW_PCT     20.0     /* FR-03 recovery threshold    */
#define DEFAULT_TRIGGER_RA_KB      1024     /* FR-02 RA floor to consider  */
#define DEFAULT_AMP_THRESHOLD       4.0     /* FR-02 prefetch amplification */
#define MIN_RA_KB                   128     /* FR-03 hard floor            */
#define MAX_RA_KB                  4096     /* FR-03 hard ceiling          */
#define PAGE_SIZE_BYTES            4096
#define SECTOR_SIZE_BYTES            512
#define DEVICE_NAME_MAX               64
#define SYSFS_PATH_MAX               256
#define LOG_MSG_MAX                 1024

/* ----------------------------------------------------------------------
 * Daemon state machine
 *
 *   NORMAL       - passive monitoring, no sysfs mutations
 *   REMEDIATING  - actively stepping read_ahead_kb DOWN toward MIN_RA_KB
 *   RECOVERING   - iowait has stabilized; stepping read_ahead_kb UP toward
 *                  MAX_RA_KB to test for returning spatial locality
 * -------------------------------------------------------------------- */
typedef enum {
    STATE_NORMAL = 0,
    STATE_REMEDIATING,
    STATE_RECOVERING
} daemon_state_t;

static const char *state_name(daemon_state_t s)
{
    switch (s) {
    case STATE_NORMAL:      return "NORMAL";
    case STATE_REMEDIATING: return "REMEDIATING";
    case STATE_RECOVERING:  return "RECOVERING";
    default:                return "UNKNOWN";
    }
}

/* ----------------------------------------------------------------------
 * Parsed /proc/stat CPU jiffies (aggregate "cpu " line).
 * -------------------------------------------------------------------- */
typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
} cpu_stat_t;

/* ----------------------------------------------------------------------
 * Parsed /proc/diskstats fields for the target block device.
 * Field numbering follows Documentation/admin-guide/iostats.rst.
 * -------------------------------------------------------------------- */
typedef struct {
    unsigned long long reads_completed;       /* field  4 */
    unsigned long long reads_merged;          /* field  5 */
    unsigned long long sectors_read;          /* field  6 */
    unsigned long long read_ticks_ms;         /* field  7 */
    unsigned long long writes_completed;      /* field  8 */
    unsigned long long writes_merged;         /* field  9 */
    unsigned long long sectors_written;       /* field 10 */
    unsigned long long write_ticks_ms;        /* field 11 */
    unsigned long long ios_in_progress;       /* field 12 */
    unsigned long long io_ticks_ms;           /* field 13 */
    unsigned long long weighted_io_ticks_ms;  /* field 14 */
} disk_stat_t;

/* ----------------------------------------------------------------------
 * Parsed /proc/vmstat counters used to derive application-level demand.
 * -------------------------------------------------------------------- */
typedef struct {
    unsigned long long pgmajfault;   /* major page faults (hit disk) */
    unsigned long long pgfault;      /* total page faults            */
} vmstat_t;

/* ----------------------------------------------------------------------
 * Target device bookkeeping.
 * -------------------------------------------------------------------- */
typedef struct {
    char     name[DEVICE_NAME_MAX];
    char     sysfs_ra_path[SYSFS_PATH_MAX];
    unsigned int current_ra_kb;       /* last value observed from sysfs */
    unsigned int last_written_ra_kb;  /* last value we wrote ourselves  */
} target_device_t;

/* ----------------------------------------------------------------------
 * Runtime configuration (single global; single-threaded main loop).
 * -------------------------------------------------------------------- */
typedef struct {
    int             loop_interval_ms;
    int             sampling_window_s;
    int             recovery_window_s;
    double          iowait_high_pct;
    double          iowait_low_pct;
    double          amp_threshold;
    unsigned int    trigger_ra_kb;
    bool            use_syslog;
    bool            use_json;
    bool            dry_run;
    bool            verbose_metrics;
    target_device_t dev;
    volatile sig_atomic_t running;
} config_t;

static config_t g_cfg = {
    .loop_interval_ms  = DEFAULT_LOOP_INTERVAL_MS,
    .sampling_window_s = DEFAULT_SAMPLING_WINDOW_S,
    .recovery_window_s = DEFAULT_RECOVERY_WINDOW_S,
    .iowait_high_pct   = DEFAULT_IOWAIT_HIGH_PCT,
    .iowait_low_pct    = DEFAULT_IOWAIT_LOW_PCT,
    .amp_threshold     = DEFAULT_AMP_THRESHOLD,
    .trigger_ra_kb     = DEFAULT_TRIGGER_RA_KB,
    .use_syslog        = false,
    .use_json          = true,
    .dry_run           = false,
    .verbose_metrics   = false,
    .running           = 1,
};

/* ====================================================================== */
/* Logging                                                                */
/* ====================================================================== */

/*
 * Escape a string for safe embedding in a JSON string literal. The output
 * buffer is always NUL-terminated; truncation is silent (we prefer a partial
 * log line over no log line).
 */
static void json_escape(const char *in, char *out, size_t out_sz)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 6 < out_sz; i++) {
        unsigned char c = (unsigned char)in[i];
        switch (c) {
        case '"':  out[j++] = '\\'; out[j++] = '"';  break;
        case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
        case '\n': out[j++] = '\\'; out[j++] = 'n';  break;
        case '\r': out[j++] = '\\'; out[j++] = 'r';  break;
        case '\t': out[j++] = '\\'; out[j++] = 't';  break;
        default:
            if (c < 0x20) {
                j += (size_t)snprintf(out + j, out_sz - j, "\\u%04x", c);
            } else {
                out[j++] = (char)c;
            }
        }
    }
    out[j] = '\0';
}

/*
 * Emit a structured log record. JSON form:
 *   {"ts":"2025-...Z","level":"INFO","event":"startup","msg":"device=sda ..."}
 * Plain form:
 *   [INFO] startup: device=sda ...
 *
 * When logging to syslog, the priority is derived from the level string so
 * WARN / ERROR records are correctly classified by syslog consumers.
 */
static int level_to_syslog_priority(const char *level)
{
    if (strcmp(level, "ERROR") == 0) return LOG_ERR;
    if (strcmp(level, "WARN")  == 0) return LOG_WARNING;
    return LOG_INFO;
}

static void log_event(const char *level, const char *event,
                      const char *fmt, ...)
{
    char raw[LOG_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(raw, sizeof(raw), fmt, ap);
    va_end(ap);

    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);

    if (g_cfg.use_json) {
        char esc[LOG_MSG_MAX * 2];
        json_escape(raw, esc, sizeof(esc));
        char line[LOG_MSG_MAX * 2 + 128];
        int n = snprintf(line, sizeof(line),
                         "{\"ts\":\"%s\",\"level\":\"%s\",\"event\":\"%s\",\"msg\":\"%s\"}\n",
                         ts, level, event, esc);
        if (g_cfg.use_syslog) {
            syslog(level_to_syslog_priority(level),
                   "%.*s", n > 0 ? n - 1 : 0, line);
        } else {
            fputs(line, stderr);
        }
    } else {
        if (g_cfg.use_syslog) {
            syslog(level_to_syslog_priority(level),
                   "[%s] %s: %s", level, event, raw);
        } else {
            fprintf(stderr, "[%s] %s: %s\n", level, event, raw);
        }
    }
}

/* NOTE: deliberately prefixed APG_* to avoid clashing with the
 * LOG_INFO / LOG_ERR priority macros exported by <syslog.h>. */
#define APG_INFO(event, ...)  log_event("INFO",  event, __VA_ARGS__)
#define APG_WARN(event, ...)  log_event("WARN",  event, __VA_ARGS__)
#define APG_ERR(event, ...)   log_event("ERROR", event, __VA_ARGS__)
#define APG_METRIC(...)       log_event("INFO", "metric", __VA_ARGS__)

/* ====================================================================== */
/* Signal handling                                                        */
/* ====================================================================== */

static void on_signal(int signo)
{
    (void)signo;
    /* Flip the liveness flag; main loop will observe it next iteration. */
    g_cfg.running = 0;
}

static void install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    /* SA_RESTART so nanosleep() resumes on EINTR rather than failing. */
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    /* SIGPIPE must never kill the daemon. */
    signal(SIGPIPE, SIG_IGN);
}

/* ====================================================================== */
/* Time helpers                                                           */
/* ====================================================================== */

static void sleep_ms(int ms)
{
    struct timespec ts = {
        .tv_sec  = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ====================================================================== */
/* Ring buffer for rolling-window statistics                              */
/* ====================================================================== */

typedef struct {
    double *samples;
    size_t  head;
    size_t  count;
    size_t  cap;
} ring_t;

static bool ring_init(ring_t *r, size_t cap)
{
    if (cap == 0) cap = 1;
    r->samples = calloc(cap, sizeof(double));
    if (!r->samples) return false;
    r->head = 0;
    r->count = 0;
    r->cap = cap;
    return true;
}

static void ring_free(ring_t *r)
{
    free(r->samples);
    r->samples = NULL;
    r->head = r->count = r->cap = 0;
}

static void ring_push(ring_t *r, double v)
{
    r->samples[r->head] = v;
    r->head = (r->head + 1) % r->cap;
    if (r->count < r->cap) r->count++;
}

static double ring_avg_last_n(const ring_t *r, size_t n)
{
    if (r->count == 0 || n == 0) return 0.0;
    size_t take = n < r->count ? n : r->count;
    double s = 0.0;
    for (size_t i = 0; i < take; i++) {
        size_t idx = (r->head + r->cap - take + i) % r->cap;
        s += r->samples[idx];
    }
    return s / (double)take;
}

static bool ring_all_below_last_n(const ring_t *r, size_t n, double threshold)
{
    if (r->count == 0 || n == 0) return false;
    size_t take = n < r->count ? n : r->count;
    for (size_t i = 0; i < take; i++) {
        size_t idx = (r->head + r->cap - take + i) % r->cap;
        if (r->samples[idx] > threshold) return false;
    }
    return true;
}

/* ====================================================================== */
/* Telemetry: /proc/stat (CPU jiffies)                                    */
/* ====================================================================== */

/*
 * Parse the aggregate "cpu " line from /proc/stat. Layout:
 *   cpu  user nice system idle iowait irq softirq steal guest guest_nice
 *
 * The iowait field (5th) is the time the CPU was idle with an outstanding
 * disk I/O. Comparing deltas between successive samples yields the
 * instantaneous %iowait which is the primary signal for prefetch thrashing.
 */
static bool read_cpu_stat(cpu_stat_t *out)
{
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        APG_ERR("proc_stat_open", "errno=%d path=/proc/stat", errno);
        return false;
    }
    char line[512];
    bool ok = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu ", 4) != 0) continue;
        unsigned long long u, n, s, id, iw, irq, siq, st;
        int matched = sscanf(line + 4,
                             "%llu %llu %llu %llu %llu %llu %llu %llu",
                             &u, &n, &s, &id, &iw, &irq, &siq, &st);
        if (matched >= 5) {
            out->user    = u;
            out->nice    = n;
            out->system  = s;
            out->idle    = id;
            out->iowait  = iw;
            out->irq     = matched > 5 ? irq : 0;
            out->softirq = matched > 6 ? siq : 0;
            out->steal   = matched > 7 ? st  : 0;
            ok = true;
        }
        break;
    }
    fclose(fp);
    if (!ok) {
        APG_ERR("proc_stat_parse", "reason=no_aggregate_cpu_line");
    }
    return ok;
}

static double compute_iowait_pct(const cpu_stat_t *prev, const cpu_stat_t *cur)
{
    unsigned long long prev_total =
        prev->user + prev->nice + prev->system + prev->idle +
        prev->iowait + prev->irq + prev->softirq + prev->steal;
    unsigned long long cur_total =
        cur->user + cur->nice + cur->system + cur->idle +
        cur->iowait + cur->irq + cur->softirq + cur->steal;
    unsigned long long dt_total  = cur_total  > prev_total  ? cur_total  - prev_total  : 0;
    unsigned long long dt_iowait = cur->iowait > prev->iowait ? cur->iowait - prev->iowait : 0;
    if (dt_total == 0) return 0.0;
    return 100.0 * (double)dt_iowait / (double)dt_total;
}

/* ====================================================================== */
/* Telemetry: /proc/diskstats                                             */
/* ====================================================================== */

/*
 * Parse the per-device line for our target block device. Layout:
 *   major minor name reads_done reads_merged sectors_read read_ticks ...
 *
 * We use sectors_read (field 6) to measure physical bytes pulled from disk.
 * Combined with pgmajfault deltas from /proc/vmstat, this lets us compute a
 * "prefetch amplification" ratio: bytes_read_from_disk / bytes_app_actually_
 * requested. High amplification = the read-ahead block is mostly wasted.
 */
static bool read_disk_stat(target_device_t *dev, disk_stat_t *out)
{
    FILE *fp = fopen("/proc/diskstats", "r");
    if (!fp) {
        APG_ERR("proc_diskstats_open", "errno=%d path=/proc/diskstats", errno);
        return false;
    }
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        unsigned int major, minor;
        char name[DEVICE_NAME_MAX];
        int name_end = 0;
        /* %n captures the byte offset past the name token. */
        int matched = sscanf(line, "%u %u %63s%n",
                             &major, &minor, name, &name_end);
        if (matched != 3) continue;
        if (strcmp(name, dev->name) != 0) continue;

        /* Tokenize the remaining 11 numeric fields manually so we tolerate
         * minor format drift across kernel versions. */
        const char *p = line + name_end;
        unsigned long long fields[11];
        int n = 0;
        while (*p && n < 11) {
            while (*p && isspace((unsigned char)*p)) p++;
            if (!*p) break;
            char *end;
            errno = 0;
            unsigned long long v = strtoull(p, &end, 10);
            if (end == p) break;
            fields[n++] = v;
            p = end;
        }
        if (n >= 11) {
            out->reads_completed      = fields[0];
            out->reads_merged         = fields[1];
            out->sectors_read         = fields[2];
            out->read_ticks_ms        = fields[3];
            out->writes_completed     = fields[4];
            out->writes_merged        = fields[5];
            out->sectors_written      = fields[6];
            out->write_ticks_ms       = fields[7];
            out->ios_in_progress      = fields[8];
            out->io_ticks_ms          = fields[9];
            out->weighted_io_ticks_ms = fields[10];
            found = true;
        }
        break;
    }
    fclose(fp);
    if (!found) {
        APG_ERR("diskstats_lookup", "device=%s reason=not_found", dev->name);
    }
    return found;
}

/* ====================================================================== */
/* Telemetry: /proc/vmstat (page-fault counters)                          */
/* ====================================================================== */

/*
 * pgmajfault counts major page faults -- faults that required I/O to satisfy.
 * For mmap workloads this is the best available proxy for "logical pages the
 * application actually requested from the kernel". Coupled with sectors_read
 * it yields the prefetch amplification ratio used by the FR-02 heuristic.
 */
static bool read_vmstat(vmstat_t *out)
{
    FILE *fp = fopen("/proc/vmstat", "r");
    if (!fp) {
        APG_ERR("proc_vmstat_open", "errno=%d path=/proc/vmstat", errno);
        return false;
    }
    char line[256];
    out->pgmajfault = 0;
    out->pgfault    = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "pgmajfault ", 11) == 0) {
            out->pgmajfault = strtoull(line + 11, NULL, 10);
        } else if (strncmp(line, "pgfault ", 8) == 0) {
            out->pgfault = strtoull(line + 8, NULL, 10);
        }
    }
    fclose(fp);
    return true;
}

/* ====================================================================== */
/* sysfs: read_ahead_kb get / set                                         */
/* ====================================================================== */

/*
 * Read the kernel's current runtime prefetch threshold. This is the value
 * the block layer will use for the next read() / mmap() fault on the device.
 */
static bool read_read_ahead_kb(target_device_t *dev, unsigned int *out)
{
    int fd = open(dev->sysfs_ra_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        APG_ERR("sysfs_ra_open_read", "errno=%d path=%s", errno, dev->sysfs_ra_path);
        return false;
    }
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    int saved_errno = errno;
    close(fd);
    if (n <= 0) {
        APG_ERR("sysfs_ra_read", "errno=%d path=%s", n == 0 ? EIO : saved_errno,
                dev->sysfs_ra_path);
        return false;
    }
    buf[n] = '\0';
    char *end;
    errno = 0;
    unsigned long v = strtoul(buf, &end, 10);
    if (end == buf) {
        APG_ERR("sysfs_ra_parse", "path=%s raw=%s", dev->sysfs_ra_path, buf);
        return false;
    }
    *out = (unsigned int)v;
    return true;
}

/*
 * Directly write the new read-ahead value to the sysfs attribute. We bypass
 * blockdev(8) -- the kernel's sysfs store handler is just:
 *   queue_sysfs_store() -> queue_limit_store() -> blk_queue_set_read_ahead()
 * so a 4-byte ASCII write achieves the same effect with no fork/exec overhead,
 * no shell, and no privilege-escalation surface.
 */
static bool write_read_ahead_kb(target_device_t *dev, unsigned int kb)
{
    if (g_cfg.dry_run) {
        APG_INFO("dry_run_write", "path=%s value=%u", dev->sysfs_ra_path, kb);
        dev->last_written_ra_kb = kb;
        return true;
    }
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%u\n", kb);
    if (len < 0 || (size_t)len >= sizeof(buf)) {
        APG_ERR("sysfs_ra_fmt", "value=%u", kb);
        return false;
    }
    int fd = open(dev->sysfs_ra_path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        APG_ERR("sysfs_ra_open_write", "errno=%d path=%s", errno, dev->sysfs_ra_path);
        return false;
    }
    ssize_t n = write(fd, buf, (size_t)len);
    int saved_errno = errno;
    close(fd);
    if (n < 0) {
        APG_ERR("sysfs_ra_write", "errno=%d path=%s value=%u", saved_errno,
                dev->sysfs_ra_path, kb);
        return false;
    }
    if (n != len) {
        APG_WARN("sysfs_ra_partial_write", "path=%s expected=%d actual=%zd",
                 dev->sysfs_ra_path, len, n);
        return false;
    }
    dev->last_written_ra_kb = kb;
    return true;
}

/* ====================================================================== */
/* Read-ahead step-down / step-up (powers of two)                         */
/* ====================================================================== */

static unsigned int step_down_ra(unsigned int current)
{
    if (current <= MIN_RA_KB) return MIN_RA_KB;
    unsigned int next = current >> 1;
    if (next < MIN_RA_KB) next = MIN_RA_KB;
    return next;
}

static unsigned int step_up_ra(unsigned int current)
{
    if (current >= MAX_RA_KB) return MAX_RA_KB;
    unsigned int next = current << 1;
    if (next > MAX_RA_KB) next = MAX_RA_KB;
    if (next < MIN_RA_KB) next = MIN_RA_KB;
    return next;
}

/* ====================================================================== */
/* Argument parsing                                                       */
/* ====================================================================== */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --device <name> [options]\n"
        "\n"
        "Adaptive Prefetch Guard - detects and remediates Linux page-cache\n"
        "I/O thrashing caused by oversized block read-ahead.\n"
        "\n"
        "Required:\n"
        "  -d, --device <name>           Target block device (e.g. sda, nvme0n1)\n"
        "\n"
        "Optional:\n"
        "  -i, --interval-ms <n>         Telemetry poll interval (default: 500)\n"
        "  -H, --iowait-high-pct <f>     Trigger iowait %% (default: 50.0)\n"
        "  -L, --iowait-low-pct <f>      Recovery iowait %% (default: 20.0)\n"
        "  -s, --sampling-window-s <n>   Trigger sustain window (default: 5)\n"
        "  -r, --recovery-window-s <n>   Stabilization window (default: 60)\n"
        "  -T, --trigger-ra-kb <n>       Min RA to consider trigger (default: 1024)\n"
        "  -A, --amp-threshold <f>       Prefetch amplification limit (default: 4.0)\n"
        "      --syslog                  Emit logs to syslog instead of stderr\n"
        "      --plain                   Plain text logs (default: JSON)\n"
        "      --dry-run                 Do not write to sysfs (monitor only)\n"
        "  -v, --verbose                 Emit per-loop metric logs\n"
        "  -h, --help                    Show this help\n"
        "\n"
        "Examples:\n"
        "  sudo %s --device sda\n"
        "  sudo %s --device nvme0n1 --interval-ms 250 --syslog --verbose\n"
        "  sudo %s --device sda --dry-run    # monitor, never touch sysfs\n",
        prog, prog, prog, prog);
}

static bool parse_args(int argc, char **argv, config_t *cfg)
{
    static struct option longopts[] = {
        {"device",            required_argument, NULL, 'd'},
        {"interval-ms",       required_argument, NULL, 'i'},
        {"iowait-high-pct",   required_argument, NULL, 'H'},
        {"iowait-low-pct",    required_argument, NULL, 'L'},
        {"sampling-window-s", required_argument, NULL, 's'},
        {"recovery-window-s", required_argument, NULL, 'r'},
        {"trigger-ra-kb",     required_argument, NULL, 'T'},
        {"amp-threshold",     required_argument, NULL, 'A'},
        {"syslog",            no_argument,       NULL, 1001},
        {"plain",             no_argument,       NULL, 1002},
        {"dry-run",           no_argument,       NULL, 1003},
        {"verbose",           no_argument,       NULL, 'v'},
        {"help",              no_argument,       NULL, 'h'},
        {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "d:i:H:L:s:r:T:A:vh", longopts, NULL)) != -1) {
        switch (c) {
        case 'd':
            if (strlen(optarg) >= DEVICE_NAME_MAX) {
                fprintf(stderr, "Error: device name too long (max %d)\n",
                        DEVICE_NAME_MAX - 1);
                return false;
            }
            snprintf(cfg->dev.name, sizeof(cfg->dev.name), "%s", optarg);
            break;
        case 'i':
            cfg->loop_interval_ms = atoi(optarg);
            if (cfg->loop_interval_ms < 50) {
                fprintf(stderr, "Error: --interval-ms must be >= 50\n");
                return false;
            }
            break;
        case 'H': cfg->iowait_high_pct = atof(optarg); break;
        case 'L': cfg->iowait_low_pct  = atof(optarg); break;
        case 's':
            cfg->sampling_window_s = atoi(optarg);
            if (cfg->sampling_window_s < 1) {
                fprintf(stderr, "Error: --sampling-window-s must be >= 1\n");
                return false;
            }
            break;
        case 'r':
            cfg->recovery_window_s = atoi(optarg);
            if (cfg->recovery_window_s < 1) {
                fprintf(stderr, "Error: --recovery-window-s must be >= 1\n");
                return false;
            }
            break;
        case 'T':
            cfg->trigger_ra_kb = (unsigned int)strtoul(optarg, NULL, 10);
            break;
        case 'A': cfg->amp_threshold = atof(optarg); break;
        case 1001: cfg->use_syslog = true;  break;
        case 1002: cfg->use_json   = false; break;
        case 1003: cfg->dry_run    = true;  break;
        case 'v':  cfg->verbose_metrics = true; break;
        case 'h':  usage(argv[0]); exit(0);
        default:   usage(argv[0]); return false;
        }
    }
    if (cfg->dev.name[0] == '\0') {
        fprintf(stderr, "Error: --device is required\n\n");
        usage(argv[0]);
        return false;
    }
    snprintf(cfg->dev.sysfs_ra_path, sizeof(cfg->dev.sysfs_ra_path),
             "/sys/block/%s/queue/read_ahead_kb", cfg->dev.name);
    return true;
}

/* ====================================================================== */
/* Main daemon loop                                                       */
/* ====================================================================== */

static int run_daemon(config_t *cfg)
{
    /* Derive sample-window sizes from the configured interval. The rings
     * hold enough samples to cover the longest window we ever query. */
    size_t samples_per_iowait_window =
        (size_t)((double)cfg->sampling_window_s * 1000.0 / cfg->loop_interval_ms);
    size_t samples_per_recovery_window =
        (size_t)((double)cfg->recovery_window_s * 1000.0 / cfg->loop_interval_ms);
    if (samples_per_iowait_window == 0)   samples_per_iowait_window = 1;
    if (samples_per_recovery_window == 0) samples_per_recovery_window = 1;

    /* The recovery ring must be at least as large as the iowait ring since
     * we may inspect either window from it. */
    size_t recovery_cap = samples_per_recovery_window > samples_per_iowait_window
                          ? samples_per_recovery_window
                          : samples_per_iowait_window;

    ring_t iowait_ring, recovery_ring;
    if (!ring_init(&iowait_ring, samples_per_iowait_window) ||
        !ring_init(&recovery_ring, recovery_cap)) {
        APG_ERR("oom", "ring=init");
        ring_free(&iowait_ring);
        ring_free(&recovery_ring);
        return 1;
    }

    /* ----- Initial baseline reads ----- */
    cpu_stat_t prev_cpu;
    disk_stat_t prev_disk;
    vmstat_t prev_vmstat;
    bool have_prev_cpu    = read_cpu_stat(&prev_cpu);
    bool have_prev_disk   = read_disk_stat(&cfg->dev, &prev_disk);
    bool have_prev_vmstat = read_vmstat(&prev_vmstat);
    unsigned long long prev_sectors_read   = have_prev_disk   ? prev_disk.sectors_read   : 0;
    unsigned long long prev_pgmajfault     = have_prev_vmstat ? prev_vmstat.pgmajfault   : 0;

    unsigned int initial_ra = 0;
    if (!read_read_ahead_kb(&cfg->dev, &initial_ra)) {
        APG_ERR("init_ra_read", "device=%s path=%s", cfg->dev.name,
                cfg->dev.sysfs_ra_path);
        ring_free(&iowait_ring);
        ring_free(&recovery_ring);
        return 1;
    }
    cfg->dev.current_ra_kb     = initial_ra;
    cfg->dev.last_written_ra_kb = initial_ra;

    daemon_state_t state = STATE_NORMAL;
    double last_step_up_time = 0.0;     /* throttle recovery ramp-up */
    const double STEP_UP_MIN_INTERVAL_S = 10.0;
    double last_step_down_time = 0.0;   /* throttle step-down too     */
    const double STEP_DOWN_MIN_INTERVAL_S = 1.0;

    APG_INFO("startup",
             "device=%s ra_kb_initial=%u loop_ms=%d "
             "sampling_window_s=%d recovery_window_s=%d "
             "iowait_high=%.1f iowait_low=%.1f amp_threshold=%.2f "
             "trigger_ra_kb=%u dry_run=%d state=%s",
             cfg->dev.name, initial_ra, cfg->loop_interval_ms,
             cfg->sampling_window_s, cfg->recovery_window_s,
             cfg->iowait_high_pct, cfg->iowait_low_pct, cfg->amp_threshold,
             cfg->trigger_ra_kb, (int)cfg->dry_run, state_name(state));

    /* ----- Main loop ----- */
    while (cfg->running) {
        sleep_ms(cfg->loop_interval_ms);

        /* ---- Telemetry acquisition ---- */
        cpu_stat_t  cpu;
        disk_stat_t disk;
        vmstat_t    vmstat;
        if (!read_cpu_stat(&cpu))  continue;
        if (!read_disk_stat(&cfg->dev, &disk)) continue;
        /* vmstat is best-effort: a missing read shouldn't kill the loop,
         * we just lose the amplification metric for that sample. */
        vmstat_t *p_vmstat = read_vmstat(&vmstat) ? &vmstat : NULL;

        /* ---- %iowait delta ---- */
        double iowait_pct = 0.0;
        if (have_prev_cpu) {
            iowait_pct = compute_iowait_pct(&prev_cpu, &cpu);
        }
        prev_cpu = cpu;
        have_prev_cpu = true;

        /* ---- Disk sectors delta ---- */
        unsigned long long sectors_delta = 0;
        if (disk.sectors_read >= prev_sectors_read) {
            sectors_delta = disk.sectors_read - prev_sectors_read;
        }
        prev_sectors_read = disk.sectors_read;

        /* ---- Major page faults delta ---- */
        unsigned long long pgmajfault_delta = 0;
        if (p_vmstat && vmstat.pgmajfault >= prev_pgmajfault) {
            pgmajfault_delta = vmstat.pgmajfault - prev_pgmajfault;
        }
        if (p_vmstat) prev_pgmajfault = vmstat.pgmajfault;

        /* ---- Refresh cached RA from sysfs (in case admin changed it) ---- */
        unsigned int sysfs_ra = 0;
        if (read_read_ahead_kb(&cfg->dev, &sysfs_ra)) {
            if (sysfs_ra != cfg->dev.current_ra_kb) {
                APG_WARN("ra_external_change",
                         "device=%s was=%u now=%u",
                         cfg->dev.name, cfg->dev.current_ra_kb, sysfs_ra);
                cfg->dev.current_ra_kb = sysfs_ra;
            }
        }

        /* ---- Prefetch amplification ----
         *
         * bytes_read_from_disk = sectors_delta * 512
         * bytes_app_requested  = pgmajfault_delta * 4096   (one page per fault)
         * amplification        = bytes_read / bytes_app
         *
         * A sequential workload that fully consumes its read-ahead blocks
         * will trend toward amp ~= 1.0 (every prefetched byte gets faulted
         * in eventually). A random mmap workload that triggers 4 MiB of
         * read-ahead per 4 KiB fault runs amp ~= 1024 -- the textbook
         * cache-pollution signature.
         */
        double prefetch_amp = 0.0;
        if (pgmajfault_delta > 0) {
            double bytes_read = (double)sectors_delta * (double)SECTOR_SIZE_BYTES;
            double bytes_app  = (double)pgmajfault_delta * (double)PAGE_SIZE_BYTES;
            prefetch_amp = bytes_read / bytes_app;
        }

        /* ---- Push samples into rolling windows ---- */
        ring_push(&iowait_ring,   iowait_pct);
        ring_push(&recovery_ring, iowait_pct);

        if (cfg->verbose_metrics) {
            APG_METRIC("device=%s iowait_pct=%.2f ra_kb=%u "
                       "sectors_delta=%llu pgmajfault_delta=%llu "
                       "prefetch_amp=%.2f state=%s",
                       cfg->dev.name, iowait_pct, cfg->dev.current_ra_kb,
                       (unsigned long long)sectors_delta,
                       (unsigned long long)pgmajfault_delta,
                       prefetch_amp, state_name(state));
        }

        double now = now_sec();
        double avg_iowait_short =
            ring_avg_last_n(&iowait_ring, samples_per_iowait_window);

        /* ---- State machine ---- */
        switch (state) {

        case STATE_NORMAL: {
            /* FR-02 trigger conditions:
             *   (a) sustained iowait above high threshold over sampling window
             *   (b) prefetch amplification above amp_threshold (low utilization)
             *   (c) current read_ahead_kb >= trigger_ra_kb (e.g. 1024)
             *
             * All three must hold. (a) alone could be legitimate sequential
             * I/O; (b) alone could be a cold-cache ramp; (c) alone is the
             * kernel default. The conjunction is the pathological signature.
             */
            bool cond_iowait = avg_iowait_short > cfg->iowait_high_pct;
            bool cond_amp    = prefetch_amp > cfg->amp_threshold;
            bool cond_ra     = cfg->dev.current_ra_kb >= cfg->trigger_ra_kb;

            if (cond_iowait && cond_amp && cond_ra) {
                APG_WARN("thrashing_detected",
                         "device=%s iowait_avg=%.2f prefetch_amp=%.2f "
                         "ra_kb=%u trigger_ra_kb=%u",
                         cfg->dev.name, avg_iowait_short, prefetch_amp,
                         cfg->dev.current_ra_kb, cfg->trigger_ra_kb);
                state = STATE_REMEDIATING;
                last_step_down_time = now;  /* allow immediate first step */
            }
            break;
        }

        case STATE_REMEDIATING: {
            /* FR-03 step-down: keep halving read_ahead_kb while iowait stays
             * hot. Throttle so the kernel/page-cache has time to react to
             * each new threshold before we cut again. */
            if (avg_iowait_short > cfg->iowait_high_pct &&
                cfg->dev.current_ra_kb > MIN_RA_KB &&
                (now - last_step_down_time) >= STEP_DOWN_MIN_INTERVAL_S) {
                unsigned int next = step_down_ra(cfg->dev.current_ra_kb);
                if (next != cfg->dev.current_ra_kb) {
                    unsigned int from = cfg->dev.current_ra_kb;
                    if (write_read_ahead_kb(&cfg->dev, next)) {
                        cfg->dev.current_ra_kb = next;
                        APG_WARN("ra_step_down",
                                 "device=%s from=%u to=%u iowait_avg=%.2f",
                                 cfg->dev.name, from, next, avg_iowait_short);
                    }
                }
                last_step_down_time = now;
            }

            /* FR-03 recovery phase: if iowait has stayed below the low
             * threshold for the entire recovery window, transition to
             * RECOVERING and begin probing upward. */
            if (ring_all_below_last_n(&recovery_ring,
                                      samples_per_recovery_window,
                                      cfg->iowait_low_pct)) {
                double avg_long =
                    ring_avg_last_n(&recovery_ring, samples_per_recovery_window);
                APG_INFO("stabilized",
                         "device=%s iowait_avg=%.2f window_s=%d",
                         cfg->dev.name, avg_long, cfg->recovery_window_s);
                state = STATE_RECOVERING;
                last_step_up_time = now;
            }
            break;
        }

        case STATE_RECOVERING: {
            /* FR-03 recovery phase: gradually step read_ahead_kb back up to
             * test whether spatial locality has returned. Throttle to one
             * step per STEP_UP_MIN_INTERVAL_S so we don't blow past the
             * stable point. */
            if (avg_iowait_short < cfg->iowait_low_pct &&
                cfg->dev.current_ra_kb < MAX_RA_KB &&
                (now - last_step_up_time) >= STEP_UP_MIN_INTERVAL_S) {
                unsigned int next = step_up_ra(cfg->dev.current_ra_kb);
                if (next != cfg->dev.current_ra_kb) {
                    unsigned int from = cfg->dev.current_ra_kb;
                    if (write_read_ahead_kb(&cfg->dev, next)) {
                        cfg->dev.current_ra_kb = next;
                        APG_INFO("ra_step_up",
                                 "device=%s from=%u to=%u iowait_avg=%.2f",
                                 cfg->dev.name, from, next, avg_iowait_short);
                    }
                }
                last_step_up_time = now;
            } else if (avg_iowait_short > cfg->iowait_high_pct) {
                /* Stepped up too aggressively -- back to remediating. */
                APG_WARN("ra_step_up_backfire",
                         "device=%s iowait_avg=%.2f ra_kb=%u",
                         cfg->dev.name, avg_iowait_short,
                         cfg->dev.current_ra_kb);
                state = STATE_REMEDIATING;
                last_step_down_time = now;
            } else if (cfg->dev.current_ra_kb >= MAX_RA_KB) {
                APG_INFO("recovery_complete",
                         "device=%s ra_kb=%u", cfg->dev.name,
                         cfg->dev.current_ra_kb);
                state = STATE_NORMAL;
            }
            break;
        }
        } /* end switch */
    }

    /* ----- Graceful shutdown ----- */
    APG_INFO("shutdown",
             "device=%s ra_kb_final=%u ra_kb_last_written=%u state=%s",
             cfg->dev.name, cfg->dev.current_ra_kb,
             cfg->dev.last_written_ra_kb, state_name(state));

    ring_free(&iowait_ring);
    ring_free(&recovery_ring);
    return 0;
}

/* ====================================================================== */
/* Entry point                                                            */
/* ====================================================================== */

int main(int argc, char **argv)
{
    if (!parse_args(argc, argv, &g_cfg)) {
        return 2;
    }
    if (g_cfg.use_syslog) {
        openlog("apg", LOG_PID | LOG_NDELAY, LOG_DAEMON);
    }
    install_signal_handlers();
    int rc = run_daemon(&g_cfg);
    if (g_cfg.use_syslog) {
        closelog();
    }
    return rc;
}
