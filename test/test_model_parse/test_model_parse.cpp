#include <stdio.h>
#include <string.h>

#include <oa3/model/byte_reader.h>
#include <oa3/model/entities.h>
#include <oa3/model/intervals.h>
#include <oa3/model/json_arena.h>
#include <oa3/model/parse.h>
#include <oa3/model/time.h>
#include <unity.h>

using oa3::ParseResult;

static oa3::StaticJsonArena<> arena;
static oa3::Event events[OA3_MAX_EVENTS];
static oa3::Event event;
static oa3::Program programs[OA3_MAX_PROGRAMS];
static oa3::Program program;
static oa3::Notification notification;
static char fixture[16384];
static char scratch[16384];

void setUp() {}
void tearDown() {}

static size_t load_fixture(const char* path) {
    FILE* f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    size_t n = fread(fixture, 1, sizeof(fixture) - 1, f);
    fclose(f);
    TEST_ASSERT_TRUE(n < sizeof(fixture) - 1);
    fixture[n] = '\0';
    return n;
}

static ParseResult event_from(const char* json, const oa3::ParseOptions* opt = nullptr) {
    return oa3::parse_event(json, strlen(json), arena, &event, opt);
}

static ParseResult program_from(const char* json, const oa3::ParseOptions* opt = nullptr) {
    return oa3::parse_program(json, strlen(json), arena, &program, opt);
}

static ParseResult notification_from(const char* json) {
    return oa3::parse_notification(json, strlen(json), arena, &notification);
}

#define ASSERT_RESULT(expected, actual) \
    TEST_ASSERT_EQUAL_STRING(oa3::to_string(expected), oa3::to_string(actual))

// A ByteReader that hands out one byte per readBytes() call.
class TrickleReader final : public oa3::ByteReader {
public:
    TrickleReader(const char* data, size_t len) : _data(data), _len(len), _pos(0) {}
    int read() override { return _pos < _len ? static_cast<unsigned char>(_data[_pos++]) : -1; }
    size_t readBytes(char* buffer, size_t length) override {
        if (length == 0 || _pos >= _len) {
            return 0;
        }
        buffer[0] = _data[_pos++];
        return 1;
    }

private:
    const char* _data;
    size_t _len;
    size_t _pos;
};

// python-oa3 tests/test_entities.py:23-74 (RAW_PROGRAM, RAW_EVENT)
static const char kRawProgram[] = R"({
  "id": "prog-001",
  "createdDateTime": "2024-06-15T10:00:00Z",
  "modificationDateTime": "2024-06-15T12:00:00Z",
  "objectType": "PROGRAM",
  "programName": "Test DR Program",
  "intervalPeriod": {"start": "2024-06-15T10:00:00Z", "duration": "PT1H"},
  "programDescriptions": [{"URL": "https://example.com/program"}],
  "payloadDescriptors": [
    {"objectType": "EVENT_PAYLOAD_DESCRIPTOR", "payloadType": "PRICE", "units": "KWH", "currency": "USD"}
  ],
  "attributes": [{"type": "PRICE", "values": [0.15, 0.25]}],
  "targets": ["target-1"]
})";

static const char kRawEvent[] = R"({
  "id": "evt-001",
  "createdDateTime": "2024-06-15T10:00:00Z",
  "modificationDateTime": "2024-06-15T12:00:00Z",
  "objectType": "EVENT",
  "programID": "prog-001",
  "eventName": "Peak Event",
  "duration": "PT2H",
  "priority": 1,
  "targets": ["target-1"],
  "payloadDescriptors": [
    {"objectType": "EVENT_PAYLOAD_DESCRIPTOR", "payloadType": "PRICE", "units": "KWH", "currency": "USD"}
  ],
  "intervalPeriod": {"start": "2024-06-15T14:00:00Z", "duration": "PT2H"},
  "intervals": [{"id": 0, "payloads": [{"type": "PRICE", "values": [0.5]}]}]
})";

// python-oa3 tests/test_entities.py:153-193 (TestCoerceProgram)
static void test_python_program() {
    ASSERT_RESULT(ParseResult::kOk, program_from(kRawProgram));
    TEST_ASSERT_EQUAL_STRING("prog-001", program.id);
    TEST_ASSERT_EQUAL_STRING("Test DR Program", program.program_name);
    TEST_ASSERT_EQUAL_UINT8(1, program.targets.count);
    TEST_ASSERT_EQUAL_STRING("target-1", program.targets.items[0]);
    TEST_ASSERT_EQUAL_UINT8(1, program.descriptor_count);
    TEST_ASSERT_EQUAL_STRING("PRICE", program.descriptors[0].payload_type);
    TEST_ASSERT_EQUAL_STRING("KWH", program.descriptors[0].units);
    TEST_ASSERT_EQUAL_STRING("USD", program.descriptors[0].currency);
    TEST_ASSERT_TRUE(program.has_modification_date_time);
    TEST_ASSERT_EQUAL_INT(2024, program.modification_date_time.year);
    TEST_ASSERT_EQUAL_UINT8(12, program.modification_date_time.hour);
    TEST_ASSERT_FALSE(program.local);
}

// python-oa3 tests/test_entities.py:196-262 (TestCoerceEvent)
static void test_python_event() {
    ASSERT_RESULT(ParseResult::kOk, event_from(kRawEvent));
    TEST_ASSERT_EQUAL_STRING("evt-001", event.id);
    TEST_ASSERT_EQUAL_STRING("prog-001", event.program_id);
    TEST_ASSERT_EQUAL_STRING("Peak Event", event.event_name);
    TEST_ASSERT_TRUE(event.has_priority);
    TEST_ASSERT_EQUAL_UINT32(1, event.priority);
    TEST_ASSERT_TRUE(event.has_duration);
    TEST_ASSERT_EQUAL_INT32(2, event.duration.hours);
    TEST_ASSERT_TRUE(event.has_interval_period);
    TEST_ASSERT_TRUE(event.interval_period.has_start);
    TEST_ASSERT_EQUAL_UINT8(14, event.interval_period.start.hour);
    TEST_ASSERT_EQUAL_UINT8(1, event.targets.count);
    TEST_ASSERT_EQUAL_UINT8(1, event.descriptor_count);
    TEST_ASSERT_EQUAL_STRING("PRICE", event.descriptors[0].payload_type);
    TEST_ASSERT_EQUAL_UINT16(1, event.interval_count);
    const oa3::Interval& iv = event.intervals[0];
    TEST_ASSERT_EQUAL_INT32(0, iv.id);
    TEST_ASSERT_FALSE(iv.has_interval_period);
    TEST_ASSERT_EQUAL_UINT8(1, iv.payload_count);
    TEST_ASSERT_EQUAL_STRING("PRICE", iv.payloads[0].type);
    const oa3::Value* v = oa3::payload_value(event, iv.payloads[0], 0);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_TRUE(v->kind == oa3::ValueKind::kNumber);
    TEST_ASSERT_EQUAL_STRING("0.5", v->text);
    TEST_ASSERT_EQUAL_DOUBLE(0.5, v->number);
    TEST_ASSERT_NOT_NULL(oa3::find_descriptor(event, iv.payloads[0].type));
}

static void test_python_event_export_price_case() {
    const char* json = R"({"programID":"prog-001","objectType":"EVENT",
      "payloadDescriptors":[{"objectType":"EVENT_PAYLOAD_DESCRIPTOR","payloadType":"EXPORT_PRICE",
                             "units":"KWH","currency":"USD"}],
      "intervals":[{"id":0,"payloads":[{"type":"EXPORT_PRICE","values":[0.10]}]}]})";
    ASSERT_RESULT(ParseResult::kOk, event_from(json));
    TEST_ASSERT_EQUAL_STRING("EXPORT_PRICE", event.intervals[0].payloads[0].type);
    TEST_ASSERT_EQUAL_STRING("EXPORT_PRICE", event.descriptors[0].payload_type);
    TEST_ASSERT_EQUAL_STRING("0.10", event.values[0].text);
    TEST_ASSERT_EQUAL_STRING("", event.id);
}

