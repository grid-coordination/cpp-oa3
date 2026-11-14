# Changelog

All notable changes to `cpp-oa3` are recorded here. Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- A smaller `Event`, 3,056 bytes with the default capacities instead of 5,400 (an `Interval` is 48 bytes instead of 144), with breaking changes to the entity structs:
  - Each event stores its payload type strings once, in `types` (`OA3_MAX_EVENT_TYPES`, default 4); `Payload::type` is replaced by `type_index`, read through `payload_type(event, payload)`. `find_payload()` takes the event, and `find_type()` is new. More distinct types than fit is `kCapacityExceeded`.
  - Entities hold durations as `CompactDuration` (20 bytes against `Duration`'s 36): calendar months, calendar days and elapsed seconds and nanoseconds, the parts `add()` applies, so every addition gives the same instant. `to_compact()` and `to_duration()` convert; `to_duration()` gives normalized fields (P14M as P1Y2M, P2W as P14D, PT90M as PT1H30M). An entity duration whose total months, days or seconds (hours, minutes and seconds together; 2^31 seconds is about 68 years) leave int32 is now `kInvalidField`. `Duration` itself and `parse_duration()`, `format_duration()` and `add()` are unchanged.
  - `Interval` holds its period in its own fields (`start`, `duration`, `has_start`, `has_duration`, and `has_interval_period` when the member was present) instead of an `IntervalPeriod`. `IntervalResolver::resolve()` takes the `Interval`.
  - An interval's own `randomizeStart` is kept only when the build defines `OA3_INTERVAL_RANDOMIZE_START`; otherwise it is ignored and the event's applies to every interval.

### Added

- `VenClient::events_revision()`, which changes whenever the stored events or program change, so a display can redraw a forecast from `price_schedule()` only when it is stale.
- A single event buffer: `VenStorage::scratch` may be nullptr, and `StaticVenStorage<N, ArenaSize, false>` gives that form. A fetch then parses into the live events, and one that fails after its response began to parse clears them and reports `has_data` false until a later fetch succeeds. With `OA3_MAX_EVENTS=2` and an 8 KB arena the storage is 14,344 bytes on the ESP32.

## [0.2.0] - 2026-11-14

MQTT notifications, run overnight on the ESP32 by esp32-firmware: 16-18 hours each on an ESP32-POE-ISO-WROVER (with `exclusive_tls`), an ESP32-S3 and an ESP32-P4, with no failures and no heap drift.

### Changed

- `start()` refuses a base URL that is not `https://` unless `allow_plaintext` is set. Schemes are case-insensitive.

### Added

- `price_schedule()`: the prices in force across a bounded window, one step per span `price_at()` reports, with gaps as steps without a value, for drawing a forecast.
- MQTT notifications (milestone M4), with `VenConfig::notifications = NotificationMode::kMqtt`, a port's `MqttSession` and a caller-owned `NotificationStorage` in `VenStorage::notifications` (10,088 bytes on the ESP32; with notifications off the `ci/esp32` build uses 504 bytes more RAM than without M4):
  - After the program resolves and the first `/events` fetch, the client reads `/notifiers` (or uses `mqtt_uri`), fetches the program's events topics and, with `follow_program`, the program's own topics, connects anonymously (MQTTS unless `allow_plaintext`), subscribes at QoS 1 (the `ALL` topic when the VTN lists one), and fetches `/events` again. Every reconnect repeats the whole cycle: topics are fetched again, the subscriptions are set-diffed when the broker kept the session, and `/events` is fetched again.
  - Event CREATE, UPDATE and DELETE notifications (POST and PUT read as CREATE and UPDATE) are applied to the held events within the window and capacity, by the same keep-earliest rule as a fetch; an older `modificationDateTime` is ignored. A retained CREATE or UPDATE applies only as a newer update of a stored event, and a CREATE or UPDATE of an event the last fetch lacked is ignored when modified before that fetch, so a broker's retained messages cannot restore a deleted event. A notification that arrives between two `/events` pages is applied after the last page. `on_price_change` fires without an HTTP request. A program UPDATE refreshes the program unless its `modificationDateTime` is older (or, retained, not newer); a program DELETE drops the events, reports no price in force and resolves the program again. An oversized or unparseable live notification triggers a fetch.
  - While subscribed, `/events` is polled every `notify_poll_period_s`; when the session is lost or cannot be opened, polling returns to `poll_period_s` and reconnects back off from `backoff_min_s`. A VTN that answers discovery with 404, 405 or 501, or with a body the client cannot read, is `UNAVAILABLE` and is asked again every `poll_period_s`.
  - `exclusive_tls`, for the classic ESP32: no HTTPS request while an MQTTS session may hold TLS. The session uses `clean_session` false; `disconnect()` is called before each HTTPS request and after any failed connect or subscribe, and the session is resumed after the request. Until a resume finds the broker kept the session, `/events` is polled every `poll_period_s`.
  - `VenStatusInfo` reports `notify` (`NotifyState`), `mqtt_result`, `mqtt_failures` and `notifications`, and `on_status` fires when `notify` changes.
- `VenConfig::allow_plaintext` (default false), a documented deviation from OpenADR 3.1.0, which requires TLS: it admits an `http://` base URL and plain `mqtt://` or `tcp://` brokers, preferring a TLS URI when one is offered, and the client logs a warning the first time it uses each. The POSIX demo's `--allow-plaintext` sets it. See [`doc/porting.md`](doc/porting.md#plaintext).
- `oa3_model`: `parse_notifiers()` (the spec's object and the VTN-RI array), `parse_topics()`, `parse_broker_uri()` (only `mqtts`, `ssl`, `mqtt` and `tcp` schemes) and `select_broker()`. `parse_notification()` accepts a DELETE whose object carries only its `id`.

## [0.1.0] - 2026-11-13

First release: a polling OpenADR 3.1.0 VEN that runs on the ESP32. An ESP32 port in esp32-firmware ran it for 10 hours on an Olimex ESP32-POE-ISO-WROVER against the price server, following every hourly price change with no heap drift.

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
