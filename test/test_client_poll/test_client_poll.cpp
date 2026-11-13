#include <stdio.h>
#include <string.h>

#include <oa3/client/ven_client.h>
#include <unity.h>

#include "../support/client_fakes.h"

using fakes::kBaseUrl;
using fakes::kProgramId;
using fakes::kProgramName;
using oa3::VenStatus;

static fakes::FakeTransport http;
static fakes::FakeClock clk;
static fakes::Recorder rec;
static oa3::StaticVenStorage<> storage;
static oa3::VenClient client(http, clk, storage.storage());
static oa3::StaticVenStorage<1> storage1;
static oa3::VenClient client1(http, clk, storage1.storage());

static char programs_fixture[4096];
static char events_fixture[16384];

#define ASSERT_STATUS(expected, actual) \
    TEST_ASSERT_EQUAL_STRING(oa3::to_string(expected), oa3::to_string(actual))

// Two one-hour events at 20:00Z: one for the fixture program, one for another.
static const char kMixedPrograms[] = R"([
{"id":"e-other","programID":"other","intervalPeriod":{"start":"2026-11-13T20:00:00Z","duration":"PT1H"},
 "intervals":[{"id":0,"payloads":[{"type":"PRICE","values":[9.9]}]}]},
{"id":"e-own","programID":"b54d3d1f-bc87-4e47-bbc1-2b95958283fc","intervalPeriod":{"start":"2026-11-13T20:00:00Z","duration":"PT1H"},
 "intervals":[{"id":0,"payloads":[{"type":"PRICE","values":[0.1]}]}]}
])";

static const char kTargeted[] = R"([
{"id":"e-theirs","programID":"b54d3d1f-bc87-4e47-bbc1-2b95958283fc","targets":["T2"],
 "intervalPeriod":{"start":"2026-11-13T20:00:00Z","duration":"PT1H"},
 "intervals":[{"id":0,"payloads":[{"type":"PRICE","values":[9.9]}]}]},
{"id":"e-open","programID":"b54d3d1f-bc87-4e47-bbc1-2b95958283fc",
 "intervalPeriod":{"start":"2026-11-13T21:00:00Z","duration":"PT1H"},
 "intervals":[{"id":0,"payloads":[{"type":"PRICE","values":[0.2]}]}]},
{"id":"e-mine","programID":"b54d3d1f-bc87-4e47-bbc1-2b95958283fc","targets":["X","T1"],
 "intervalPeriod":{"start":"2026-11-13T20:00:00Z","duration":"PT1H"},
 "intervals":[{"id":0,"payloads":[{"type":"PRICE","values":[0.1]}]}]}
])";

void setUp() {
    http.reset();
    clk = fakes::FakeClock{};
    clk.set("2026-11-13T20:00:00Z");
    clk.ms = 5000;
    rec.reset();
    client.stop();
    client1.stop();
}
void tearDown() {}

static oa3::VenConfig config() {
    oa3::VenConfig c{};
    c.base_url = kBaseUrl;
    c.program_name = kProgramName;
    c.on_status = fakes::Recorder::on_status;
    c.on_price_change = fakes::Recorder::on_price;
    c.ctx = &rec;
    return c;
}

static void resolve(oa3::VenClient& vc, const oa3::VenConfig& c) {
    http.push_ok(programs_fixture, "/programs?");
    TEST_ASSERT_TRUE(vc.start(c));
    vc.loop();
    TEST_ASSERT_NOT_NULL(vc.program());
}