// python-oa3 tests/test_entities.py:542-596 (TestCoerceNotification)
static void test_python_notification_snake() {
    const char* json = R"({"object_type":"EVENT","operation":"CREATE","object":{
      "id":"evt-002","created_date_time":"2024-06-15T10:00:00Z",
      "modification_date_time":"2024-06-15T12:00:00Z","program_id":"prog-001",
      "event_name":"Notified Event",
      "intervals":[{"id":0,"payloads":[{"type":"PRICE","values":[1.00]}]}]}})";
    ASSERT_RESULT(ParseResult::kOk, notification_from(json));
    TEST_ASSERT_TRUE(notification.object_type == oa3::ObjectType::kEvent);
    TEST_ASSERT_TRUE(notification.operation == oa3::Operation::kCreate);
    TEST_ASSERT_TRUE(notification.has_object);
    TEST_ASSERT_EQUAL_STRING("Notified Event", notification.event.event_name);
    TEST_ASSERT_EQUAL_STRING("prog-001", notification.event.program_id);
    TEST_ASSERT_TRUE(notification.event.has_modification_date_time);
    TEST_ASSERT_EQUAL_STRING("1.00", notification.event.values[0].text);
}

static void test_python_notification_camel() {
    const char* json = R"({"objectType":"EVENT","operation":"CREATE","object":{
      "id":"evt-003","createdDateTime":"2024-06-15T10:00:00Z",
      "modificationDateTime":"2024-06-15T12:00:00Z","objectType":"EVENT",
      "programID":"prog-001","eventName":"Spec Event",
      "intervals":[{"id":0,"payloads":[{"type":"PRICE","values":[2.00]}]}]}})";
    ASSERT_RESULT(ParseResult::kOk, notification_from(json));
    TEST_ASSERT_TRUE(notification.object_type == oa3::ObjectType::kEvent);
    TEST_ASSERT_TRUE(notification.operation == oa3::Operation::kCreate);
    TEST_ASSERT_TRUE(notification.has_object);
    TEST_ASSERT_EQUAL_STRING("Spec Event", notification.event.event_name);
    TEST_ASSERT_EQUAL_STRING("prog-001", notification.event.program_id);
}

// python-oa3 tests/test_entities.py:523-539 (TestIsNotification)
static void test_python_is_notification() {
    ASSERT_RESULT(ParseResult::kOk,
                  notification_from(R"({"object_type":"EVENT","operation":"CREATE","object":{"programID":"p1"}})"));
    TEST_ASSERT_TRUE(notification.has_object);
    TEST_ASSERT_EQUAL_STRING("p1", notification.event.program_id);
    ASSERT_RESULT(ParseResult::kOk,
                  notification_from(R"({"objectType":"EVENT","operation":"CREATE","object":{"programID":"p1"}})"));
    ASSERT_RESULT(ParseResult::kInvalidField, notification_from(R"({"objectType":"EVENT"})"));
}

// clj-oa3 test/openadr3/entities_test.clj:426-434 (notification-camelcase-test)
static void test_clj_notification_camel_post() {
    snprintf(scratch, sizeof(scratch), R"({"objectType":"PROGRAM","operation":"POST","object":%s})",
             kRawProgram);
    ASSERT_RESULT(ParseResult::kOk, notification_from(scratch));
    TEST_ASSERT_TRUE(notification.object_type == oa3::ObjectType::kProgram);
    TEST_ASSERT_TRUE(notification.operation == oa3::Operation::kCreate);
    TEST_ASSERT_TRUE(notification.has_object);
    TEST_ASSERT_EQUAL_STRING("prog-001", notification.program.id);
}

// clj-oa3 test/openadr3/entities_test.clj:436-448 (notification-snake-case-test)
static void test_clj_notification_snake() {
    const char* json = R"({"object_type":"PROGRAM","operation":"POST","object":{
      "id":"prog-002","created_date_time":"2024-06-15T09:30:00Z",
      "modification_date_time":"2024-06-15T10:00:00Z","object_type":"PROGRAM",
      "program_name":"Snake Program"}})";
    ASSERT_RESULT(ParseResult::kOk, notification_from(json));
    TEST_ASSERT_TRUE(notification.object_type == oa3::ObjectType::kProgram);
    TEST_ASSERT_TRUE(notification.operation == oa3::Operation::kCreate);
    TEST_ASSERT_EQUAL_STRING("prog-002", notification.program.id);
    TEST_ASSERT_EQUAL_STRING("Snake Program", notification.program.program_name);
    TEST_ASSERT_TRUE(notification.program.has_modification_date_time);
}

// clj-oa3 test/openadr3/entities_test.clj:457-469 (notification?-test)
static void test_clj_notification_envelope() {
    ASSERT_RESULT(ParseResult::kOk, notification_from(R"({"objectType":"VEN","operation":"POST","object":{}})"));
    TEST_ASSERT_TRUE(notification.object_type == oa3::ObjectType::kVen);
    TEST_ASSERT_FALSE(notification.has_object);
    ASSERT_RESULT(ParseResult::kOk, notification_from(R"({"object_type":"VEN","operation":"POST","object":{}})"));
    ASSERT_RESULT(ParseResult::kInvalidField, notification_from(R"({"operation":"POST","object":{}})"));
    ASSERT_RESULT(ParseResult::kInvalidField, notification_from(R"({"objectType":"PROGRAM","object":{}})"));
    ASSERT_RESULT(ParseResult::kInvalidField,
                  notification_from(R"({"objectType":"PROGRAM","operation":"POST","object":null})"));
    ASSERT_RESULT(ParseResult::kJsonError, notification_from("not a map"));
    ASSERT_RESULT(ParseResult::kInvalidField, notification_from("null"));
}

static void test_notification_details() {
    ASSERT_RESULT(ParseResult::kOk, notification_from(R"({"objectType":"SUBSCRIPTION","operation":"DELETE",
      "object":{"id":"s"},"targets":["ven_0999"]})"));
    TEST_ASSERT_TRUE(notification.operation == oa3::Operation::kDelete);
    TEST_ASSERT_FALSE(notification.has_object);
    TEST_ASSERT_EQUAL_UINT8(1, notification.targets.count);
    TEST_ASSERT_EQUAL_STRING("ven_0999", notification.targets.items[0]);

    ASSERT_RESULT(ParseResult::kOk, notification_from(R"({"objectType":"EVENT","operation":"PUT",
      "object":{"programID":"p"},"targets":null})"));
    TEST_ASSERT_TRUE(notification.operation == oa3::Operation::kUpdate);
    TEST_ASSERT_EQUAL_UINT8(0, notification.targets.count);

    ASSERT_RESULT(ParseResult::kOk,
                  notification_from(R"({"objectType":"BOGUS","operation":"WHATEVER","object":{}})"));
    TEST_ASSERT_TRUE(notification.object_type == oa3::ObjectType::kUnknown);
    TEST_ASSERT_TRUE(notification.operation == oa3::Operation::kUnknown);
    TEST_ASSERT_FALSE(notification.has_object);

    // An inner objectType must agree with the envelope.
    ASSERT_RESULT(ParseResult::kInvalidField, notification_from(R"({"objectType":"EVENT","operation":"POST",
      "object":{"objectType":"PROGRAM","programID":"p"}})"));
    ASSERT_RESULT(ParseResult::kInvalidField,
                  notification_from(R"({"objectType":"EVENT","operation":"POST","object":[]})"));
}

