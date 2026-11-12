#include <oa3/model/intervals.h>
#include <oa3/model/time.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

using oa3::DateTime;
using oa3::Duration;
using oa3::Event;
using oa3::PriceAt;
using oa3::ResolvedInterval;

static Event g_events[3];

void setUp() { memset(g_events, 0, sizeof(g_events)); }
void tearDown() {}

static DateTime dt(const char* s) {
    DateTime d{};
    TEST_ASSERT_TRUE_MESSAGE(oa3::parse_datetime(s, &d), s);
    return d;
}

static Duration dur(const char* s) {
    Duration d{};
    TEST_ASSERT_TRUE_MESSAGE(oa3::parse_duration(s, &d), s);
    return d;
}

static int64_t ux(const char* s) { return oa3::to_unix(dt(s)); }

static void set_event_period(Event& e, const char* start, const char* duration) {
    e.has_interval_period = true;
    if (start) {
        e.interval_period.has_start = true;
        e.interval_period.start = dt(start);
    }
    if (duration) {
        e.interval_period.has_duration = true;
        e.interval_period.duration = dur(duration);
    }
}

// Appends an interval with one payload of `type` holding `values`; start and duration
// may be null, and with both null the interval has no intervalPeriod.
static oa3::Interval& add_interval(Event& e, int32_t id, const char* start,
                                   const char* duration, const char* type,
                                   const double* values, uint16_t n) {
    oa3::Interval& iv = e.intervals[e.interval_count++];
    iv.id = id;
    if (start || duration) {
        iv.has_interval_period = true;
        if (start) {
            iv.interval_period.has_start = true;
            iv.interval_period.start = dt(start);
        }
        if (duration) {
            iv.interval_period.has_duration = true;
            iv.interval_period.duration = dur(duration);
        }
    }
    if (type) {
        oa3::Payload& p = iv.payloads[iv.payload_count++];
        strcpy(p.type, type);
        p.first_value = e.value_count;
        p.value_count = n;
        for (uint16_t i = 0; i < n; ++i) {
            oa3::Value& v = e.values[e.value_count++];
            v.kind = oa3::ValueKind::kNumber;
            v.number = values[i];
            snprintf(v.text, sizeof(v.text), "%g", values[i]);
        }
    }
    return iv;
}

static oa3::Interval& add_price(Event& e, int32_t id, const char* start,
                                const char* duration, double price) {
    return add_interval(e, id, start, duration, "PRICE", &price, 1);
}

static void assert_dt(const char* expected, const DateTime& actual) {
    TEST_ASSERT_EQUAL_INT64(ux(expected), oa3::to_unix(actual));
}

// --- resolution: python-oa3 tests/test_entities.py:265-417 -------------------------------

static void test_passthrough() {
    Event& e = g_events[0];
    set_event_period(e, "2024-06-15T10:00:00Z", "PT1H");
    add_price(e, 0, "2024-06-15T20:00:00Z", "PT15M", 0.10);
    add_price(e, 1, "2024-06-15T21:00:00Z", "PT30M", 0.20);
    ResolvedInterval r[4];
    TEST_ASSERT_EQUAL_size_t(2, oa3::resolve_intervals(e, r, 4));
    assert_dt("2024-06-15T20:00:00Z", r[0].start);
    TEST_ASSERT_EQUAL_INT32(15, r[0].duration.minutes);
    assert_dt("2024-06-15T21:00:00Z", r[1].start);
    TEST_ASSERT_EQUAL_INT32(30, r[1].duration.minutes);
    TEST_ASSERT_EQUAL_INT64(ux("2024-06-15T21:30:00Z"), r[1].end_unix);
}

static void test_inherits_start_and_duration() {
    Event& e = g_events[0];
    set_event_period(e, "2024-06-15T14:00:00Z", "PT1H");
    add_price(e, 0, nullptr, nullptr, 0.10);
    ResolvedInterval r[1];
    TEST_ASSERT_EQUAL_size_t(1, oa3::resolve_intervals(e, r, 1));
    TEST_ASSERT_TRUE(r[0].has_start);
    TEST_ASSERT_TRUE(r[0].has_duration);
    TEST_ASSERT_FALSE(r[0].has_randomize_start);
    TEST_ASSERT_TRUE(r[0].has_period);
    assert_dt("2024-06-15T14:00:00Z", r[0].start);
    TEST_ASSERT_EQUAL_INT32(1, r[0].duration.hours);
    TEST_ASSERT_EQUAL_INT64(ux("2024-06-15T14:00:00Z"), r[0].start_unix);
    TEST_ASSERT_EQUAL_INT64(ux("2024-06-15T15:00:00Z"), r[0].end_unix);
}

static void test_duration_only_inherits_start() {
    Event& e = g_events[0];
    set_event_period(e, "2024-06-15T14:00:00Z", "PT1H");
    add_price(e, 0, nullptr, "PT15M", 0.10);
    ResolvedInterval r[1];
    TEST_ASSERT_EQUAL_size_t(1, oa3::resolve_intervals(e, r, 1));
    assert_dt("2024-06-15T14:00:00Z", r[0].start);
    TEST_ASSERT_EQUAL_INT32(15, r[0].duration.minutes);
    TEST_ASSERT_EQUAL_INT32(0, r[0].duration.hours);
}

