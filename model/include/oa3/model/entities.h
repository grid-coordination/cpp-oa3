#pragma once

#include <stddef.h>
#include <stdint.h>

#include <oa3/model/limits.h>
#include <oa3/model/time.h>

namespace oa3 {

static_assert(OA3_MAX_TARGETS <= 255, "TargetList::count is uint8_t");
static_assert(OA3_MAX_DESCRIPTORS <= 255, "descriptor_count is uint8_t");
static_assert(OA3_MAX_PAYLOADS <= 255, "Interval::payload_count is uint8_t");
static_assert(OA3_MAX_EVENT_TYPES <= 255, "Payload::type_index is uint8_t");
static_assert(OA3_MAX_INTERVALS <= 65535, "Event::interval_count is uint16_t");
static_assert(OA3_MAX_VALUES <= 65535, "Event::value_count is uint16_t");

// Entity structs are plain aggregates: value-initialize them (Event e{}) or let a parse
// function fill them. A has_* flag is false when the field was absent or null.

enum class ValueKind : uint8_t { kNumber, kString, kBool, kPoint };

struct Value {
    double number;  // kNumber; kPoint: x
    union {
        double y;                           // kPoint
        char text[OA3_VALUE_TEXT_MAX + 1];  // kNumber: the literal exactly as on the wire; kString
    };
    ValueKind kind;
    bool boolean;
};

// A valuesMap. Its type is event.types[type_index] (payload_type()), and its values are
// event.values[first_value .. first_value + value_count).
struct Payload {
    uint8_t type_index;
    uint16_t first_value;
    uint16_t value_count;
};

struct PayloadDescriptor {
    char payload_type[OA3_TYPE_MAX + 1];
    char units[OA3_UNITS_MAX + 1];
    char currency[OA3_CURRENCY_MAX + 1];
};

// 3.1.0 targets are strings. non_string_count counts other elements (3.0.1 valuesMap
// targets), which are not kept.
struct TargetList {
    uint8_t count;
    uint8_t non_string_count;
    char items[OA3_MAX_TARGETS][OA3_TARGET_MAX + 1];
};

struct IntervalPeriod {
    DateTime start;
    CompactDuration duration;
    CompactDuration randomize_start;
    bool has_start;
    bool has_duration;
    bool has_randomize_start;
};

// An interval's intervalPeriod is held in its own fields; has_interval_period is set when the
// member was present, even as {}. An interval's randomizeStart is kept only when the build
// defines OA3_INTERVAL_RANDOMIZE_START; otherwise it is ignored, and the event's applies.
struct Interval {
    DateTime start;
    CompactDuration duration;
#ifdef OA3_INTERVAL_RANDOMIZE_START
    CompactDuration randomize_start;
#endif
    int32_t id;
    Payload payloads[OA3_MAX_PAYLOADS];
    uint8_t payload_count;
    bool has_interval_period : 1;
    bool has_start : 1;
    bool has_duration : 1;
#ifdef OA3_INTERVAL_RANDOMIZE_START
    bool has_randomize_start : 1;
#endif
};

struct Event {
    char id[OA3_ID_MAX + 1];
    char program_id[OA3_ID_MAX + 1];
    char event_name[OA3_NAME_MAX + 1];
    DateTime modification_date_time;
    IntervalPeriod interval_period;
    CompactDuration duration;
    uint32_t priority;  // lower is higher priority
    bool has_modification_date_time;
    bool has_priority;
    bool has_duration;
    bool has_interval_period;
    TargetList targets;
    uint8_t descriptor_count;
    PayloadDescriptor descriptors[OA3_MAX_DESCRIPTORS];
    uint8_t type_count;
    char types[OA3_MAX_EVENT_TYPES][OA3_TYPE_MAX + 1];
    uint16_t interval_count;
    uint16_t value_count;
    Interval intervals[OA3_MAX_INTERVALS];
    Value values[OA3_MAX_VALUES];
};

struct Program {
    char id[OA3_ID_MAX + 1];
    char program_name[OA3_NAME_MAX + 1];
    bool has_modification_date_time;
    DateTime modification_date_time;
    // A LOCAL attribute or a localPrice of true: the spec has both (Definition.md:1366, :3142).
    bool local;
    TargetList targets;
    uint8_t descriptor_count;
    PayloadDescriptor descriptors[OA3_MAX_DESCRIPTORS];
};

enum class ObjectType : uint8_t { kUnknown, kProgram, kEvent, kReport, kSubscription, kVen, kResource };

// VTN-RI and the spec's own example send "POST" (Definition.md:501) where the enum is CRUD:
// POST reads as kCreate, PUT as kUpdate, GET as kRead.
enum class Operation : uint8_t { kUnknown, kCreate, kRead, kUpdate, kDelete };

struct Notification {
    ObjectType object_type;
    Operation operation;
    TargetList targets;
    bool has_object;  // object_type is kProgram or kEvent and the matching member is filled
    Program program;
    Event event;
};

ObjectType parse_object_type(const char* s);
Operation parse_operation(const char* s);
const char* to_string(ObjectType t);
const char* to_string(Operation op);

// "" when type_index is out of range.
const char* payload_type(const Event& event, const Payload& payload);
// The index of type in event.types, or -1.
int find_type(const Event& event, const char* type);

// nullptr when absent.
const Payload* find_payload(const Event& event, const Interval& interval, const char* type);
const Value* payload_value(const Event& event, const Payload& payload, size_t i);
const PayloadDescriptor* find_descriptor(const Event& event, const char* payload_type);
const PayloadDescriptor* find_descriptor(const Program& program, const char* payload_type);

}  // namespace oa3
