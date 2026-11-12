# Changelog

All notable changes to `cpp-oa3` are recorded here. Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- CMake and PlatformIO build skeleton, Unity test harness, and CI for host (GCC, Clang) and ESP32.
- `oa3_model` component (milestone M1), with no heap use in steady state:
  - RFC 3339 datetimes (including the VTN-RI space-separated form) and ISO 8601 durations, with calendar-aware `add()` on the local wall clock.
  - Fixed-size `Program`, `Event` and `Notification` structs, with capacities set by `OA3_*` macros in `limits.h`.
  - Streaming parsers for programs, events and notifications, in camelCase or VTN-RI snake_case, over a caller-owned `JsonArena` for ArduinoJson; number values keep their exact wire text.
  - Interval resolution with `intervalPeriod` inheritance, the `0001-01-01` sequential start, and event-level `randomizeStart`.
  - `price_at()`: the value in force at an instant across events, with priority, multi-value sub-intervals, event `duration` looping, and the next change time.
  - Target matching helpers.
