// ParseOptions::later_start: the earliest start left out for lying beyond the window end.

#include <stdio.h>
#include <string.h>

#include <oa3/model/parse.h>
#include <oa3/model/time.h>
#include <oa3/model/time_interval.h>
#include <unity.h>

static oa3::StaticJsonArena<> arena;
static oa3::Event events[4];
static oa3::Notification note;
static oa3::TimeInterval win;
static oa3::DateTime later;

#define ASSERT_RESULT(expected, actual) \
    TEST_ASSERT_EQUAL_STRING(oa3::to_string(expected), oa3::to_string(actual))

void setUp() {
    memset(events, 0, sizeof(events));
    oa3::DateTime s{};
    oa3::Duration d{};
    TEST_ASSERT_TRUE(oa3::parse_datetime("2026-11-13T20:00:00Z", &s));
    TEST_ASSERT_TRUE(oa3::parse_duration("PT24H", &d));
    TEST_ASSERT_TRUE(oa3::make_interval(s, d, &win));
    later = oa3::DateTime{INT64_MAX, 0, 0};
}
void tearDown() {}

static oa3::ParseOptions options() {
    oa3::ParseOptions o{};
    o.window = &win;
    o.later_start = &later;
    return o;
}

static void assert_later(const char* expected) {
    char buf[oa3::kDateTimeStringSize];
    TEST_ASSERT_TRUE(oa3::format_datetime(later, buf, sizeof(buf)) > 0);
    TEST_ASSERT_EQUAL_STRING(expected, buf);
}

static size_t parse(const char* json) {
    const oa3::ParseOptions o = options();
    size_t count = 0;
    ASSERT_RESULT(oa3::ParseResult::kOk,
                  oa3::parse_events(json, strlen(json), arena, events, 4, &count, &o));
    return count;
}

// e1 in the window; e2 and e3 after it, e3 the earlier; e0 before it.
static void test_events_after_window() {
    const size_t n = parse(R"([
{"id":"e0","programID":"p1","intervalPeriod":{"start":"2026-11-13T18:00:00Z","duration":"PT1H"},"intervals":[{"id":0}]},
{"id":"e1","programID":"p1","intervalPeriod":{"start":"2026-11-13T20:00:00Z","duration":"PT1H"},"intervals":[{"id":0}]},
{"id":"e2","programID":"p1","intervalPeriod":{"start":"2026-11-15T01:00:00Z","duration":"PT1H"},"intervals":[{"id":0}]},
{"id":"e3","programID":"p1","intervalPeriod":{"start":"2026-11-14T20:00:00Z","duration":"PT1H"},"intervals":[{"id":0}]}
])");
    TEST_ASSERT_EQUAL_size_t(1, n);
    assert_later("2026-11-14T20:00:00Z");
}

// Intervals beyond the window are kept while they fit; the first that does not sets later.
static void test_interval_after_window() {
    static char json[8192];
    int n = snprintf(json, sizeof(json),
                     R"([{"id":"e1","programID":"p1","intervalPeriod":)"
                     R"({"start":"2026-11-14T19:00:00Z","duration":"PT1H"},"intervals":[)");
    for (int i = 0; i < OA3_MAX_INTERVALS + 2; ++i) {
        n += snprintf(json + n, sizeof(json) - n, "%s{\"id\":%d}", i > 0 ? "," : "", i);
    }
    snprintf(json + n, sizeof(json) - n, "]}]");
    TEST_ASSERT_EQUAL_size_t(1, parse(json));
    TEST_ASSERT_EQUAL_UINT8(OA3_MAX_INTERVALS, events[0].interval_count);
    // 2026-11-14T19:00Z plus OA3_MAX_INTERVALS hours.
    TEST_ASSERT_EQUAL_INT64(1794682800 + 3600 * OA3_MAX_INTERVALS, oa3::to_unix(later));
}

static void test_interval_after_window_kept() {
    const size_t n = parse(R"([
{"id":"e1","programID":"p1","intervalPeriod":{"start":"2026-11-14T19:00:00Z","duration":"PT1H"},"intervals":[
 {"id":0},{"id":1},{"id":2}]}
])");
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT8(3, events[0].interval_count);
    TEST_ASSERT_EQUAL_INT64(INT64_MAX, later.seconds);
}

