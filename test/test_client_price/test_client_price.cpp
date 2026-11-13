#include <string.h>

#include <oa3/client/ven_client.h>
#include <unity.h>

#include "../support/client_fakes.h"

using fakes::kBaseUrl;
using fakes::kProgramName;
using oa3::VenStatus;

static fakes::FakeTransport http;
static fakes::FakeClock clk;
static fakes::Recorder rec;
static oa3::StaticVenStorage<> storage;
static oa3::VenClient client(http, clk, storage.storage());

static char programs_fixture[4096];
static char events_fixture[16384];

static const char kEventDescriptor[] = R"([
{"id":"e1","programID":"b54d3d1f-bc87-4e47-bbc1-2b95958283fc",
 "payloadDescriptors":[{"payloadType":"PRICE","units":"MWH","currency":"EUR"}],
 "intervalPeriod":{"start":"2026-11-13T20:00:00Z","duration":"PT1H"},
 "intervals":[{"id":5,"payloads":[{"type":"PRICE","values":[42]}]}]}
])";

void setUp() {
    http.reset();
    clk = fakes::FakeClock{};
    clk.set("2026-11-13T20:00:00Z");
    clk.ms = 1000;
    rec.reset();
    client.stop();
}
void tearDown() {}

static oa3::VenConfig config() {
    oa3::VenConfig c{};
    c.base_url = kBaseUrl;
    c.program_name = kProgramName;
    c.poll_period_s = 7200;
    c.on_status = fakes::Recorder::on_status;
    c.on_price_change = fakes::Recorder::on_price;
    c.ctx = &rec;
    return c;
}

static void start_and_fetch(const char* events) {
    http.push_ok(programs_fixture);
    http.push_ok(events);
    TEST_ASSERT_TRUE(client.start(config()));
    client.loop();
    TEST_ASSERT_EQUAL_size_t(0, rec.prices);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(2, http.requests);
}

static void assert_time(const char* iso, const oa3::DateTime& t) {
    oa3::DateTime expected;
    TEST_ASSERT_TRUE(oa3::parse_datetime(iso, &expected));
    TEST_ASSERT_EQUAL_INT64(expected.seconds, t.seconds);
}

static void test_fires_once_on_first_evaluation() {
    start_and_fetch(events_fixture);
    TEST_ASSERT_EQUAL_size_t(1, rec.prices);
    const oa3::PriceInfo& p = rec.last_price();
    TEST_ASSERT_TRUE(p.active);
    TEST_ASSERT_TRUE(p.kind == oa3::ValueKind::kNumber);
    TEST_ASSERT_EQUAL_STRING("0.043034", p.text);
    TEST_ASSERT_EQUAL_DOUBLE(0.043034, p.number);
    TEST_ASSERT_EQUAL_STRING("517ac65c-d418-335a-8078-596d02796a28", p.event_id);
    TEST_ASSERT_EQUAL_INT32(12, p.interval_id);
    assert_time("2026-11-13T20:00:00Z", p.span.start);
    assert_time("2026-11-13T21:00:00Z", p.span.end);
    TEST_ASSERT_FALSE(p.span.end_indefinite);
    TEST_ASSERT_TRUE(p.has_next_change);
    assert_time("2026-11-13T21:00:00Z", p.next_change);
    TEST_ASSERT_EQUAL_MEMORY(&p, &client.price(), sizeof(p));
}

static void test_units_from_program_descriptor() {
    start_and_fetch(events_fixture);
    TEST_ASSERT_EQUAL_STRING("KWH", rec.last_price().units);
    TEST_ASSERT_EQUAL_STRING("USD", rec.last_price().currency);
}

static void test_units_from_event_descriptor() {
    start_and_fetch(kEventDescriptor);
    TEST_ASSERT_EQUAL_STRING("MWH", rec.last_price().units);
    TEST_ASSERT_EQUAL_STRING("EUR", rec.last_price().currency);
    TEST_ASSERT_EQUAL_STRING("42", rec.last_price().text);
    TEST_ASSERT_EQUAL_INT32(5, rec.last_price().interval_id);
}

static void test_no_fire_within_the_hour() {
    start_and_fetch(events_fixture);
    client.loop();
    clk.advance_s(1800);
    client.loop();
    clk.advance_s(1799);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(1, rec.prices);
    TEST_ASSERT_EQUAL_size_t(2, http.requests);
}

