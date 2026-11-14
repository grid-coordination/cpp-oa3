// Overlapping events at equal priority: the newer wins, and an event the winners cover is
// superseded.

#include <stdio.h>
#include <string.h>

#include <oa3/model/intervals.h>
#include <oa3/model/parse.h>
#include <oa3/model/time.h>
#include <oa3/model/time_interval.h>
#include <unity.h>

static oa3::StaticJsonArena<> arena;
static oa3::Event events[4];
static char json[4096];
static const char* const kTypes[] = {"PRICE"};

#define ASSERT_RESULT(expected, actual) \
    TEST_ASSERT_EQUAL_STRING(oa3::to_string(expected), oa3::to_string(actual))

void setUp() { memset(events, 0, sizeof(events)); }
void tearDown() {}

static oa3::DateTime at(const char* s) {
    oa3::DateTime t{};
    TEST_ASSERT_TRUE(oa3::parse_datetime(s, &t));
    return t;
}

static oa3::TimeInterval window(const char* start, const char* duration) {
    oa3::Duration d{};
    oa3::TimeInterval w{};
    TEST_ASSERT_TRUE(oa3::parse_duration(duration, &d));
    TEST_ASSERT_TRUE(oa3::make_interval(at(start), d, &w));
    return w;
}

// An event of hourly intervals from 2026-11-13T<first>:00Z, count of them, value base + hour.
// mdt and priority may be nullptr / negative.
static size_t hourly(char* buf, size_t size, const char* id, int first, int count, int base,
                     const char* mdt, int priority = -1) {
    size_t n = static_cast<size_t>(snprintf(buf, size, R"({"id":"%s","programID":"p",)", id));
    if (mdt != nullptr) {
        n += static_cast<size_t>(snprintf(buf + n, size - n, R"("modificationDateTime":"%s",)", mdt));
    }
    if (priority >= 0) {
        n += static_cast<size_t>(snprintf(buf + n, size - n, R"("priority":%d,)", priority));
    }
    n += static_cast<size_t>(snprintf(buf + n, size - n, R"("intervals":[)"));
    for (int i = 0; i < count; ++i) {
        const int h = first + i;
        n += static_cast<size_t>(snprintf(
            buf + n, size - n,
            R"(%s{"id":%d,"intervalPeriod":{"start":"2026-11-1%dT%02d:00:00Z","duration":"PT1H"},)"
            R"("payloads":[{"type":"PRICE","values":[%d]}]})",
            i == 0 ? "" : ",", i, 3 + h / 24, h % 24, base + h));
    }
    n += static_cast<size_t>(snprintf(buf + n, size - n, "]}"));
    TEST_ASSERT_TRUE(n < size);
    return n;
}

// Parses "[a,b,...]" of the given event objects.
static size_t parse(const oa3::TimeInterval& w, size_t capacity, oa3::ParseResult expected,
                    const char* a, const char* b = nullptr, const char* c = nullptr) {
    snprintf(json, sizeof(json), "[%s%s%s%s%s]", a, b ? "," : "", b ? b : "", c ? "," : "",
             c ? c : "");
    oa3::ParseOptions o{};
    o.payload_types = kTypes;
    o.payload_type_count = 1;
    o.window = &w;
    size_t count = 0;
    ASSERT_RESULT(expected, oa3::parse_events(json, strlen(json), arena, events, capacity, &count, &o));
    return count;
}

static double price(const char* t, size_t count) {
    oa3::PriceAt p;
    TEST_ASSERT_TRUE(oa3::price_at(events, count, at(t), "PRICE", &p));
    return p.value->number;
}

static char a[1024], b[1024], c[1024];