static void test_first_fetch() {
    resolve(client, config());
    TEST_ASSERT_EQUAL_size_t(1, http.requests);
    http.push_ok(events_fixture, "/events?programID=b54d3d1f-bc87-4e47-bbc1-2b95958283fc");
    client.loop();
    TEST_ASSERT_EQUAL_size_t(2, http.requests);
    TEST_ASSERT_EQUAL_size_t(0, http.unexpected);
    TEST_ASSERT_EQUAL_STRING(
        "https://price.grid-coordination.energy/openadr3/3.1.0/events?programID="
        "b54d3d1f-bc87-4e47-bbc1-2b95958283fc&skip=0&limit=50",
        http.last_url());
    TEST_ASSERT_EQUAL_size_t(1, http.header_count);
    TEST_ASSERT_EQUAL_STRING("Accept", http.header_names[0]);
    TEST_ASSERT_EQUAL_STRING("application/json", http.header_values[0]);
    TEST_ASSERT_EQUAL_UINT32(15000, http.timeout_ms);
    ASSERT_STATUS(VenStatus::kOk, client.status().status);
    TEST_ASSERT_TRUE(client.status().has_data);
    TEST_ASSERT_EQUAL_INT(200, client.status().http_status);
    TEST_ASSERT_EQUAL_size_t(2, client.event_count());
    oa3::DateTime t;
    TEST_ASSERT_TRUE(client.last_fetch(&t));
    TEST_ASSERT_EQUAL_INT64(clk.wall.seconds, t.seconds);
    TEST_ASSERT_EQUAL_size_t(1, rec.prices);
    TEST_ASSERT_TRUE(rec.last_price().active);
    TEST_ASSERT_EQUAL_STRING("0.043034", rec.last_price().text);
}

static void test_no_fetch_before_success() {
    oa3::DateTime t;
    resolve(client, config());
    TEST_ASSERT_FALSE(client.last_fetch(&t));
    TEST_ASSERT_FALSE(client.price().active);
    TEST_ASSERT_EQUAL_size_t(0, rec.prices);
}

static void test_poll_period_honored() {
    resolve(client, config());
    http.push_ok(events_fixture);
    client.loop();
    TEST_ASSERT_EQUAL_UINT32(900000, client.status().retry_in_ms);
    clk.advance_ms(899999);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(2, http.requests);
    TEST_ASSERT_EQUAL_UINT32(1, client.ms_until_due());
    clk.advance_ms(1);
    http.push_ok(events_fixture, "/events?");
    client.loop();
    TEST_ASSERT_EQUAL_size_t(3, http.requests);
    TEST_ASSERT_EQUAL_size_t(0, http.unexpected);
}

static void test_one_request_per_loop() {
    http.push_ok(programs_fixture);
    http.push_ok(events_fixture);
    TEST_ASSERT_TRUE(client.start(config()));
    TEST_ASSERT_EQUAL_size_t(0, http.requests);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(1, http.requests);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(2, http.requests);
    client.loop();
    client.loop();
    TEST_ASSERT_EQUAL_size_t(2, http.requests);
}

static void test_request_poll() {
    http.push_ok("[]");
    TEST_ASSERT_TRUE(client.start(config()));
    client.loop();
    ASSERT_STATUS(VenStatus::kProgramNotFound, client.status().status);
    client.request_poll();
    client.loop();
    TEST_ASSERT_EQUAL_size_t(1, http.requests);

    resolve(client, config());
    http.push_ok(events_fixture);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(3, http.requests);
    clk.advance_s(60);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(3, http.requests);
    client.request_poll();
    TEST_ASSERT_EQUAL_UINT32(0, client.ms_until_due());
    http.push_ok(events_fixture);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(4, http.requests);
    TEST_ASSERT_EQUAL_UINT32(900000, client.ms_until_due());
}

static void test_window_passed_to_parse() {
    resolve(client, config());
    http.push_ok(events_fixture);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(2, client.event_count());
    TEST_ASSERT_EQUAL_STRING("66f13cb2-7aff-3a38-a20c-9e8eb508e678", client.events()[0].id);
    TEST_ASSERT_EQUAL_STRING("517ac65c-d418-335a-8078-596d02796a28", client.events()[1].id);
    TEST_ASSERT_EQUAL_UINT16(12, client.events()[1].interval_count);

    oa3::VenConfig c = config();
    TEST_ASSERT_TRUE(oa3::parse_duration("PT1H", &c.horizon));
    resolve(client, c);
    http.push_ok(events_fixture);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(1, client.event_count());
    TEST_ASSERT_EQUAL_STRING("517ac65c-d418-335a-8078-596d02796a28", client.events()[0].id);
    TEST_ASSERT_EQUAL_UINT16(1, client.events()[0].interval_count);
}