static void test_fires_at_hour_boundary_without_request() {
    start_and_fetch(events_fixture);
    clk.advance_s(1800);
    client.loop();
    TEST_ASSERT_EQUAL_UINT32(1800000, client.ms_until_due());
    clk.advance_ms(1799500);
    TEST_ASSERT_EQUAL_UINT32(500, client.ms_until_due());
    client.loop();
    TEST_ASSERT_EQUAL_size_t(1, rec.prices);
    clk.advance_ms(500);
    TEST_ASSERT_EQUAL_UINT32(0, client.ms_until_due());
    client.loop();
    TEST_ASSERT_EQUAL_size_t(2, rec.prices);
    TEST_ASSERT_EQUAL_size_t(2, http.requests);
    TEST_ASSERT_EQUAL_STRING("0.045918", rec.last_price().text);
    TEST_ASSERT_EQUAL_INT32(13, rec.last_price().interval_id);
    assert_time("2026-11-13T22:00:00Z", rec.last_price().next_change);
    TEST_ASSERT_EQUAL_UINT32(3600000, client.ms_until_due());
}

static void test_no_fire_after_refetch_with_same_value() {
    start_and_fetch(events_fixture);
    clk.advance_s(600);
    client.request_poll();
    http.push_ok(events_fixture);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(3, http.requests);
    TEST_ASSERT_EQUAL_size_t(1, rec.prices);
    TEST_ASSERT_EQUAL_STRING("0.043034", client.price().text);
}

// The 24 h window of the fetch at 2026-11-13T20:00Z ends the data at 2026-11-14T20:00Z.
static void test_fires_on_change_to_inactive() {
    oa3::VenConfig c = config();
    c.poll_period_s = 200000;
    c.stale_after_polls = 0;
    http.push_ok(programs_fixture);
    http.push_ok(events_fixture);
    TEST_ASSERT_TRUE(client.start(c));
    client.loop();
    client.loop();
    TEST_ASSERT_EQUAL_size_t(1, rec.prices);
    clk.advance_s(86399);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(2, rec.prices);
    TEST_ASSERT_TRUE(rec.last_price().active);
    clk.advance_s(1);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(3, rec.prices);
    TEST_ASSERT_FALSE(rec.last_price().active);
    TEST_ASSERT_FALSE(client.price().active);
    TEST_ASSERT_EQUAL_size_t(2, http.requests);
    clk.advance_s(3600);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(3, rec.prices);
}

// The fetch at 20:00Z drops the 19:00Z interval; a step back to 19:30Z fetches again.
static void test_clock_stepped_back() {
    start_and_fetch(events_fixture);
    clk.wall.seconds -= 1800;
    http.push_ok(events_fixture);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(3, http.requests);
    TEST_ASSERT_EQUAL_size_t(0, http.unexpected);
    TEST_ASSERT_TRUE(client.price().active);
    TEST_ASSERT_EQUAL_INT32(11, client.price().interval_id);
    TEST_ASSERT_EQUAL_STRING("0.043034", rec.price[0].text);
    TEST_ASSERT_EQUAL_INT32(11, rec.last_price().interval_id);
}

static void test_clock_stepped_forward() {
    start_and_fetch(events_fixture);
    clk.wall.seconds += 2 * 86400;
    http.push_ok("[]");
    client.loop();
    TEST_ASSERT_EQUAL_size_t(3, http.requests);
    TEST_ASSERT_FALSE(client.price().active);
}

static void test_small_clock_step_ignored() {
    start_and_fetch(events_fixture);
    clk.wall.seconds -= 4;
    client.loop();
    clk.wall.seconds += 4;
    client.loop();
    TEST_ASSERT_EQUAL_size_t(2, http.requests);
}

static void test_empty_data_fires_inactive_once() {
    start_and_fetch("[]");
    TEST_ASSERT_EQUAL_size_t(1, rec.prices);
    TEST_ASSERT_FALSE(rec.last_price().active);
    TEST_ASSERT_FALSE(rec.last_price().has_next_change);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(1, rec.prices);
    TEST_ASSERT_EQUAL_UINT32(7200000, client.ms_until_due());
}

int main() {
    fakes::load_file(OA3_FIXTURE_DIR "/price_server_programs.json", programs_fixture,
                     sizeof(programs_fixture));
    fakes::load_file(OA3_FIXTURE_DIR "/price_server_events.json", events_fixture,
                     sizeof(events_fixture));
    UNITY_BEGIN();
    RUN_TEST(test_fires_once_on_first_evaluation);
    RUN_TEST(test_units_from_program_descriptor);
    RUN_TEST(test_units_from_event_descriptor);
    RUN_TEST(test_no_fire_within_the_hour);
    RUN_TEST(test_fires_at_hour_boundary_without_request);
    RUN_TEST(test_no_fire_after_refetch_with_same_value);
    RUN_TEST(test_fires_on_change_to_inactive);
    RUN_TEST(test_clock_stepped_back);
    RUN_TEST(test_clock_stepped_forward);
    RUN_TEST(test_small_clock_step_ignored);
    RUN_TEST(test_empty_data_fires_inactive_once);
    return UNITY_END();
}