static void test_newer_mdt_wins_whatever_the_order() {
    const oa3::TimeInterval w = window("2026-11-13T00:00:00Z", "P2D");
    hourly(a, sizeof(a), "new", 20, 2, 200, "2026-11-13T19:00:00Z");
    hourly(b, sizeof(b), "old", 20, 4, 100, "2026-11-13T18:00:00Z");
    TEST_ASSERT_EQUAL_size_t(2, parse(w, 4, oa3::ParseResult::kOk, a, b));
    TEST_ASSERT_EQUAL_DOUBLE(220, price("2026-11-13T20:30:00Z", 2));
    TEST_ASSERT_EQUAL_DOUBLE(221, price("2026-11-13T21:30:00Z", 2));
    TEST_ASSERT_EQUAL_DOUBLE(122, price("2026-11-13T22:30:00Z", 2));
}

static void test_later_arrival_wins_without_mdt() {
    const oa3::TimeInterval w = window("2026-11-13T00:00:00Z", "P2D");
    // Without a modificationDateTime an event counts as older than one with it.
    hourly(a, sizeof(a), "first", 20, 2, 100, "2026-11-13T19:00:00Z");
    hourly(b, sizeof(b), "second", 20, 2, 200, nullptr);
    TEST_ASSERT_EQUAL_size_t(2, parse(w, 4, oa3::ParseResult::kOk, a, b));
    TEST_ASSERT_EQUAL_DOUBLE(120, price("2026-11-13T20:30:00Z", 2));
    hourly(a, sizeof(a), "first", 20, 2, 100, nullptr);
    hourly(b, sizeof(b), "second", 20, 2, 200, nullptr);
    TEST_ASSERT_EQUAL_size_t(2, parse(w, 4, oa3::ParseResult::kOk, a, b));
    TEST_ASSERT_EQUAL_DOUBLE(220, price("2026-11-13T20:30:00Z", 2));
    hourly(a, sizeof(a), "first", 20, 2, 100, "2026-11-13T19:00:00Z");
    hourly(b, sizeof(b), "second", 20, 2, 200, "2026-11-13T19:00:00Z");
    TEST_ASSERT_EQUAL_size_t(2, parse(w, 4, oa3::ParseResult::kOk, a, b));
    TEST_ASSERT_EQUAL_DOUBLE(220, price("2026-11-13T20:30:00Z", 2));
}

// Mixing events with and without modificationDateTime stays transitive: A (mdt 10, first),
// B (none, second), C (mdt 5, third) rank A > C > B whatever the order.
static void test_precedence_is_transitive() {
    oa3::Precedence pa{false, 0, true, oa3::DateTime{}, 0};
    oa3::Precedence pb{false, 0, false, oa3::DateTime{}, 1};
    oa3::Precedence pc{false, 0, true, oa3::DateTime{}, 2};
    TEST_ASSERT_TRUE(oa3::from_unix(10, &pa.modification_date_time));
    TEST_ASSERT_TRUE(oa3::from_unix(5, &pc.modification_date_time));
    TEST_ASSERT_TRUE(oa3::takes_precedence(pa, pc));
    TEST_ASSERT_TRUE(oa3::takes_precedence(pc, pb));
    TEST_ASSERT_TRUE(oa3::takes_precedence(pa, pb));
    TEST_ASSERT_FALSE(oa3::takes_precedence(pb, pa));
}

static void test_better_priority_beats_newer() {
    const oa3::TimeInterval w = window("2026-11-13T00:00:00Z", "P2D");
    hourly(a, sizeof(a), "old", 20, 2, 100, "2026-11-13T18:00:00Z", 0);
    hourly(b, sizeof(b), "new", 20, 2, 200, "2026-11-13T19:00:00Z", 1);
    TEST_ASSERT_EQUAL_size_t(2, parse(w, 4, oa3::ParseResult::kOk, a, b));
    TEST_ASSERT_EQUAL_DOUBLE(120, price("2026-11-13T20:30:00Z", 2));
    TEST_ASSERT_TRUE(oa3::superseded(events[1], 1, events, 2, nullptr, w, "PRICE"));
    TEST_ASSERT_FALSE(oa3::superseded(events[0], 0, events, 2, nullptr, w, "PRICE"));
}