static void test_price_server_programs() {
    size_t n = load_fixture(OA3_FIXTURE_DIR "/price_server_programs.json");
    size_t count = 99;
    ASSERT_RESULT(ParseResult::kOk,
                  oa3::parse_programs(fixture, n, arena, programs, OA3_MAX_PROGRAMS, &count));
    TEST_ASSERT_EQUAL_size_t(1, count);
    const oa3::Program& p = programs[0];
    TEST_ASSERT_EQUAL_STRING("b54d3d1f-bc87-4e47-bbc1-2b95958283fc", p.id);
    TEST_ASSERT_EQUAL_STRING("EELEC-013532223", p.program_name);
    TEST_ASSERT_EQUAL_UINT8(1, p.descriptor_count);
    const oa3::PayloadDescriptor* d = oa3::find_descriptor(p, "PRICE");
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_STRING("KWH", d->units);
    TEST_ASSERT_EQUAL_STRING("USD", d->currency);
    TEST_ASSERT_TRUE(p.has_modification_date_time);
    TEST_ASSERT_EQUAL_UINT32(471661791, p.modification_date_time.nanos);
    TEST_ASSERT_EQUAL_UINT8(9, p.modification_date_time.frac_digits);
    TEST_ASSERT_EQUAL_UINT8(0, p.targets.count);
    TEST_ASSERT_FALSE(p.local);
}

static void test_price_server_events() {
    size_t n = load_fixture(OA3_FIXTURE_DIR "/price_server_events.json");
    size_t count = 0;
    ASSERT_RESULT(ParseResult::kOk,
                  oa3::parse_events(fixture, n, arena, events, OA3_MAX_EVENTS, &count));
    TEST_ASSERT_EQUAL_size_t(3, count);
    const char* names[] = {"EELEC-013532223-2026-11-14", "EELEC-013532223-2026-11-13",
                           "EELEC-013532223-2026-11-12"};
    for (size_t i = 0; i < count; ++i) {
        const oa3::Event& e = events[i];
        TEST_ASSERT_EQUAL_STRING(names[i], e.event_name);
        TEST_ASSERT_EQUAL_STRING("b54d3d1f-bc87-4e47-bbc1-2b95958283fc", e.program_id);
        TEST_ASSERT_EQUAL_UINT16(24, e.interval_count);
        TEST_ASSERT_EQUAL_UINT16(24, e.value_count);
        TEST_ASSERT_TRUE(e.has_modification_date_time);
        TEST_ASSERT_EQUAL_UINT8(9, e.modification_date_time.frac_digits);
        TEST_ASSERT_TRUE(e.has_interval_period);
        TEST_ASSERT_EQUAL_INT32(24, e.interval_period.duration.hours);
        TEST_ASSERT_FALSE(e.has_priority);
        for (uint16_t k = 0; k < e.interval_count; ++k) {
            TEST_ASSERT_EQUAL_INT32(k, e.intervals[k].id);
            TEST_ASSERT_TRUE(e.intervals[k].has_interval_period);
            TEST_ASSERT_EQUAL_UINT8(1, e.intervals[k].payload_count);
            TEST_ASSERT_EQUAL_UINT16(k, e.intervals[k].payloads[0].first_value);
        }
    }
    const oa3::Event& first = events[0];
    TEST_ASSERT_EQUAL_STRING("66f13cb2-7aff-3a38-a20c-9e8eb508e678", first.id);
    TEST_ASSERT_EQUAL_UINT32(621411877, first.modification_date_time.nanos);
    TEST_ASSERT_EQUAL_UINT8(8, first.interval_period.start.hour);
    TEST_ASSERT_EQUAL_UINT8(14, first.interval_period.start.day);
    const oa3::Payload* p = oa3::find_payload(first.intervals[0], "PRICE");
    TEST_ASSERT_NOT_NULL(p);
    const oa3::Value* v = oa3::payload_value(first, *p, 0);
    TEST_ASSERT_EQUAL_STRING("0.050294", v->text);
    TEST_ASSERT_EQUAL_DOUBLE(0.050294, v->number);
    TEST_ASSERT_EQUAL_STRING("0.03222", first.values[13].text);
    TEST_ASSERT_EQUAL_STRING("0.053783", first.values[23].text);
    TEST_ASSERT_EQUAL_STRING("0.050992", events[2].values[23].text);

    char msg[96];
    snprintf(msg, sizeof(msg), "events fixture arena high water: %u of %u bytes",
             static_cast<unsigned>(arena.high_water()), static_cast<unsigned>(arena.capacity()));
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE(arena.high_water() < arena.capacity());
}

static void test_streaming_matches_buffer() {
    size_t n = load_fixture(OA3_FIXTURE_DIR "/price_server_events.json");
    static oa3::Event streamed[OA3_MAX_EVENTS];
    size_t a = 0;
    size_t b = 0;
    ASSERT_RESULT(ParseResult::kOk, oa3::parse_events(fixture, n, arena, events, OA3_MAX_EVENTS, &a));
    TrickleReader in(fixture, n);
    ASSERT_RESULT(ParseResult::kOk, oa3::parse_events(in, arena, streamed, OA3_MAX_EVENTS, &b));
    TEST_ASSERT_EQUAL_size_t(a, b);
    TEST_ASSERT_EQUAL_MEMORY(events, streamed, sizeof(oa3::Event) * a);
}

static void test_truncated_json() {
    size_t n = load_fixture(OA3_FIXTURE_DIR "/price_server_events.json");
    size_t count = 99;
    ASSERT_RESULT(ParseResult::kJsonError,
                  oa3::parse_events(fixture, n / 2, arena, events, OA3_MAX_EVENTS, &count));
    TEST_ASSERT_EQUAL_size_t(1, count);
    ASSERT_RESULT(ParseResult::kJsonError, event_from(""));
    ASSERT_RESULT(ParseResult::kJsonError, event_from(R"({"programID":"p")"));
    ASSERT_RESULT(ParseResult::kJsonError,
                  oa3::parse_events("", 0, arena, events, OA3_MAX_EVENTS, &count));
    ASSERT_RESULT(ParseResult::kJsonError,
                  oa3::parse_events("[{\"programID\":\"p\"}", strlen("[{\"programID\":\"p\"}"), arena, events, OA3_MAX_EVENTS, &count));
    TEST_ASSERT_EQUAL_size_t(1, count);
    ASSERT_RESULT(ParseResult::kJsonError,
                  oa3::parse_events("[{\"programID\":\"p\"} {}]", strlen("[{\"programID\":\"p\"} {}]"), arena, events, OA3_MAX_EVENTS, &count));
}

static void test_small_arena_too_large() {
    static oa3::StaticJsonArena<1024> small;
    size_t n = load_fixture(OA3_FIXTURE_DIR "/price_server_events.json");
    size_t count = 99;
    ASSERT_RESULT(ParseResult::kTooLarge,
                  oa3::parse_events(fixture, n, small, events, OA3_MAX_EVENTS, &count));
    TEST_ASSERT_EQUAL_size_t(0, count);
    TEST_ASSERT_TRUE(small.exhausted());
}

static void test_too_deep() {
    ASSERT_RESULT(ParseResult::kTooLarge,
                  event_from(R"({"programID":"p","x":[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]})"));
}