static void test_start_only_inherits_duration() {
    Event& e = g_events[0];
    set_event_period(e, "2024-06-15T14:00:00Z", "PT1H");
    add_price(e, 0, "2024-06-15T22:00:00Z", nullptr, 0.10);
    ResolvedInterval r[1];
    TEST_ASSERT_EQUAL_size_t(1, oa3::resolve_intervals(e, r, 1));
    assert_dt("2024-06-15T22:00:00Z", r[0].start);
    TEST_ASSERT_EQUAL_INT32(1, r[0].duration.hours);
}

static void test_no_inheritance_possible() {
    Event& e = g_events[0];
    add_price(e, 0, nullptr, nullptr, 0.10);
    ResolvedInterval r[1];
    TEST_ASSERT_EQUAL_size_t(1, oa3::resolve_intervals(e, r, 1));
    TEST_ASSERT_FALSE(r[0].has_start);
    TEST_ASSERT_FALSE(r[0].has_duration);
    TEST_ASSERT_FALSE(r[0].has_randomize_start);
    TEST_ASSERT_FALSE(r[0].has_period);
}

static void test_sequential_starts() {
    Event& e = g_events[0];
    set_event_period(e, "2024-06-15T10:00:00Z", "PT1H");
    for (int i = 0; i < 4; ++i) add_price(e, i, nullptr, nullptr, i);
    ResolvedInterval r[4];
    TEST_ASSERT_EQUAL_size_t(4, oa3::resolve_intervals(e, r, 4));
    const char* starts[] = {"2024-06-15T10:00:00Z", "2024-06-15T11:00:00Z",
                            "2024-06-15T12:00:00Z", "2024-06-15T13:00:00Z"};
    for (int i = 0; i < 4; ++i) {
        assert_dt(starts[i], r[i].start);
        TEST_ASSERT_EQUAL_INT32(1, r[i].duration.hours);
        TEST_ASSERT_EQUAL_UINT16(i, r[i].index);
    }
}

static void test_does_not_mutate_event() {
    Event& e = g_events[0];
    set_event_period(e, "2024-06-15T14:00:00Z", "PT1H");
    add_price(e, 0, nullptr, nullptr, 0.10);
    memcpy(&g_events[1], &e, sizeof(Event));
    ResolvedInterval r[1];
    oa3::resolve_intervals(e, r, 1);
    TEST_ASSERT_EQUAL_MEMORY(&g_events[1], &e, sizeof(Event));
    TEST_ASSERT_FALSE(e.intervals[0].has_interval_period);
}

static void test_empty_intervals() {
    Event& e = g_events[0];
    set_event_period(e, "2024-06-15T14:00:00Z", "PT1H");
    ResolvedInterval r[1];
    TEST_ASSERT_EQUAL_size_t(0, oa3::resolve_intervals(e, r, 1));
    oa3::IntervalResolver resolver(e);
    TEST_ASSERT_FALSE(resolver.next(&r[0]));
}

static void test_id_passthrough() {
    Event& e = g_events[0];
    set_event_period(e, "2024-06-15T14:00:00Z", "PT1H");
    add_price(e, 7, nullptr, nullptr, 0.42);
    ResolvedInterval r[1];
    TEST_ASSERT_EQUAL_size_t(1, oa3::resolve_intervals(e, r, 1));
    TEST_ASSERT_EQUAL_INT32(7, r[0].id);
    TEST_ASSERT_EQUAL_UINT16(0, r[0].index);
}

// --- resolution: clj-oa3 test/openadr3/entities_test.clj:183-285 -------------------------

static void test_clj_duration_only_accumulates() {
    Event& e = g_events[0];
    set_event_period(e, "2024-06-15T12:00:00Z", "PT1H");
    add_price(e, 0, nullptr, "PT15M", 1);
    add_price(e, 1, nullptr, "PT45M", 2);
    ResolvedInterval r[2];
    TEST_ASSERT_EQUAL_size_t(2, oa3::resolve_intervals(e, r, 2));
    assert_dt("2024-06-15T12:00:00Z", r[0].start);
    TEST_ASSERT_EQUAL_INT32(15, r[0].duration.minutes);
    assert_dt("2024-06-15T12:15:00Z", r[1].start);
    TEST_ASSERT_EQUAL_INT32(45, r[1].duration.minutes);
}

static void test_clj_start_only_end() {
    Event& e = g_events[0];
    set_event_period(e, "2024-06-15T12:00:00Z", "PT1H");
    add_price(e, 0, "2024-06-15T15:30:00Z", nullptr, 1);
    ResolvedInterval r[1];
    oa3::resolve_intervals(e, r, 1);
    TEST_ASSERT_EQUAL_INT64(ux("2024-06-15T16:30:00Z"), r[0].end_unix);
}

static void test_clj_sequential_pt30m() {
    Event& e = g_events[0];
    set_event_period(e, "2024-06-15T12:00:00Z", "PT30M");
    for (int i = 0; i < 4; ++i) add_price(e, i, nullptr, nullptr, i);
    ResolvedInterval r[4];
    TEST_ASSERT_EQUAL_size_t(4, oa3::resolve_intervals(e, r, 4));
    assert_dt("2024-06-15T12:00:00Z", r[0].start);
    assert_dt("2024-06-15T12:30:00Z", r[1].start);
    assert_dt("2024-06-15T13:00:00Z", r[2].start);
    assert_dt("2024-06-15T13:30:00Z", r[3].start);
}

