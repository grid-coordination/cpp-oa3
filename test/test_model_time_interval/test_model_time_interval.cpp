#include <oa3/model/time_interval.h>
#include <oa3/model/time.h>
#include <stdio.h>
#include <unity.h>

using oa3::AllenRelation;
using oa3::DateTime;
using oa3::TimeInterval;

void setUp() {}
void tearDown() {}

static DateTime at(int64_t seconds) {
    DateTime d{};
    d.seconds = seconds;
    return d;
}

static TimeInterval iv(int64_t start, int64_t end) { return TimeInterval{at(start), at(end), false}; }

static TimeInterval from(int64_t start) { return TimeInterval{at(start), DateTime{}, true}; }

static void assert_relation(AllenRelation expected, const TimeInterval& a, const TimeInterval& b) {
    TEST_ASSERT_EQUAL_STRING(oa3::to_string(expected), oa3::to_string(oa3::allen_relation(a, b)));
    TEST_ASSERT_EQUAL_STRING(oa3::to_string(oa3::inverse(expected)), oa3::to_string(oa3::allen_relation(b, a)));
}

static void test_thirteen_relations() {
    const TimeInterval b = iv(10, 20);
    assert_relation(AllenRelation::kBefore, iv(0, 5), b);
    assert_relation(AllenRelation::kMeets, iv(0, 10), b);
    assert_relation(AllenRelation::kOverlaps, iv(5, 15), b);
    assert_relation(AllenRelation::kStarts, iv(10, 15), b);
    assert_relation(AllenRelation::kDuring, iv(12, 18), b);
    assert_relation(AllenRelation::kFinishes, iv(15, 20), b);
    assert_relation(AllenRelation::kEquals, iv(10, 20), b);
    assert_relation(AllenRelation::kAfter, iv(25, 30), b);
    assert_relation(AllenRelation::kMetBy, iv(20, 30), b);
    assert_relation(AllenRelation::kOverlappedBy, iv(15, 25), b);
    assert_relation(AllenRelation::kStartedBy, iv(10, 25), b);
    assert_relation(AllenRelation::kContains, iv(5, 25), b);
    assert_relation(AllenRelation::kFinishedBy, iv(5, 20), b);
}

static void test_inverse_is_an_involution() {
    for (int r = 0; r <= static_cast<int>(AllenRelation::kFinishedBy); ++r) {
        const AllenRelation rel = static_cast<AllenRelation>(r);
        TEST_ASSERT_EQUAL_STRING(oa3::to_string(rel), oa3::to_string(oa3::inverse(oa3::inverse(rel))));
    }
}

static void test_zero_length() {
    const TimeInterval b = iv(10, 20);
    assert_relation(AllenRelation::kBefore, iv(5, 5), b);
    assert_relation(AllenRelation::kStarts, iv(10, 10), b);
    assert_relation(AllenRelation::kDuring, iv(15, 15), b);
    assert_relation(AllenRelation::kMetBy, iv(20, 20), b);
    assert_relation(AllenRelation::kAfter, iv(25, 25), b);
    assert_relation(AllenRelation::kEquals, iv(10, 10), iv(10, 10));
    assert_relation(AllenRelation::kBefore, iv(10, 10), iv(11, 11));
}

static void test_indefinite_end() {
    assert_relation(AllenRelation::kEquals, from(10), from(10));
    assert_relation(AllenRelation::kFinishes, from(15), from(10));
    assert_relation(AllenRelation::kStarts, iv(10, 20), from(10));
    assert_relation(AllenRelation::kDuring, iv(15, 20), from(10));
    assert_relation(AllenRelation::kBefore, iv(0, 5), from(10));
    assert_relation(AllenRelation::kMeets, iv(0, 10), from(10));
    assert_relation(AllenRelation::kOverlaps, iv(5, 15), from(10));
    assert_relation(AllenRelation::kDuring, iv(15, 15), from(10));
    assert_relation(AllenRelation::kStarts, iv(10, 10), from(10));
    TEST_ASSERT_TRUE(oa3::contains_instant(from(10), at(INT64_MAX)));
    TEST_ASSERT_FALSE(oa3::contains_instant(from(10), at(9)));
}

static void test_fractional_endpoints() {
    TimeInterval a = iv(10, 10);
    a.end.nanos = 1;
    assert_relation(AllenRelation::kMeets, a, TimeInterval{a.end, at(20), false});
    assert_relation(AllenRelation::kFinishes, TimeInterval{a.end, at(20), false}, iv(10, 20));
    TEST_ASSERT_TRUE(oa3::contains_instant(a, at(10)));
    TEST_ASSERT_FALSE(oa3::contains_instant(a, a.end));
}