static void test_array_capacity_and_errors() {
    size_t count = 99;
    const char* two = R"([{"id":"a","programName":"A"}, {"id":"b","programName":"B"}])";
    ASSERT_RESULT(ParseResult::kCapacityExceeded,
                  oa3::parse_programs(two, strlen(two), arena, programs, 1, &count));
    TEST_ASSERT_EQUAL_size_t(1, count);
    TEST_ASSERT_EQUAL_STRING("a", programs[0].id);
    ASSERT_RESULT(ParseResult::kOk, oa3::parse_programs(two, strlen(two), arena, programs, 2, &count));
    TEST_ASSERT_EQUAL_size_t(2, count);
    TEST_ASSERT_EQUAL_STRING("B", programs[1].program_name);

    ASSERT_RESULT(ParseResult::kOk, oa3::parse_programs(" [ ] ", 5, arena, programs, 2, &count));
    TEST_ASSERT_EQUAL_size_t(0, count);

    const char* bad_second = R"([{"id":"a","programName":"A"},{"id":"b"},{"id":"c","programName":"C"}])";
    ASSERT_RESULT(ParseResult::kInvalidField,
                  oa3::parse_programs(bad_second, strlen(bad_second), arena, programs, 4, &count));
    TEST_ASSERT_EQUAL_size_t(1, count);

    ASSERT_RESULT(ParseResult::kInvalidField, oa3::parse_events("[1]", 3, arena, events, 4, &count));
    ASSERT_RESULT(ParseResult::kJsonError, oa3::parse_events("{}", 2, arena, events, 4, &count));
}

static void test_value_kinds() {
    const char* json = R"({"programID":"p","intervals":[{"id":0,"payloads":[
      {"type":"A","values":[5, -2.5e+3, 1e-3, "12", true, false, {"x":1.5,"y":-2}, null, 0]}]}]})";
    ASSERT_RESULT(ParseResult::kOk, event_from(json));
    TEST_ASSERT_EQUAL_UINT16(8, event.value_count);
    TEST_ASSERT_EQUAL_UINT16(8, event.intervals[0].payloads[0].value_count);
    TEST_ASSERT_TRUE(event.values[0].kind == oa3::ValueKind::kNumber);
    TEST_ASSERT_EQUAL_STRING("5", event.values[0].text);
    TEST_ASSERT_EQUAL_DOUBLE(5.0, event.values[0].number);
    TEST_ASSERT_EQUAL_STRING("-2.5e+3", event.values[1].text);
    TEST_ASSERT_EQUAL_DOUBLE(-2500.0, event.values[1].number);
    TEST_ASSERT_EQUAL_STRING("1e-3", event.values[2].text);
    TEST_ASSERT_EQUAL_DOUBLE(0.001, event.values[2].number);
    TEST_ASSERT_TRUE(event.values[3].kind == oa3::ValueKind::kString);
    TEST_ASSERT_EQUAL_STRING("12", event.values[3].text);
    TEST_ASSERT_TRUE(event.values[4].kind == oa3::ValueKind::kBool);
    TEST_ASSERT_TRUE(event.values[4].boolean);
    TEST_ASSERT_FALSE(event.values[5].boolean);
    TEST_ASSERT_TRUE(event.values[6].kind == oa3::ValueKind::kPoint);
    TEST_ASSERT_EQUAL_DOUBLE(1.5, event.values[6].number);
    TEST_ASSERT_EQUAL_DOUBLE(-2.0, event.values[6].y);
    TEST_ASSERT_EQUAL_STRING("0", event.values[7].text);
    TEST_ASSERT_NULL(oa3::payload_value(event, event.intervals[0].payloads[0], 8));
}

static void test_values_whitespace_and_escapes() {
    const char* json = "{\n \"programID\" : \"p\" ,\n \"intervals\" : [ {\n  \"payloads\" : [ {"
                       " \"type\" : \"A\\\"\" , \"values\" : [ 0.25 ,\t1\n] } , {\"values\": [ 7 ],"
                       " \"type\": \"B\"} ],\"id\" : 3 } ]\n}";
    ASSERT_RESULT(ParseResult::kOk, event_from(json));
    TEST_ASSERT_EQUAL_INT32(3, event.intervals[0].id);
    TEST_ASSERT_EQUAL_STRING("A\"", event.intervals[0].payloads[0].type);
    TEST_ASSERT_EQUAL_STRING("0.25", event.values[0].text);
    TEST_ASSERT_EQUAL_STRING("1", event.values[1].text);
    TEST_ASSERT_EQUAL_STRING("B", event.intervals[0].payloads[1].type);
    TEST_ASSERT_EQUAL_UINT16(2, event.intervals[0].payloads[1].first_value);
    TEST_ASSERT_EQUAL_STRING("7", event.values[2].text);
}

static void test_value_capacity() {
    ASSERT_RESULT(ParseResult::kCapacityExceeded, event_from(R"({"programID":"p","intervals":[{"id":0,
      "payloads":[{"type":"A","values":[0.123456789012345678901234567]}]}]})"));
    ASSERT_RESULT(ParseResult::kCapacityExceeded, event_from(R"({"programID":"p","intervals":[{"id":0,
      "payloads":[{"type":"A","values":["a string much longer than the value text buffer"]}]}]})"));
    ASSERT_RESULT(ParseResult::kCapacityExceeded, event_from(R"({"programID":"p","intervals":[{"id":0,
      "payloads":[{"type":"A","values":[1]},{"type":"B","values":[1]},{"type":"C","values":[1]}]}]})"));

    size_t len = static_cast<size_t>(
        snprintf(scratch, sizeof(scratch), R"({"programID":"p","intervals":[{"id":0,"payloads":[{"type":"A","values":[)"));
    for (int i = 0; i <= OA3_MAX_VALUES; ++i) {
        len += static_cast<size_t>(snprintf(scratch + len, sizeof(scratch) - len, "%s%d", i ? "," : "", i));
    }
    snprintf(scratch + len, sizeof(scratch) - len, "]}]}]}");
    ASSERT_RESULT(ParseResult::kCapacityExceeded, event_from(scratch));
}

static size_t build_intervals(int n) {
    size_t len = static_cast<size_t>(snprintf(scratch, sizeof(scratch),
                                              R"({"programID":"p","intervalPeriod":{"start":"2026-11-01T07:00:00Z","duration":"PT1H"},"intervals":[)"));
    for (int i = 0; i < n; ++i) {
        len += static_cast<size_t>(snprintf(scratch + len, sizeof(scratch) - len,
                                            R"(%s{"id":%d,"payloads":[{"type":"PRICE","values":[0.%03d]}]})",
                                            i ? "," : "", i, i));
    }
    len += static_cast<size_t>(snprintf(scratch + len, sizeof(scratch) - len, "]}"));
    return len;
}

static void test_interval_capacity() {
    build_intervals(25);
    ASSERT_RESULT(ParseResult::kOk, event_from(scratch));
    TEST_ASSERT_EQUAL_UINT16(25, event.interval_count);
    TEST_ASSERT_EQUAL_STRING("0.024", event.values[24].text);
    build_intervals(26);
    ASSERT_RESULT(ParseResult::kCapacityExceeded, event_from(scratch));
}

