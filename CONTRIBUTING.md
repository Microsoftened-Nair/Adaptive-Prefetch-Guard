# Contributing to Adaptive Prefetch Guard (apg)

First off: **thank you** for taking the time to contribute. This project
is small, focused, and deliberately minimalist — keeping it that way is a
feature, not a bug. The guidelines below exist to keep the codebase
healthy and to make reviewing your contribution as easy as possible.

---

## Table of Contents

1. [Code of Conduct](#code-of-conduct)
2. [Project Ground Rules](#project-ground-rules)
3. [Development Environment](#development-environment)
4. [Building & Testing](#building--testing)
5. [Code Style](#code-style)
6. [Submitting Changes](#submitting-changes)
7. [Reporting Bugs](#reporting-bugs)
8. [Security Vulnerabilities](#security-vulnerabilities)
9. [Maintainer Notes](#maintainer-notes)

---

## Code of Conduct

Everyone participating in this project — issues, PRs, reviews, chat,
email — is expected to uphold the [Contributor Covenant 2.1](CODE_OF_CONDUCT.md).
Be kind, be patient, assume good intent. The maintainer will enforce this
as needed.

---

## Project Ground Rules

Before you invest significant time in a PR, please read these. They
define what this project *is* and what it will not become.

### In scope

- Detecting and remediating page-cache I/O thrashing caused by oversized
  block read-ahead, on Linux, via direct sysfs manipulation.
- Pure C11 + POSIX. No external runtime dependencies. No external build
  dependencies beyond a C compiler and `make`.
- Single-file daemon. `apg.c` is intentionally monolithic; splitting it
  into multiple translation units is not a goal unless it materially
  improves compilation time or test isolation.
- Observability via structured JSON logs.

### Out of scope (will be rejected)

- Adding a configuration file. CLI flags + environment are sufficient.
  Configuration files introduce parsing code, schema validation, and
  reload semantics that this daemon does not need.
- Adding a metrics endpoint (Prometheus, StatsD, etc.). The JSON log
  stream is the canonical export; downstream collectors can scrape it.
  A future `--metrics-addr` flag is conceivable but not currently planned.
- Supporting non-Linux operating systems. The entire premise —
  `/proc/stat`, `/proc/diskstats`, `/sys/block/*/queue/read_ahead_kb` —
  is Linux-specific.
- Supporting the ZFS filesystem stack (ZFS bypasses the page cache).
- Linking against any library other than glibc.
- Adding a daemon-control socket or D-Bus interface.

### Things to discuss before implementing

- New telemetry sources (e.g. `/proc/meminfo`, cgroup stats). Open an
  issue first to discuss whether the new signal is worth the parsing cost.
- Changes to the default values of the trigger thresholds. Defaults are
  a UX decision that affects every user; we should align on them.
- Changes to the JSON log schema. These break downstream consumers.
- Performance optimizations that sacrifice readability. The hot path is
  < 1 ms per loop; micro-optimizations are rarely justified.

---

## Development Environment

### Minimum

- Linux (any modern distribution; Ubuntu 22.04+ recommended)
- `gcc >= 9` or `clang >= 10`
- `make`
- `glibc-devel` / `libc6-dev`

### For evaluation and integration testing

- `fio` (`apt install fio`)
- `jq` (`apt install jq`)
- A spare block device or a loopback device you can format and mount

### Recommended editor setup

- `clangd` with `compile_commands.json` (run `make bear` if you have
  `bear` installed; otherwise the `Makefile` is simple enough that
  `clangd` will work without it)
- Editor config: 4-space indentation, no tabs, 80-column soft wrap

---

## Building & Testing

### Release build

```bash
make
./apg --help
```

### Debug build (with AddressSanitizer + UndefinedBehaviorSanitizer)

```bash
make debug
sudo ./apg --device <dev> --dry-run --verbose --interval-ms 250
```

The ASan build will catch:
- Buffer overflows (heap and stack)
- Use-after-free
- Double-free
- Signed integer overflow
- Null pointer dereference
- Memory leaks at exit

If you change anything non-trivial, **run the ASan build before opening
a PR.**

### Static analysis

```bash
make analyze   # runs the compiler in -c mode with full warnings
```

Optionally, if you have `clang` and `scan-build`:

```bash
scan-build make clean && scan-build make
```

### Smoke test (no real device needed)

```bash
# Dry-run against any block device present on your machine.
# Produces metric logs every 500 ms; Ctrl-C to stop.
sudo ./apg --device $(ls /sys/block | grep -v 'loop\|ram\|zram' | head -1) \
    --dry-run --verbose --interval-ms 500
```

### Evaluation test (requires a real device with ≥ 3 GiB free)

```bash
sudo apt install fio jq
sudo ./eval.sh /dev/<your-device> /mnt
```

### CI

GitHub Actions runs on every push and PR:

- Build matrix: Ubuntu 22.04, Ubuntu 24.04, Debian 12
- `make` (release build, all warnings)
- `make debug` (ASan build, runs `--dry-run` for 5 s)
- YAML lint on `apg.service`

Run the same checks locally before pushing.

---

## Code Style

### Hard rules (enforced by `-Wpedantic`)

- C11 with GNU extensions (`-std=gnu11`)
- All warnings enabled: `-Wall -Wextra -Wpedantic`
- No compiler-specific pragmas
- No `#pragma once` (use traditional `#ifndef` guards)
- No variable-length arrays (VLAs)
- No designators in array initializers that the standard doesn't allow

### Soft conventions

- **Indentation**: 4 spaces, no tabs.
- **Line length**: 80 columns soft limit. Longer lines are acceptable
  for string literals and unavoidable function signatures; do not
  break a string literal across lines for the sake of column count.
- **Brace style**: K&R — opening brace on the same line as the
  controlling statement. Always brace single-statement bodies.
- **Naming**:
  - `snake_case` for functions, variables, struct members.
  - `UPPER_SNAKE_CASE` for macros and constants.
  - `PascalCase` is reserved for type names ending in `_t` (e.g.
    `cpu_stat_t`, `disk_stat_t`).
- **Typedefs**: typedef all `struct` and `enum` types with a `_t`
  suffix. Do not typedef pointers.
- **File layout**: keep functions in roughly the order they're called
  from `main()`. Helpers go above their callers; the entry point goes
  last.
- **Comments**: explain *why*, not *what*. The code already says what.
  Reference the kernel documentation when the logic depends on a
  specific kernel interface (e.g. `/proc/diskstats` field numbers).
- **Logging**: use the `APG_INFO` / `APG_WARN` / `APG_ERR` /
  `APG_METRIC` macros. Never call `fprintf(stderr, ...)` or `syslog(...)`
  directly except inside `log_event()` itself.
- **Error handling**: prefer `bool` return + `out` parameter over
  global error state. Log the error at the point of detection; do not
  propagate error strings up the call stack.
- **Memory**: no allocations in the main loop. All `malloc`/`calloc`
  happens at startup; nothing is freed mid-run except at shutdown.
- **Signal safety**: only `volatile sig_atomic_t` is safe to write
  from a signal handler. Do not call any libc function from a handler.

### File header

Every `.c` and `.h` file starts with:

```c
/*
 * <filename> - <one-line description>
 *
 * <longer description if useful>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
```

### Commit messages

Format:

```
<scope>: <imperative summary in 50 chars>

<body explaining what and why, wrapped at 72 chars>

<footer with issue refs, breaking-change notes, etc.>
```

Examples:

```
telemetry: tolerate missing pgmajfault in /proc/vmstat

Some embedded kernels omit pgmajfault. Treat it as 0 rather than
aborting the loop, and log a one-time WARN so the operator knows
amplification tracking is degraded.

Fixes: #42
```

```
state-machine: raise recovery step-up interval to 15s

10s was too aggressive on hosts with hot cold-cache ramps; the
recovery probe would re-trigger thrashing. 15s gives the page
cache time to absorb the larger read-ahead window before the next
probe.
```

---

## Submitting Changes

### Before you open a PR

1. Fork the repository and create a feature branch:
   ```bash
   git checkout -b fix/some-bug
   ```
2. Make your changes. Keep commits focused — one logical change per
   commit. If you find yourself writing "and also..." in a commit
   message, split the commit.
3. Run all of:
   ```bash
   make clean && make
   make debug
   make analyze
   ```
4. If you added a new CLI flag, update:
   - `usage()` in `apg.c`
   - The argument reference table in `README.md` §7.1
   - The tuning guide in `README.md` §15.1 if relevant
5. If you added a new log event, update the event catalog in
   `README.md` §10.2.
6. Update `CHANGELOG.md` under the `[Unreleased]` section.
7. Commit and push:
   ```bash
   git push origin fix/some-bug
   ```

### PR description

The PR template will prompt you for:

- **Summary**: what and why, in 1–3 sentences.
- **Type of change**: bug fix / new feature / refactor / docs / perf.
- **Backwards compatibility**: does this break any existing CLI,
  log format, or behavior?
- **Testing**: how you verified the change.
- **Checklist**: confirms you ran `make`, `make debug`, `make analyze`,
  updated docs and changelog.

### Review criteria

The maintainer will look for:

- **Correctness**: does it do what the PR claims?
- **Error handling**: are failure paths handled? Are errors logged?
- **No new allocations in the hot path**: see "Memory" under Code Style.
- **No new external dependencies**: see "Out of scope".
- **Tested**: did the ASan build run cleanly?
- **Documentation**: are README, CHANGELOG, and code comments updated?
- **Scope**: is the PR focused? A PR that does 5 unrelated things will
  be asked to split.

### Review turnaround

PRs are usually reviewed within 3 business days. If a week passes
without a response, ping the PR with a comment — the maintainer may
have missed the notification.

### After merge

- Your change will appear in the next release tag.
- You'll be credited in `CHANGELOG.md` unless you ask not to be.

---

## Reporting Bugs

Open a [GitHub Issue](https://github.com/Microsoftened-Nair/Adaptive-Prefetch-Guard/issues/new/choose)
and use the **Bug Report** template. Key information:

1. **apg version**: `apg --help` doesn't print the version; use
   `git rev-parse --short HEAD` or the release tag.
2. **Kernel version**: `uname -a`
3. **Distribution**: `cat /etc/os-release`
4. **Exact command line** you used to start `apg`.
5. **What you expected to happen.**
6. **What actually happened.** Paste the relevant log lines (JSON
   preferred). If the daemon crashed, paste the last 50 lines of
   `journalctl -u apg` or stderr output.
7. **Reproduction steps** if you can isolate them.

### Reproducible test case

The gold standard is:

```bash
# 1. Start apg
sudo ./apg --device <dev> --verbose > apg.log 2>&1 &
APG_PID=$!

# 2. Run the workload that triggers the bug (fio job, DB query, etc.)

# 3. Stop apg
kill -TERM $APG_PID

# 4. Attach apg.log and any iostat/mpstat output captured during the run
```

---

## Security Vulnerabilities

If you discover a security vulnerability:

1. **Do not open a public GitHub issue.**
2. Email the maintainer at `nairmeghraj@gmail.com` with `[apg security]` in
   the subject line.
3. Include:
   - A description of the vulnerability
   - The affected version (`git describe --tags`)
   - A minimal reproduction (if possible)
   - Suggested fix (optional)
4. You will receive an acknowledgement within 48 hours.
5. We will coordinate a fix and disclosure timeline. Credit will be
   given in the release notes unless you prefer to remain anonymous.

The threat model: `apg` runs as root and writes to sysfs. A bug that
allows an unprivileged user to influence the value written to
`read_ahead_kb` (e.g. via a symlink race on `/sys/block/`) is a
critical vulnerability.

---

## Maintainer Notes

(For the project maintainer; included here for transparency.)

### Release process

1. Update `CHANGELOG.md`: move `[Unreleased]` items to a new
   `[X.Y.Z] - YYYY-MM-DD` section.
2. Update version strings in `apg.service` / `Makefile` if applicable.
3. Tag:
   ```bash
   git tag -a vX.Y.Z -m "Release vX.Y.Z"
   git push origin vX.Y.Z
   ```
4. Create a GitHub Release with the changelog entry as the body.
5. Attach the static binary built on the oldest supported distribution
   (Ubuntu 22.04) for maximum compatibility.

### Versioning

- **Major (X)**: breaking changes to CLI, log format, or systemd unit.
- **Minor (Y)**: new features, new CLI flags, new log events. Backwards
  compatible.
- **Patch (Z)**: bug fixes only.

### Branching

- `main` is always releasable. PRs land on `main` via squash-merge.
- Long-lived feature branches are discouraged; if a change is too big
  for one PR, break it into a sequence of smaller PRs.

---

*This document is itself a work in progress. PRs to improve it are
welcome.*