// Over every pair from a grid of endpoints, zero-length and indefinite included: each pair
// has the inverse relation reversed, and intersects and contains_instant agree with the
// point sets.
static void test_grid_against_point_sets() {
    const int kMax = 6;
    TimeInterval set[64];
    int n = 0;
    for (int s = 0; s <= kMax; ++s) {
        for (int e = s; e <= kMax; ++e) {
            set[n++] = iv(s, e);
        }
        set[n++] = from(s);
    }
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            const TimeInterval& a = set[i];
            const TimeInterval& b = set[j];
            char msg[64];
            snprintf(msg, sizeof(msg), "a=[%lld,%lld%s) b=[%lld,%lld%s)", static_cast<long long>(a.start.seconds),
                     static_cast<long long>(a.end.seconds), a.end_indefinite ? "+inf" : "",
                     static_cast<long long>(b.start.seconds), static_cast<long long>(b.end.seconds),
                     b.end_indefinite ? "+inf" : "");
            TEST_ASSERT_EQUAL_STRING_MESSAGE(oa3::to_string(oa3::inverse(oa3::allen_relation(a, b))),
                                             oa3::to_string(oa3::allen_relation(b, a)), msg);

            const int64_t ae = a.end_indefinite ? 1000 : a.end.seconds;
            const int64_t be = b.end_indefinite ? 1000 : b.end.seconds;
            const int64_t lo = a.start.seconds > b.start.seconds ? a.start.seconds : b.start.seconds;
            const int64_t hi = ae < be ? ae : be;
            // A zero-length interval intersects what it is in, as an instant; two of them
            // intersect when equal.
            const bool a_zero = ae == a.start.seconds;
            const bool b_zero = be == b.start.seconds;
            bool expected = lo < hi;
            if (a_zero && b_zero) {
                expected = a.start.seconds == b.start.seconds;
            } else if (a_zero) {
                expected = b.start.seconds <= a.start.seconds && a.start.seconds < be;
            } else if (b_zero) {
                expected = a.start.seconds <= b.start.seconds && b.start.seconds < ae;
            }
            TEST_ASSERT_EQUAL_MESSAGE(expected, oa3::intersects(a, b), msg);
        }
        for (int t = -1; t <= kMax + 1; ++t) {
            const TimeInterval& a = set[i];
            const bool inside = t >= a.start.seconds && (a.end_indefinite || t < a.end.seconds);
            TEST_ASSERT_EQUAL(inside, oa3::contains_instant(a, at(t)));
        }
    }
}

static void test_make_interval() {
    DateTime start{};
    TEST_ASSERT_TRUE(oa3::parse_datetime("2026-11-14T08:00:00Z", &start));
    oa3::Duration d{};
    TimeInterval out{};

    TEST_ASSERT_TRUE(oa3::parse_duration("PT1H", &d));
    TEST_ASSERT_TRUE(oa3::make_interval(start, d, &out));
    TEST_ASSERT_FALSE(out.end_indefinite);
    TEST_ASSERT_EQUAL_INT64(oa3::to_unix(start) + 3600, oa3::to_unix(out.end));

    TEST_ASSERT_TRUE(oa3::parse_duration("PT0S", &d));
    TEST_ASSERT_TRUE(oa3::make_interval(start, d, &out));
    TEST_ASSERT_EQUAL_INT(0, oa3::compare(out.start, out.end));

    TEST_ASSERT_TRUE(oa3::parse_duration("P9999Y", &d));
    TEST_ASSERT_TRUE(oa3::make_interval(start, d, &out));
    TEST_ASSERT_TRUE(out.end_indefinite);

    TEST_ASSERT_TRUE(oa3::parse_duration("P40000Y", &d));
    TEST_ASSERT_TRUE(oa3::make_interval(start, d, &out));
    TEST_ASSERT_TRUE(out.end_indefinite);

    TEST_ASSERT_TRUE(oa3::parse_duration("-PT1H", &d));
    TEST_ASSERT_FALSE(oa3::make_interval(start, d, &out));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_thirteen_relations);
    RUN_TEST(test_inverse_is_an_involution);
    RUN_TEST(test_zero_length);
    RUN_TEST(test_indefinite_end);
    RUN_TEST(test_fractional_endpoints);
    RUN_TEST(test_grid_against_point_sets);
    RUN_TEST(test_make_interval);
    return UNITY_END();
}
