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

// A number value's wire literal, or a string value. 23 holds a double's shortest round-trip
// literal; a Value is then 40 bytes.
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

// Payloads kept per interval, after the payload-type filter. Price-server events carry one.
#ifndef OA3_MAX_PAYLOADS
#define OA3_MAX_PAYLOADS 1
#endif

// Distinct payload types kept per event, after the payload-type filter. Each type string is
// stored once in the event and its payloads refer to it by index.
#ifndef OA3_MAX_EVENT_TYPES
#define OA3_MAX_EVENT_TYPES 4
#endif

// Values kept per event, across all its intervals' payloads: OA3_MAX_INTERVALS times the
// values per interval. A PRICE and a GHG payload per interval need 2 and 50.
#ifndef OA3_MAX_VALUES
#define OA3_MAX_VALUES 25
#endif

// /notifiers: broker URIs kept from the MQTT binding, and the length of each.
#ifndef OA3_NOTIFIER_URIS_MAX
#define OA3_NOTIFIER_URIS_MAX 4
#endif

#ifndef OA3_NOTIFIER_URI_MAX
#define OA3_NOTIFIER_URI_MAX 191
#endif

// A broker host name from a URI.
#ifndef OA3_HOST_MAX
#define OA3_HOST_MAX 127
#endif

// The MQTT binding's OAUTH2_BEARER_TOKEN username: "{clientID}" or a literal.
#ifndef OA3_MQTT_USERNAME_MAX
#define OA3_MQTT_USERNAME_MAX 63
#endif

// One topic name from a /notifiers/mqtt/topics response. Price-server event topics are 73
// characters: OpenADR/3.1.0/programs/<36-character id>/events/update.
#ifndef OA3_TOPIC_MAX
#define OA3_TOPIC_MAX 191
#endif

// Defaults for caller-owned arrays; the model takes a pointer and a capacity.
//
// Sizing for a window (ParseOptions::window): parse_events keeps only events that overlap the
// window, and drops each kept event's intervals that do not (unless the event loops). An MCU
// sizes OA3_MAX_EVENTS for the events one window can overlap and OA3_MAX_INTERVALS for the
// intervals one event can have in it. The price server publishes, per program, one event per
// day of 24 hourly intervals (23 or 25 on DST days): fetching /events?programID=<id>, a 24 h
// window overlaps at most 2 events, so OA3_MAX_EVENTS 2 and the default OA3_MAX_INTERVALS 25.
// A 72 h window needs OA3_MAX_EVENTS 4. When more events overlap than fit, the ones that reach
// into the window earliest are kept and the parse returns kTruncated.
#ifndef OA3_MAX_EVENTS
#define OA3_MAX_EVENTS 4
#endif

#ifndef OA3_MAX_PROGRAMS
#define OA3_MAX_PROGRAMS 4
#endif

// Bytes for the ArduinoJson arena that holds one filtered top-level object. A variant slot is
// two pointers wide, so a 64-bit host needs more than a 32-bit ESP32 for the same event. The
// arena holds the event as sent, before a window drops intervals: on a 64-bit host a
// price-server interval costs about 375 bytes, and a 72-interval event about 31.7 KB.
// test_event_at_every_limit checks the host figure. Not usable in #if.
#ifndef OA3_JSON_ARENA_SIZE
#define OA3_JSON_ARENA_SIZE (sizeof(void*) > 4 ? 32768 : 16384)
#endif
