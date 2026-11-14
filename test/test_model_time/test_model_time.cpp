#include <oa3/model/time.h>
#include <string.h>
#include <unity.h>

void setUp() {}
void tearDown() {}

using oa3::DateTime;
using oa3::Duration;
using oa3::CivilTime;

static DateTime dt_of(const char* s) {
    DateTime dt{};
    TEST_ASSERT_TRUE_MESSAGE(oa3::parse_datetime(s, &dt), s);
    return dt;
}

static Duration dur_of(const char* s) {
    Duration d{};
    TEST_ASSERT_TRUE_MESSAGE(oa3::parse_duration(s, &d), s);
    return d;
}

static void assert_dt_roundtrip(const char* s) {
    char buf[oa3::kDateTimeStringSize];
    DateTime dt = dt_of(s);
    TEST_ASSERT_EQUAL_size_t(strlen(s), oa3::format_datetime(dt, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(s, buf);
}

static void assert_dur_roundtrip(const char* s) {
    char buf[oa3::kDurationStringSize];
    Duration d = dur_of(s);
    TEST_ASSERT_EQUAL_size_t(strlen(s), oa3::format_duration(d, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(s, buf);
}

static void assert_utc(const char* wire, const char* expected) {
    char buf[oa3::kDateTimeStringSize];
    TEST_ASSERT_TRUE_MESSAGE(oa3::format_datetime(dt_of(wire), buf, sizeof(buf)) > 0, wire);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, buf, wire);
}

static Duration through_compact(const Duration& d) {
    oa3::CompactDuration c{};
    TEST_ASSERT_TRUE(oa3::to_compact(d, &c));
    return oa3::to_duration(c);
}

// Also through a CompactDuration, which must give the same instant.
static void assert_add(const char* start, const char* dur, const char* expected) {
    char buf[oa3::kDateTimeStringSize];
    DateTime out{};
    TEST_ASSERT_TRUE_MESSAGE(oa3::add(dt_of(start), dur_of(dur), &out), dur);
    TEST_ASSERT_TRUE(oa3::format_datetime(out, buf, sizeof(buf)) > 0);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, buf, dur);
    DateTime compact_out{};
    TEST_ASSERT_TRUE_MESSAGE(oa3::add(dt_of(start), through_compact(dur_of(dur)), &compact_out), dur);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, oa3::compare(out, compact_out), dur);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(out.frac_digits, compact_out.frac_digits, dur);
}

// python-oa3 tests/test_time.py:16-62 (parse_datetime)
static void test_parse_rfc3339() {
    CivilTime c = oa3::to_civil(dt_of("2024-06-15T10:30:00Z"));
    TEST_ASSERT_EQUAL_INT(2024, c.year);
    TEST_ASSERT_EQUAL_UINT8(6, c.month);
    TEST_ASSERT_EQUAL_UINT8(15, c.day);
    TEST_ASSERT_EQUAL_UINT8(10, c.hour);
    TEST_ASSERT_EQUAL_UINT8(30, c.minute);
    TEST_ASSERT_EQUAL_UINT8(0, c.second);
}

static void test_parse_with_offset() {
    TEST_ASSERT_EQUAL_INT(0, oa3::compare(dt_of("2024-06-15T10:30:00-05:00"), dt_of("2024-06-15T15:30:00Z")));
    TEST_ASSERT_EQUAL_UINT8(15, oa3::to_civil(dt_of("2024-06-15T10:30:00-05:00")).hour);
}

static void test_parse_space_with_tz() {
    TEST_ASSERT_EQUAL_UINT8(10, oa3::to_civil(dt_of("2024-06-15 10:30:00Z")).hour);
}

static void test_parse_vtn_ri_mqtt_format() {
    CivilTime c = oa3::to_civil(dt_of("2026-03-08 19:22:06"));
    TEST_ASSERT_EQUAL_INT(2026, c.year);
    TEST_ASSERT_EQUAL_UINT8(3, c.month);
    TEST_ASSERT_EQUAL_UINT8(8, c.day);
    TEST_ASSERT_EQUAL_UINT8(19, c.hour);
    TEST_ASSERT_EQUAL_UINT8(22, c.minute);
    TEST_ASSERT_EQUAL_UINT8(6, c.second);
    assert_utc("2026-03-08 19:22:06", "2026-03-08T19:22:06Z");
}

static void test_parse_empty_and_null() {
    DateTime dt{};
    TEST_ASSERT_FALSE(oa3::parse_datetime("", &dt));
    TEST_ASSERT_FALSE(oa3::parse_datetime("   ", &dt));
    TEST_ASSERT_FALSE(oa3::parse_datetime(nullptr, &dt));
}

// clj-oa3 test/openadr3/entities_test.clj:474-483 (VTN-RI form is UTC)
static void test_vtn_ri_form_equals_utc() {
    TEST_ASSERT_EQUAL_INT(0, oa3::compare(dt_of("2024-06-15 09:30:00"), dt_of("2024-06-15T09:30:00Z")));
}

// clj-oa3 test/openadr3/entities_test.clj:485-504 (same instant); the offset is now applied, not kept.
static void test_offset_converted_to_utc() {
    TEST_ASSERT_EQUAL_INT(0, oa3::compare(dt_of("2026-05-03T00:00:00-07:00"), dt_of("2026-05-03T07:00:00Z")));
    assert_utc("2026-05-03T00:00:00-07:00", "2026-05-03T07:00:00Z");
    TEST_ASSERT_EQUAL_INT(0, oa3::compare(dt_of("2026-05-03T00:00:00+00:00"), dt_of("2026-05-03T00:00:00Z")));
}

static void test_parse_date_alone() {
    DateTime dt = dt_of("2024-06-15");
    TEST_ASSERT_EQUAL_UINT8(0, oa3::to_civil(dt).hour);
    char buf[oa3::kDateTimeStringSize];
    oa3::format_datetime(dt, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("2024-06-15T00:00:00Z", buf);
}

static void test_parse_variants_accepted() {
    assert_utc("  2024-06-15t10:30:00z \r\n", "2024-06-15T10:30:00Z");
    DateTime dt = dt_of("2024-02-29T23:59:59.5+14:00");
    TEST_ASSERT_EQUAL_UINT32(500000000, dt.nanos);
    TEST_ASSERT_EQUAL_UINT8(1, dt.frac_digits);
    assert_utc("2024-02-29T23:59:59.5+14:00", "2024-02-29T09:59:59.5Z");
    assert_utc("2024-06-15T10:30:00-00:00", "2024-06-15T10:30:00Z");
}

static void test_parse_fraction_truncated() {
    DateTime dt = dt_of("2026-04-11T18:59:48.4716617919999Z");
    TEST_ASSERT_EQUAL_UINT32(471661791, dt.nanos);
    TEST_ASSERT_EQUAL_UINT8(9, dt.frac_digits);
}

static void test_parse_rejects() {
    static const char* const kBad[] = {
        "2024-6-15T10:30:00Z",   "24-06-15",
        "2024-13-01",            "2024-00-01",
        "2023-02-29",            "2024-04-31",
        "2024-06-00",            "2024-06-15T24:00:00Z",
        "2024-06-15T10:60:00Z",  "2024-06-15T10:30:60Z",
        "2024-06-15T10:30Z",     "2024-06-15T10:30:00.Z",
        "2024-06-15T10:30:00+5:00", "2024-06-15T10:30:00+05",
        "2024-06-15T10:30:00+0500", "2024-06-15T10:30:00+24:00",
        "2024-06-15T10:30:00+05:60", "2024-06-15X10:30:00Z",
        "2024-06-15Z",           "2024-06-15T10:30:00ZZ",
        "2024-06-15T10:30:00Z x", "2024-06-15T",
        "x2024-06-15",
    };
    for (const char* s : kBad) {
        DateTime dt{};
        TEST_ASSERT_FALSE_MESSAGE(oa3::parse_datetime(s, &dt), s);
    }
}

static void test_parse_never_reads_past_len() {
    const char s[] = "2024-06-15T10:30:00Zjunk";
    DateTime dt{};
    TEST_ASSERT_TRUE(oa3::parse_datetime(s, 20, &dt));
    TEST_ASSERT_TRUE(oa3::parse_datetime(s, 19, &dt));
    TEST_ASSERT_EQUAL_INT64(1718447400, oa3::to_unix(dt));
    TEST_ASSERT_FALSE(oa3::parse_datetime(s, 18, &dt));
    TEST_ASSERT_FALSE(oa3::parse_datetime(s, 0, &dt));
}

// python-oa3 tests/test_time.py:101-135 (wire round trip); offsets now come back as UTC.
static void test_wire_roundtrip() {
    assert_dt_roundtrip("2024-06-15T10:30:00Z");
    assert_dt_roundtrip("2024-06-15T10:30:00.123456Z");
    assert_utc("2024-06-15T10:30:00+00:00", "2024-06-15T10:30:00Z");
    assert_utc("2024-06-15T10:30:00-07:00", "2024-06-15T17:30:00Z");
    assert_utc("2024-06-15T10:30:00+05:30", "2024-06-15T05:00:00Z");
    assert_utc("2024-06-15T10:30:00.123456-07:00", "2024-06-15T17:30:00.123456Z");
}

static void test_wire_roundtrip_fractions() {
    assert_dt_roundtrip("2026-04-11T18:59:48.471661791Z");
    assert_dt_roundtrip("2026-04-11T18:59:48.0Z");
    assert_utc("2026-04-11T18:59:48.100-09:30", "2026-04-12T04:29:48.100Z");
    assert_dt_roundtrip("0000-01-01T00:00:00Z");
    assert_dt_roundtrip("9999-12-31T23:59:59.999999999Z");
}

static void test_format_limits() {
    DateTime dt = dt_of("9999-12-31T23:59:59.999999999Z");
    char buf[oa3::kDateTimeStringSize];
    TEST_ASSERT_EQUAL_size_t(30, oa3::format_datetime(dt, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_size_t(0, oa3::format_datetime(dt, buf, 30));
    TEST_ASSERT_EQUAL_size_t(0, oa3::format_datetime(dt, nullptr, 0));
    // An offset can carry a wire year 0000 or 9999 outside the formattable years.
    TEST_ASSERT_EQUAL_size_t(0, oa3::format_datetime(dt_of("9999-12-31T23:59:59-00:01"), buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_size_t(0, oa3::format_datetime(dt_of("0000-01-01T00:00:00+00:01"), buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_size_t(0, oa3::format_datetime(DateTime{INT64_MAX, 0, 0}, buf, sizeof(buf)));
}

static void test_to_unix() {
    TEST_ASSERT_EQUAL_INT64(0, oa3::to_unix(dt_of("1970-01-01T00:00:00Z")));
    TEST_ASSERT_EQUAL_INT64(1718447400, oa3::to_unix(dt_of("2024-06-15T10:30:00Z")));
    TEST_ASSERT_EQUAL_INT64(1718447400, oa3::to_unix(dt_of("2024-06-15T03:30:00-07:00")));
    TEST_ASSERT_EQUAL_INT64(-1, oa3::to_unix(dt_of("1969-12-31T23:59:59.9Z")));
    TEST_ASSERT_EQUAL_INT64(951782400, oa3::to_unix(dt_of("2000-02-29")));
    TEST_ASSERT_EQUAL_INT64(-62167219200LL, oa3::to_unix(dt_of("0000-01-01T00:00:00Z")));
    TEST_ASSERT_EQUAL_INT64(253402300799LL, oa3::to_unix(dt_of("9999-12-31T23:59:59Z")));
}

static DateTime from_unix_ok(int64_t secs, uint32_t nanos = 0) {
    DateTime dt{};
    TEST_ASSERT_TRUE(oa3::from_unix(secs, nanos, &dt));
    return dt;
}

static void test_from_unix() {
    char buf[oa3::kDateTimeStringSize];
    DateTime dt = from_unix_ok(1718447400);
    oa3::format_datetime(dt, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("2024-06-15T10:30:00Z", buf);

    dt = from_unix_ok(1718447400, 250000000);
    TEST_ASSERT_EQUAL_UINT8(2, dt.frac_digits);
    oa3::format_datetime(dt, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("2024-06-15T10:30:00.25Z", buf);

    dt = from_unix_ok(1718447400, 1250000000);
    TEST_ASSERT_EQUAL_INT64(1718447401, oa3::to_unix(dt));
    TEST_ASSERT_EQUAL_UINT32(250000000, dt.nanos);

    dt = from_unix_ok(-1, 999999999);
    oa3::format_datetime(dt, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1969-12-31T23:59:59.999999999Z", buf);

    dt = from_unix_ok(-62167219200LL);
    oa3::format_datetime(dt, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("0000-01-01T00:00:00Z", buf);

    // Beyond the wire range, as P9999Y results reach.
    CivilTime c = oa3::to_civil(from_unix_ok(253402300800LL));
    TEST_ASSERT_EQUAL_INT(10000, c.year);
    TEST_ASSERT_EQUAL_UINT8(1, c.month);
    TEST_ASSERT_EQUAL_UINT8(1, c.day);
    // 12000-01-01 is five 400-year cycles (146097 days each) after 10000-01-01.
    c = oa3::to_civil(from_unix_ok((2932897LL + 5 * 146097) * 86400 + 3661));
    TEST_ASSERT_EQUAL_INT(12000, c.year);
    TEST_ASSERT_EQUAL_UINT8(1, c.month);
    TEST_ASSERT_EQUAL_UINT8(1, c.day);
    TEST_ASSERT_EQUAL_UINT8(1, c.hour);
    TEST_ASSERT_EQUAL_UINT8(1, c.minute);
    TEST_ASSERT_EQUAL_UINT8(1, c.second);

    dt = from_unix_ok(-86400LL * 800000);
    TEST_ASSERT_EQUAL_INT64(-86400LL * 800000, oa3::to_unix(dt));
    TEST_ASSERT_TRUE(oa3::to_civil(dt).year < 0);

    TEST_ASSERT_TRUE(oa3::from_unix(1718447400, &dt));
    TEST_ASSERT_EQUAL_INT64(1718447400, oa3::to_unix(dt));
}

static void test_from_unix_out_of_range() {
    DateTime dt{};
    TEST_ASSERT_FALSE(oa3::from_unix(2000000000000LL, &dt));
    TEST_ASSERT_FALSE(oa3::from_unix(-2000000000000LL, &dt));
    TEST_ASSERT_FALSE(oa3::from_unix(INT64_MAX, &dt));
    TEST_ASSERT_FALSE(oa3::from_unix(INT64_MIN, &dt));
    TEST_ASSERT_FALSE(oa3::from_unix(INT64_MAX, UINT32_MAX, &dt));
    // 32767-12-31T23:59:59Z is the last second in range.
    const int64_t last = (oa3::days_from_civil(32768, 1, 1)) * 86400 - 1;
    dt = from_unix_ok(last);
    TEST_ASSERT_EQUAL_INT(32767, oa3::to_civil(dt).year);
    TEST_ASSERT_FALSE(oa3::from_unix(last + 1, &dt));
    TEST_ASSERT_FALSE(oa3::from_unix(last, 1000000000, &dt));
}

static void test_offsets_cross_utc_dates() {
    TEST_ASSERT_EQUAL_INT64(oa3::to_unix(dt_of("2024-06-16T03:00:00Z")),
                            oa3::to_unix(dt_of("2024-06-15T20:00:00-07:00")));
    TEST_ASSERT_EQUAL_INT64(oa3::to_unix(dt_of("2024-12-31T23:30:00Z")),
                            oa3::to_unix(dt_of("2025-01-01T00:30:00+01:00")));
    assert_utc("2025-01-01T00:30:00+01:00", "2024-12-31T23:30:00Z");
    assert_add("2024-06-15T23:00:00-07:00", "PT2H", "2024-06-16T08:00:00Z");
}

static void test_civil_roundtrip_range() {
    for (int64_t days = -1000000; days <= 1000000; days += 37) {
        int64_t y = 0;
        unsigned m = 0;
        unsigned d = 0;
        oa3::civil_from_days(days, &y, &m, &d);
        TEST_ASSERT_TRUE(m >= 1 && m <= 12);
        TEST_ASSERT_TRUE(d >= 1 && d <= oa3::days_in_month(y, m));
        TEST_ASSERT_EQUAL_INT64(days, oa3::days_from_civil(y, m, d));
    }
}

static void test_days_in_month() {
    TEST_ASSERT_EQUAL_UINT(29, oa3::days_in_month(2024, 2));
    TEST_ASSERT_EQUAL_UINT(28, oa3::days_in_month(2023, 2));
    TEST_ASSERT_EQUAL_UINT(28, oa3::days_in_month(1900, 2));
    TEST_ASSERT_EQUAL_UINT(29, oa3::days_in_month(2000, 2));
    TEST_ASSERT_EQUAL_UINT(29, oa3::days_in_month(-4, 2));
    TEST_ASSERT_EQUAL_UINT(30, oa3::days_in_month(2024, 4));
    TEST_ASSERT_EQUAL_UINT(31, oa3::days_in_month(2024, 12));
    TEST_ASSERT_EQUAL_UINT(0, oa3::days_in_month(2024, 13));
}

static void test_compare() {
    TEST_ASSERT_TRUE(oa3::compare(dt_of("2024-06-15T10:30:00Z"), dt_of("2024-06-15T10:30:01Z")) < 0);
    TEST_ASSERT_TRUE(oa3::compare(dt_of("2024-06-15T10:30:00.2Z"), dt_of("2024-06-15T10:30:00.1Z")) > 0);
    TEST_ASSERT_EQUAL_INT(0, oa3::compare(dt_of("2024-06-15T10:30:00.1Z"), dt_of("2024-06-15T10:30:00.100Z")));
    TEST_ASSERT_TRUE(oa3::compare(dt_of("2024-06-15T10:30:00+05:30"), dt_of("2024-06-15T10:30:00Z")) < 0);
}

// python-oa3 tests/test_time.py:65-90 (parse_duration)
static void test_parse_duration_reference() {
    Duration d = dur_of("PT1H");
    TEST_ASSERT_EQUAL_INT32(1, d.hours);
    d = dur_of("P1DT2H30M");
    TEST_ASSERT_EQUAL_INT32(1, d.days);
    TEST_ASSERT_EQUAL_INT32(2, d.hours);
    TEST_ASSERT_EQUAL_INT32(30, d.minutes);
    TEST_ASSERT_FALSE(oa3::parse_duration("", &d));
    TEST_ASSERT_FALSE(oa3::parse_duration(nullptr, &d));
}

static void test_parse_duration_fields() {
    Duration d = dur_of("P1Y2M3DT4H5M6.789S");
    TEST_ASSERT_EQUAL_INT32(1, d.years);
    TEST_ASSERT_EQUAL_INT32(2, d.months);
    TEST_ASSERT_EQUAL_INT32(0, d.weeks);
    TEST_ASSERT_EQUAL_INT32(3, d.days);
    TEST_ASSERT_EQUAL_INT32(4, d.hours);
    TEST_ASSERT_EQUAL_INT32(5, d.minutes);
    TEST_ASSERT_EQUAL_INT32(6, d.seconds);
    TEST_ASSERT_EQUAL_INT32(789000000, d.nanos);
    TEST_ASSERT_EQUAL_UINT8(3, d.frac_digits);

    d = dur_of("P2W");
    TEST_ASSERT_EQUAL_INT32(2, d.weeks);
    d = dur_of("PT36H");
    TEST_ASSERT_EQUAL_INT32(36, d.hours);
    TEST_ASSERT_EQUAL_INT32(0, d.days);
    d = dur_of("P9999Y");
    TEST_ASSERT_TRUE(oa3::is_indefinite(d));
    d = dur_of("PT0.1234567891S");
    TEST_ASSERT_EQUAL_INT32(123456789, d.nanos);
    TEST_ASSERT_EQUAL_UINT8(9, d.frac_digits);
    d = dur_of("P2147483647D");
    TEST_ASSERT_EQUAL_INT32(2147483647, d.days);
}

static void test_parse_duration_negative() {
    Duration d = dur_of("-P1DT2H0.5S");
    TEST_ASSERT_EQUAL_INT32(-1, d.days);
    TEST_ASSERT_EQUAL_INT32(-2, d.hours);
    TEST_ASSERT_EQUAL_INT32(0, d.seconds);
    TEST_ASSERT_EQUAL_INT32(-500000000, d.nanos);
    TEST_ASSERT_TRUE(oa3::is_negative(d));
    TEST_ASSERT_FALSE(oa3::is_zero(d));
    TEST_ASSERT_FALSE(oa3::is_indefinite(dur_of("-P9999Y")));
}

static void test_parse_duration_rejects() {
    static const char* const kBad[] = {
        "P",       "PT",       "-P",       "P1DT",     "P1W2D",    "P1D1W",    "PT1H1H",  "P1M1Y",
        "PT1M1H",  "P1H",      "PT1D",     "P1.5D",    "PT1.5H",   "PT1.5M",   "PT1.S",   "PT.5S",
        "P1",      "PT1",      "1D",       "p1D",      "P1d",      "+P1D",     "P-1D",    "--P1D",
        " P1D",    "P1D ",     "P2147483648D", "PT99999999999S", "PTS",  "P1Y2M3D4H",
    };
    for (const char* s : kBad) {
        Duration d{};
        TEST_ASSERT_FALSE_MESSAGE(oa3::parse_duration(s, &d), s);
    }
}

static void test_parse_duration_never_reads_past_len() {
    const char s[] = "PT1H30Mxx";
    Duration d{};
    TEST_ASSERT_TRUE(oa3::parse_duration(s, 7, &d));
    TEST_ASSERT_EQUAL_INT32(30, d.minutes);
    TEST_ASSERT_TRUE(oa3::parse_duration(s, 4, &d));
    TEST_ASSERT_EQUAL_INT32(0, d.minutes);
    TEST_ASSERT_FALSE(oa3::parse_duration(s, 2, &d));
}

// python-oa3 tests/test_time.py:148-167 (duration round-trip)
static void test_duration_roundtrip_reference() {
    assert_dur_roundtrip("PT1H");
    assert_dur_roundtrip("PT2H30M");
    assert_dur_roundtrip("P1DT2H30M");
    assert_dur_roundtrip("PT15M");
    assert_dur_roundtrip("PT0S");
}

static void test_duration_roundtrip_more() {
    assert_dur_roundtrip("P1Y2M3DT4H5M6.789S");
    assert_dur_roundtrip("P2W");
    assert_dur_roundtrip("PT36H");
    assert_dur_roundtrip("P9999Y");
    assert_dur_roundtrip("-P1DT2H");
    assert_dur_roundtrip("-PT0.5S");
    assert_dur_roundtrip("PT1.500S");
    assert_dur_roundtrip("PT0.000000001S");
}

static void test_format_duration_normalized_forms() {
    char buf[oa3::kDurationStringSize];
    oa3::format_duration(dur_of("P0D"), buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("PT0S", buf);
    oa3::format_duration(dur_of("P1Y0M"), buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("P1Y", buf);
    oa3::format_duration(dur_of("P1DT0H"), buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("P1D", buf);
}

static void test_format_duration_errors() {
    char buf[oa3::kDurationStringSize];
    Duration d{};
    d.hours = 1;
    d.minutes = -1;
    TEST_ASSERT_EQUAL_size_t(0, oa3::format_duration(d, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_size_t(0, oa3::format_duration(dur_of("PT1H"), buf, 4));
    TEST_ASSERT_EQUAL_size_t(4, oa3::format_duration(dur_of("PT1H"), buf, 5));

    Duration worst{};
    worst.years = worst.months = worst.weeks = worst.days = INT32_MIN;
    worst.hours = worst.minutes = worst.seconds = INT32_MIN;
    worst.nanos = -999999999;
    worst.frac_digits = 9;
    const size_t n = oa3::format_duration(worst, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0 && n < oa3::kDurationStringSize);
}

static void test_duration_predicates() {
    Duration z{};
    TEST_ASSERT_TRUE(oa3::is_zero(z));
    TEST_ASSERT_FALSE(oa3::is_negative(z));
    TEST_ASSERT_TRUE(oa3::is_zero(dur_of("PT0S")));
    TEST_ASSERT_FALSE(oa3::is_zero(dur_of("PT0.1S")));
    TEST_ASSERT_TRUE(oa3::is_indefinite(dur_of("P10000Y")));
    TEST_ASSERT_FALSE(oa3::is_indefinite(dur_of("P9998Y")));
}

static void test_duration_sum() {
    Duration out{};
    TEST_ASSERT_TRUE(oa3::duration_sum(dur_of("PT15M"), dur_of("PT15M"), &out));
    TEST_ASSERT_EQUAL_INT32(30, out.minutes);
    TEST_ASSERT_TRUE(oa3::duration_sum(dur_of("P1M"), dur_of("P1M"), &out));
    TEST_ASSERT_EQUAL_INT32(2, out.months);
    TEST_ASSERT_EQUAL_INT32(0, out.days);
    TEST_ASSERT_TRUE(oa3::duration_sum(dur_of("PT0.75S"), dur_of("PT0.5S"), &out));
    TEST_ASSERT_EQUAL_INT32(1, out.seconds);
    TEST_ASSERT_EQUAL_INT32(250000000, out.nanos);
    TEST_ASSERT_EQUAL_UINT8(2, out.frac_digits);
    char buf[oa3::kDurationStringSize];
    oa3::format_duration(out, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("PT1.25S", buf);
    TEST_ASSERT_TRUE(oa3::duration_sum(dur_of("PT1S"), dur_of("-PT0.25S"), &out));
    TEST_ASSERT_EQUAL_INT32(0, out.seconds);
    TEST_ASSERT_EQUAL_INT32(750000000, out.nanos);
    TEST_ASSERT_TRUE(oa3::duration_sum(dur_of("-PT1S"), dur_of("PT0.25S"), &out));
    TEST_ASSERT_EQUAL_INT32(0, out.seconds);
    TEST_ASSERT_EQUAL_INT32(-750000000, out.nanos);
    TEST_ASSERT_FALSE(oa3::duration_sum(dur_of("P2147483647D"), dur_of("P1D"), &out));
    TEST_ASSERT_TRUE(oa3::duration_sum(Duration{}, Duration{}, &out));
    TEST_ASSERT_TRUE(oa3::is_zero(out));
}

// Every duration vector above: the compact form survives to_duration() and back, and adds as
// the original does at instants that exercise month ends, leap days and fractions.
static void test_compact_duration_roundtrip() {
    static const char* const kVectors[] = {
        "PT1H", "P1DT2H30M", "P1Y2M3DT4H5M6.789S", "P2W", "PT36H", "P9999Y", "PT0.1234567891S",
        "P2147483647D", "-P1DT2H0.5S", "-P9999Y", "PT1H30M", "PT2H30M", "PT15M", "PT0S",
        "-P1DT2H", "-PT0.5S", "PT1.500S", "PT0.000000001S", "P0D", "P1Y0M", "P1DT0H", "PT0.1S",
        "P10000Y", "P9998Y", "PT0.75S", "PT0.25S", "-PT0.25S", "P1M", "PT1S", "P1Y", "P4Y",
        "P1M1D", "P1MT24H", "P1Y1M", "PT25H", "-PT1H", "-P1M", "-P1D", "P40000Y", "-P40000Y",
    };
    static const char* const kStarts[] = {
        "2024-01-31T20:00:00Z", "2024-02-29T00:00:00Z", "2023-12-31T23:59:59.250Z",
        "2026-11-01T07:00:00Z", "2024-06-15T10:00:00.750Z",
    };
    char want[oa3::kDurationStringSize];
    char got[oa3::kDurationStringSize];
    for (const char* v : kVectors) {
        const Duration d = dur_of(v);
        oa3::CompactDuration c{};
        TEST_ASSERT_TRUE_MESSAGE(oa3::to_compact(d, &c), v);
        const Duration back = oa3::to_duration(c);
        oa3::CompactDuration again{};
        TEST_ASSERT_TRUE_MESSAGE(oa3::to_compact(back, &again), v);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&c, &again, sizeof(c), v);
        TEST_ASSERT_EQUAL_MESSAGE(oa3::is_zero(d), oa3::is_zero(back), v);
        TEST_ASSERT_EQUAL_MESSAGE(oa3::is_negative(d), oa3::is_negative(back), v);
        TEST_ASSERT_EQUAL_MESSAGE(oa3::is_indefinite(d), oa3::is_indefinite(back), v);
        TEST_ASSERT_TRUE_MESSAGE(oa3::format_duration(back, got, sizeof(got)) > 0, v);
        Duration reparsed{};
        TEST_ASSERT_TRUE_MESSAGE(oa3::parse_duration(got, &reparsed), got);
        oa3::format_duration(reparsed, want, sizeof(want));
        TEST_ASSERT_EQUAL_STRING_MESSAGE(got, want, v);
        for (const char* start : kStarts) {
            DateTime a{};
            DateTime b{};
            const bool ok_a = oa3::add(dt_of(start), d, &a);
            const bool ok_b = oa3::add(dt_of(start), back, &b);
            TEST_ASSERT_EQUAL_MESSAGE(ok_a, ok_b, v);
            if (ok_a) {
                TEST_ASSERT_EQUAL_INT_MESSAGE(0, oa3::compare(a, b), v);
            }
        }
    }
}

static void assert_compact_form(const char* in, const char* out) {
    char buf[oa3::kDurationStringSize];
    TEST_ASSERT_TRUE_MESSAGE(oa3::format_duration(through_compact(dur_of(in)), buf, sizeof(buf)) > 0, in);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(out, buf, in);
}

static void test_compact_duration_forms() {
    assert_compact_form("P14M", "P1Y2M");
    assert_compact_form("P2W", "P14D");
    assert_compact_form("PT90M", "PT1H30M");
    assert_compact_form("PT36H", "PT36H");
    assert_compact_form("PT1.500S", "PT1.500S");
    assert_compact_form("-P1Y14M", "-P2Y2M");
    assert_compact_form("-PT3661.5S", "-PT1H1M1.5S");

    oa3::CompactDuration c{};
    TEST_ASSERT_TRUE(oa3::to_compact(dur_of("PT596523H"), &c));
    TEST_ASSERT_FALSE(oa3::to_compact(dur_of("PT596524H"), &c));
    TEST_ASSERT_FALSE(oa3::to_compact(dur_of("P178956971Y"), &c));
    TEST_ASSERT_FALSE(oa3::to_compact(dur_of("P306783379W"), &c));
    TEST_ASSERT_FALSE(oa3::to_compact(dur_of("PT2147483647H"), &c));
    TEST_ASSERT_FALSE(oa3::to_compact(dur_of("P1D"), nullptr));
}

// pendulum results measured through python-oa3 _advance_datetime (P1M add exit criteria)
static void test_add_calendar_pendulum() {
    assert_add("2024-01-31T20:00:00-08:00", "P1M", "2024-03-01T04:00:00Z");
    assert_add("2023-01-31T00:00:00Z", "P1M", "2023-02-28T00:00:00Z");
    assert_add("2024-02-29T00:00:00Z", "P1Y", "2025-02-28T00:00:00Z");
    assert_add("2024-02-29T00:00:00Z", "P4Y", "2028-02-29T00:00:00Z");
    assert_add("2024-01-31T00:00:00Z", "P1M1D", "2024-03-01T00:00:00Z");
    assert_add("2024-01-31T00:00:00Z", "P1MT24H", "2024-03-01T00:00:00Z");
    assert_add("2024-01-31T00:00:00Z", "P1Y1M", "2025-02-28T00:00:00Z");
    assert_add("2024-03-31T23:30:00+05:30", "P1M", "2024-04-30T18:00:00Z");
    // pendulum added on the -08:00 wall clock (03-29T20:00-08:00, 05-30T20:00-08:00); in UTC the
    // month is added to 03-01 and 05-01.
    assert_add("2024-02-29T20:00:00-08:00", "P1M", "2024-04-01T04:00:00Z");
    assert_add("2024-04-30T20:00:00-08:00", "P1M", "2024-06-01T04:00:00Z");
}

static void test_add_elapsed() {
    assert_add("2024-06-15T23:00:00Z", "PT1H", "2024-06-16T00:00:00Z");
    assert_add("2024-12-31T23:59:59Z", "PT1S", "2025-01-01T00:00:00Z");
    assert_add("2026-11-01T07:00:00Z", "PT25H", "2026-11-02T08:00:00Z");
    assert_add("2024-06-15T00:00:00Z", "P2W", "2024-06-29T00:00:00Z");
    assert_add("2024-06-15T00:00:00Z", "PT36H", "2024-06-16T12:00:00Z");
    assert_add("2024-06-15T10:00:00-07:00", "PT15M", "2024-06-15T17:15:00Z");
    assert_add("2024-06-15 10:00:00", "PT1H", "2024-06-15T11:00:00Z");
}

static void test_add_negative() {
    assert_add("2024-06-16T00:00:00Z", "-PT1H", "2024-06-15T23:00:00Z");
    assert_add("2024-03-31T00:00:00Z", "-P1M", "2024-02-29T00:00:00Z");
    assert_add("2024-01-01T00:00:00Z", "-P1D", "2023-12-31T00:00:00Z");
    assert_add("2024-06-15T10:00:00.250Z", "-PT0.5S", "2024-06-15T09:59:59.750Z");
}

static void test_add_indefinite_and_fraction() {
    DateTime out{};
    TEST_ASSERT_TRUE(oa3::add(dt_of("2024-06-15T10:00:00Z"), dur_of("P9999Y"), &out));
    TEST_ASSERT_EQUAL_INT(12023, oa3::to_civil(out).year);
    TEST_ASSERT_EQUAL_INT64(oa3::to_unix(dt_of("2024-06-15T10:00:00Z")) +
                                (oa3::days_from_civil(12023, 6, 15) - oa3::days_from_civil(2024, 6, 15)) * 86400,
                            oa3::to_unix(out));

    assert_add("2024-06-15T10:00:00.250Z", "PT0.5S", "2024-06-15T10:00:00.750Z");
    assert_add("2024-06-15T10:00:00.750Z", "PT0.5S", "2024-06-15T10:00:01.250Z");
    assert_add("2024-06-15T10:00:00Z", "PT0.5S", "2024-06-15T10:00:00.5Z");

    TEST_ASSERT_FALSE(oa3::add(dt_of("2024-06-15T10:00:00Z"), dur_of("P40000Y"), &out));
    TEST_ASSERT_FALSE(oa3::add(dt_of("2024-06-15T10:00:00Z"), dur_of("-P40000Y"), &out));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_rfc3339);
    RUN_TEST(test_parse_with_offset);
    RUN_TEST(test_parse_space_with_tz);
    RUN_TEST(test_parse_vtn_ri_mqtt_format);
    RUN_TEST(test_parse_empty_and_null);
    RUN_TEST(test_vtn_ri_form_equals_utc);
    RUN_TEST(test_offset_converted_to_utc);
    RUN_TEST(test_parse_date_alone);
    RUN_TEST(test_parse_variants_accepted);
    RUN_TEST(test_parse_fraction_truncated);
    RUN_TEST(test_parse_rejects);
    RUN_TEST(test_parse_never_reads_past_len);
    RUN_TEST(test_wire_roundtrip);
    RUN_TEST(test_wire_roundtrip_fractions);
    RUN_TEST(test_format_limits);
    RUN_TEST(test_to_unix);
    RUN_TEST(test_from_unix);
    RUN_TEST(test_from_unix_out_of_range);
    RUN_TEST(test_offsets_cross_utc_dates);
    RUN_TEST(test_civil_roundtrip_range);
    RUN_TEST(test_days_in_month);
    RUN_TEST(test_compare);
    RUN_TEST(test_parse_duration_reference);
    RUN_TEST(test_parse_duration_fields);
    RUN_TEST(test_parse_duration_negative);
    RUN_TEST(test_parse_duration_rejects);
    RUN_TEST(test_parse_duration_never_reads_past_len);
    RUN_TEST(test_duration_roundtrip_reference);
    RUN_TEST(test_duration_roundtrip_more);
    RUN_TEST(test_format_duration_normalized_forms);
    RUN_TEST(test_format_duration_errors);
    RUN_TEST(test_duration_predicates);
    RUN_TEST(test_duration_sum);
    RUN_TEST(test_compact_duration_roundtrip);
    RUN_TEST(test_compact_duration_forms);
    RUN_TEST(test_add_calendar_pendulum);
    RUN_TEST(test_add_elapsed);
    RUN_TEST(test_add_negative);
    RUN_TEST(test_add_indefinite_and_fraction);
    return UNITY_END();
}