static void test_clj_no_event_period_two_intervals() {
    Event& e = g_events[0];
    add_price(e, 0, nullptr, nullptr, 1);
    add_price(e, 1, nullptr, nullptr, 2);
    ResolvedInterval r[2];
    TEST_ASSERT_EQUAL_size_t(2, oa3::resolve_intervals(e, r, 2));
    for (int i = 0; i < 2; ++i) {
        TEST_ASSERT_FALSE(r[i].has_start);
        TEST_ASSERT_FALSE(r[i].has_duration);
        TEST_ASSERT_FALSE(r[i].has_period);
    }
}

// --- resolution: edge cases ---------------------------------------------------------------

static void test_capacity_limits_output() {
    Event& e = g_events[0];
    set_event_period(e, "2024-06-15T12:00:00Z", "PT1H");
    for (int i = 0; i < 5; ++i) add_price(e, i, nullptr, nullptr, i);
    ResolvedInterval r[3];
    TEST_ASSERT_EQUAL_size_t(3, oa3::resolve_intervals(e, r, 3));
    TEST_ASSERT_EQUAL_size_t(0, oa3::resolve_intervals(e, r, 0));
}

// User_Guide.md:635: the event intervalPeriod is a default for every interval.
static void test_randomize_start_inherited_from_event() {
    Event& e = g_events[0];
    set_event_period(e, "2024-06-15T12:00:00Z", "PT1H");
    e.interval_period.has_randomize_start = true;
    e.interval_period.randomize_start = dur("PT5M");
    oa3::Interval& iv0 = add_price(e, 0, nullptr, nullptr, 1);
    iv0.has_interval_period = true;
    iv0.interval_period.has_randomize_start = true;
    iv0.interval_period.randomize_start = dur("PT10M");
    add_price(e, 1, nullptr, nullptr, 2);
    ResolvedInterval r[2];
    oa3::resolve_intervals(e, r, 2);
    TEST_ASSERT_TRUE(r[0].has_randomize_start);
    TEST_ASSERT_EQUAL_INT32(10, r[0].randomize_start.minutes);
    TEST_ASSERT_TRUE(r[1].has_randomize_start);
    TEST_ASSERT_EQUAL_INT32(5, r[1].randomize_start.minutes);

    g_events[1] = Event{};
    add_price(g_events[1], 0, "2024-06-15T12:00:00Z", "PT1H", 1);
    oa3::resolve_intervals(g_events[1], r, 1);
    TEST_ASSERT_FALSE(r[0].has_randomize_start);
}