static void test_intervals_fields() {
    ASSERT_RESULT(ParseResult::kOk, event_from(R"({"programID":"p","intervals":[
      {"id":0,"intervalPeriod":{}},
      {"id":1,"intervalPeriod":{"start":"2024-06-15T14:00:00Z","duration":"PT15M","randomizeStart":"PT5M"}},
      {"id":2,"interval_period":{"randomize_start":"PT1M"},"payloads":null},
      {"id":-7}]})"));
    TEST_ASSERT_EQUAL_UINT16(4, event.interval_count);
    TEST_ASSERT_TRUE(event.intervals[0].has_interval_period);
    TEST_ASSERT_FALSE(event.intervals[0].interval_period.has_start);
    TEST_ASSERT_FALSE(event.intervals[0].interval_period.has_duration);
    TEST_ASSERT_FALSE(event.intervals[0].interval_period.has_randomize_start);
    TEST_ASSERT_TRUE(event.intervals[1].interval_period.has_start);
    TEST_ASSERT_EQUAL_INT32(15, event.intervals[1].interval_period.duration.minutes);
    TEST_ASSERT_EQUAL_INT32(5, event.intervals[1].interval_period.randomize_start.minutes);
    TEST_ASSERT_TRUE(event.intervals[2].interval_period.has_randomize_start);
    TEST_ASSERT_EQUAL_UINT8(0, event.intervals[2].payload_count);
    TEST_ASSERT_EQUAL_INT32(-7, event.intervals[3].id);
    TEST_ASSERT_FALSE(event.intervals[3].has_interval_period);

    ASSERT_RESULT(ParseResult::kInvalidField, event_from(R"({"programID":"p","intervals":[{"payloads":[]}]})"));
    ASSERT_RESULT(ParseResult::kInvalidField, event_from(R"({"programID":"p","intervals":[{"id":1.5}]})"));
    ASSERT_RESULT(ParseResult::kInvalidField, event_from(R"({"programID":"p","intervals":[{"id":"0"}]})"));
    ASSERT_RESULT(ParseResult::kInvalidField, event_from(R"({"programID":"p","intervals":[{"id":4294967296}]})"));
    ASSERT_RESULT(ParseResult::kInvalidField,
                  event_from(R"({"programID":"p","intervals":[{"id":0,"payloads":[{"values":[1]}]}]})"));
}

static void test_event_fields() {
    ASSERT_RESULT(ParseResult::kOk, event_from(R"({"programID":"p","priority":null,"createdDateTime":null,
      "eventName":null,"duration":null,"intervalPeriod":null,"targets":null,"payloadDescriptors":null,
      "intervals":null,"reportDescriptors":[{"payloadType":"X"}]})"));
    TEST_ASSERT_FALSE(event.has_priority);
    TEST_ASSERT_FALSE(event.has_duration);
    TEST_ASSERT_FALSE(event.has_interval_period);
    TEST_ASSERT_EQUAL_UINT16(0, event.interval_count);
    TEST_ASSERT_EQUAL_STRING("", event.event_name);

    ASSERT_RESULT(ParseResult::kOk, event_from(R"({"programID":"p","priority":0})"));
    TEST_ASSERT_TRUE(event.has_priority);
    TEST_ASSERT_EQUAL_UINT32(0, event.priority);
    ASSERT_RESULT(ParseResult::kInvalidField, event_from(R"({"programID":"p","priority":-1})"));
    ASSERT_RESULT(ParseResult::kInvalidField, event_from(R"({"programID":"p","priority":1.5})"));
    ASSERT_RESULT(ParseResult::kInvalidField, event_from(R"({"programID":"p","priority":"1"})"));
    ASSERT_RESULT(ParseResult::kInvalidField, event_from(R"({"id":"e"})"));
    ASSERT_RESULT(ParseResult::kInvalidField, event_from(R"({"programID":"p","objectType":"PROGRAM"})"));
    ASSERT_RESULT(ParseResult::kInvalidField, event_from(R"({"programID":"p","duration":"P"})"));
    ASSERT_RESULT(ParseResult::kInvalidField, event_from(R"({"programID":"p","duration":5})"));
    ASSERT_RESULT(ParseResult::kInvalidField,
                  event_from(R"({"programID":"p","modificationDateTime":"2024-13-01T00:00:00Z"})"));
    ASSERT_RESULT(ParseResult::kInvalidField,
                  event_from(R"({"programID":"p","intervalPeriod":{"start":"yesterday"}})"));
    ASSERT_RESULT(ParseResult::kInvalidField, event_from(R"({"programID":"p","intervalPeriod":"PT1H"})"));
    ASSERT_RESULT(ParseResult::kCapacityExceeded,
                  event_from(R"({"programID":"p","id":"0123456789012345678901234567890123456789012345678901234567890123"})"));
    ASSERT_RESULT(ParseResult::kInvalidField, event_from("[]"));
}

static void test_event_snake_case() {
    ASSERT_RESULT(ParseResult::kOk, event_from(R"({"id":"e","program_id":"p","event_name":"Snake",
      "object_type":"EVENT","modification_date_time":"2026-03-08 19:22:06",
      "interval_period":{"start":"2026-03-08 19:00:00","duration":"PT1H"},
      "payload_descriptors":[{"object_type":"EVENT_PAYLOAD_DESCRIPTOR","payload_type":"PRICE","units":"KWH"}],
      "intervals":[{"id":0,"interval_period":{"duration":"PT30M"},"payloads":[{"type":"PRICE","values":[0.2]}]}]})"));
    TEST_ASSERT_EQUAL_STRING("p", event.program_id);
    TEST_ASSERT_EQUAL_STRING("Snake", event.event_name);
    TEST_ASSERT_TRUE(event.has_modification_date_time);
    TEST_ASSERT_TRUE(event.modification_date_time.zone == oa3::ZoneForm::kNone);
    TEST_ASSERT_TRUE(event.has_interval_period);
    TEST_ASSERT_EQUAL_UINT8(1, event.descriptor_count);
    TEST_ASSERT_EQUAL_STRING("PRICE", event.descriptors[0].payload_type);
    TEST_ASSERT_EQUAL_STRING("", event.descriptors[0].currency);
    TEST_ASSERT_EQUAL_INT32(30, event.intervals[0].interval_period.duration.minutes);
    TEST_ASSERT_EQUAL_STRING("0.2", event.values[0].text);
}

static void test_event_name_truncation() {
    // 62 ASCII bytes, then a 3-byte em-dash that would straddle the 63-byte limit.
    snprintf(scratch, sizeof(scratch), "{\"programID\":\"p\",\"eventName\":\"%s\xE2\x80\x94tail\"}",
             "01234567890123456789012345678901234567890123456789012345678901");
    ASSERT_RESULT(ParseResult::kOk, event_from(scratch));
    TEST_ASSERT_EQUAL_size_t(62, strlen(event.event_name));

    snprintf(scratch, sizeof(scratch), "{\"programID\":\"p\",\"eventName\":\"%s\xE2\x80\x94\"}",
             "012345678901234567890123456789012345678901234567890123456789");
    ASSERT_RESULT(ParseResult::kOk, event_from(scratch));
    TEST_ASSERT_EQUAL_size_t(63, strlen(event.event_name));
}

static void test_targets() {
    ASSERT_RESULT(ParseResult::kOk,
                  event_from(R"({"programID":"p","targets":["a",{"type":"GROUP","values":[1,2]},"b",null]})"));
    TEST_ASSERT_EQUAL_UINT8(2, event.targets.count);
    TEST_ASSERT_EQUAL_UINT8(2, event.targets.non_string_count);
    TEST_ASSERT_EQUAL_STRING("b", event.targets.items[1]);
    ASSERT_RESULT(ParseResult::kCapacityExceeded,
                  event_from(R"({"programID":"p","targets":["a","b","c","d","e"]})"));
    ASSERT_RESULT(ParseResult::kCapacityExceeded,
                  event_from(R"({"programID":"p","targets":["0123456789012345678901234567890123456789012345678"]})"));
    ASSERT_RESULT(ParseResult::kInvalidField, event_from(R"({"programID":"p","targets":"a"})"));
}

