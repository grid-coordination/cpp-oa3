#pragma once

#include <stddef.h>
#include <stdint.h>

#include <oa3/model/byte_reader.h>
#include <oa3/model/entities.h>
#include <oa3/model/json_arena.h>
#include <oa3/model/time_interval.h>

namespace oa3 {

enum class ParseResult : uint8_t {
    kOk,
    kJsonError,         // malformed, truncated or empty JSON
    kTooLarge,          // the arena ran out, or nesting too deep
    kCapacityExceeded,  // more items, or a longer string, than a fixed struct holds
    kInvalidField,      // a required field is missing, or a field has the wrong type or format
    kTruncated,         // parse_events with a window: more events overlap it than fit; the ones
                        // reaching into it earliest are kept, complete and usable
};

const char* to_string(ParseResult r);

struct ParseOptions {
    // Interval payloads, descriptors and values are kept only for these types; nullptr keeps
    // every type.
    const char* const* payload_types;
    size_t payload_type_count;
    // Keeps only what overlaps this window, e.g. [now, now + 24 h). parse_events skips an event
    // outside it without using a slot; every event parse drops the intervals outside it, unless
    // the event has a duration (it may loop), and writes the resolved period into each interval
    // kept after a dropped one. An event with no resolvable interval period is kept. nullptr
    // keeps everything; an end before the start is kInvalidField.
    const TimeInterval* window;
    // parse_events and append_events skip, without using a slot, an event whose programID is
    // present and differs from program_id (when non-null), and one that targets_match()
    // rejects against ven_targets (when non-null).
    const char* program_id = nullptr;
    const char* const* ven_targets = nullptr;
    size_t ven_target_count = 0;
};

// Each function fills *out from scratch. Keys are read in camelCase or in VTN-RI snake_case
// (program_id is programID). The arena is reset per top-level object. options may be null.

ParseResult parse_program(const char* json, size_t len, JsonArena& arena, Program* out,
                          const ParseOptions* options = nullptr);
ParseResult parse_program(ByteReader& in, JsonArena& arena, Program* out,
                          const ParseOptions* options = nullptr);

// A /programs response. *count is the number of programs filled, also on failure.
// kCapacityExceeded when the array has more than capacity elements.
ParseResult parse_programs(const char* json, size_t len, JsonArena& arena, Program* out,
                           size_t capacity, size_t* count, const ParseOptions* options = nullptr);
ParseResult parse_programs(ByteReader& in, JsonArena& arena, Program* out, size_t capacity,
                           size_t* count, const ParseOptions* options = nullptr);

// Scans a /programs response for the program whose programName equals name, without storing
// the others: an element with another, missing or oversized programName is skipped unchecked.
// *found and *out are set on a match, and scanning continues to the end of the array.
// *scanned is the number of array elements read, also on failure, so a caller can tell the
// last page (fewer than its limit).
ParseResult find_program(const char* json, size_t len, JsonArena& arena, const char* name,
                         Program* out, bool* found, size_t* scanned,
                         const ParseOptions* options = nullptr);
ParseResult find_program(ByteReader& in, JsonArena& arena, const char* name, Program* out,
                         bool* found, size_t* scanned, const ParseOptions* options = nullptr);

ParseResult parse_event(const char* json, size_t len, JsonArena& arena, Event* out,
                        const ParseOptions* options = nullptr);
ParseResult parse_event(ByteReader& in, JsonArena& arena, Event* out,
                        const ParseOptions* options = nullptr);

// An /events response, as parse_programs. With options->window, kTruncated instead of
// kCapacityExceeded; kept events stay in arrival order.
ParseResult parse_events(const char* json, size_t len, JsonArena& arena, Event* out,
                         size_t capacity, size_t* count, const ParseOptions* options = nullptr);
ParseResult parse_events(ByteReader& in, JsonArena& arena, Event* out, size_t capacity,
                         size_t* count, const ParseOptions* options = nullptr);

// As parse_events, but keeps the *count events already in out and adds to them, so the pages
// of one /events query can be read into one array. *scanned is the number of array elements
// read, also on failure, so a caller can tell the last page (fewer than its limit).
ParseResult append_events(const char* json, size_t len, JsonArena& arena, Event* out,
                          size_t capacity, size_t* count, size_t* scanned,
                          const ParseOptions* options = nullptr);
ParseResult append_events(ByteReader& in, JsonArena& arena, Event* out, size_t capacity,
                          size_t* count, size_t* scanned, const ParseOptions* options = nullptr);

ParseResult parse_notification(const char* json, size_t len, JsonArena& arena,
                               Notification* out, const ParseOptions* options = nullptr);
ParseResult parse_notification(ByteReader& in, JsonArena& arena, Notification* out,
                               const ParseOptions* options = nullptr);

}  // namespace oa3
