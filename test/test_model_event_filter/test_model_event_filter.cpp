#include <string.h>

#include <oa3/model/parse.h>
#include <oa3/model/time.h>
#include <oa3/model/time_interval.h>
#include <unity.h>

static oa3::StaticJsonArena<> arena;
static oa3::Event events[2];

#define ASSERT_RESULT(expected, actual) \
    TEST_ASSERT_EQUAL_STRING(oa3::to_string(expected), oa3::to_string(actual))

void setUp() { memset(events, 0, sizeof(events)); }
void tearDown() {}

// Three events at 20:00Z: targeted T2, of another program, then targeted T1.
static const char kLate[] = R"([
{"id":"t2","programID":"p1","targets":["T2"],
 "intervalPeriod":{"start":"2026-11-13T20:00:00Z","duration":"PT1H"},
 "intervals":[{"id":0,"payloads":[{"type":"PRICE","values":[1]}]}]},
{"id":"other","programID":"p2",
 "intervalPeriod":{"start":"2026-11-13T20:00:00Z","duration":"PT1H"},
 "intervals":[{"id":0,"payloads":[{"type":"PRICE","values":[2]}]}]},
{"id":"t1","programID":"p1","targets":["X","T1"],
 "intervalPeriod":{"start":"2026-11-13T20:00:00Z","duration":"PT1H"},
 "intervals":[{"id":0,"payloads":[{"type":"PRICE","values":[3]}]}]}
])";

static const char* const kVenTargets[] = {"T1"};

static oa3::TimeInterval window() {
    oa3::DateTime s{};
    oa3::Duration d{};
    oa3::TimeInterval w{};
    TEST_ASSERT_TRUE(oa3::parse_datetime("2026-11-13T20:00:00Z", &s));
    TEST_ASSERT_TRUE(oa3::parse_duration("PT24H", &d));
    TEST_ASSERT_TRUE(oa3::make_interval(s, d, &w));
    return w;
}

static void test_windowed_filter_uses_no_slot() {
    const oa3::TimeInterval w = window();
    oa3::ParseOptions opt{};
    opt.window = &w;
    opt.program_id = "p1";
    opt.ven_targets = kVenTargets;
    opt.ven_target_count = 1;
    size_t count = 0;
    ASSERT_RESULT(oa3::ParseResult::kOk,
                  oa3::parse_events(kLate, strlen(kLate), arena, events, 1, &count, &opt));
    TEST_ASSERT_EQUAL_size_t(1, count);
    TEST_ASSERT_EQUAL_STRING("t1", events[0].id);
}

static void test_unwindowed_filter_at_capacity() {
    oa3::ParseOptions opt{};
    opt.program_id = "p1";
    opt.ven_targets = kVenTargets;
    opt.ven_target_count = 1;
    size_t count = 0;
    ASSERT_RESULT(oa3::ParseResult::kOk,
                  oa3::parse_events(kLate, strlen(kLate), arena, events, 1, &count, &opt));
    TEST_ASSERT_EQUAL_size_t(1, count);
    TEST_ASSERT_EQUAL_STRING("t1", events[0].id);

    opt.ven_targets = nullptr;
    ASSERT_RESULT(oa3::ParseResult::kCapacityExceeded,
                  oa3::parse_events(kLate, strlen(kLate), arena, events, 1, &count, &opt));
    TEST_ASSERT_EQUAL_size_t(1, count);
    TEST_ASSERT_EQUAL_STRING("t2", events[0].id);
}

static void test_open_events_pass() {
    static const char kOpen[] = R"([
{"id":"open","programID":"p1","targets":[],"intervalPeriod":{"start":"2026-11-13T20:00:00Z","duration":"PT1H"},
 "intervals":[{"id":0,"payloads":[{"type":"PRICE","values":[1]}]}]}
])";
    oa3::ParseOptions opt{};
    opt.program_id = "p1";
    opt.ven_targets = kVenTargets;
    opt.ven_target_count = 1;
    size_t count = 0;
    ASSERT_RESULT(oa3::ParseResult::kOk,
                  oa3::parse_events(kOpen, strlen(kOpen), arena, events, 2, &count, &opt));
    TEST_ASSERT_EQUAL_size_t(1, count);
}

static void test_no_filter_keeps_all() {
    size_t count = 0;
    ASSERT_RESULT(oa3::ParseResult::kCapacityExceeded,
                  oa3::parse_events(kLate, strlen(kLate), arena, events, 2, &count));
    TEST_ASSERT_EQUAL_size_t(2, count);
}

static void test_append_events_accumulates() {
    static const char kPage1[] = R"([
{"id":"a","programID":"p1","intervalPeriod":{"start":"2026-11-13T22:00:00Z","duration":"PT1H"},
 "intervals":[{"id":0,"payloads":[{"type":"PRICE","values":[1]}]}]},
{"id":"old","programID":"p1","intervalPeriod":{"start":"2026-11-12T20:00:00Z","duration":"PT1H"},
 "intervals":[{"id":0,"payloads":[{"type":"PRICE","values":[1]}]}]}
])";
    static const char kPage2[] = R"([
{"id":"b","programID":"p1","intervalPeriod":{"start":"2026-11-13T21:00:00Z","duration":"PT1H"},
 "intervals":[{"id":0,"payloads":[{"type":"PRICE","values":[2]}]}]},
{"id":"c","programID":"p1","intervalPeriod":{"start":"2026-11-13T20:00:00Z","duration":"PT1H"},
 "intervals":[{"id":0,"payloads":[{"type":"PRICE","values":[3]}]}]}
])";
    const oa3::TimeInterval w = window();
    oa3::ParseOptions opt{};
    opt.window = &w;
    size_t count = 0;
    size_t scanned = 99;
    ASSERT_RESULT(oa3::ParseResult::kOk, oa3::append_events(kPage1, strlen(kPage1), arena, events,
                                                            2, &count, &scanned, &opt));
    TEST_ASSERT_EQUAL_size_t(1, count);
    TEST_ASSERT_EQUAL_size_t(2, scanned);
    ASSERT_RESULT(oa3::ParseResult::kTruncated, oa3::append_events(kPage2, strlen(kPage2), arena,
                                                                   events, 2, &count, &scanned,
                                                                   &opt));
    TEST_ASSERT_EQUAL_size_t(2, scanned);
    TEST_ASSERT_EQUAL_size_t(2, count);
    TEST_ASSERT_EQUAL_STRING("b", events[0].id);
    TEST_ASSERT_EQUAL_STRING("c", events[1].id);

    count = 1;
    ASSERT_RESULT(oa3::ParseResult::kJsonError,
                  oa3::append_events("[{\"id\":", 7, arena, events, 2, &count, &scanned));
    TEST_ASSERT_EQUAL_size_t(0, scanned);
    TEST_ASSERT_EQUAL_size_t(1, count);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_windowed_filter_uses_no_slot);
    RUN_TEST(test_unwindowed_filter_at_capacity);
    RUN_TEST(test_open_events_pass);
    RUN_TEST(test_no_filter_keeps_all);
    RUN_TEST(test_append_events_accumulates);
    return UNITY_END();
}
