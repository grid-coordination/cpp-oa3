# Changelog

All notable changes to `cpp-oa3` are recorded here. Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- CMake and PlatformIO build skeleton, Unity test harness, and CI for host (GCC, Clang) and ESP32.
- `oa3_model` component (milestone M1), with no heap use in steady state:
  - UTC-only time: `DateTime` is a UTC instant (Unix seconds and nanoseconds). Parsing accepts `Z`, `+hh:mm`/`-hh:mm` offsets and the zone-less VTN-RI form (read as UTC) and converts to UTC; formatting always writes `Z`. ISO 8601 durations, with calendar-aware `add()` on the UTC date.
  - `TimeInterval` (`time_interval.h`), a half-open UTC interval with an explicit indefinite end (`P9999Y`), and `allen_relation()` over Allen's 13 interval relations, with `intersects()` and `contains_instant()`.
  - Fixed-size `Program`, `Event` and `Notification` structs, with capacities set by `OA3_*` macros in `limits.h`. Defaults fit one 25-interval, single-payload event (`OA3_MAX_PAYLOADS` 1, `OA3_MAX_VALUES` 25); an `Event` is 5,400 bytes.
  - Streaming parsers for programs, events and notifications, in camelCase or VTN-RI snake_case, over a caller-owned `JsonArena` for ArduinoJson; number values keep their exact wire text.
  - `ParseOptions::window`: `parse_events()` keeps only events that overlap the window and drops their intervals outside it, so an MCU sizes `OA3_MAX_EVENTS` to its horizon (2 for 24 h of one price-server program). When more events overlap than fit, the earliest are kept, in arrival order, and the result is `kTruncated`. The JSON arena still holds each event as sent. A window whose end precedes its start is `kInvalidField`.
  - Interval resolution with `intervalPeriod` inheritance, the `0001-01-01` sequential start, and event-level `randomizeStart`; resolved intervals expose a `TimeInterval`. `event_extent()` gives an event's hull and, with a `duration`, its looping span.
  - `price_at()`: the value in force at an instant across events, with priority, multi-value sub-intervals, event `duration` looping, and the next change time.
  - Target matching helpers.
  - `find_program()`: scans a `/programs` page for one `programName` without storing the others, and reports how many elements it read.
  - `ParseOptions::program_id` and `ven_targets`: `parse_events()` skips an event of another program, or one whose targets do not match the VEN, before it takes a slot.
  - `append_events()`: reads further `/events` pages into the same array, keeping the window's earliest events across pages.
- `oa3_client` component (milestone M3): `VenClient`, a polling VEN for one program with no threads and no heap.
  - Resolves the program by id (`GET /programs/<id>`) or by name, through the price server's `programName` query or, when a VTN refuses it with a 4xx, by paging `/programs`.
  - Polls `/events?programID=<id>` with `targets`, `skip` and `limit`, one request per `loop()`, reading further pages on the following calls. Only the configured payload type and the events in `[now, now + horizon)` are kept; a failed fetch keeps the last good events.
  - `on_price_change` fires when the value in force changes, including at interval boundaries without a request; `on_status` reports `VenStatus` changes, with the HTTP and parse results and the retry delay.
  - Exponential backoff on failure, `STALE` after `stale_after_polls` poll periods without a successful fetch, `WAITING_FOR_CLOCK` until the port's wall clock is valid, and a fresh fetch when the wall clock steps by more than 5 s.
  - Port interfaces `HttpTransport` (one request at a time; no connection outlives a call), `Clock` and `LogSink`. `HttpTransport::post()` has a default that returns `kUnsupported`, for the VTN authentication of a later release.
  - Caller-owned `VenStorage` (event arrays and JSON arena), so a port can place it in PSRAM; `StaticVenStorage<>` for static storage.
- POSIX port (`ports/posix`): `CurlHttpTransport` over libcurl, `PosixClock`, `stderr_log`, and the `oa3-ven-demo` program, with CI jobs for GCC and Clang.
- `doc/porting.md`: the contract a port implements, with guidance for arduino-esp32 3.x.
