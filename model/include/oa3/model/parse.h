#pragma once

#include <stddef.h>
#include <stdint.h>

#include <oa3/model/byte_reader.h>
#include <oa3/model/entities.h>
#include <oa3/model/json_arena.h>

namespace oa3 {

enum class ParseResult : uint8_t {
    kOk,
    kJsonError,         // malformed, truncated or empty JSON
    kTooLarge,          // the arena ran out, or nesting too deep
    kCapacityExceeded,  // more items, or a longer string, than a fixed struct holds
    kInvalidField,      // a required field is missing, or a field has the wrong type or format
};

const char* to_string(ParseResult r);

struct ParseOptions {
    // Interval payloads, descriptors and values are kept only for these types; nullptr keeps
    // every type.
    const char* const* payload_types;
    size_t payload_type_count;
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

ParseResult parse_event(const char* json, size_t len, JsonArena& arena, Event* out,
                        const ParseOptions* options = nullptr);
ParseResult parse_event(ByteReader& in, JsonArena& arena, Event* out,
                        const ParseOptions* options = nullptr);

// An /events response, as parse_programs.
ParseResult parse_events(const char* json, size_t len, JsonArena& arena, Event* out,
                         size_t capacity, size_t* count, const ParseOptions* options = nullptr);
ParseResult parse_events(ByteReader& in, JsonArena& arena, Event* out, size_t capacity,
                         size_t* count, const ParseOptions* options = nullptr);

ParseResult parse_notification(const char* json, size_t len, JsonArena& arena,
                               Notification* out, const ParseOptions* options = nullptr);
ParseResult parse_notification(ByteReader& in, JsonArena& arena, Notification* out,
                               const ParseOptions* options = nullptr);

}  // namespace oa3
