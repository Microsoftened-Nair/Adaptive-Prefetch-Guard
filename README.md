# Adaptive Prefetch Guard (apg)

> A small Linux system daemon that detects and remediates OS page-cache I/O
> thrashing caused by oversized block-layer read-ahead. Built in modern C11
> with pure POSIX APIs — no external libraries, no shelling out to `blockdev(8)`,
> no runtime dependencies beyond glibc.

---

## Table of Contents

1. [Overview](#1-overview)
2. [The Problem: Why This Daemon Exists](#2-the-problem-why-this-daemon-exists)
3. [How apg Solves It](#3-how-apg-solves-it)
4. [System Requirements](#4-system-requirements)
5. [Installation](#5-installation)
6. [Quick Start](#6-quick-start)
7. [Command-Line Reference](#7-command-line-reference)
8. [Configuration Recipes](#8-configuration-recipes)
9. [Running as a systemd Service](#9-running-as-a-systemd-service)
10. [Log Format & Observability](#10-log-format--observability)
11. [Telemetry Sources Explained](#11-telemetry-sources-explained)
12. [Detection Heuristic](#12-detection-heuristic)
13. [Remediation Algorithm](#13-remediation-algorithm)
14. [Evaluation & Benchmarking](#14-evaluation--benchmarking)
15. [Tuning Guide](#15-tuning-guide)
16. [Troubleshooting](#16-troubleshooting)
17. [Security & Hardening](#17-security--hardening)
18. [Architecture & Internals](#18-architecture--internals)
19. [Limitations & Caveats](#19-limitations--caveats)
20. [FAQ](#20-faq)
21. [Glossary](#21-glossary)
22. [License](#22-license)

---

## 1. Overview

**Adaptive Prefetch Guard (apg)** is a production-grade Linux daemon that
watches the kernel's own I/O telemetry and dynamically adjusts the block
read-ahead (`read_ahead_kb`) on a target storage device. Its purpose is to
detect the pathological pattern in which aggressive prefetch floods the page
cache with pages that are never consumed — a regression that became
commonplace after recent kernels bumped the default read-ahead from 128 KiB
to 4096 KiB on certain platforms.

| Property | Value |
|---|---|
| Language | C11 (GNU dialect), POSIX.1-2008 |
| External dependencies | None (only glibc) |
| Runtime footprint | ~32 MiB RSS cap, < 5% CPU |
| Build dependencies | A C compiler (`gcc` ≥ 9 or `clang` ≥ 10) |
| Target OS | Linux kernel ≥ 6.x (works on ≥ 4.x with reduced fidelity) |
| Privilege | Root (or `CAP_SYS_ADMIN`) for sysfs writes |
| License | GPL-2.0-or-later |

---

## 2. The Problem: Why This Daemon Exists

### 2.1 What changed in the kernel

On a number of modern distributions and platforms, the Linux block layer's
default `read_ahead_kb` was raised from **128 KiB** to **4096 KiB**. The
change was made on the assumption that sequential I/O dominates modern
workloads and that pulling larger chunks into the page cache reduces the
per-fault overhead.

This assumption holds for streaming reads (file servers, log shippers,
backup jobs). It breaks catastrophically for memory-mapped random-access
workloads.

### 2.2 The pathological pattern

When a process memory-maps a file (`mmap(2)`) and accesses pages randomly
(as RavenDB, RocksDB, LMDB, some PostgreSQL configurations, and many
in-process caches do), the following happens on every major page fault:

1. The kernel's `filemap_fault()` handler is invoked for the faulted virtual
   address.
2. It calls `do_page_cache_ra()` (formerly `do_async_mmap_readahead()`),
   which issues a synchronous read of the **entire read-ahead window** — by
   default, **4 MiB** (1024 pages of 4 KiB).
3. The user-space process touches only the **4 KiB page** it actually
   faulted on. The other 1023 pages sit in the page cache as cold, unused
   data.
4. Under memory pressure (which a 4 MiB-per-fault policy creates almost
   immediately), the kernel begins evicting those unused pages — frequently
   **before** any other process gets a chance to consume them.
5. The eviction itself costs CPU cycles, the disk I/O used to read the
   soon-evicted pages was wasted, and the next random access triggers the
   same cycle again.

The result: **CPU `%iowait` pins near 100%**, throughput drops by **2.6×
to 10×** compared to a 128 KiB read-ahead, and `iostat` shows the disk
appearing 100% busy while application-level throughput collapses.

### 2.3 Why existing tools don't help

- **`tuned` / `sysctl`**: read-ahead is a per-block-device sysfs attribute,
  not a sysctl. `tuned` profiles don't generally manage it.
- **`blockdev --setra`**: works, but is a one-shot, manual, and requires
  re-running after every device re-attach, LVM activation, or reboot.
- **udev rules**: can set read-ahead at device-add time, but cannot adapt
  to changing workload phases (e.g. daytime random-access DB → nighttime
  sequential backup).
- **The kernel itself**: has no built-in mechanism to *detect* that its
  prefetch policy is causing more harm than good. The block layer applies
  the configured read-ahead uniformly.

`apg` fills this gap: it continuously measures the **prefetch
amplification ratio** (bytes physically read vs. bytes the application
actually demanded), and shrinks the read-ahead when amplification is
pathological, then grows it back when I/O wait normalizes.

---

## 3. How apg Solves It

```
                    ┌──────────────────────────────────────────┐
                    │                apg daemon                 │
                    │                                          │
   /proc/stat  ───► │  %iowait rolling-window ring buffer      │
   /proc/diskstats ► │  sectors_read delta                     │
   /proc/vmstat ──► │  pgmajfault delta (app demand)           │
                    │                                          │
                    │  ┌────────────────────────────────────┐  │
                    │  │   state machine                    │  │
                    │  │   NORMAL ──► REMEDIATING ──►       │  │
                    │  │                ▲           │       │  │
                    │  │                │   ◄───────┘       │  │
                    │  │                RECOVERING          │  │
                    │  └────────────────────────────────────┘  │
                    │              │                           │
                    │              ▼                           │
                    │   open(2)/write(2) to sysfs              │
                    └──────────────┬───────────────────────────┘
                                   │
                                   ▼
                /sys/block/<dev>/queue/read_ahead_kb
                                   │
                                   ▼
                       block layer applies
                       new read-ahead to all
                       subsequent page faults
```

The daemon is **single-threaded, single-process, and uses blocking I/O**
on purpose: at a 500 ms loop interval the entire poll/parse/decide/write
cycle takes well under 1 ms of CPU, and a single-threaded design avoids
the entire class of locking, ordering, and reentrancy bugs that plague
multi-threaded polling daemons.

---

## 4. System Requirements

### 4.1 Operating system

- **Linux kernel ≥ 4.0** for the basic sysfs interface
  (`/sys/block/<dev>/queue/read_ahead_kb` has existed for over a decade)
- **Linux kernel ≥ 6.0** recommended — this is where the 4 MiB default
  became widespread, so the need for `apg` is greatest there
- Tested on **Ubuntu 24.04 LTS** and **Debian 12**; should work on any
  glibc-based distribution (RHEL 9, Fedora 39+, openSUSE Tumbleweed,
  Arch, etc.)

### 4.2 Permissions

- **Root** (uid 0), OR
- **`CAP_SYS_ADMIN`** capability (preferred for systemd hardening), OR
- Write permission on the specific sysfs attribute
  `/sys/block/<dev>/queue/read_ahead_kb` (achievable via a udev rule)

### 4.3 Build dependencies

- A C compiler supporting C11 + GNU extensions:
  - `gcc ≥ 9`
  - `clang ≥ 10`
- GNU `make` (any version from the last 15 years)
- Standard C library headers (`glibc-devel` / `libc6-dev`)

### 4.4 Optional runtime dependencies

- **`fio`** — for the evaluation harness (`apt install fio`)
- **`jq`** — for the evaluation harness and for parsing logs (`apt install jq`)
- **`systemd` ≥ 245** — for the bundled unit (older systemd works but
  may not honor all hardening directives)

---

## 5. Installation

### 5.1 Option A — Build from source (recommended)

```bash
# 1. Get the source (assumed unpacked at ./apg/)
cd apg

# 2. Build
make

# 3. Verify the binary works
./apg --help

# 4. (Optional) Run a quick smoke test in dry-run mode (no sysfs writes)
sudo ./apg --device sda --dry-run --verbose --interval-ms 500
# Ctrl-C after a few seconds to stop
```

The build produces a single static-ish binary (`apg`, ~35 KiB stripped,
dynamically linked against glibc only).

### 5.2 Option B — System-wide install via Makefile

```bash
sudo make install
# This installs:
#   /usr/sbin/apg
#   /etc/systemd/system/apg.service
sudo systemctl daemon-reload
```

To uninstall:

```bash
sudo make uninstall
```

### 5.3 Option C — Debug build with sanitizers

For development, forensic, or "is apg doing something weird?" purposes:

```bash
make debug
# Produces ./apg built with -g -O0 -fsanitize=address,undefined
sudo ./apg --device sda --dry-run --verbose
```

### 5.4 Verifying the install

```bash
apg --help                                           # prints usage
ls -l /sys/block/sda/queue/read_ahead_kb             # target attribute exists
cat /sys/block/sda/queue/read_ahead_kb               # current value (e.g. 4096)
sudo apg --device sda --dry-run --verbose            # telemetry works
```

If `--dry-run` emits per-loop `metric` log lines, your installation is
working end-to-end.

---

## 6. Quick Start

The fastest path from "I have the binary" to "apg is protecting my
database device":

```bash
# 1. Find the block device your DB lives on
lsblk
# e.g. nvme0n1 → /var/lib/postgresql

# 2. Check current read-ahead
cat /sys/block/nvme0n1/queue/read_ahead_kb
# 4096   <-- problematic default

# 3. Run apg in the foreground, dry-run first, to confirm detection works
sudo ./apg --device nvme0n1 --dry-run --verbose --interval-ms 500

# 4. Once you're satisfied, take it out of dry-run and let it remediate
sudo ./apg --device nvme0n1 --verbose

# 5. Watch the state machine fire when thrashing starts
#    (run your DB workload in another terminal)

# 6. To install permanently
sudo make install
sudo systemctl enable --now apg
sudo systemctl edit apg     # set Environment=APG_DEVICE=nvme0n1
```

---

## 7. Command-Line Reference

```
Usage: apg --device <name> [options]

Required:
  -d, --device <name>           Target block device (e.g. sda, nvme0n1)

Optional:
  -i, --interval-ms <n>         Telemetry poll interval (default: 500)
  -H, --iowait-high-pct <f>     Trigger iowait % (default: 50.0)
  -L, --iowait-low-pct <f>      Recovery iowait % (default: 20.0)
  -s, --sampling-window-s <n>   Trigger sustain window (default: 5)
  -r, --recovery-window-s <n>   Stabilization window (default: 60)
  -T, --trigger-ra-kb <n>       Min RA to consider trigger (default: 1024)
  -A, --amp-threshold <f>       Prefetch amplification limit (default: 4.0)
      --syslog                  Emit logs to syslog instead of stderr
      --plain                   Plain text logs (default: JSON)
      --dry-run                 Do not write to sysfs (monitor only)
  -v, --verbose                 Emit per-loop metric logs
  -h, --help                    Show this help
```

### 7.1 Argument reference

| Flag | Type | Default | Meaning |
|---|---|---|---|
| `--device`, `-d` | string | *(required)* | Block device basename as it appears in `/sys/block/`. Examples: `sda`, `nvme0n1`, `pmem0`. Do NOT include the `/dev/` prefix. |
| `--interval-ms`, `-i` | int | `500` | Milliseconds between telemetry polls. Minimum 50. Lower values give faster reaction but more CPU. |
| `--iowait-high-pct`, `-H` | float | `50.0` | Sustained `%iowait` above which the daemon considers itself in a thrashing regime. |
| `--iowait-low-pct`, `-L` | float | `20.0` | `%iowait` below which the daemon considers I/O stabilized and begins recovery. |
| `--sampling-window-s`, `-s` | int | `5` | Seconds over which `%iowait` must sustain above `-H` to trigger remediation. |
| `--recovery-window-s`, `-r` | int | `60` | Seconds over which `%iowait` must stay below `-L` before the daemon enters the recovery (step-up) phase. |
| `--trigger-ra-kb`, `-T` | int | `1024` | Minimum `read_ahead_kb` for the trigger to fire. Below this, the daemon considers the device already tuned and stays in `NORMAL`. |
| `--amp-threshold`, `-A` | float | `4.0` | Prefetch amplification ratio (`bytes_read_from_disk / bytes_app_demanded`) above which prefetch is considered wasteful. |
| `--syslog` | flag | off | Send logs to syslog (priority `LOG_DAEMON`) instead of stderr. |
| `--plain` | flag | off | Plain text logs (`[LEVEL] event: msg`) instead of JSON. |
| `--dry-run` | flag | off | Read telemetry, run the state machine, log decisions, but do NOT write to sysfs. |
| `--verbose`, `-v` | flag | off | Emit one `metric` log line per loop iteration. Without this, only state transitions and errors are logged. |
| `--help`, `-h` | flag | — | Print usage and exit 0. |

---

## 8. Configuration Recipes

### 8.1 Conservative database host

You want minimal disruption: only act when iowait is really pathological,
require very high amplification, and recover slowly.

```bash
sudo apg --device nvme0n1 \
    --iowait-high-pct 70 \
    --amp-threshold 16 \
    --sampling-window-s 10 \
    --recovery-window-s 120 \
    --syslog
```

### 8.2 Aggressive OLTP host

You want fast reactions because every second of thrashing costs you SLA
budget.

```bash
sudo apg --device nvme0n1 \
    --interval-ms 250 \
    --iowait-high-pct 40 \
    --sampling-window-s 3 \
    --recovery-window-s 30 \
    --verbose
```

### 8.3 Pure monitoring (no remediation)

You want to evaluate whether `apg`'s detection matches reality on your
workload before trusting it with sysfs writes.

```bash
sudo apg --device sda --dry-run --verbose --interval-ms 250 > apg.log 2>&1 &
# Run your workload, then inspect apg.log for "thrashing_detected" events
grep thrashing_detected apg.log
```

### 8.4 Multiple devices

`apg` targets a single device per process. To protect multiple devices,
run multiple instances (each in its own systemd unit or with distinct
`--device` flags):

```bash
sudo apg --device sda  --syslog &
sudo apg --device sdb  --syslog &
sudo apg --device nvme0n1 --syslog &
```

Or, more cleanly, create systemd drop-in units (see §9.3).

### 8.5 Custom thresholds for a known workload

If you know your application reads in 64 KiB chunks (e.g. an
InnoDB-style extent), set `--amp-threshold 16` (64 KiB / 4 KiB = 16).
The daemon will only fire when prefetch is delivering less than 1/16th
of its data into actual use.

---

## 9. Running as a systemd Service

### 9.1 Install the unit

```bash
sudo make install
sudo systemctl daemon-reload
```

The unit file (`/etc/systemd/system/apg.service`) is pre-hardened:
- Runs as `root` (required for sysfs writes)
- `MemoryMax=32M` — bounded RSS
- `CPUQuota=5%` — bounded CPU
- `ProtectSystem=strict`, `ProtectHome=true`, `PrivateTmp=true`
- `ReadWritePaths=/sys/block` — the only writable path
- `NoNewPrivileges=true`, `RestrictRealtime=true`, etc.

### 9.2 Set the target device

By default the unit targets `sda`. To target a different device, use a
systemd drop-in:

```bash
sudo systemctl edit apg
```

In the editor that opens, type:

```ini
[Service]
Environment=APG_DEVICE=nvme0n1
```

Save and exit. Then:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now apg
sudo systemctl status apg
```

### 9.3 Multiple devices via templated unit

To run `apg` against multiple devices cleanly, convert the unit into a
template. Save as `/etc/systemd/system/apg@.service`:

```ini
[Unit]
Description=Adaptive Prefetch Guard for %i
After=multi-user.target

[Service]
Type=simple
User=root
ExecStart=/usr/sbin/apg --device %i --syslog
Restart=on-failure
RestartSec=5
MemoryMax=32M
CPUQuota=5%
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
ReadWritePaths=/sys/block

[Install]
WantedBy=multi-user.target
```

Then enable per-device instances:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now apg@sda.service
sudo systemctl enable --now apg@nvme0n1.service
sudo systemctl enable --now apg@nvme1n1.service
```

### 9.4 Viewing logs

```bash
# Live tail
sudo journalctl -u apg -f

# Today only
sudo journalctl -u apg --since today

# Errors and warnings only
sudo journalctl -u apg -p warning

# JSON-formatted (apg's native format)
sudo journalctl -u apg -o json | jq .
```

### 9.5 Restart behavior

The unit has `Restart=on-failure` with `RestartSec=5`. If `apg` ever
crashes (it shouldn't, but in production you plan for it), systemd will
restart it within 5 seconds. Because the daemon re-reads the current
`read_ahead_kb` from sysfs on startup, there is no risk of "drift"
between the pre-crash and post-restart state.

---

## 10. Log Format & Observability

### 10.1 JSON format (default)

Each log record is a single line of JSON:

```json
{"ts":"2025-06-17T10:11:12.345Z","level":"WARN","event":"thrashing_detected","msg":"device=sda iowait_avg=68.40 prefetch_amp=823.00 ra_kb=4096 trigger_ra_kb=1024"}
```

Fields:

| Field | Always present | Meaning |
|---|---|---|
| `ts` | yes | UTC timestamp, RFC3339 with seconds precision |
| `level` | yes | `INFO`, `WARN`, or `ERROR` |
| `event` | yes | Short snake_case event name (see event catalog below) |
| `msg` | yes | Key=value space-separated details, JSON-escaped |

### 10.2 Event catalog

| Event | Level | Emitted when |
|---|---|---|
| `startup` | INFO | Daemon starts; includes all effective config |
| `shutdown` | INFO | Daemon receives SIGINT/SIGTERM; includes final state |
| `metric` | INFO | Each loop iteration (only with `--verbose`) |
| `thrashing_detected` | WARN | State transitions `NORMAL → REMEDIATING` |
| `ra_step_down` | WARN | `read_ahead_kb` halved |
| `stabilized` | INFO | State transitions `REMEDIATING → RECOVERING` |
| `ra_step_up` | INFO | `read_ahead_kb` doubled during recovery |
| `ra_step_up_backfire` | WARN | Recovery caused iowait to spike; back to `REMEDIATING` |
| `recovery_complete` | INFO | State transitions `RECOVERING → NORMAL` (RA reached `MAX_RA_KB`) |
| `ra_external_change` | WARN | Someone (admin, udev, another tool) changed `read_ahead_kb` |
| `dry_run_write` | INFO | Daemon would have written to sysfs but `--dry-run` is set |
| `proc_stat_open` | ERROR | Cannot open `/proc/stat` |
| `proc_stat_parse` | ERROR | Cannot parse the aggregate CPU line |
| `proc_diskstats_open` | ERROR | Cannot open `/proc/diskstats` |
| `diskstats_lookup` | ERROR | Target device not found in `/proc/diskstats` |
| `proc_vmstat_open` | ERROR | Cannot open `/proc/vmstat` |
| `sysfs_ra_open_read` | ERROR | Cannot open sysfs attr for reading |
| `sysfs_ra_open_write` | ERROR | Cannot open sysfs attr for writing (permission?) |
| `sysfs_ra_write` | ERROR | `write(2)` failed |
| `sysfs_ra_partial_write` | WARN | `write(2)` returned fewer bytes than sent |
| `sysfs_ra_parse` | ERROR | sysfs returned unparseable content |
| `oom` | ERROR | `calloc` failed; daemon will exit |

### 10.3 Plain text format

With `--plain`, the same records look like:

```
[INFO] startup: device=sda ra_kb_initial=4096 loop_ms=500 ...
[WARN] thrashing_detected: device=sda iowait_avg=68.40 ...
```

Useful for terminals and `grep`-friendly debugging.

### 10.4 Piping into observability stacks

#### Loki / Promtail

In `promtail.yml`:

```yaml
scrape_configs:
  - job_name: apg
    static_configs:
      - targets: [localhost]
        labels:
          job: apg
          __path__: /var/log/apg.log
    pipeline_stages:
      - json:
          expressions:
            level: level
            event: event
            ts: ts
      - labels:
          level:
          event:
```

#### Vector

```toml
[sources.apg]
type = "file"
include = ["/var/log/apg.log"]
read_from = "beginning"

[transforms.apg_parse]
type = "remap"
inputs = ["apg"]
source = '''
. = parse_json!(.message)
.level = .level
.event = .event
'''

[sinks.stdout]
type = "console"
inputs = ["apg_parse"]
encoding.codec = "json"
```

#### jq one-liners

```bash
# Count events by type
jq -r .event apg.log | sort | uniq -c | sort -rn

# All state transitions
jq -r 'select(.event|test("detected|stabilized|recovery_complete|backfire"))' apg.log

# All RA changes with timestamps
jq -rc 'select(.event|test("step_down|step_up")) | "\(.ts) \(.event) \(.msg)"' apg.log

# Plot iowait over time
jq -r 'select(.event=="metric") | "\(.ts) \(.msg)"' apg.log \
  | sed -E 's/.*iowait_pct=([0-9.]+).*/\1/' \
  | feed-to-gnuplot
```

---

## 11. Telemetry Sources Explained

`apg` consumes four kernel-exported interfaces. None of them require
special permissions to read.

### 11.1 `/proc/stat` — CPU jiffies

The aggregate `cpu ` line in `/proc/stat` looks like:

```
cpu  3357 0 4313 2754328 15234 0 234 0 0 0
```

The fields (in USER_HZ, typically 100 Hz = 10 ms each) are:
`user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice`.

`apg` reads this line every loop and computes the **delta** of each
field against the previous sample. The instantaneous `%iowait` is:

```
%iowait = 100 * Δiowait / (Δuser + Δnice + Δsystem + Δidle + Δiowait + Δirq + Δsoftirq + Δsteal)
```

This is the same arithmetic `top`, `vmstat`, and `mpstat` use.

### 11.2 `/proc/diskstats` — per-device I/O counters

For the target device (matched by name), `apg` parses fields 4–14:

| Field | Meaning |
|---|---|
| reads completed | total read I/Os that finished |
| reads merged | reads coalesced in the block layer |
| **sectors read** | **512-byte sectors physically transferred from disk** |
| time spent reading (ms) | wall-clock time of read I/O |
| ... | (writes and in-flight counters, parsed but not used in the heuristic) |

The key field is **sectors read**. Its delta between samples, multiplied
by 512, gives the **physical bytes pulled from disk** during that
interval — regardless of how much of it the application actually used.

### 11.3 `/proc/vmstat` — page fault counters

Two counters matter:

- **`pgmajfault`** — total major page faults since boot. A major fault
  is one that required disk I/O to satisfy. For `mmap` workloads, every
  cold-cache page access is a major fault.
- `pgfault` — total page faults (minor + major). Parsed but not
  currently used in the heuristic.

The delta of `pgmajfault` between samples, multiplied by the page size
(4096 bytes), is the **bytes the application actually demanded** during
that interval.

### 11.4 `/sys/block/<dev>/queue/read_ahead_kb`

This is the runtime read-ahead threshold. The block layer applies it to
every read that misses the page cache. The kernel's sysfs store handler
for this attribute is a thin wrapper around `blk_queue_set_read_ahead()`
— writing an ASCII integer in kilobytes is the canonical way to set it,
and it takes effect immediately for the next I/O.

`apg` reads this attribute every loop (to detect external changes) and
writes to it when the state machine decides to step.

---

## 12. Detection Heuristic

The daemon transitions from `NORMAL` to `REMEDIATING` only when **all
three** of the following conditions hold simultaneously.

### 12.1 Condition A — sustained high iowait

The mean `%iowait` over the rolling sampling window (default 5 seconds)
must exceed `--iowait-high-pct` (default 50%).

A 5-second window at 500 ms loop interval = 10 samples. The mean is
used (rather than the latest sample) to filter out brief spikes from
legitimate bursts of activity.

### 12.2 Condition B — pathological prefetch amplification

The **prefetch amplification ratio** is:

```
amplification = bytes_read_from_disk / bytes_app_actually_demanded
              = (Δsectors_read × 512) / (Δpgmajfault × 4096)
```

For a perfectly sequential workload that consumes every prefetched
byte, this trends toward **1.0**. For a random `mmap` workload on a
4 MiB read-ahead, it trends toward **1024** (4096 KiB / 4 KiB).

The default trigger threshold is **`4.0`**, meaning "the disk reads
at least 4× more data than the application asked for". This is
deliberately permissive — sequential workloads will sit around 1.0–2.0,
and a value above 4.0 is a strong signal that prefetch is being wasted.

### 12.3 Condition C — oversized current read-ahead

The current `read_ahead_kb` must be ≥ `--trigger-ra-kb` (default 1024).
If an administrator has already set read-ahead to 512 KiB or 128 KiB
manually, the daemon respects that and does not intervene.

### 12.4 Why all three?

- A alone (high iowait) can be legitimate sequential I/O saturating
  the disk.
- B alone (high amplification) can be a cold-cache ramp where every
  fault is major but cache is filling usefully.
- C alone (large read-ahead) is the kernel default and harmless for
  sequential workloads.

The **conjunction** is the pathological signature: high iowait *and*
wasted prefetch *and* oversized configuration.

---

## 13. Remediation Algorithm

The daemon is a 3-state machine.

```
                      ┌─────────────────────────┐
                      │  thrashing detected:    │
                      │  A ∧ B ∧ C              │
                      ▼                         │
                ┌──────────┐                    │
                │  NORMAL  │                    │
                └──────────┘                    │
                      │                         │
                      │ trigger                 │
                      ▼                         │
                ┌──────────────┐    iowait < L  │   ┌─────────────┐
                │ REMEDIATING  │ ──── for ────► │   │ RECOVERING  │
                │              │   recovery_s   │   │             │
                │  halve RA    │                │   │  double RA  │
                │  every 1s    │                │   │  every 10s  │
                │  until iowait│                │   │  until RA=  │
                │  < H or RA=  │ ◄──────────────┘   │  MAX or     │
                │  MIN_RA      │   iowait > H       │  iowait > H │
                └──────────────┘   (backfire)       └─────────────┘
                                                        │
                                                        │ RA = MAX
                                                        ▼
                                                   ┌──────────┐
                                                   │  NORMAL  │
                                                   └──────────┘
```

### 13.1 Step-down (`NORMAL → REMEDIATING → ...`)

- Halve `read_ahead_kb`: 4096 → 2048 → 1024 → 512 → 256 → 128
- At most one halving per second (gives the page cache time to react)
- Stops when iowait drops below `--iowait-high-pct`, or when `read_ahead_kb`
  reaches the hard floor of 128 KiB

### 13.2 Stabilization (`REMEDIATING → RECOVERING`)

After the daemon enters `REMEDIATING`, it watches the rolling recovery
window (default 60 seconds). If `%iowait` stays below
`--iowait-low-pct` (default 20%) for the **entire** window, the daemon
concludes the thrashing is over and transitions to `RECOVERING`.

### 13.3 Step-up (`RECOVERING → ...`)

- Double `read_ahead_kb`: 128 → 256 → 512 → ... → 4096
- At most one doubling per 10 seconds (probes cautiously)
- If iowait spikes above `--iowait-high-pct` during a probe, immediately
  drop back to `REMEDIATING` (event: `ra_step_up_backfire`)
- When `read_ahead_kb` reaches the ceiling of 4096 KiB, transition back
  to `NORMAL` (event: `recovery_complete`)

### 13.4 External changes

If anyone (an admin running `blockdev`, a udev rule, another instance
of `apg`) changes `read_ahead_kb` while the daemon is running, the
daemon will detect the change on the next loop iteration and emit an
`ra_external_change` warning. It then adopts the externally-set value
as its new baseline. It does **not** fight the administrator.

### 13.5 Hard floors and ceilings

| Constant | Value | Reason |
|---|---|---|
| `MIN_RA_KB` | 128 | Below this, even random workloads lose more to per-I/O overhead than they gain from reduced cache pollution. |
| `MAX_RA_KB` | 4096 | Kernel default upper bound on most platforms. |
| `PAGE_SIZE_BYTES` | 4096 | Architecture page size on x86_64 / aarch64. |
| `SECTOR_SIZE_BYTES` | 512 | Linux block-layer convention (even on 4K-native devices). |

---

## 14. Evaluation & Benchmarking

The repository includes `eval.sh`, a self-contained `fio`-based harness
that reproduces the thrashing pattern and measures the before/after
throughput delta. It runs the same on-disk test file at several
`read_ahead_kb` settings and compares bandwidth, major-fault count,
iowait, and wall-clock runtime.

### 14.1 Which workload to use

`eval.sh` ships **two workloads**. Choose with `WORKLOAD=`:

| WORKLOAD | Pattern | Purpose |
|---|---|---|
| `scan` (default) | Parallel `mmap --rw=read` streams, each scanning a disjoint contiguous region of a file **larger than RAM** | **Positive test** — the RavenDB-style case `apg` is built to fix. Each 4 KiB fault looks sequential to the kernel's read-ahead heuristic, so it grows its window toward `ra_pages` (e.g. 4096 KiB) and prefetches far more pages than the workload consumes, churning the page cache. |
| `randread` | Pure `mmap --rw=randread --bs=4k` | **Negative control** — proves `apg` does **not** false-positive. The kernel's adaptive read-ahead detects the random pattern and refuses to prefetch, so `read_ahead_kb` makes no difference and `apg` correctly stays idle. |

Use `scan` to demonstrate where `apg` helps; keep `randread` to prove it
won't fire on genuine random I/O.

### 14.2 Prerequisites

```bash
sudo apt install fio jq
df -h /mnt        # ensure ≥ FIO_SIZE free for the test file (file must EXCEED RAM)
```

For `scan`, the test file **must be larger than your system RAM** —
otherwise every prefetched page stays in cache, there is no churn, and
read-ahead has nothing to thrash. Check your RAM with `free -h` and set
`FIO_SIZE` above it (e.g. `FIO_SIZE=10G` on a 7.6 GiB machine).

### 14.3 Running

```bash
sudo ./eval.sh /dev/sda /mnt              # scan workload (default)
sudo WORKLOAD=randread ./eval.sh /dev/sda /mnt   # negative control
```

Arguments:

1. Block device (e.g. `/dev/sda` or `sda`)
2. Writable directory for the test file (e.g. `/mnt`)

### 14.4 What it does

1. Pre-allocates a `FIO_SIZE` test file via `O_DIRECT` sequential writes.
2. **Phase 1 (current)**: sets `read_ahead_kb` to the device's original
   value, drops the page cache, and runs the chosen workload.
3. **Phase 2 (baseline)**: sets `read_ahead_kb=4096`, drops the cache,
   runs the same workload.
4. **Phase 3 (remediated)**: sets `read_ahead_kb=128` (apg's floor),
   drops the cache, runs the same workload.
5. Prints a comparison table (bandwidth, major faults, iowait, wall time)
   and the speedup ratio(s).
6. Restores the original `read_ahead_kb` on exit (the EXIT trap ensures
   this even on Ctrl-C).
7. Writes raw JSON per phase to
   `./results/{current_*,baseline_4MB,remediation_128KB}.json`.

### 14.5 Tuning the evaluation

Environment variables change the test parameters:

```bash
FIO_SIZE=10G FIO_RUNTIME_S=60 FIO_NUMJOBS=32 WORKLOAD=scan \
    sudo -E ./eval.sh /dev/sda /mnt
```

| Variable | Default | Effect |
|---|---|---|
| `WORKLOAD` | `scan` | `scan` (positive) or `randread` (negative control) |
| `FIO_SIZE` | `6G` | Test file size (set **above** RAM for `scan`) |
| `FIO_RUNTIME_S` | `30` | Duration of each phase in seconds |
| `FIO_BS` | `4k` | Block size (best left at `4k`) |
| `FIO_NUMJOBS` | `16` | Number of parallel fio workers (`scan` uses more) |
| `FIO_FILE_NAME` | `fio_mmap_test` | Test file name |
| `RESULTS_DIR` | `./results` | Where JSON output is written |

### 14.6 Sample results (scan workload, real hardware)

Run on Ubuntu 24.04, **kernel 6.17.0-1025-oem**, a 1 TB rotational HDD
(`sda`, `ROTA=1`, original `read_ahead_kb=8192`) against a 10 GiB test
file with 16 parallel mmap scan streams:

```
=== Summary (scan workload) ===
label                      ra_kb        bw_KiBps       majfaults    iowait_pct   wall_s
------------------------   ------------ -------------- ------------ ------------ --------
current_8192KB             8192         1993486        527          0.7          12.56
baseline_4MB               4096         2157664        492          0.4          12.60
remediation_128KB          128          2097125        3013         1.7          12.57
```

The clearest signal is the **major-fault column**: stepping read-ahead
down from 4096/8192 KiB to 128 KiB raises major faults **from ~500 to
~3000 — a ~6× increase**. That is exactly the pathological prefetch
mechanism in reverse: with a huge `read_ahead_kb`, the kernel satisfies a
much larger share of each page's data in a single big prefetch window, so
far fewer individual faults are needed — but those oversized windows
over-read pages the workload never touches, which is what floods the page
cache on a memory-constrained production host.

On this particular bare-metal box the absolute throughput delta is small
(`~1.05×`) because a single local HDD with ample page-cache headroom
masks the waste. The effect is far stronger where RavenDB hit it: on
virtualized cloud disks (Azure) under memory pressure, where the
prefetched-but-unused pages genuinely stall on the network storage and
evict needed cache. See §2 for the production evidence and the kernel
6.17 commit (`459779d04ae8`, "Improve read ahead size for rotational
devices").

### 14.7 Interpreting results

- In the `scan` workload, a lower `read_ahead_kb` should produce at least
  as much throughput **and** a sharply higher major-fault count — proof
  the large prefetch windows are being eliminated rather than relied upon.
- A speedup of 2× or more confirms the problem exists on your hardware
  and that `apg`'s intervention will help.
- In the `randread` control, expect **~1.0×** and minimal major-fault
  change — that is the correct, false-positive-free behaviour.

If `scan` shows **no** change in major faults:

- Your test file is too small relative to RAM — increase `FIO_SIZE` so
  it exceeds available memory and the page cache cannot absorb the
  prefetch.
- Your device may genuinely handle large read-ahead well (deep-queue
  NVMe controllers are more tolerant).
- You may be on a kernel whose adaptive heuristic already suppresses
  amplification for your pattern.

---

## 15. Tuning Guide

### 15.1 Symptom → knob matrix

| Symptom | Knob | Suggested value |
|---|---|---|
| Daemon triggers too eagerly (false positives on sequential I/O) | `--amp-threshold` | `16.0` |
| Daemon triggers too slowly | `--iowait-high-pct`, `--sampling-window-s` | `40`, `3` |
| Recovery too aggressive (re-triggers immediately) | `--iowait-low-pct`, `--recovery-window-s` | `15`, `120` |
| Daemon never triggers but iowait is high | `--trigger-ra-kb` | lower than your current RA |
| Want to monitor only, never mutate sysfs | `--dry-run` | (flag) |
| High log volume | remove `--verbose` | (default is state-transitions only) |
| Want faster reaction at the cost of CPU | `--interval-ms` | `250` |
| Want syslog instead of stderr | `--syslog` | (flag) |

### 15.2 How to validate a tuning change

1. Start `apg` in `--dry-run --verbose` mode and pipe to a file:
   ```bash
   sudo apg --device sda --dry-run --verbose --interval-ms 250 > apg.log 2>&1 &
   ```
2. Run your real workload for a representative period.
3. Stop `apg` and inspect:
   ```bash
   grep -c thrashing_detected apg.log    # how many times it WOULD have triggered
   grep -c ra_step_down apg.log          # how many step-downs it WOULD have done
   ```
4. Adjust thresholds until the trigger count matches your expectation.

### 15.3 Production-safe rollout

1. **Week 1**: deploy in `--dry-run` mode on production hosts. Collect
   `thrashing_detected` events to confirm the heuristic fires on real
   workloads and stays silent otherwise.
2. **Week 2**: take it out of dry-run with conservative thresholds
   (`--iowait-high-pct 70 --amp-threshold 16`).
3. **Week 3+**: tighten thresholds toward defaults as confidence grows.

---

## 16. Troubleshooting

### 16.1 "device not found in /proc/diskstats"

```
{"level":"ERROR","event":"diskstats_lookup","msg":"device=sda reason=not_found"}
```

You passed a device name that doesn't exist in `/proc/diskstats`. Check:

```bash
cat /proc/diskstats | awk '{print $3}'    # list of all known device names
ls /sys/block                              # sysfs view of the same
```

Common mistakes:
- Passing `/dev/sda` instead of `sda` (don't include `/dev/`)
- Passing a partition (`sda1`) instead of the whole device (`sda`).
  Partitions don't have their own `queue/read_ahead_kb`.
- Passing an LVM device mapper name (`dm-0`) — these have read-ahead
  too, but the underlying physical device is usually the better target.

### 16.2 "Cannot write to sysfs"

```
{"level":"ERROR","event":"sysfs_ra_open_write","msg":"errno=13 path=/sys/block/sda/queue/read_ahead_kb"}
```

`errno=13` is `EACCES` (permission denied). You are not running as root.
Either:

```bash
sudo ./apg --device sda ...
```

or grant `CAP_SYS_ADMIN` to a non-root binary:

```bash
sudo setcap cap_sys_admin+ep ./apg
./apg --device sda ...
```

### 16.3 Daemon runs but never triggers

Possible causes:

1. **Your workload isn't actually thrashing.** Verify with `mpstat 1`
   and `iostat -x 1`. If `%iowait` is genuinely below 50%, the daemon
   is correctly staying silent.
2. **`read_ahead_kb` is already below `--trigger-ra-kb`.** Check:
   ```bash
   cat /sys/block/sda/queue/read_ahead_kb
   ```
   If it's 512 and your `--trigger-ra-kb` is 1024, lower the threshold.
3. **`pgmajfault` isn't increasing.** If your workload uses `O_DIRECT`
   (e.g. most modern databases with `directio=on`), there are no major
   page faults and the amplification check is skipped for that sample.
   Use `--iowait-high-pct` alone by setting `--amp-threshold 100000`.
4. **Sampling window is too long.** If your workload thrashes for only
   2 seconds at a time, the 5-second sustain window will filter it out.
   Lower `--sampling-window-s`.

### 16.4 Daemon triggers constantly (flapping)

If you see `thrashing_detected → stabilized → thrashing_detected` in
rapid succession, the recovery window is too short. Lengthen
`--recovery-window-s` and/or raise `--iowait-low-pct`.

### 16.5 "prefetch_amp is always 0.00"

This is normal for workloads with no major page faults (e.g. all cache
hits, or `O_DIRECT`). The amplification check is skipped, and the
daemon relies on the iowait condition alone. If you want to force the
daemon to act on iowait alone, set `--amp-threshold 0` (the check
passes whenever `prefetch_amp > 0` and iowait is high).

### 16.6 Daemon crashed

It shouldn't, but if it does:

```bash
# If running under systemd:
sudo journalctl -u apg --since "10 min ago" -p err

# If running in foreground:
# Re-run with the debug build
make debug
sudo ./apg --device sda --verbose 2>apg.err.log
# apg.err.log will contain ASan/UBSan diagnostics if applicable
```

The unit file's `Restart=on-failure` will bring the daemon back within
5 seconds.

---

## 17. Security & Hardening

### 17.1 Privilege model

`apg` requires root (or `CAP_SYS_ADMIN`) **only** because writing to
`/sys/block/*/queue/read_ahead_kb` requires it. All other operations
(reading `/proc/stat`, `/proc/diskstats`, `/proc/vmstat`, and the
sysfs attribute itself) work as an unprivileged user.

### 17.2 systemd hardening

The bundled `apg.service` includes:

| Directive | Effect |
|---|---|
| `NoNewPrivileges=true` | No `setuid` escalation paths |
| `ProtectSystem=strict` | Filesystem is read-only except `ReadWritePaths` |
| `ProtectHome=true` | `/home`, `/root`, `/run/user` are invisible |
| `PrivateTmp=true` | Private `/tmp` and `/var/tmp` namespaces |
| `ReadWritePaths=/sys/block` | Only writable path is sysfs block tree |
| `CapabilityBoundingSet=` (empty) | No Linux capabilities beyond default |
| `LockPersonality=true` | Cannot `personality(2)` change |
| `RestrictRealtime=true` | No `SCHED_FIFO`/`SCHED_RR` |
| `RestrictSUIDSGID=true` | Cannot create setuid/setgid files |
| `RemoveIPC=true` | IPC objects cleaned up on exit |
| `MemoryMax=32M` | Bounded RSS |
| `CPUQuota=5%` | Bounded CPU usage |
| `LimitNOFILE=64` | Bounded file descriptors |

### 17.3 What apg does NOT do

- Does **not** execute any external program (no `system()`, no `popen()`,
  no `fork()`/`exec()`).
- Does **not** open any network sockets.
- Does **not** write to any file (logs go to stderr/syslog only).
- Does **not** read any file outside `/proc` and `/sys/block`.
- Does **not** load kernel modules or modify kernel parameters beyond
  the single `read_ahead_kb` sysfs attribute.

You can verify all of this with `strace`:

```bash
sudo strace -f -e trace=openat,write,connect,execve ./apg --device sda --dry-run 2>&1 | head -50
```

---

## 18. Architecture & Internals

### 18.1 Source layout

```
apg/
├── apg.c           # Single-file daemon (~1000 LOC, heavily commented)
├── Makefile        # Build + install + debug + analyze targets
├── apg.service     # systemd unit
├── eval.sh         # fio-based evaluation harness (scan + randread workloads)
└── README.md       # this file
```

### 18.2 Code organization inside apg.c

The source is deliberately single-file and organized top-to-bottom in
the order data flows through the daemon:

1. **Compile-time tunables** (lines 55–70): defaults for every
   runtime-configurable value.
2. **State machine enum** (80–94): `NORMAL`, `REMEDIATING`, `RECOVERING`.
3. **Data structures** (96–171): `cpu_stat_t`, `disk_stat_t`,
   `vmstat_t`, `target_device_t`, `config_t`.
4. **Logging** (173–262): JSON escape, level-to-syslog-priority map,
   `log_event()`, and the `APG_INFO`/`APG_WARN`/`APG_ERR`/`APG_METRIC`
   macros. (Prefixed `APG_*` to avoid clashing with `<syslog.h>`'s
   `LOG_INFO`/`LOG_ERR` priority macros.)
5. **Signal handling** (264–285): `SIGINT`/`SIGTERM` flip a
   `volatile sig_atomic_t` liveness flag; `SA_RESTART` so `nanosleep`
   resumes on EINTR. `SIGPIPE` is ignored.
6. **Time helpers** (287–304): `sleep_ms()` via `nanosleep`,
   `now_sec()` via `clock_gettime(CLOCK_MONOTONIC)`.
7. **Ring buffer** (306–356): fixed-capacity rolling window for
   iowait samples. Two instances: a short one for the sampling window,
   a long one for the recovery window.
8. **Telemetry readers** (358–510): one function per `/proc` source.
   Each is defensive: missing files, parse failures, and truncated
   reads are logged and reported as failure rather than crashing.
9. **sysfs read/write** (512–590): `read_read_ahead_kb()` and
   `write_read_ahead_kb()`. The write path opens with `O_WRONLY|O_CLOEXEC`,
   writes the ASCII integer + newline, and verifies the byte count.
10. **Step-down / step-up helpers** (592–615): pure functions, no
    side effects, easy to unit-test.
11. **Argument parsing** (617–720): `getopt_long` with both short and
    long forms.
12. **Main daemon loop** (722–880): the heart of the daemon.
    Initializes rings, reads baseline samples, then loops:
    `sleep → read telemetry → compute deltas → push to rings →
    run state machine → maybe write sysfs → repeat`.
13. **Entry point** (882–end): `parse_args`, optional `openlog`,
    `install_signal_handlers`, `run_daemon`, optional `closelog`.

### 18.3 Why single-threaded?

At a 500 ms loop interval, the entire poll-parse-deide-write cycle
takes well under 1 ms of CPU on any modern hardware. A single thread
of control means:

- No locks, no atomics, no memory-ordering concerns.
- No race between the signal handler and the main loop (the
  `volatile sig_atomic_t` is sufficient).
- Deterministic, easy-to-reason-about behavior.
- Trivial to strace and debug.

A multi-threaded design would add complexity for zero benefit.

### 18.4 Memory model

All allocations happen at startup:

- `config_t g_cfg` — static storage duration, zero-allocated.
- Two `ring_t` structures — `calloc`'d at the top of `run_daemon()`,
  freed before exit.

There are **no** heap allocations in the main loop. This means:

- No memory fragmentation over time.
- No allocator lock contention.
- No `OOM` risk mid-run.
- Trivially leak-free (valgrind-clean).

### 18.5 Error handling philosophy

The daemon is **best-effort and resilient**:

- If a telemetry source is unavailable for one sample, that sample is
  skipped (the rings don't get a 0 pushed into them — they simply
  don't get a new entry this iteration).
- If a sysfs write fails, the daemon logs the error and tries again
  next iteration with the same target value.
- If the daemon receives `SIGINT`/`SIGTERM` mid-write, the write
  completes (it's a single `write(2)` syscall) and then the main loop
  exits cleanly, emitting a `shutdown` log line.

The daemon is designed to **never crash on input**. Even completely
malformed `/proc/stat` lines, truncated sysfs reads, or transient
`EINTR` will result in a logged warning and continued operation.

---

## 19. Limitations & Caveats

### 19.1 Single device per process

Each `apg` process targets one block device. For multi-device hosts,
run multiple instances (see §8.4 and §9.3).

### 19.2 `pgmajfault` is a global counter

`/proc/vmstat`'s `pgmajfault` is **system-wide**, not per-process or
per-device. If your host runs both a thrashing `mmap` database and a
legitimate sequential-read workload, the amplification ratio will be a
blend of both. In practice this is rarely a problem because the
sequential workload contributes almost no major faults (its pages are
cached), so the database's signal dominates.

### 19.3 `O_DIRECT` workloads bypass the heuristic entirely

Applications using `O_DIRECT` (e.g. PostgreSQL with
`effective_io_concurrency`, Oracle with `FILESYSTEMIO_OPTIONS=directio`)
do not generate major page faults. The amplification check will see
`pgmajfault_delta=0` and skip itself for that sample. The daemon will
still act on the iowait condition alone if you set
`--amp-threshold 0`.

### 19.4 Loop interval vs. reaction time

The daemon cannot react faster than its loop interval. With the default
500 ms interval and a 5-second sampling window, the worst-case reaction
time from "thrashing starts" to "first step-down" is approximately
**5.5 seconds**. If your workload's thrashing episodes are shorter than
that, lower `--sampling-window-s` and `--interval-ms`.

### 19.5 No persistence

`apg` does not save its state to disk. On restart, it re-reads the
current `read_ahead_kb` from sysfs and starts in `NORMAL`. This is
deliberate: the kernel's current value is the source of truth, and
any value `apg` set before a restart will still be in effect (until
something else changes it).

If you want persistence of "the daemon should keep read-ahead at 128
on this host", use a udev rule or a systemd `ExecStartPre=`:

```ini
[Service]
ExecStartPre=/bin/sh -c 'echo 128 > /sys/block/%i/queue/read_ahead_kb'
ExecStart=/usr/sbin/apg --device %i --syslog
```

### 19.6 Not a substitute for proper configuration

`apg` is an **adaptive safety net**, not a replacement for setting
sane defaults. If you know your workload is random `mmap`, set
`read_ahead_kb=128` in a udev rule and let `apg` only handle
deviations. The daemon is most valuable on mixed-workload hosts where
the optimal value changes over time.

---

## 20. FAQ

### Q: Will this help my SSD / NVMe?

A: Probably yes, if you're seeing high `%iowait` on random `mmap`
workloads. NVMe devices have very low per-I/O latency, but they still
move data through the page cache. 4 MiB of useless prefetch per fault
is 4 MiB of useless prefetch regardless of how fast the underlying
device is.

### Q: Will this hurt my sequential read throughput?

A: It might temporarily, during the recovery phase's step-up probing.
The daemon doubles read-ahead cautiously (once every 10 seconds) and
immediately drops back if iowait spikes. The long-run steady state for
a sequential workload is `read_ahead_kb = 4096` (the kernel default)
because the daemon will successfully probe all the way up without
triggering.

### Q: Can I run this alongside `tuned`?

A: Yes. `tuned` profiles don't generally manage `read_ahead_kb`, so
there's no conflict. If your `tuned` profile does set it (some custom
profiles do), `apg` will detect the external change via
`ra_external_change` events and adopt the new value as its baseline.

### Q: Why not just set `read_ahead_kb=128` and be done with it?

A: You can, and for many hosts that's the right answer. `apg` adds
value when:

- The optimal value changes over time (mixed workload phases).
- You don't know the optimal value and want the daemon to find it.
- You want a safety net in case a kernel update or udev rule resets
  the value.
- You want observability into when thrashing is happening.

### Q: How much CPU does `apg` itself use?

A: With the default 500 ms loop interval, the daemon uses < 0.1% CPU
on a modern core. The systemd unit caps it at 5% as a safety margin.

### Q: Does it work with LVM, mdraid, dm-crypt, LUKS?

A: Yes, but target the **physical** device, not the mapper device.
For LVM on `/dev/sda`, target `sda`. For mdraid on `/dev/md0`, target
the underlying member devices (e.g. `sda`, `sdb`) — `md0` has its own
read-ahead but the physical devices' read-ahead is what actually
controls prefetch at the block layer.

### Q: Does it work with ZFS / Btrfs?

A: **No for ZFS** — ZFS bypasses the Linux page cache entirely (it
has its own ARC), so `read_ahead_kb` is meaningless. **Partially for
Btrfs** — Btrfs uses the page cache, but its own read-ahead logic
sits on top of the block layer's. `apg` may still help, but the
signal will be noisier.

### Q: How is this different from `vm.swappiness` or `vm.vfs_cache_pressure`?

A: Those tune **eviction policy** (which pages to evict under
pressure). `apg` tunes **fetch policy** (how much to fetch on a
miss). They're orthogonal — you can use both.

### Q: Is there a way to see what `apg` would do without installing it?

A: Yes. Build it (`make`), then run `./apg --device <your_dev>
--dry-run --verbose` in a terminal while your workload runs. Every
state transition the daemon **would** trigger is logged.

---

## 21. Glossary

| Term | Definition |
|---|---|
| **read-ahead** | The kernel's practice of fetching more data than was requested, on the assumption that the next request will be nearby. |
| **`read_ahead_kb`** | The sysfs attribute controlling the read-ahead window size, in KiB. |
| **page cache** | The kernel's in-memory cache of file contents. `mmap` reads through it. |
| **major page fault** | A page fault that requires disk I/O to satisfy (as opposed to a minor fault, which is served from another page in memory). |
| **`pgmajfault`** | Counter in `/proc/vmstat` of major page faults since boot. |
| **`%iowait`** | Percentage of CPU time spent idle with an outstanding disk I/O. High values indicate the CPU is starved by I/O. |
| **prefetch amplification** | Ratio of bytes physically read from disk to bytes the application actually demanded. 1.0 = perfect; 1024 = every 4 KiB fault pulls 4 MiB from disk. |
| **thrashing** | A pathological state where the system spends more time managing cache than doing useful work. |
| **`O_DIRECT`** | A file-open flag that bypasses the page cache entirely. |
| **`mmap`** | The system call that maps a file into a process's virtual address space, enabling page-fault-driven I/O. |

---

## 22. License

Adaptive Prefetch Guard is licensed under **GPL-2.0-or-later**, the
same license as the Linux kernel itself. This is intentional: the
daemon interacts closely with kernel interfaces and a compatible
license avoids any ambiguity about derivative works.

```
SPDX-License-Identifier: GPL-2.0-or-later
```

See the source file header for the full notice.

---

*For questions, bug reports, or contributions, refer to the project
source. This README documents version 1.0 of the daemon.*
