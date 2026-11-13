#include <string.h>

#include <oa3/client/ven_client.h>
#include <unity.h>

#include "../support/client_fakes.h"

using fakes::kBaseUrl;
using fakes::kProgramName;
using oa3::HttpResult;
using oa3::VenStatus;

static fakes::FakeTransport http;
static fakes::FakeClock clk;
static fakes::Recorder rec;
static oa3::StaticVenStorage<> storage;
static oa3::VenClient client(http, clk, storage.storage());

static char programs_fixture[4096];
static char events_fixture[16384];

#define ASSERT_STATUS(expected, actual) \
    TEST_ASSERT_EQUAL_STRING(oa3::to_string(expected), oa3::to_string(actual))

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
    c.backoff_min_s = 10;
    c.backoff_max_s = 80;
    c.stale_after_polls = 0;
    c.on_status = fakes::Recorder::on_status;
    c.on_price_change = fakes::Recorder::on_price;
    c.ctx = &rec;
    return c;
}

// Resolved and polling; the next loop() fetches events.
static void resolve(const oa3::VenConfig& c) {
    http.push_ok(programs_fixture);
    TEST_ASSERT_TRUE(client.start(c));
    client.loop();
    TEST_ASSERT_NOT_NULL(client.program());
    rec.reset();
}

static void test_exponential_and_capped() {
    resolve(config());
    const uint32_t expected[] = {10000, 20000, 40000, 80000, 80000, 80000};
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        http.push_status(500);
        client.loop();
        TEST_ASSERT_EQUAL_size_t(i + 2, http.requests);
        TEST_ASSERT_EQUAL_size_t(i + 1, rec.statuses);
        ASSERT_STATUS(VenStatus::kHttpError, rec.last_status().status);
        TEST_ASSERT_EQUAL_UINT16(i + 1, rec.last_status().consecutive_failures);
        TEST_ASSERT_EQUAL_UINT32(expected[i], rec.last_status().retry_in_ms);
        TEST_ASSERT_EQUAL_UINT32(expected[i], client.ms_until_due());
        clk.advance_ms(expected[i] - 1);
        client.loop();
        TEST_ASSERT_EQUAL_size_t(i + 2, http.requests);
        clk.advance_ms(1);
    }
    TEST_ASSERT_EQUAL_size_t(0, http.unexpected);
}

static void test_large_failure_count_does_not_overflow() {
    oa3::VenConfig c = config();
    c.backoff_min_s = 1;
    c.backoff_max_s = 2000000;
    resolve(c);
    uint32_t last = 0;
    http.push(fakes::Response{nullptr, HttpResult::kOk, 500, "", true, 40});
    for (int i = 0; i < 40; ++i) {
        client.loop();
        last = client.status().retry_in_ms;
        clk.advance_ms(last);
    }
    TEST_ASSERT_EQUAL_UINT32(2000000000u, last);
    TEST_ASSERT_EQUAL_UINT16(40, client.status().consecutive_failures);
}

static void test_reset_on_success() {
    resolve(config());
    http.push_status(500);
    client.loop();
    clk.advance_s(10);
    http.push_status(500);
    client.loop();
    TEST_ASSERT_EQUAL_UINT16(2, client.status().consecutive_failures);
    clk.advance_s(20);
    http.push_ok(events_fixture);
    client.loop();
    ASSERT_STATUS(VenStatus::kOk, client.status().status);
    TEST_ASSERT_EQUAL_UINT16(0, client.status().consecutive_failures);
    TEST_ASSERT_EQUAL_UINT32(900000, client.status().retry_in_ms);
    clk.advance_s(900);
    http.push_status(502);
    client.loop();
    TEST_ASSERT_EQUAL_UINT16(1, client.status().consecutive_failures);
    TEST_ASSERT_EQUAL_UINT32(10000, client.status().retry_in_ms);
}