static void test_payload_filter() {
    const char* types[] = {"EXPORT_PRICE"};
    oa3::ParseOptions opt = {types, 1};
    const char* json = R"({"programID":"p",
      "payloadDescriptors":[{"payloadType":"PRICE"},{"payloadType":"EXPORT_PRICE","units":"KWH"},
                            {"payloadType":"GHG"},{"payloadType":"X"},{"payloadType":"Y"}],
      "intervals":[{"id":0,"payloads":[{"type":"PRICE","values":[1,2,3]},{"type":"EXPORT_PRICE","values":[0.04]},
                                       {"type":"GHG","values":[9]}]},
                   {"id":1,"payloads":[{"values":[5],"type":"GHG"},{"values":[0.05],"type":"EXPORT_PRICE"}]}]})";
    ASSERT_RESULT(ParseResult::kOk, event_from(json, &opt));
    TEST_ASSERT_EQUAL_UINT8(1, event.descriptor_count);
    TEST_ASSERT_EQUAL_STRING("EXPORT_PRICE", event.descriptors[0].payload_type);
    TEST_ASSERT_EQUAL_UINT8(1, event.intervals[0].payload_count);
    TEST_ASSERT_EQUAL_UINT8(1, event.intervals[1].payload_count);
    TEST_ASSERT_EQUAL_UINT16(2, event.value_count);
    TEST_ASSERT_EQUAL_STRING("0.04", event.values[0].text);
    TEST_ASSERT_EQUAL_STRING("0.05", event.values[1].text);
    TEST_ASSERT_EQUAL_UINT16(1, event.intervals[1].payloads[0].first_value);
    TEST_ASSERT_NULL(oa3::find_payload(event.intervals[0], "PRICE"));

    ASSERT_RESULT(ParseResult::kCapacityExceeded, event_from(json));
}

static void test_program_fields() {
    ASSERT_RESULT(ParseResult::kOk, program_from(R"({"id":"x","program_name":"Snake","local_price":true})"));
    TEST_ASSERT_TRUE(program.local);
    TEST_ASSERT_EQUAL_STRING("Snake", program.program_name);
    ASSERT_RESULT(ParseResult::kOk,
                  program_from(R"({"id":"x","programName":"n","attributes":[{"type":"LOCAL","values":[true]}]})"));
    TEST_ASSERT_TRUE(program.local);
    ASSERT_RESULT(ParseResult::kOk,
                  program_from(R"({"id":"x","programName":"n","attributes":[{"type":"LOCAL","values":[]}]})"));
    TEST_ASSERT_TRUE(program.local);
    ASSERT_RESULT(ParseResult::kOk, program_from(R"({"id":"x","programName":"n","localPrice":false,
      "attributes":[{"type":"LOCAL","values":[false]},{"type":"OTHER","values":[true]}]})"));
    TEST_ASSERT_FALSE(program.local);
    ASSERT_RESULT(ParseResult::kOk, program_from(R"({"id":"x","programName":"n","payloadDescriptors":[
      {"objectType":"REPORT_PAYLOAD_DESCRIPTOR","payloadType":"USAGE"},
      {"objectType":"EVENT_PAYLOAD_DESCRIPTOR","payloadType":"PRICE"}]})"));
    TEST_ASSERT_EQUAL_UINT8(1, program.descriptor_count);
    TEST_ASSERT_EQUAL_STRING("PRICE", program.descriptors[0].payload_type);

    ASSERT_RESULT(ParseResult::kInvalidField, program_from(R"({"programName":"n"})"));
    ASSERT_RESULT(ParseResult::kInvalidField, program_from(R"({"id":"x"})"));
    ASSERT_RESULT(ParseResult::kInvalidField, program_from(R"({"id":"x","programName":"n","objectType":"EVENT"})"));
    ASSERT_RESULT(ParseResult::kCapacityExceeded, program_from(R"({"id":"x",
      "programName":"0123456789012345678901234567890123456789012345678901234567890123"})"));
    ASSERT_RESULT(ParseResult::kInvalidField,
                  program_from(R"({"id":"x","programName":"n","payloadDescriptors":[{"units":"KWH"}]})"));
}

static void test_program_trickle() {
    TrickleReader in(kRawProgram, strlen(kRawProgram));
    ASSERT_RESULT(ParseResult::kOk, oa3::parse_program(in, arena, &program));
    TEST_ASSERT_EQUAL_STRING("Test DR Program", program.program_name);
}

static void test_entities_helpers() {
    TEST_ASSERT_TRUE(oa3::parse_object_type("PROGRAM") == oa3::ObjectType::kProgram);
    TEST_ASSERT_TRUE(oa3::parse_object_type("RESOURCE") == oa3::ObjectType::kResource);
    TEST_ASSERT_TRUE(oa3::parse_object_type("program") == oa3::ObjectType::kUnknown);
    TEST_ASSERT_TRUE(oa3::parse_object_type(nullptr) == oa3::ObjectType::kUnknown);
    TEST_ASSERT_TRUE(oa3::parse_operation("POST") == oa3::Operation::kCreate);
    TEST_ASSERT_TRUE(oa3::parse_operation("GET") == oa3::Operation::kRead);
    TEST_ASSERT_TRUE(oa3::parse_operation("READ") == oa3::Operation::kRead);
    TEST_ASSERT_TRUE(oa3::parse_operation("PUT") == oa3::Operation::kUpdate);
    TEST_ASSERT_TRUE(oa3::parse_operation("delete") == oa3::Operation::kUnknown);
    TEST_ASSERT_EQUAL_STRING("SUBSCRIPTION", oa3::to_string(oa3::ObjectType::kSubscription));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", oa3::to_string(oa3::ObjectType::kUnknown));
    TEST_ASSERT_EQUAL_STRING("CREATE", oa3::to_string(oa3::Operation::kCreate));
    TEST_ASSERT_EQUAL_STRING("UPDATE", oa3::to_string(oa3::Operation::kUpdate));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", oa3::to_string(oa3::Operation::kUnknown));
    TEST_ASSERT_EQUAL_STRING("TOO_LARGE", oa3::to_string(ParseResult::kTooLarge));

    oa3::Event e{};
    e.value_count = 2;
    e.intervals[0].payload_count = 1;
    strcpy(e.intervals[0].payloads[0].type, "PRICE");
    e.intervals[0].payloads[0].first_value = 1;
    e.intervals[0].payloads[0].value_count = 2;
    TEST_ASSERT_NULL(oa3::find_payload(e.intervals[0], "GHG"));
    const oa3::Payload* p = oa3::find_payload(e.intervals[0], "PRICE");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_PTR(&e.values[1], oa3::payload_value(e, *p, 0));
    TEST_ASSERT_NULL(oa3::payload_value(e, *p, 1));
    TEST_ASSERT_NULL(oa3::find_descriptor(e, "PRICE"));
}

static void test_number_literals() {
    ASSERT_RESULT(ParseResult::kOk, event_from(R"({"programID":"p","intervals":[{"id":0,"payloads":[
      {"type":"A","values":[0.1, 0.050294, -0, 1E2, 3.141592653589793, 12345678901234567890123, 1e400, 5e-400]},
      {"type":"B","values":[{"x":0.050294,"y":1e-3}]}]}]})"));
    TEST_ASSERT_TRUE(event.values[0].number == 0.1);
    TEST_ASSERT_TRUE(event.values[1].number == 0.050294);
    TEST_ASSERT_TRUE(event.values[2].number == 0.0);
    TEST_ASSERT_EQUAL_STRING("-0", event.values[2].text);
    TEST_ASSERT_TRUE(event.values[3].number == 100.0);
    TEST_ASSERT_TRUE(event.values[4].number == 3.141592653589793);
    TEST_ASSERT_DOUBLE_WITHIN(1e9, 1.2345678901234567e22, event.values[5].number);
    TEST_ASSERT_TRUE(event.values[6].number > 1e308);
    TEST_ASSERT_TRUE(event.values[7].number == 0.0);
    TEST_ASSERT_TRUE(event.values[8].kind == oa3::ValueKind::kPoint);
    TEST_ASSERT_TRUE(event.values[8].number == 0.050294);
    TEST_ASSERT_TRUE(event.values[8].y == 0.001);

    ASSERT_RESULT(ParseResult::kJsonError, event_from(R"({"programID":"p","intervals":[{"id":0,
      "payloads":[{"type":"A","values":[01]}]}]})"));
    ASSERT_RESULT(ParseResult::kJsonError, event_from(R"({"programID":"p","intervals":[{"id":0,
      "payloads":[{"type":"A","values":[1.]}]}]})"));
    ASSERT_RESULT(ParseResult::kJsonError, event_from(R"({"programID":"p","intervals":[{"id":0,
      "payloads":[{"type":"A","values":[-]}]}]})"));
    ASSERT_RESULT(ParseResult::kJsonError, event_from(R"({"programID":"p","intervals":[{"id":0,
      "payloads":[{"type":"A","values":[1e+]}]}]})"));
}

