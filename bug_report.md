---
name: Bug Report
about: Something isn't working as expected
title: "[BUG] "
labels: bug, triage
assignees: ''
---

## Summary

<!-- One or two sentences describing the problem. -->

## Expected Behavior

<!-- What you thought would happen. -->

## Actual Behavior

<!-- What actually happened. Paste any error output. -->

## Steps to Reproduce

1.
2.
3.

## Environment

- **apg version**: <!-- `git describe --tags` or release tag, e.g. v1.0.0 -->
- **Kernel**: <!-- `uname -r` -->
- **Distribution**: <!-- `cat /etc/os-release | head -2` -->
- **Target device**: <!-- e.g. /dev/nvme0n1 -->
- **Command line**: <!-- exact `apg` invocation -->

## Log Output

```
Paste the relevant JSON log lines here. If the daemon crashed, paste the
last 50 lines of `journalctl -u apg` or stderr output.
```

## Additional Context

<!-- Any other context: workload description, `iostat -x 1` output,
`mpstat 1` output, related kernel changes, etc. -->

## Confirmation

- [ ] I have searched existing issues for duplicates.
- [ ] I have read the [README](../README.md), especially the
      [Troubleshooting](../README.md#16-troubleshooting) section.
- [ ] I can reproduce this on the latest release / `main` branch.