static void test_truncated_accepted() {
    resolve(client1, config());
    http.push_ok(events_fixture);
    client1.loop();
    ASSERT_STATUS(VenStatus::kTruncated, client1.status().status);
    ASSERT_STATUS(VenStatus::kTruncated, rec.last_status().status);
    TEST_ASSERT_EQUAL_STRING("TRUNCATED", oa3::to_string(client1.status().parse_result));
    TEST_ASSERT_TRUE(client1.status().has_data);
    TEST_ASSERT_EQUAL_UINT16(0, client1.status().consecutive_failures);
    TEST_ASSERT_EQUAL_size_t(1, client1.event_count());
    TEST_ASSERT_EQUAL_STRING("517ac65c-d418-335a-8078-596d02796a28", client1.events()[0].id);
    TEST_ASSERT_TRUE(client1.price().active);
    TEST_ASSERT_EQUAL_STRING("0.043034", client1.price().text);
    TEST_ASSERT_EQUAL_UINT32(900000, client1.ms_until_due());
}

static void test_filters_other_programs() {
    resolve(client, config());
    http.push_ok(kMixedPrograms);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(1, client.event_count());
    TEST_ASSERT_EQUAL_STRING("e-own", client.events()[0].id);
    TEST_ASSERT_EQUAL_STRING("0.1", client.price().text);
}

static void test_filters_targets() {
    oa3::VenConfig c = config();
    const char* targets[] = {"T1"};
    c.targets = targets;
    c.target_count = 1;
    resolve(client, c);
    http.push_ok(kTargeted, "&targets=T1");
    client.loop();
    TEST_ASSERT_EQUAL_size_t(0, http.unexpected);
    TEST_ASSERT_EQUAL_size_t(2, client.event_count());
    TEST_ASSERT_EQUAL_STRING("e-open", client.events()[0].id);
    TEST_ASSERT_EQUAL_STRING("e-mine", client.events()[1].id);
    TEST_ASSERT_EQUAL_STRING("0.1", client.price().text);

    resolve(client, config());
    http.push_ok(kTargeted);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(3, client.event_count());
}

static void test_failure_keeps_last_good_data() {
    resolve(client, config());
    http.push_ok(events_fixture);
    client.loop();
    const oa3::Event* first = client.events();
    clk.advance_s(900);
    http.push_status(200, "[{\"id\":");
    client.loop();
    ASSERT_STATUS(VenStatus::kParseError, client.status().status);
    TEST_ASSERT_EQUAL_PTR(first, client.events());
    TEST_ASSERT_EQUAL_size_t(2, client.event_count());
    TEST_ASSERT_EQUAL_STRING("66f13cb2-7aff-3a38-a20c-9e8eb508e678", client.events()[0].id);
    TEST_ASSERT_TRUE(client.status().has_data);
    TEST_ASSERT_TRUE(client.price().active);
    oa3::DateTime t;
    TEST_ASSERT_TRUE(client.last_fetch(&t));
    TEST_ASSERT_EQUAL_INT64(clk.wall.seconds - 900, t.seconds);

    clk.advance_s(10);
    http.push_result(oa3::HttpResult::kTimeout);
    client.loop();
    ASSERT_STATUS(VenStatus::kTransportError, client.status().status);
    TEST_ASSERT_EQUAL_size_t(2, client.event_count());

    clk.advance_s(20);
    http.push_ok(kMixedPrograms);
    client.loop();
    TEST_ASSERT_TRUE(first != client.events());
    TEST_ASSERT_EQUAL_size_t(1, client.event_count());
    ASSERT_STATUS(VenStatus::kOk, client.status().status);
}


