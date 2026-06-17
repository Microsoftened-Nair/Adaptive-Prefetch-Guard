<!-- Thanks for contributing! Please fill in the sections below. -->

## Summary

<!-- 1-3 sentences: what does this PR do and why? -->

## Type of Change

- [ ] Bug fix (non-breaking change which fixes an issue)
- [ ] New feature (non-breaking change which adds functionality)
- [ ] Refactor (no functional change)
- [ ] Documentation only
- [ ] Performance improvement
- [ ] Breaking change (fix or feature that would cause existing
      functionality to not work as expected)

## Backwards Compatibility

<!-- Does this change any existing CLI flag, log format, or behavior?
If yes, explain how it's compatible (or why the break is justified). -->

- [ ] This change is fully backwards compatible.
- [ ] This change breaks: <!-- describe what breaks and why -->

## Testing

<!-- How did you verify this change works? Be specific. -->

- [ ] `make` passes with `-Wall -Wextra -Wpedantic`
- [ ] `make debug` passes (ASan + UBSan, no sanitizer errors)
- [ ] Smoke test: `sudo ./apg --device <dev> --dry-run --verbose` runs
      for at least 30 seconds without errors.
- [ ] `eval.sh` produces the expected results (if the change affects
      detection or remediation).

```
Paste any relevant output here.
```

## Documentation Updates

If this PR adds or changes user-visible behavior, I have updated:

- [ ] `usage()` in `apg.c`
- [ ] CLI reference table in `README.md` §7.1
- [ ] Event catalog in `README.md` §10.2 (if new log events)
- [ ] Tuning guide in `README.md` §15.1 (if new tunable)
- [ ] `CHANGELOG.md` under `[Unreleased]`

## Checklist

- [ ] My code follows the [style guide](../CONTRIBUTING.md#code-style)
- [ ] I have run `make analyze` and resolved all warnings
- [ ] I have not introduced any new external dependencies
- [ ] I have not added any heap allocations in the main loop
- [ ] My commits are focused (one logical change per commit)
- [ ] My commit messages follow the
      [convention](../CONTRIBUTING.md#commit-messages)

## Linked Issues

<!-- e.g. "Fixes #42", "Refs #17" -->

Fixes #