static int64_t ux(const char* s) {
    oa3::DateTime d{};
    TEST_ASSERT_TRUE_MESSAGE(oa3::parse_datetime(s, &d), s);
    return oa3::to_unix(d);
}

// A JSON string starting with \u0001 once posed as a number literal and was strcpy'd
// unbounded into Value::text.
static void test_marker_bytes_in_strings() {
    static char long_ones[201];
    memset(long_ones, '1', 200);
    long_ones[200] = '\0';
    size_t n = static_cast<size_t>(snprintf(scratch, sizeof(scratch),
        "{\"programID\":\"p\",\"intervals\":[{\"id\":0,\"payloads\":[{\"type\":\"X\",\"values\":["));
    for (int i = 0; i < 49; ++i) {
        n += static_cast<size_t>(snprintf(scratch + n, sizeof(scratch) - n, "%d,", i));
    }
    snprintf(scratch + n, sizeof(scratch) - n, "\"\\u0001%s\"]}]}]}", long_ones);
    ASSERT_RESULT(ParseResult::kCapacityExceeded, event_from(scratch));

    ASSERT_RESULT(ParseResult::kOk, event_from(R"({"programID":"p","intervals":[{"id":0,"payloads":[
      {"type":"X","values":["\u00011.5","\u0002abc",'\u00017',"",2]}]}]})"));
    TEST_ASSERT_EQUAL_UINT16(5, event.value_count);
    TEST_ASSERT_TRUE(event.values[0].kind == oa3::ValueKind::kString);
    TEST_ASSERT_EQUAL_STRING("\x01" "1.5", event.values[0].text);
    TEST_ASSERT_TRUE(event.values[1].kind == oa3::ValueKind::kString);
    TEST_ASSERT_EQUAL_STRING("\x02" "abc", event.values[1].text);
    TEST_ASSERT_TRUE(event.values[2].kind == oa3::ValueKind::kString);
    TEST_ASSERT_EQUAL_STRING("", event.values[3].text);
    TEST_ASSERT_TRUE(event.values[4].kind == oa3::ValueKind::kNumber);
    TEST_ASSERT_EQUAL_STRING("2", event.values[4].text);

    ASSERT_RESULT(ParseResult::kInvalidField, event_from(R"({"programID":"p","intervals":[{"id":0,
      "payloads":[{"type":"X","values":[{"x":"\u00011","y":2}]}]}]})"));
    ASSERT_RESULT(ParseResult::kCapacityExceeded, event_from(R"({"programID":"p","intervals":[{"id":0,
      "payloads":[{"type":"X","values":["012345678901234567890123"]}]}]})"));
}

// python-oa3 time.py:24-26 and clj-oa3 entities.clj:47-56 read a blank string as absent.
static void test_blank_datetime_and_duration_absent() {
    ASSERT_RESULT(ParseResult::kOk, event_from(R"({"programID":"p","modificationDateTime":"",
      "duration":"  ","intervalPeriod":{"start":"","duration":"PT1H","randomizeStart":""}})"));
    TEST_ASSERT_FALSE(event.has_modification_date_time);
    TEST_ASSERT_FALSE(event.has_duration);
    TEST_ASSERT_TRUE(event.has_interval_period);
    TEST_ASSERT_FALSE(event.interval_period.has_start);
    TEST_ASSERT_TRUE(event.interval_period.has_duration);
    TEST_ASSERT_FALSE(event.interval_period.has_randomize_start);
    ASSERT_RESULT(ParseResult::kOk, program_from(R"({"id":"i","programName":"p","modificationDateTime":" "})"));
    TEST_ASSERT_FALSE(program.has_modification_date_time);
    ASSERT_RESULT(ParseResult::kInvalidField, event_from(R"({"programID":"p","modificationDateTime":42})"));
}

// Every field at its OA3_* limit: 63-character ids and name, 4 targets of 47, 4 descriptors,
// 25 intervals of 2 payloads, 50 values of 23 characters.
static void test_event_at_every_limit() {
    const char* id63 = "id-012345678901234567890123456789012345678901234567890123456789";
    const char* t47 = "0123456789012345678901234567890123456789012345";
    TEST_ASSERT_EQUAL_size_t(OA3_ID_MAX, strlen(id63));
    TEST_ASSERT_EQUAL_size_t(OA3_TARGET_MAX, strlen(t47) + 1);
    size_t n = static_cast<size_t>(snprintf(scratch, sizeof(scratch),
        "{\"id\":\"%s\",\"programID\":\"%s\",\"eventName\":\"%s\",\"objectType\":\"EVENT\","
        "\"priority\":0,\"duration\":\"PT25H\",\"modificationDateTime\":\"2026-11-01T00:00:00.123456789Z\","
        "\"intervalPeriod\":{\"start\":\"2026-11-01T07:00:00Z\",\"duration\":\"PT1H\",\"randomizeStart\":\"PT5M\"},"
        "\"targets\":[\"%sa\",\"%sb\",\"%sc\",\"%sd\"],\"payloadDescriptors\":["
        "{\"objectType\":\"EVENT_PAYLOAD_DESCRIPTOR\",\"payloadType\":\"PRICE\",\"units\":\"KWH\",\"currency\":\"USD\"},"
        "{\"objectType\":\"EVENT_PAYLOAD_DESCRIPTOR\",\"payloadType\":\"GHG\",\"units\":\"G_PER_KWH\"},"
        "{\"objectType\":\"EVENT_PAYLOAD_DESCRIPTOR\",\"payloadType\":\"EXPORT_PRICE\",\"units\":\"KWH\",\"currency\":\"USD\"},"
        "{\"objectType\":\"EVENT_PAYLOAD_DESCRIPTOR\",\"payloadType\":\"SIMPLE\"}],\"intervals\":[",
        id63, id63, id63, t47, t47, t47, t47));
    for (int i = 0; i < OA3_MAX_INTERVALS; ++i) {
        n += static_cast<size_t>(snprintf(scratch + n, sizeof(scratch) - n,
            "%s{\"id\":%d,\"intervalPeriod\":{\"start\":\"2026-11-01T%02d:00:00.000000000-07:00\","
            "\"duration\":\"PT1H\",\"randomizeStart\":\"PT1M\"},\"payloads\":["
            "{\"type\":\"PRICE\",\"values\":[0.1234567890123456789%02d]},"
            "{\"type\":\"GHG\",\"values\":[300.00000000000000000%02d]}]}",
            i ? "," : "", i, i % 24, i, i));
    }
    snprintf(scratch + n, sizeof(scratch) - n, "]}");
    ASSERT_RESULT(ParseResult::kOk, event_from(scratch));
    TEST_ASSERT_EQUAL_UINT16(OA3_MAX_INTERVALS, event.interval_count);
    TEST_ASSERT_EQUAL_UINT16(OA3_MAX_VALUES, event.value_count);
    TEST_ASSERT_EQUAL_size_t(OA3_VALUE_TEXT_MAX, strlen(event.values[49].text));
    TEST_ASSERT_EQUAL_UINT8(OA3_MAX_TARGETS, event.targets.count);
    TEST_ASSERT_EQUAL_UINT8(OA3_MAX_DESCRIPTORS, event.descriptor_count);
    char msg[96];
    snprintf(msg, sizeof(msg), "at-limit event arena high water: %u of %u bytes",
             static_cast<unsigned>(arena.high_water()), static_cast<unsigned>(arena.capacity()));
    TEST_MESSAGE(msg);
}