// Interval 1 fills the values pool, so interval 2 (whose new type is rolled back with its
// values) is left out, and interval 3 after it although it would fit.
static void test_interval_after_window_values_full() {
    static char json[4096];
    int n = snprintf(json, sizeof(json),
                     R"([{"id":"e1","programID":"p1","intervalPeriod":)"
                     R"({"start":"2026-11-14T19:00:00Z","duration":"PT1H"},"intervals":[)"
                     R"({"id":0,"payloads":[{"type":"PRICE","values":[1]}]},)"
                     R"({"id":1,"payloads":[{"type":"PRICE","values":[)");
    for (int i = 0; i < OA3_MAX_VALUES - 1; ++i) {
        n += snprintf(json + n, sizeof(json) - n, "%s%d", i > 0 ? "," : "", i);
    }
    snprintf(json + n, sizeof(json) - n,
             R"(]}]},{"id":2,"payloads":[{"type":"USAGE","values":[2]}]},{"id":3}]}])");
    TEST_ASSERT_EQUAL_size_t(1, parse(json));
    TEST_ASSERT_EQUAL_UINT8(2, events[0].interval_count);
    TEST_ASSERT_EQUAL_UINT16(OA3_MAX_VALUES, events[0].value_count);
    TEST_ASSERT_EQUAL_UINT8(1, events[0].type_count);
    assert_later("2026-11-14T21:00:00Z");
}

static void test_nothing_after_window() {
    parse(R"([
{"id":"e0","programID":"p1","intervalPeriod":{"start":"2026-11-13T18:00:00Z","duration":"PT1H"},"intervals":[{"id":0}]},
{"id":"e1","programID":"p1","intervalPeriod":{"start":"2026-11-14T19:00:00Z","duration":"PT1H"},"intervals":[{"id":0}]}
])");
    TEST_ASSERT_EQUAL_INT64(INT64_MAX, later.seconds);
}

static void test_looping_event_after_window() {
    const size_t n = parse(R"([
{"id":"e1","programID":"p1","duration":"PT2H","intervalPeriod":{"start":"2026-11-14T21:00:00Z","duration":"PT1H"},
 "intervals":[{"id":0}]}
])");
    TEST_ASSERT_EQUAL_size_t(0, n);
    assert_later("2026-11-14T21:00:00Z");
}

// Only lowered: a later candidate leaves an earlier value.
static void test_only_lowered() {
    TEST_ASSERT_TRUE(oa3::parse_datetime("2026-11-14T20:30:00Z", &later));
    parse(R"([
{"id":"e2","programID":"p1","intervalPeriod":{"start":"2026-11-15T01:00:00Z","duration":"PT1H"},"intervals":[{"id":0}]}
])");
    assert_later("2026-11-14T20:30:00Z");
}

// A notification's event that lies beyond the window keeps its intervals; the client, not the
// parse, notes when it comes within the window.
static void test_notification_after_window() {
    static const char json[] = R"({"objectType":"EVENT","operation":"CREATE","object":
{"id":"e2","programID":"p1","intervalPeriod":{"start":"2026-11-14T22:00:00Z","duration":"PT1H"},
 "intervals":[{"id":0}]}})";
    const oa3::ParseOptions o = options();
    ASSERT_RESULT(oa3::ParseResult::kOk,
                  oa3::parse_notification(json, strlen(json), arena, &note, &o));
    TEST_ASSERT_EQUAL_UINT8(1, note.event.interval_count);
    TEST_ASSERT_EQUAL_INT64(INT64_MAX, later.seconds);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_events_after_window);
    RUN_TEST(test_interval_after_window);
    RUN_TEST(test_interval_after_window_kept);
    RUN_TEST(test_interval_after_window_values_full);
    RUN_TEST(test_nothing_after_window);
    RUN_TEST(test_looping_event_after_window);
    RUN_TEST(test_only_lowered);
    RUN_TEST(test_notification_after_window);
    return UNITY_END();
}