static void test_transport_results() {
    oa3::VenConfig c = config();
    c.backoff_max_s = 10;
    resolve(c);
    const HttpResult results[] = {HttpResult::kInvalidUrl, HttpResult::kDnsFailed,
                                  HttpResult::kConnectFailed, HttpResult::kTlsFailed,
                                  HttpResult::kTimeout, HttpResult::kTooLarge,
                                  HttpResult::kIoError};
    for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); ++i) {
        http.push_result(results[i]);
        client.loop();
        ASSERT_STATUS(VenStatus::kTransportError, client.status().status);
        TEST_ASSERT_EQUAL_STRING(oa3::to_string(results[i]),
                                 oa3::to_string(client.status().http_result));
        TEST_ASSERT_EQUAL_INT(0, client.status().http_status);
        TEST_ASSERT_EQUAL_size_t(i + 1, rec.statuses);
        clk.advance_s(10);
    }
}

static void test_handler_ran_but_transport_failed() {
    resolve(config());
    http.push(fakes::Response{nullptr, HttpResult::kIoError, 200, events_fixture, true, 1});
    client.loop();
    ASSERT_STATUS(VenStatus::kTransportError, client.status().status);
    TEST_ASSERT_FALSE(client.status().has_data);
    TEST_ASSERT_EQUAL_size_t(0, client.event_count());
}

static void test_http_error() {
    resolve(config());
    http.push_status(500, "{\"title\":\"oops\"}");
    client.loop();
    ASSERT_STATUS(VenStatus::kHttpError, client.status().status);
    TEST_ASSERT_EQUAL_INT(500, client.status().http_status);
    ASSERT_STATUS(VenStatus::kHttpError, rec.last_status().status);
    TEST_ASSERT_EQUAL_STRING("OK", oa3::to_string(client.status().http_result));
}

static void test_parse_error() {
    resolve(config());
    http.push_ok("[{\"id\":\"e\",");
    client.loop();
    ASSERT_STATUS(VenStatus::kParseError, client.status().status);
    TEST_ASSERT_EQUAL_STRING("JSON_ERROR", oa3::to_string(client.status().parse_result));
    clk.advance_s(10);
    http.push_ok("{}");
    client.loop();
    ASSERT_STATUS(VenStatus::kParseError, client.status().status);
    TEST_ASSERT_EQUAL_STRING("JSON_ERROR", oa3::to_string(client.status().parse_result));
    TEST_ASSERT_EQUAL_UINT16(2, client.status().consecutive_failures);
}

static void test_ok_without_handler() {
    resolve(config());
    http.push(fakes::Response{nullptr, HttpResult::kOk, 200, events_fixture, false, 1});
    client.loop();
    ASSERT_STATUS(VenStatus::kTransportError, client.status().status);
    TEST_ASSERT_EQUAL_STRING("IO_ERROR", oa3::to_string(client.status().http_result));
    TEST_ASSERT_EQUAL_size_t(0, client.event_count());
}

static void test_monotonic_wrap() {
    clk.ms = 0xFFFFFFFFu - 4999u;
    oa3::VenConfig c = config();
    c.poll_period_s = 10;
    resolve(c);
    http.push_ok(events_fixture);
    client.loop();
    ASSERT_STATUS(VenStatus::kOk, client.status().status);
    TEST_ASSERT_EQUAL_UINT32(10000, client.ms_until_due());
    clk.advance_ms(9999);
    TEST_ASSERT_TRUE(clk.ms < 10000);
    TEST_ASSERT_EQUAL_UINT32(1, client.ms_until_due());
    client.loop();
    TEST_ASSERT_EQUAL_size_t(2, http.requests);
    clk.advance_ms(1);
    http.push_status(500);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(3, http.requests);
    TEST_ASSERT_EQUAL_UINT32(10000, client.ms_until_due());
}

int main() {
    fakes::load_file(OA3_FIXTURE_DIR "/price_server_programs.json", programs_fixture,
                     sizeof(programs_fixture));
    fakes::load_file(OA3_FIXTURE_DIR "/price_server_events.json", events_fixture,
                     sizeof(events_fixture));
    UNITY_BEGIN();
    RUN_TEST(test_exponential_and_capped);
    RUN_TEST(test_large_failure_count_does_not_overflow);
    RUN_TEST(test_reset_on_success);
    RUN_TEST(test_transport_results);
    RUN_TEST(test_handler_ran_but_transport_failed);
    RUN_TEST(test_http_error);
    RUN_TEST(test_parse_error);
    RUN_TEST(test_ok_without_handler);
    RUN_TEST(test_monotonic_wrap);
    return UNITY_END();
}