static void test_superseded_needs_full_cover() {
    const oa3::TimeInterval w = window("2026-11-13T20:00:00Z", "PT24H");
    hourly(a, sizeof(a), "old", 20, 4, 100, "2026-11-13T18:00:00Z");
    hourly(b, sizeof(b), "new", 20, 2, 200, "2026-11-13T19:00:00Z");
    TEST_ASSERT_EQUAL_size_t(2, parse(w, 4, oa3::ParseResult::kOk, a, b));
    TEST_ASSERT_FALSE(oa3::superseded(events[0], 0, events, 2, nullptr, w, "PRICE"));
    // From 22:00Z only the old event's tail is left, which the new one does not cover.
    const oa3::TimeInterval late = window("2026-11-13T22:00:00Z", "PT24H");
    TEST_ASSERT_FALSE(oa3::superseded(events[0], 0, events, 2, nullptr, late, "PRICE"));
    // The union of two newer events covers it: 20-22 and 21-24.
    hourly(c, sizeof(c), "newer", 21, 3, 300, "2026-11-13T19:30:00Z");
    TEST_ASSERT_EQUAL_size_t(3, parse(w, 4, oa3::ParseResult::kOk, a, b, c));
    TEST_ASSERT_TRUE(oa3::superseded(events[0], 0, events, 3, nullptr, w, "PRICE"));
    TEST_ASSERT_FALSE(oa3::superseded(events[1], 1, events, 3, nullptr, w, "PRICE"));
    // An extra event counts as arriving last.
    TEST_ASSERT_EQUAL_size_t(1, parse(w, 4, oa3::ParseResult::kOk, a));
    oa3::Event extra{};
    hourly(b, sizeof(b), "new", 20, 4, 200, "2026-11-13T19:00:00Z");
    oa3::ParseOptions o{};
    o.payload_types = kTypes;
    o.payload_type_count = 1;
    ASSERT_RESULT(oa3::ParseResult::kOk, oa3::parse_event(b, strlen(b), arena, &extra, &o));
    TEST_ASSERT_TRUE(oa3::superseded(events[0], 0, events, 1, &extra, w, "PRICE"));
    TEST_ASSERT_FALSE(oa3::superseded(extra, 1, events, 1, nullptr, w, "PRICE"));
}

// One slot: the event that covers the other's remaining window is kept, in either order.
static void test_one_slot_keeps_superseding_event() {
    const oa3::TimeInterval w = window("2026-11-13T21:00:00Z", "PT24H");
    hourly(a, sizeof(a), "e1", 20, 4, 100, "2026-11-13T20:00:00Z");
    hourly(b, sizeof(b), "e2", 21, 4, 200, "2026-11-13T21:00:00Z");
    TEST_ASSERT_EQUAL_size_t(1, parse(w, 1, oa3::ParseResult::kOk, a, b));
    TEST_ASSERT_EQUAL_STRING("e2", events[0].id);
    TEST_ASSERT_EQUAL_size_t(1, parse(w, 1, oa3::ParseResult::kOk, b, a));
    TEST_ASSERT_EQUAL_STRING("e2", events[0].id);
}

// Not superseded: keep-earliest decides, and the parse is truncated.
static void test_one_slot_partial_overlap_truncates() {
    const oa3::TimeInterval w = window("2026-11-13T20:00:00Z", "PT24H");
    hourly(a, sizeof(a), "e1", 20, 4, 100, "2026-11-13T20:00:00Z");
    hourly(b, sizeof(b), "e2", 21, 4, 200, "2026-11-13T21:00:00Z");
    TEST_ASSERT_EQUAL_size_t(1, parse(w, 1, oa3::ParseResult::kTruncated, a, b));
    TEST_ASSERT_EQUAL_STRING("e1", events[0].id);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_newer_mdt_wins_whatever_the_order);
    RUN_TEST(test_later_arrival_wins_without_mdt);
    RUN_TEST(test_precedence_is_transitive);
    RUN_TEST(test_better_priority_beats_newer);
    RUN_TEST(test_superseded_needs_full_cover);
    RUN_TEST(test_one_slot_keeps_superseding_event);
    RUN_TEST(test_one_slot_partial_overlap_truncates);
    return UNITY_END();
}