// Capacity 1: the matching event arrives after events another VEN or program would have
// taken the slot with.
static void test_filters_before_slots() {
    oa3::VenConfig c = config();
    const char* targets[] = {"T1"};
    c.targets = targets;
    c.target_count = 1;
    resolve(client1, c);
    static const char kLate[] = R"([
{"id":"e-t2a","programID":"b54d3d1f-bc87-4e47-bbc1-2b95958283fc","targets":["T2"],
 "intervalPeriod":{"start":"2026-11-13T20:00:00Z","duration":"PT1H"},
 "intervals":[{"id":0,"payloads":[{"type":"PRICE","values":[9.1]}]}]},
{"id":"e-other","programID":"other",
 "intervalPeriod":{"start":"2026-11-13T20:00:00Z","duration":"PT1H"},
 "intervals":[{"id":0,"payloads":[{"type":"PRICE","values":[9.2]}]}]},
{"id":"e-t1","programID":"b54d3d1f-bc87-4e47-bbc1-2b95958283fc","targets":["T1"],
 "intervalPeriod":{"start":"2026-11-13T20:00:00Z","duration":"PT1H"},
 "intervals":[{"id":0,"payloads":[{"type":"PRICE","values":[0.1]}]}]}
])";
    http.push_ok(kLate);
    client1.loop();
    ASSERT_STATUS(VenStatus::kOk, client1.status().status);
    TEST_ASSERT_EQUAL_size_t(1, client1.event_count());
    TEST_ASSERT_EQUAL_STRING("e-t1", client1.events()[0].id);
    TEST_ASSERT_EQUAL_STRING("0.1", client1.price().text);
}

static char page_a[16384];
static char page_b[16384];

// count one-hour events of the fixture program, the first at start_hour hours after
// 2026-11-13T00:00Z, ids <prefix><i>.
static const char* events_page(char* buf, size_t size, const char* prefix, unsigned count,
                               int start_hour) {
    size_t n = static_cast<size_t>(snprintf(buf, size, "["));
    for (unsigned i = 0; i < count; ++i) {
        oa3::DateTime t{};
        TEST_ASSERT_TRUE(oa3::parse_datetime("2026-11-13T00:00:00Z", &t));
        const int hour = start_hour + static_cast<int>(i);
        t.seconds += static_cast<int64_t>(hour) * 3600;
        char when[oa3::kDateTimeStringSize];
        TEST_ASSERT_TRUE(oa3::format_datetime(t, when, sizeof(when)) > 0);
        n += static_cast<size_t>(snprintf(
            buf + n, size - n,
            "%s{\"id\":\"%s%u\",\"programID\":\"%s\",\"intervalPeriod\":{\"start\":\"%s\","
            "\"duration\":\"PT1H\"},\"intervals\":[{\"id\":0,\"payloads\":[{\"type\":\"PRICE\","
            "\"values\":[%d]}]}]}",
            i == 0 ? "" : ",", prefix, i, kProgramId, when, hour));
        TEST_ASSERT_TRUE(n < size);
    }
    snprintf(buf + n, size - n, "]");
    return buf;
}