// User_Guide.md:640-645: an interval start of 0001-01-01 is the event start for the first
// interval and the previous interval's end for the others.
static void test_sequential_sentinel_start() {
    Event& e = g_events[0];
    set_event_period(e, "2026-11-12T00:00:00Z", "PT1H");
    add_price(e, 0, nullptr, nullptr, 0.1);
    add_price(e, 1, "0001-01-01", nullptr, 0.2);
    PriceAt p;
    TEST_ASSERT_TRUE(oa3::price_at(g_events, 1, ux("2026-11-12T01:30:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_DOUBLE(0.2, p.value->number);

    Event& f = g_events[1];
    set_event_period(f, "2026-11-12T00:00:00Z", "PT1H");
    add_price(f, 0, "0001-01-01T00:00:00", nullptr, 0.1);
    TEST_ASSERT_TRUE(oa3::price_at(&f, 1, ux("2026-11-12T00:30:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_DOUBLE(0.1, p.value->number);

    // After an explicit start, the next sentinel follows it, not the event start.
    Event& g = g_events[2];
    set_event_period(g, "2026-11-12T00:00:00Z", "PT1H");
    add_price(g, 0, "2026-11-12T05:00:00Z", "PT30M", 1);
    add_price(g, 1, "0001-01-01", nullptr, 2);
    add_price(g, 2, "0001-01-01", "PT15M", 3);
    ResolvedInterval r[3];
    oa3::resolve_intervals(g, r, 3);
    assert_dt("2026-11-12T05:30:00Z", r[1].start);
    assert_dt("2026-11-12T06:30:00Z", r[2].start);
    TEST_ASSERT_EQUAL_INT64(ux("2026-11-12T06:45:00Z"), r[2].end_unix);
}

static void test_sequential_sentinel_without_event_start() {
    // "Do it now": no event start, so the first sentinel stays literal.
    Event& e = g_events[0];
    add_price(e, 0, "0001-01-01", "P9999Y", 1);
    add_price(e, 1, "0001-01-01", "PT1H", 2);
    ResolvedInterval r[2];
    oa3::resolve_intervals(e, r, 2);
    TEST_ASSERT_EQUAL_INT16(1, r[0].start.year);
    TEST_ASSERT_EQUAL_INT16(10000, r[1].start.year);
    PriceAt p;
    TEST_ASSERT_TRUE(oa3::price_at(g_events, 1, ux("2026-11-12T00:00:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_DOUBLE(1, p.value->number);

    // A sentinel after an interval without a period has nothing to follow.
    Event& f = g_events[1];
    add_price(f, 0, nullptr, nullptr, 1);
    add_price(f, 1, "0001-01-01", "PT1H", 2);
    oa3::resolve_intervals(f, r, 2);
    TEST_ASSERT_FALSE(r[1].has_start);
    TEST_ASSERT_FALSE(r[1].has_period);
}

static void test_empty_interval_period_inherits() {
    Event& e = g_events[0];
    set_event_period(e, "2024-06-15T12:00:00Z", "PT1H");
    oa3::Interval& iv = add_price(e, 0, nullptr, nullptr, 1);
    iv.has_interval_period = true;  // "intervalPeriod": {}
    ResolvedInterval r[1];
    oa3::resolve_intervals(e, r, 1);
    TEST_ASSERT_TRUE(r[0].has_period);
    assert_dt("2024-06-15T12:00:00Z", r[0].start);
}

static void test_accumulates_only_present_durations() {
    // Event start without duration: interval 0 has none, interval 1 brings its own.
    Event& e = g_events[0];
    set_event_period(e, "2024-06-15T12:00:00Z", nullptr);
    add_price(e, 0, nullptr, nullptr, 1);
    add_price(e, 1, nullptr, "PT20M", 2);
    add_price(e, 2, nullptr, nullptr, 3);
    ResolvedInterval r[3];
    oa3::resolve_intervals(e, r, 3);
    TEST_ASSERT_TRUE(r[0].has_start);
    TEST_ASSERT_FALSE(r[0].has_duration);
    TEST_ASSERT_FALSE(r[0].has_period);
    assert_dt("2024-06-15T12:00:00Z", r[1].start);
    TEST_ASSERT_TRUE(r[1].has_period);
    assert_dt("2024-06-15T12:20:00Z", r[2].start);
    TEST_ASSERT_FALSE(r[2].has_duration);
}

static void test_explicit_start_does_not_reset_accumulation() {
    // python sums every prior resolved duration, whatever the starts were.
    Event& e = g_events[0];
    set_event_period(e, "2024-06-15T12:00:00Z", "PT1H");
    add_price(e, 0, "2024-06-15T20:00:00Z", nullptr, 1);
    add_price(e, 1, nullptr, nullptr, 2);
    ResolvedInterval r[2];
    oa3::resolve_intervals(e, r, 2);
    assert_dt("2024-06-15T13:00:00Z", r[1].start);
}

static void test_inherited_start_keeps_offset() {
    Event& e = g_events[0];
    set_event_period(e, "2024-01-31T20:00:00-08:00", "P1M");
    add_price(e, 0, nullptr, nullptr, 1);
    add_price(e, 1, nullptr, nullptr, 2);
    ResolvedInterval r[2];
    oa3::resolve_intervals(e, r, 2);
    TEST_ASSERT_EQUAL_INT16(-480, r[1].start.offset_minutes);
    TEST_ASSERT_EQUAL_UINT8(2, r[1].start.month);
    TEST_ASSERT_EQUAL_UINT8(29, r[1].start.day);
    TEST_ASSERT_EQUAL_INT64(ux("2024-02-29T20:00:00-08:00"), r[0].end_unix);
}

static void test_inherited_p1m_sums_before_adding() {
    // python adds the summed durations once: Jan 31 + P2M is Mar 31; Jan 31 + P1M + P1M is Mar 29.
    Event& e = g_events[0];
    set_event_period(e, "2024-01-31T20:00:00-08:00", "P1M");
    for (int i = 0; i < 3; ++i) add_price(e, i, nullptr, nullptr, i);
    ResolvedInterval r[3];
    oa3::resolve_intervals(e, r, 3);
    char buf[oa3::kDateTimeStringSize];
    oa3::format_datetime(r[2].start, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("2024-03-31T20:00:00-08:00", buf);
}

static void test_resolver_reset() {
    Event& e = g_events[0];
    set_event_period(e, "2024-06-15T12:00:00Z", "PT1H");
    add_price(e, 0, nullptr, nullptr, 1);
    add_price(e, 1, nullptr, nullptr, 2);
    oa3::IntervalResolver resolver(e);
    ResolvedInterval r;
    TEST_ASSERT_TRUE(resolver.next(&r));
    TEST_ASSERT_TRUE(resolver.next(&r));
    assert_dt("2024-06-15T13:00:00Z", r.start);
    TEST_ASSERT_FALSE(resolver.next(&r));
    resolver.reset();
    TEST_ASSERT_TRUE(resolver.next(&r));
    assert_dt("2024-06-15T12:00:00Z", r.start);
    TEST_ASSERT_TRUE(resolver.next(&r));
    assert_dt("2024-06-15T13:00:00Z", r.start);
}

// --- is_unitary_payload_type: User_Guide.md:674-677 --------------------------------------

static void test_unitary_types() {
    TEST_ASSERT_TRUE(oa3::is_unitary_payload_type("PRICE"));
    TEST_ASSERT_TRUE(oa3::is_unitary_payload_type("EXPORT_PRICE"));
    TEST_ASSERT_TRUE(oa3::is_unitary_payload_type("GHG"));
    TEST_ASSERT_FALSE(oa3::is_unitary_payload_type("price"));
    TEST_ASSERT_FALSE(oa3::is_unitary_payload_type("SIMPLE"));
    TEST_ASSERT_FALSE(oa3::is_unitary_payload_type("PRICE "));
    TEST_ASSERT_FALSE(oa3::is_unitary_payload_type(""));
    TEST_ASSERT_FALSE(oa3::is_unitary_payload_type(nullptr));
}

// --- price_at ------------------------------------------------------------------------------

// The shape of test/fixtures/price_server_events.json: newest first, 24 hourly intervals
// each with its own intervalPeriod, under an event-level PT24H period.
static void build_price_server_events() {
    const char* days[] = {"2026-11-14", "2026-11-13", "2026-11-12"};
    const double first[] = {0.050294, 0.055856, 0.057949};
    for (int d = 0; d < 3; ++d) {
        Event& e = g_events[d];
        char start[32];
        snprintf(start, sizeof(start), "%sT08:00:00Z", days[d]);
        set_event_period(e, start, "PT24H");
        int64_t base = ux(start);
        for (int i = 0; i < 24; ++i) {
            DateTime s{};
            TEST_ASSERT_TRUE(oa3::from_unix(base + 3600 * i, &s));
            char buf[oa3::kDateTimeStringSize];
            TEST_ASSERT_TRUE(oa3::format_datetime(s, buf, sizeof(buf)) > 0);
            add_price(e, i, buf, "PT1H", i == 0 ? first[d] : 0.06 + 0.001 * i);
        }
    }
}

static void test_price_server_shape_active() {
    build_price_server_events();
    PriceAt p;
    TEST_ASSERT_TRUE(oa3::price_at(g_events, 3, ux("2026-11-14T08:30:00Z"), "PRICE", &p));
    TEST_ASSERT_TRUE(p.active);
    TEST_ASSERT_EQUAL_PTR(&g_events[0], p.event);
    TEST_ASSERT_EQUAL_UINT16(0, p.interval_index);
    TEST_ASSERT_EQUAL_UINT16(0, p.value_index);
    TEST_ASSERT_EQUAL_UINT16(1, p.value_count);
    TEST_ASSERT_EQUAL_DOUBLE(0.050294, p.value->number);
    TEST_ASSERT_EQUAL_INT64(ux("2026-11-14T08:00:00Z"), p.start_unix);
    TEST_ASSERT_EQUAL_INT64(ux("2026-11-14T09:00:00Z"), p.end_unix);
    TEST_ASSERT_TRUE(p.has_next_change);
    TEST_ASSERT_EQUAL_INT64(ux("2026-11-14T09:00:00Z"), p.next_change_unix);

    TEST_ASSERT_TRUE(oa3::price_at(g_events, 3, ux("2026-11-13T08:00:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_PTR(&g_events[1], p.event);
    TEST_ASSERT_EQUAL_DOUBLE(0.055856, p.value->number);

    TEST_ASSERT_TRUE(oa3::price_at(g_events, 3, ux("2026-11-13T07:59:59Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_PTR(&g_events[2], p.event);
    TEST_ASSERT_EQUAL_UINT16(23, p.interval_index);
    TEST_ASSERT_EQUAL_INT64(ux("2026-11-13T08:00:00Z"), p.next_change_unix);
}

static void test_price_server_shape_datetime_overload() {
    build_price_server_events();
    PriceAt p;
    TEST_ASSERT_TRUE(
        oa3::price_at(g_events, 3, dt("2026-11-14T00:30:00-08:00"), "PRICE", &p));
    TEST_ASSERT_EQUAL_DOUBLE(0.050294, p.value->number);
}

static void test_before_every_event() {
    build_price_server_events();
    PriceAt p;
    TEST_ASSERT_FALSE(oa3::price_at(g_events, 3, ux("2026-11-12T07:00:00Z"), "PRICE", &p));
    TEST_ASSERT_FALSE(p.active);
    TEST_ASSERT_NULL(p.event);
    TEST_ASSERT_NULL(p.value);
    TEST_ASSERT_TRUE(p.has_next_change);
    TEST_ASSERT_EQUAL_INT64(ux("2026-11-12T08:00:00Z"), p.next_change_unix);
}

static void test_after_every_event() {
    build_price_server_events();
    PriceAt p;
    TEST_ASSERT_FALSE(oa3::price_at(g_events, 3, ux("2026-11-15T08:00:00Z"), "PRICE", &p));
    TEST_ASSERT_FALSE(p.has_next_change);
}

static void test_other_payload_type_inactive() {
    build_price_server_events();
    PriceAt p;
    TEST_ASSERT_FALSE(
        oa3::price_at(g_events, 3, ux("2026-11-14T08:30:00Z"), "EXPORT_PRICE", &p));
    TEST_ASSERT_FALSE(p.has_next_change);
    TEST_ASSERT_FALSE(oa3::price_at(g_events, 0, ux("2026-11-14T08:30:00Z"), "PRICE", &p));
    TEST_ASSERT_FALSE(oa3::price_at(nullptr, 3, ux("2026-11-14T08:30:00Z"), "PRICE", &p));
}

// User_Guide.md Example 7.3-1 (around :684-718)
static void build_multi_price(const char* type) {
    Event& e = g_events[0];
    const double v[] = {0.17, 0.03, 0.11};
    add_interval(e, 0, "2025-06-25T00:00:00.000Z", "PT3H", type, v, 3);
}

static void test_multi_valued_payload_sub_intervals() {
    build_multi_price("PRICE");
    PriceAt p;
    TEST_ASSERT_TRUE(oa3::price_at(g_events, 1, ux("2025-06-25T01:30:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_DOUBLE(0.03, p.value->number);
    TEST_ASSERT_EQUAL_UINT16(1, p.value_index);
    TEST_ASSERT_EQUAL_UINT16(1, p.value_count);
    TEST_ASSERT_EQUAL_INT64(ux("2025-06-25T01:00:00Z"), p.start_unix);
    TEST_ASSERT_EQUAL_INT64(ux("2025-06-25T02:00:00Z"), p.end_unix);
    TEST_ASSERT_EQUAL_INT64(ux("2025-06-25T02:00:00Z"), p.next_change_unix);

    TEST_ASSERT_TRUE(oa3::price_at(g_events, 1, ux("2025-06-25T00:00:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_DOUBLE(0.17, p.value->number);
    TEST_ASSERT_TRUE(oa3::price_at(g_events, 1, ux("2025-06-25T02:59:59Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_DOUBLE(0.11, p.value->number);
    TEST_ASSERT_EQUAL_INT64(ux("2025-06-25T03:00:00Z"), p.end_unix);
    TEST_ASSERT_FALSE(oa3::price_at(g_events, 1, ux("2025-06-25T03:00:00Z"), "PRICE", &p));
}

static void test_multi_valued_uneven_split() {
    Event& e = g_events[0];
    const double v[] = {1, 2, 3, 4, 5, 6, 7};
    add_interval(e, 0, "2025-06-25T00:00:00Z", "PT1H", "GHG", v, 7);
    const int64_t s = ux("2025-06-25T00:00:00Z");
    PriceAt p;
    TEST_ASSERT_TRUE(oa3::price_at(g_events, 1, s + 514, "GHG", &p));
    TEST_ASSERT_EQUAL_UINT16(1, p.value_index);
    TEST_ASSERT_EQUAL_INT64(s + 514, p.start_unix);
    TEST_ASSERT_EQUAL_INT64(s + 1028, p.end_unix);
    TEST_ASSERT_TRUE(oa3::price_at(g_events, 1, s + 3599, "GHG", &p));
    TEST_ASSERT_EQUAL_UINT16(6, p.value_index);
    TEST_ASSERT_EQUAL_INT64(s + 514 * 6, p.start_unix);
    TEST_ASSERT_EQUAL_INT64(s + 3600, p.end_unix);
}

static void test_non_unitary_type_whole_span() {
    build_multi_price("SIMPLE");
    PriceAt p;
    TEST_ASSERT_TRUE(oa3::price_at(g_events, 1, ux("2025-06-25T01:30:00Z"), "SIMPLE", &p));
    TEST_ASSERT_EQUAL_UINT16(0, p.value_index);
    TEST_ASSERT_EQUAL_UINT16(3, p.value_count);
    TEST_ASSERT_EQUAL_DOUBLE(0.17, p.value->number);
    TEST_ASSERT_EQUAL_INT64(ux("2025-06-25T00:00:00Z"), p.start_unix);
    TEST_ASSERT_EQUAL_INT64(ux("2025-06-25T03:00:00Z"), p.end_unix);
}

static void test_empty_payload_not_a_candidate() {
    Event& e = g_events[0];
    add_interval(e, 0, "2025-06-25T00:00:00Z", "PT1H", "PRICE", nullptr, 0);
    PriceAt p;
    TEST_ASSERT_FALSE(oa3::price_at(g_events, 1, ux("2025-06-25T00:30:00Z"), "PRICE", &p));
    TEST_ASSERT_FALSE(p.has_next_change);
}

static void test_interval_without_period_ignored() {
    Event& e = g_events[0];
    add_price(e, 0, nullptr, nullptr, 1.0);
    add_price(e, 1, "2025-06-25T01:00:00Z", "PT1H", 2.0);
    PriceAt p;
    TEST_ASSERT_TRUE(oa3::price_at(g_events, 1, ux("2025-06-25T01:30:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_UINT16(1, p.interval_index);
}

static void test_gap_next_change_is_next_start() {
    Event& e = g_events[0];
    add_price(e, 0, "2025-06-25T00:00:00Z", "PT1H", 1.0);
    add_price(e, 1, "2025-06-25T03:00:00Z", "PT1H", 2.0);
    PriceAt p;
    TEST_ASSERT_FALSE(oa3::price_at(g_events, 1, ux("2025-06-25T01:30:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_INT64(ux("2025-06-25T03:00:00Z"), p.next_change_unix);
}

static void test_priority_overlap() {
    Event& low = g_events[0];
    low.has_priority = true;
    low.priority = 2;
    add_price(low, 0, "2025-06-25T00:00:00Z", "PT4H", 1.0);
    Event& high = g_events[1];
    high.has_priority = true;
    high.priority = 1;
    add_price(high, 0, "2025-06-25T01:00:00Z", "PT1H", 9.0);
    PriceAt p;
    TEST_ASSERT_TRUE(oa3::price_at(g_events, 2, ux("2025-06-25T01:30:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_PTR(&high, p.event);
    TEST_ASSERT_EQUAL_DOUBLE(9.0, p.value->number);
    TEST_ASSERT_EQUAL_INT64(ux("2025-06-25T02:00:00Z"), p.next_change_unix);
    TEST_ASSERT_TRUE(oa3::price_at(g_events, 2, ux("2025-06-25T00:30:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_PTR(&low, p.event);
    TEST_ASSERT_EQUAL_INT64(ux("2025-06-25T01:00:00Z"), p.next_change_unix);
}

static void test_absent_priority_ranks_below_present() {
    Event& none = g_events[0];
    add_price(none, 0, "2025-06-25T00:00:00Z", "PT1H", 1.0);
    Event& some = g_events[1];
    some.has_priority = true;
    some.priority = 999;
    add_price(some, 0, "2025-06-25T00:00:00Z", "PT1H", 2.0);
    PriceAt p;
    TEST_ASSERT_TRUE(oa3::price_at(g_events, 2, ux("2025-06-25T00:30:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_PTR(&some, p.event);
}

static void test_equal_priority_lower_index_wins() {
    for (int i = 0; i < 2; ++i) {
        g_events[i].has_priority = true;
        g_events[i].priority = 0;
        add_price(g_events[i], 0, "2025-06-25T00:00:00Z", "PT1H", i + 1.0);
    }
    PriceAt p;
    TEST_ASSERT_TRUE(oa3::price_at(g_events, 2, ux("2025-06-25T00:30:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_PTR(&g_events[0], p.event);

    memset(g_events, 0, sizeof(g_events));
    add_price(g_events[0], 0, "2025-06-25T00:00:00Z", "PT2H", 1.0);
    add_price(g_events[0], 1, "2025-06-25T00:00:00Z", "PT1H", 2.0);
    TEST_ASSERT_TRUE(oa3::price_at(g_events, 1, ux("2025-06-25T00:30:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_UINT16(0, p.interval_index);
}

// User_Guide.md "event.duration" (:724-725): loop, extend and shorten.
static void build_daily(Event& e, const char* event_duration) {
    set_event_period(e, "2026-01-01T00:00:00Z", "PT1H");
    for (int i = 0; i < 24; ++i) add_price(e, i, nullptr, nullptr, i);
    if (event_duration) {
        e.has_duration = true;
        e.duration = dur(event_duration);
    }
}

static void test_event_duration_shortens() {
    build_daily(g_events[0], "PT12H");
    PriceAt p;
    TEST_ASSERT_TRUE(oa3::price_at(g_events, 1, ux("2026-01-01T11:30:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_DOUBLE(11, p.value->number);
    TEST_ASSERT_EQUAL_INT64(ux("2026-01-01T12:00:00Z"), p.next_change_unix);
    TEST_ASSERT_FALSE(oa3::price_at(g_events, 1, ux("2026-01-01T12:00:00Z"), "PRICE", &p));
    TEST_ASSERT_FALSE(p.has_next_change);
}

static void test_event_duration_clips_mid_interval() {
    build_daily(g_events[0], "PT90M");
    PriceAt p;
    TEST_ASSERT_TRUE(oa3::price_at(g_events, 1, ux("2026-01-01T01:15:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_INT64(ux("2026-01-01T01:30:00Z"), p.end_unix);
    TEST_ASSERT_EQUAL_INT64(ux("2026-01-01T01:30:00Z"), p.next_change_unix);
}

static void test_event_duration_p9999y_loops() {
    build_daily(g_events[0], "P9999Y");
    PriceAt p;
    TEST_ASSERT_TRUE(oa3::price_at(g_events, 1, ux("2027-02-05T05:30:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_DOUBLE(5, p.value->number);
    TEST_ASSERT_EQUAL_UINT16(5, p.interval_index);
    TEST_ASSERT_EQUAL_INT64(ux("2027-02-05T05:00:00Z"), p.start_unix);
    TEST_ASSERT_EQUAL_INT64(ux("2027-02-05T06:00:00Z"), p.end_unix);
    TEST_ASSERT_EQUAL_INT64(ux("2027-02-05T06:00:00Z"), p.next_change_unix);

    TEST_ASSERT_TRUE(oa3::price_at(g_events, 1, ux("2027-02-05T23:30:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_DOUBLE(23, p.value->number);
    TEST_ASSERT_EQUAL_INT64(ux("2027-02-06T00:00:00Z"), p.next_change_unix);

    TEST_ASSERT_FALSE(oa3::price_at(g_events, 1, ux("2025-12-31T23:00:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_INT64(ux("2026-01-01T00:00:00Z"), p.next_change_unix);
}

static void test_event_duration_p7d_repeats_for_a_week() {
    build_daily(g_events[0], "P7D");
    PriceAt p;
    TEST_ASSERT_TRUE(oa3::price_at(g_events, 1, ux("2026-01-07T22:10:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_DOUBLE(22, p.value->number);
    TEST_ASSERT_TRUE(oa3::price_at(g_events, 1, ux("2026-01-07T23:10:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_INT64(ux("2026-01-08T00:00:00Z"), p.end_unix);
    TEST_ASSERT_EQUAL_INT64(ux("2026-01-08T00:00:00Z"), p.next_change_unix);
    TEST_ASSERT_FALSE(oa3::price_at(g_events, 1, ux("2026-01-08T00:00:00Z"), "PRICE", &p));
    TEST_ASSERT_FALSE(p.has_next_change);
}

static void test_event_duration_ignored_when_not_applicable() {
    // Negative: ignored.
    build_daily(g_events[0], "-PT1H");
    PriceAt p;
    TEST_ASSERT_TRUE(oa3::price_at(g_events, 1, ux("2026-01-01T23:30:00Z"), "PRICE", &p));
    TEST_ASSERT_FALSE(oa3::price_at(g_events, 1, ux("2026-01-02T00:30:00Z"), "PRICE", &p));

    // An interval without a period: no loop.
    memset(g_events, 0, sizeof(g_events));
    Event& e = g_events[0];
    e.has_duration = true;
    e.duration = dur("P9999Y");
    add_price(e, 0, "2026-01-01T00:00:00Z", "PT1H", 1.0);
    add_price(e, 1, nullptr, nullptr, 2.0);
    TEST_ASSERT_FALSE(oa3::price_at(g_events, 1, ux("2026-01-01T01:30:00Z"), "PRICE", &p));
    TEST_ASSERT_FALSE(p.has_next_change);
}

static void test_loop_with_explicit_starts() {
    // The price-server shape (own starts) looping daily.
    Event& e = g_events[0];
    e.has_duration = true;
    e.duration = dur("P9999Y");
    add_price(e, 0, "2026-01-01T08:00:00Z", "PT1H", 1.0);
    add_price(e, 1, "2026-01-01T10:00:00Z", "PT1H", 2.0);
    PriceAt p;
    // C is 3 hours (08:00-11:00), so 3 days later (24 cycles) looks like the first day.
    TEST_ASSERT_TRUE(oa3::price_at(g_events, 1, ux("2026-01-04T08:30:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_DOUBLE(1.0, p.value->number);
    TEST_ASSERT_FALSE(oa3::price_at(g_events, 1, ux("2026-01-04T09:30:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_INT64(ux("2026-01-04T10:00:00Z"), p.next_change_unix);
}

static void test_fall_back_day_25_intervals() {
    // 2026-11-01 in America/Los_Angeles: 07:00Z to 08:00Z next day is 25 hours.
    Event& e = g_events[0];
    set_event_period(e, "2026-11-01T07:00:00Z", "PT1H");
    for (int i = 0; i < 25; ++i) add_price(e, i, nullptr, nullptr, 100 + i);
    ResolvedInterval r[OA3_MAX_INTERVALS];
    TEST_ASSERT_EQUAL_size_t(25, oa3::resolve_intervals(e, r, OA3_MAX_INTERVALS));
    TEST_ASSERT_EQUAL_INT64(ux("2026-11-02T07:00:00Z"), r[24].start_unix);
    TEST_ASSERT_EQUAL_INT64(ux("2026-11-02T08:00:00Z"), r[24].end_unix);
    PriceAt p;
    TEST_ASSERT_TRUE(oa3::price_at(g_events, 1, ux("2026-11-02T07:30:00Z"), "PRICE", &p));
    TEST_ASSERT_EQUAL_UINT16(24, p.interval_index);
    TEST_ASSERT_EQUAL_DOUBLE(124, p.value->number);
    TEST_ASSERT_EQUAL_INT64(ux("2026-11-02T08:00:00Z"), p.next_change_unix);
}

static void test_fall_back_day_offset_starts() {
    // The same day written with a -08:00 start resolves to the same instants.
    Event& e = g_events[0];
    set_event_period(e, "2026-11-01T00:00:00-07:00", "PT1H");
    for (int i = 0; i < 25; ++i) add_price(e, i, nullptr, nullptr, 100 + i);
    ResolvedInterval r[OA3_MAX_INTERVALS];
    oa3::resolve_intervals(e, r, OA3_MAX_INTERVALS);
    TEST_ASSERT_EQUAL_INT64(ux("2026-11-02T08:00:00Z"), r[24].end_unix);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_passthrough);
    RUN_TEST(test_inherits_start_and_duration);
    RUN_TEST(test_duration_only_inherits_start);
    RUN_TEST(test_start_only_inherits_duration);
    RUN_TEST(test_no_inheritance_possible);
    RUN_TEST(test_sequential_starts);
    RUN_TEST(test_does_not_mutate_event);
    RUN_TEST(test_empty_intervals);
    RUN_TEST(test_id_passthrough);
    RUN_TEST(test_clj_duration_only_accumulates);
    RUN_TEST(test_clj_start_only_end);
    RUN_TEST(test_clj_sequential_pt30m);
    RUN_TEST(test_clj_no_event_period_two_intervals);
    RUN_TEST(test_capacity_limits_output);
    RUN_TEST(test_randomize_start_inherited_from_event);
    RUN_TEST(test_sequential_sentinel_start);
    RUN_TEST(test_sequential_sentinel_without_event_start);
    RUN_TEST(test_empty_interval_period_inherits);
    RUN_TEST(test_accumulates_only_present_durations);
    RUN_TEST(test_explicit_start_does_not_reset_accumulation);
    RUN_TEST(test_inherited_start_keeps_offset);
    RUN_TEST(test_inherited_p1m_sums_before_adding);
    RUN_TEST(test_resolver_reset);
    RUN_TEST(test_unitary_types);
    RUN_TEST(test_price_server_shape_active);
    RUN_TEST(test_price_server_shape_datetime_overload);
    RUN_TEST(test_before_every_event);
    RUN_TEST(test_after_every_event);
    RUN_TEST(test_other_payload_type_inactive);
    RUN_TEST(test_multi_valued_payload_sub_intervals);
    RUN_TEST(test_multi_valued_uneven_split);
    RUN_TEST(test_non_unitary_type_whole_span);
    RUN_TEST(test_empty_payload_not_a_candidate);
    RUN_TEST(test_interval_without_period_ignored);
    RUN_TEST(test_gap_next_change_is_next_start);
    RUN_TEST(test_priority_overlap);
    RUN_TEST(test_absent_priority_ranks_below_present);
    RUN_TEST(test_equal_priority_lower_index_wins);
    RUN_TEST(test_event_duration_shortens);
    RUN_TEST(test_event_duration_clips_mid_interval);
    RUN_TEST(test_event_duration_p9999y_loops);
    RUN_TEST(test_event_duration_p7d_repeats_for_a_week);
    RUN_TEST(test_event_duration_ignored_when_not_applicable);
    RUN_TEST(test_loop_with_explicit_starts);
    RUN_TEST(test_fall_back_day_25_intervals);
    RUN_TEST(test_fall_back_day_offset_starts);
    return UNITY_END();
}