static void test_price_at_on_parsed_fixture() {
    size_t n = load_fixture(OA3_FIXTURE_DIR "/price_server_events.json");
    size_t count = 0;
    ASSERT_RESULT(ParseResult::kOk,
                  oa3::parse_events(fixture, n, arena, events, OA3_MAX_EVENTS, &count));
    oa3::PriceAt p;
    TEST_ASSERT_TRUE(oa3::price_at(events, count, ux("2026-11-15T00:30:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_PTR(&events[0], p.event);
    TEST_ASSERT_EQUAL_UINT16(16, p.interval_index);
    TEST_ASSERT_EQUAL_PTR(&events[0].values[16], p.value);
    TEST_ASSERT_EQUAL_INT64(ux("2026-11-15T01:00:00Z"), p.next_change_unix);
    TEST_ASSERT_TRUE(oa3::price_at(events, count, ux("2026-11-14T23:59:59Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_UINT16(15, p.interval_index);
}

static void test_spring_forward_day_23_intervals() {
    size_t n = static_cast<size_t>(snprintf(scratch, sizeof(scratch),
        "{\"programID\":\"p\",\"intervalPeriod\":{\"start\":\"2026-03-08T08:00:00Z\",\"duration\":\"PT23H\"},"
        "\"intervals\":["));
    for (int i = 0; i < 23; ++i) {
        oa3::DateTime s{};
        TEST_ASSERT_TRUE(oa3::from_unix(ux("2026-03-08T08:00:00Z") + 3600 * i, &s));
        char buf[oa3::kDateTimeStringSize];
        TEST_ASSERT_TRUE(oa3::format_datetime(s, buf, sizeof(buf)) > 0);
        n += static_cast<size_t>(snprintf(scratch + n, sizeof(scratch) - n,
            "%s{\"id\":%d,\"intervalPeriod\":{\"start\":\"%s\",\"duration\":\"PT1H\"},"
            "\"payloads\":[{\"type\":\"PRICE\",\"values\":[0.%02d]}]}",
            i ? "," : "", i, buf, i));
    }
    snprintf(scratch + n, sizeof(scratch) - n, "]}");
    ASSERT_RESULT(ParseResult::kOk, event_from(scratch));
    TEST_ASSERT_EQUAL_UINT16(23, event.interval_count);
    oa3::PriceAt p;
    TEST_ASSERT_TRUE(oa3::price_at(&event, 1, ux("2026-03-09T06:30:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_UINT16(22, p.interval_index);
    TEST_ASSERT_FALSE(oa3::price_at(&event, 1, ux("2026-03-09T07:00:00Z"), "PRICE", &p));
}

static void test_empty_arrays() {
    ASSERT_RESULT(ParseResult::kOk,
                  event_from(R"({"programID":"p","intervals":[],"targets":[],"payloadDescriptors":[]})"));
    TEST_ASSERT_EQUAL_UINT16(0, event.interval_count);
    TEST_ASSERT_EQUAL_UINT8(0, event.targets.count);
    TEST_ASSERT_EQUAL_UINT8(0, event.descriptor_count);
    ASSERT_RESULT(ParseResult::kOk, event_from(R"({"programID":"p","intervals":[{"id":0,"payloads":[]},
      {"id":1,"payloads":[{"type":"PRICE","values":[]}]}]})"));
    TEST_ASSERT_EQUAL_UINT8(0, event.intervals[0].payload_count);
    TEST_ASSERT_EQUAL_UINT16(0, event.intervals[1].payloads[0].value_count);
    size_t count = 99;
    ASSERT_RESULT(ParseResult::kOk, oa3::parse_events("[]", 2, arena, events, OA3_MAX_EVENTS, &count));
    TEST_ASSERT_EQUAL_size_t(0, count);
}

static void test_nested_snake_case_and_unknown_keys() {
    ASSERT_RESULT(ParseResult::kOk, notification_from(R"({"object_type":"EVENT","operation":"POST",
      "object":{"program_id":"p","interval_period":{"start":"2026-03-08 19:00:00","duration":"PT1H"},
      "payload_descriptors":[{"payload_type":"PRICE"}],"intervals":[{"id":0,
      "interval_period":{"randomize_start":"PT1M"},"payloads":[{"type":"PRICE","values":[0.2]}]}]}})"));
    TEST_ASSERT_TRUE(notification.has_object);
    TEST_ASSERT_TRUE(notification.event.has_interval_period);
    TEST_ASSERT_EQUAL_UINT8(1, notification.event.descriptor_count);
    TEST_ASSERT_TRUE(notification.event.intervals[0].interval_period.has_randomize_start);

    ASSERT_RESULT(ParseResult::kOk, event_from(R"({"programID":"p",
      "zzz":{"intervals":[{"payloads":[{"values":[1]}]}]},
      "intervals":[{"id":0,"extra":{"values":[9]},"payloads":[{"type":"PRICE","values":[0.5],"meta":[1,2]}],
      "intervalPeriod":{"start":"2024-06-15T00:00:00Z","foo":1}}]})"));
    TEST_ASSERT_EQUAL_UINT16(1, event.value_count);
    TEST_ASSERT_EQUAL_STRING("0.5", event.values[0].text);

    ASSERT_RESULT(ParseResult::kOk, event_from(R"({"programID":null,"program_id":"p"})"));
    TEST_ASSERT_EQUAL_STRING("p", event.program_id);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_python_program);
    RUN_TEST(test_python_event);
    RUN_TEST(test_python_event_export_price_case);
    RUN_TEST(test_python_notification_snake);
    RUN_TEST(test_python_notification_camel);
    RUN_TEST(test_python_is_notification);
    RUN_TEST(test_clj_notification_camel_post);
    RUN_TEST(test_clj_notification_snake);
    RUN_TEST(test_clj_notification_envelope);
    RUN_TEST(test_notification_details);
    RUN_TEST(test_price_server_programs);
    RUN_TEST(test_price_server_events);
    RUN_TEST(test_streaming_matches_buffer);
    RUN_TEST(test_truncated_json);
    RUN_TEST(test_small_arena_too_large);
    RUN_TEST(test_too_deep);
    RUN_TEST(test_array_capacity_and_errors);
    RUN_TEST(test_value_kinds);
    RUN_TEST(test_number_literals);
    RUN_TEST(test_values_whitespace_and_escapes);
    RUN_TEST(test_value_capacity);
    RUN_TEST(test_interval_capacity);
    RUN_TEST(test_intervals_fields);
    RUN_TEST(test_event_fields);
    RUN_TEST(test_event_snake_case);
    RUN_TEST(test_event_name_truncation);
    RUN_TEST(test_targets);
    RUN_TEST(test_payload_filter);
    RUN_TEST(test_program_fields);
    RUN_TEST(test_program_trickle);
    RUN_TEST(test_entities_helpers);
    RUN_TEST(test_marker_bytes_in_strings);
    RUN_TEST(test_blank_datetime_and_duration_absent);
    RUN_TEST(test_event_at_every_limit);
    RUN_TEST(test_price_at_on_parsed_fixture);
    RUN_TEST(test_spring_forward_day_23_intervals);
    RUN_TEST(test_empty_arrays);
    RUN_TEST(test_nested_snake_case_and_unknown_keys);
    return UNITY_END();
}
