#pragma once

// Capacities of the fixed-size entity structs. Each may be overridden with a -D build flag.
// An *_MAX is a length in characters; the buffer that holds it is one byte larger, for
// the NUL. The defaults fit the price server (price.grid-coordination.energy).

// objectID allows 128 (openadr3.yaml objectID); price-server ids are 36-character UUIDs.
#ifndef OA3_ID_MAX
#define OA3_ID_MAX 63
#endif

// programName and eventName. A longer eventName is truncated; a longer programName is an
// error, because a VEN looks programs up by name.
#ifndef OA3_NAME_MAX
#define OA3_NAME_MAX 63
#endif

// valuesMap.type and payloadType.
#ifndef OA3_TYPE_MAX
#define OA3_TYPE_MAX 31
#endif

#ifndef OA3_UNITS_MAX
#define OA3_UNITS_MAX 15
#endif

#ifndef OA3_CURRENCY_MAX
#define OA3_CURRENCY_MAX 7
#endif

// One target string (openadr3.yaml target allows 128).
#ifndef OA3_TARGET_MAX
#define OA3_TARGET_MAX 47
#endif

// A number value's wire literal, or a string value.
#ifndef OA3_VALUE_TEXT_MAX
#define OA3_VALUE_TEXT_MAX 23
#endif

#ifndef OA3_MAX_TARGETS
#define OA3_MAX_TARGETS 4
#endif

// Event payload descriptors kept per program or event.
#ifndef OA3_MAX_DESCRIPTORS
#define OA3_MAX_DESCRIPTORS 4
#endif

// 24 hourly intervals, 25 on the fall-back DST day.
#ifndef OA3_MAX_INTERVALS
#define OA3_MAX_INTERVALS 25
#endif

// Payloads kept per interval, after the payload-type filter.
#ifndef OA3_MAX_PAYLOADS
#define OA3_MAX_PAYLOADS 2
#endif

// Values kept per event, across all its intervals' payloads.
#ifndef OA3_MAX_VALUES
#define OA3_MAX_VALUES 50
#endif

// Defaults for caller-owned arrays; the model takes a pointer and a capacity.
#ifndef OA3_MAX_EVENTS
#define OA3_MAX_EVENTS 4
#endif

#ifndef OA3_MAX_PROGRAMS
#define OA3_MAX_PROGRAMS 4
#endif

// Bytes for the ArduinoJson arena that holds one filtered top-level object. A variant slot is
// two pointers wide, so a 64-bit host needs more than a 32-bit ESP32 for the same event.
// test_event_at_every_limit checks the host figure. Not usable in #if.
#ifndef OA3_JSON_ARENA_SIZE
#define OA3_JSON_ARENA_SIZE (sizeof(void*) > 4 ? 32768 : 16384)
#endif
