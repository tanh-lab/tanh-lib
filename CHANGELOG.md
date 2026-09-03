# Changelog

All notable changes to tanh-lib are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `Logger`: configurable platform-sink identity. `LoggerConfig::m_platform_tag`
  (Android logcat tag, journald `SYSLOG_IDENTIFIER`), `m_platform_subsystem` and
  `m_platform_category` (Apple `os_log`) name what the platform sink files records
  under, so an embedder (anira) shows up under its own name in `adb logcat -s`,
  Console.app and `log stream`. Defaults `"thl"`, `"thl"`, `"logger"` keep the
  previous output; an empty string selects the default. Set them before the first
  record: a later `set_config()` applies to records dispatched after it returns, and
  on Apple platforms creates a new `os_log_t` (which the system never releases).
- `Logger`: per-record flags. `LogRecord::m_flags` travels unchanged from the
  emitting site to the sinks (the console, file and platform sinks ignore it; the
  callback sink sees it). New overloads take the flags right after the level:
  `log_with_source(level, flags, source, group, message)`,
  `logf(level, flags, group, fmt, ...)`, `rt::logf`/`rt::log(level, flags, ...)` and
  `rt::Queue::logf`/`vlogf`/`log(level, flags, ...)`; the existing signatures pass
  0. Reserved bits: `k_flag_realtime` (1, set by `rt::Queue::drain()` on every
  record it dispatches) and `k_flag_contract_violation` (2, only ever set by the
  caller). The real-time producers stay allocation- and lock-free (one more
  `uint32_t` in the fixed-size record).
- `Logger`: `LogRecord::m_dropped_before`. A `rt::Queue::drain()` pass takes the
  queue's drop counter before it pops and puts the count on the first record it
  dispatches; every other record carries 0, so summing the field over all records
  received counts every drop exactly once. `format_plain()` appends
  `[N real-time log message(s) dropped before this record]` and `format_logfmt()`
  a `dropped_before=N` field to a record that carries a count.

- CMake: `TANH_LOG_COMPILED_MAX_LEVEL` (`AUTO`, or `1`..`4`) chooses the most verbose log
  level compiled into the call sites. `AUTO`, the default, keeps the historical rule
  (Error only in Release builds, every level otherwise); an embedder that wants its runtime
  level to be the only filter in a shipped build sets `4` as a plain variable before the
  fetch.

### Changed

- `Logger`: the synthetic drop warning of `rt::Queue::drain()` (level Warning, source
  `rt`, group `thl.logger`) is only dispatched when a pass has no record to carry the
  count. Its message is now `real-time log queue overflowed` with the count in
  `m_dropped_before` (and in the rendered suffix), instead of
  `N real-time log message(s) dropped (queue full)` after every pass with drops.