// Page one: 50 events before the window; page two: the event at 20:00Z.
static void test_events_paged() {
    resolve(client, config());
    http.push_ok(events_page(page_a, sizeof(page_a), "old", 50, 20 - 60), "&skip=0&limit=50");
    http.push_ok(events_page(page_b, sizeof(page_b), "now", 2, 20), "&skip=50&limit=50");
    const size_t statuses = rec.statuses;
    client.loop();
    TEST_ASSERT_EQUAL_size_t(2, http.requests);
    TEST_ASSERT_FALSE(client.status().has_data);
    TEST_ASSERT_EQUAL_size_t(0, client.event_count());
    TEST_ASSERT_EQUAL_UINT32(0, client.ms_until_due());
    TEST_ASSERT_EQUAL_size_t(statuses, rec.statuses);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(3, http.requests);
    TEST_ASSERT_EQUAL_size_t(0, http.unexpected);
    ASSERT_STATUS(VenStatus::kOk, client.status().status);
    TEST_ASSERT_EQUAL_size_t(2, client.event_count());
    TEST_ASSERT_EQUAL_STRING("now0", client.events()[0].id);
    TEST_ASSERT_EQUAL_STRING("20", client.price().text);
    TEST_ASSERT_EQUAL_UINT32(900000, client.ms_until_due());
}

static void test_events_page_failure_restarts() {
    resolve(client, config());
    http.push_ok(events_page(page_a, sizeof(page_a), "old", 50, 20 - 60), "&skip=0&limit=50");
    http.push_result(oa3::HttpResult::kTimeout);
    client.loop();
    client.loop();
    ASSERT_STATUS(VenStatus::kTransportError, client.status().status);
    TEST_ASSERT_FALSE(client.status().has_data);
    clk.advance_s(10);
    http.push_ok(events_page(page_b, sizeof(page_b), "now", 2, 20), "&skip=0&limit=50");
    client.loop();
    TEST_ASSERT_EQUAL_size_t(4, http.requests);
    TEST_ASSERT_EQUAL_size_t(0, http.unexpected);
    ASSERT_STATUS(VenStatus::kOk, client.status().status);
    TEST_ASSERT_EQUAL_size_t(2, client.event_count());
}

// A VTN that ignores limit returns more than it; that is the whole answer.
static void test_events_limit_ignored() {
    resolve(client, config());
    http.push_ok(events_page(page_a, sizeof(page_a), "e", 51, 20 - 49));
    client.loop();
    TEST_ASSERT_EQUAL_size_t(2, http.requests);
    ASSERT_STATUS(VenStatus::kOk, client.status().status);
    TEST_ASSERT_EQUAL_STRING("20", client.price().text);
}

static void test_events_page_cap() {
    resolve(client, config());
    events_page(page_a, sizeof(page_a), "old", 50, 20 - 60);
    http.push_ok(page_a, nullptr, OA3_EVENT_PAGES_MAX);
    for (unsigned i = 0; i < OA3_EVENT_PAGES_MAX; ++i) {
        client.loop();
    }
    TEST_ASSERT_EQUAL_size_t(1 + OA3_EVENT_PAGES_MAX, http.requests);
    ASSERT_STATUS(VenStatus::kTruncated, client.status().status);
    TEST_ASSERT_TRUE(client.status().has_data);
    TEST_ASSERT_EQUAL_UINT32(900000, client.ms_until_due());
}

int main() {
    fakes::load_file(OA3_FIXTURE_DIR "/price_server_programs.json", programs_fixture,
                     sizeof(programs_fixture));
    fakes::load_file(OA3_FIXTURE_DIR "/price_server_events.json", events_fixture,
                     sizeof(events_fixture));
    UNITY_BEGIN();
    RUN_TEST(test_first_fetch);
    RUN_TEST(test_no_fetch_before_success);
    RUN_TEST(test_poll_period_honored);
    RUN_TEST(test_one_request_per_loop);
    RUN_TEST(test_request_poll);
    RUN_TEST(test_window_passed_to_parse);
    RUN_TEST(test_truncated_accepted);
    RUN_TEST(test_filters_other_programs);
    RUN_TEST(test_filters_targets);
    RUN_TEST(test_failure_keeps_last_good_data);
    RUN_TEST(test_filters_before_slots);
    RUN_TEST(test_events_paged);
    RUN_TEST(test_events_page_failure_restarts);
    RUN_TEST(test_events_limit_ignored);
    RUN_TEST(test_events_page_cap);
    return UNITY_END();
}
