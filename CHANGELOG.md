# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Pending
- COPR / Launchpad PPA packaging
- AUR package
- Man page (`man apg(8)`)
- Integration tests against synthetic `/proc` fixtures

## [1.0.0] - 2026-06-17

### Added
- **Initial public release** of the Adaptive Prefetch Guard daemon.
- `apg.c` — single-file C11 daemon (~1000 LOC) implementing the full
  telemetry → state machine → sysfs remediation pipeline.
- Telemetry polling of `/proc/stat` (CPU jiffies → `%iowait`),
  `/proc/diskstats` (sectors_read), `/proc/vmstat` (`pgmajfault`), and
  `/sys/block/<dev>/queue/read_ahead_kb`.
- Three-state machine (`NORMAL` → `REMEDIATING` → `RECOVERING`) with
  powers-of-two step-down (4096 → 128 KiB) and step-up (128 → 4096 KiB).
- Three-condition trigger: sustained `%iowait > 50%` over 5 s window AND
  prefetch amplification > 4.0 AND `read_ahead_kb ≥ 1024`.
- Recovery: 60 s stabilization below 20% iowait, then 10 s-spaced step-up
  probes with immediate backfire rollback on iowait spike.
- Structured JSON logging (RFC3339 UTC timestamps, syslog priority mapping)
  with optional `--plain` text mode and `--syslog` sink.
- Direct POSIX `open(2)`/`write(2)` to sysfs — no `fork`/`exec`, no
  `blockdev(8)`, no shell.
- Graceful `SIGINT`/`SIGTERM` handling via `volatile sig_atomic_t`
  liveness flag with `SA_RESTART`.
- `--dry-run` mode for non-mutating observation.
- Full CLI tuning surface: `--iowait-high-pct`, `--iowait-low-pct`,
  `--amp-threshold`, `--sampling-window-s`, `--recovery-window-s`,
  `--trigger-ra-kb`, `--interval-ms`.
- `Makefile` with `release` / `debug` (ASan+UBSan) / `analyze` /
  `install` / `uninstall` targets.
- `apg.service` systemd unit with hardening (`ProtectSystem=strict`,
  `MemoryMax=32M`, `CPUQuota=5%`, `ReadWritePaths=/sys/block`).
- `eval.sh` — `fio`-based evaluation harness (`--ioengine=mmap --rw=randread`)
  comparing 4096 KiB vs 128 KiB read-ahead throughput.
- `README.md` — 22-section user and developer guide.
- `COPYING` — full GPL-2.0 license text.
- `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, issue/PR templates.
- GitHub Actions CI workflow (Ubuntu 22.04, 24.04, Debian 12).

### Security
- This is the initial release; no prior security advisories apply.
- Reported vulnerabilities should be emailed to the maintainer (see
  `CONTRIBUTING.md`) before public disclosure.

[Unreleased]: https://github.com/Microsoftened-Nair/Adaptive-Prefetch-Guard/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/Microsoftened-Nair/Adaptive-Prefetch-Guard/releases/tag/v1.0.0
