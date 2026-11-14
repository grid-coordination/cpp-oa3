#include <stdint.h>
#include <string.h>

#include <oa3/client/ven_client.h>
#include <unity.h>

#include "../support/client_fakes.h"

using fakes::kBaseUrl;
using fakes::kProgramId;
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
    c.on_status = fakes::Recorder::on_status;
    c.on_price_change = fakes::Recorder::on_price;
    c.ctx = &rec;
    return c;
}

static void start_and_fetch() {
    http.push_ok(programs_fixture);
    http.push_ok(events_fixture);
    TEST_ASSERT_TRUE(client.start(config()));
    client.loop();
    client.loop();
    ASSERT_STATUS(VenStatus::kOk, client.status().status);
}

static void test_waiting_for_clock() {
    clk.valid = false;
    TEST_ASSERT_TRUE(client.start(config()));
    TEST_ASSERT_TRUE(client.running());
    client.loop();
    ASSERT_STATUS(VenStatus::kWaitingForClock, client.status().status);
    TEST_ASSERT_EQUAL_size_t(1, rec.statuses);
    ASSERT_STATUS(VenStatus::kWaitingForClock, rec.last_status().status);
    TEST_ASSERT_FALSE(rec.last_status().has_data);
    client.loop();
    clk.advance_s(3600);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(1, rec.statuses);
    TEST_ASSERT_EQUAL_size_t(0, http.requests);
    TEST_ASSERT_EQUAL_UINT32(1000, client.ms_until_due());

    clk.valid = true;
    TEST_ASSERT_EQUAL_UINT32(0, client.ms_until_due());
    http.push_ok(programs_fixture);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(1, http.requests);
    TEST_ASSERT_EQUAL_size_t(2, rec.statuses);
    ASSERT_STATUS(VenStatus::kResolvingProgram, rec.last_status().status);
}

static void test_clock_lost_while_polling() {
    start_and_fetch();
    const size_t before = rec.statuses;
    clk.valid = false;
    clk.advance_s(900);
    client.loop();
    ASSERT_STATUS(VenStatus::kWaitingForClock, client.status().status);
    TEST_ASSERT_EQUAL_size_t(before + 1, rec.statuses);
    TEST_ASSERT_EQUAL_size_t(2, http.requests);
    TEST_ASSERT_TRUE(client.price().active);
    clk.valid = true;
    http.push_ok(events_fixture);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(3, http.requests);
    ASSERT_STATUS(VenStatus::kOk, client.status().status);
}

// Failures from 900 s fall at 900, 910, 930, 970, 1050, 1210, 1530, 2130 and 2730 s; with
// stale_after_polls 3 the data goes stale at 2700 s, between two requests.
static void test_stale_and_recovery() {
    start_and_fetch();
    rec.reset();
    http.push(fakes::Response{nullptr, HttpResult::kOk, 500, "", true, 9});
    uint32_t elapsed_ms = 0;
    size_t failures = 0;
    while (elapsed_ms < 2700000) {
        uint32_t step = client.ms_until_due();
        if (elapsed_ms + step > 2700000) {
            step = 2700000 - elapsed_ms;
        }
        clk.advance_ms(step);
        elapsed_ms += step;
        const size_t requests = http.requests;
        client.loop();
        if (http.requests != requests) {
            ++failures;
            TEST_ASSERT_EQUAL_size_t(failures, rec.statuses);
            TEST_ASSERT_EQUAL_UINT16(failures, rec.last_status().consecutive_failures);
        }
        if (elapsed_ms < 2700000) {
            ASSERT_STATUS(VenStatus::kHttpError, client.status().status);
        }
    }
    TEST_ASSERT_EQUAL_size_t(8, failures);
    TEST_ASSERT_EQUAL_size_t(2 + 8, http.requests);
    ASSERT_STATUS(VenStatus::kStale, client.status().status);
    TEST_ASSERT_EQUAL_size_t(9, rec.statuses);
    ASSERT_STATUS(VenStatus::kStale, rec.last_status().status);
    TEST_ASSERT_EQUAL_INT(500, rec.last_status().http_status);
    TEST_ASSERT_TRUE(rec.last_status().has_data);
    TEST_ASSERT_TRUE(client.price().active);
    TEST_ASSERT_EQUAL_size_t(2, client.event_count());

    client.loop();
    TEST_ASSERT_EQUAL_size_t(9, rec.statuses);

    clk.advance_ms(client.ms_until_due());
    client.loop();
    ASSERT_STATUS(VenStatus::kStale, client.status().status);
    TEST_ASSERT_EQUAL_size_t(10, rec.statuses);
    TEST_ASSERT_EQUAL_UINT16(9, rec.last_status().consecutive_failures);

    http.push_ok(events_fixture);
    clk.advance_ms(client.ms_until_due());
    client.loop();
    ASSERT_STATUS(VenStatus::kOk, client.status().status);
    TEST_ASSERT_EQUAL_size_t(11, rec.statuses);
    TEST_ASSERT_EQUAL_UINT16(0, rec.last_status().consecutive_failures);
    TEST_ASSERT_EQUAL_size_t(0, http.unexpected);
}

static void test_never_stale_with_zero() {
    http.push_ok(programs_fixture);
    http.push_ok(events_fixture);
    oa3::VenConfig c = config();
    c.stale_after_polls = 0;
    TEST_ASSERT_TRUE(client.start(c));
    client.loop();
    client.loop();
    http.push(fakes::Response{nullptr, HttpResult::kOk, 500, "", true, 20});
    for (int i = 0; i < 20; ++i) {
        clk.advance_ms(client.ms_until_due());
        client.loop();
        ASSERT_STATUS(VenStatus::kHttpError, client.status().status);
    }
}

static void test_fire_on_change_only() {
    start_and_fetch();
    TEST_ASSERT_EQUAL_size_t(2, rec.statuses);
    ASSERT_STATUS(VenStatus::kResolvingProgram, rec.status[0].status);
    ASSERT_STATUS(VenStatus::kOk, rec.status[1].status);
    TEST_ASSERT_TRUE(rec.status[1].has_data);
    TEST_ASSERT_EQUAL_UINT32(900000, rec.status[1].retry_in_ms);
    for (int i = 0; i < 3; ++i) {
        clk.advance_s(900);
        http.push_ok(events_fixture);
        client.loop();
    }
    TEST_ASSERT_EQUAL_size_t(5, http.requests);
    TEST_ASSERT_EQUAL_size_t(2, rec.statuses);
}

static void test_fire_on_every_failure() {
    start_and_fetch();
    rec.reset();
    for (int i = 0; i < 3; ++i) {
        clk.advance_ms(client.ms_until_due());
        http.push_result(HttpResult::kTimeout);
        client.loop();
    }
    TEST_ASSERT_EQUAL_size_t(3, rec.statuses);
    TEST_ASSERT_EQUAL_UINT16(1, rec.status[0].consecutive_failures);
    TEST_ASSERT_EQUAL_UINT16(3, rec.status[2].consecutive_failures);
    ASSERT_STATUS(VenStatus::kTransportError, rec.status[2].status);
}

static void test_start_rejections() {
    static oa3::Event e1[1];
    static oa3::Event e2[1];
    static oa3::StaticJsonArena<1024> small_arena;

    char long_url[OA3_URL_MAX + 2];
    memset(long_url, 'u', sizeof(long_url) - 1);
    long_url[sizeof(long_url) - 1] = '\0';
    char long_name[OA3_NAME_MAX + 2];
    memset(long_name, 'n', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    char long_id[OA3_ID_MAX + 2];
    memset(long_id, 'i', sizeof(long_id) - 1);
    long_id[sizeof(long_id) - 1] = '\0';
    char long_target[OA3_TARGET_MAX + 2];
    memset(long_target, 't', sizeof(long_target) - 1);
    long_target[sizeof(long_target) - 1] = '\0';
    char long_type[OA3_TYPE_MAX + 2];
    memset(long_type, 'p', sizeof(long_type) - 1);
    long_type[sizeof(long_type) - 1] = '\0';
    const char* too_many[OA3_MAX_TARGETS + 1] = {};
    for (size_t i = 0; i <= OA3_MAX_TARGETS; ++i) {
        too_many[i] = "T";
    }
    const char* bad_target[] = {long_target};
    const char* null_target[] = {nullptr};
    // Each target encodes to three times its length, so a near-limit base URL cannot fit them.
    char wide_target[OA3_TARGET_MAX + 1];
    memset(wide_target, ' ', sizeof(wide_target) - 1);
    wide_target[sizeof(wide_target) - 1] = '\0';
    const char* wide[OA3_MAX_TARGETS];
    for (size_t i = 0; i < OA3_MAX_TARGETS; ++i) {
        wide[i] = wide_target;
    }
    char near_limit_url[OA3_URL_MAX - 120];
    memset(near_limit_url, 'u', sizeof(near_limit_url) - 1);
    near_limit_url[sizeof(near_limit_url) - 1] = '\0';
    memcpy(near_limit_url, "https://", 8);

    oa3::VenConfig c;
    TEST_ASSERT_TRUE(client.start(config()));

    c = config(); c.base_url = nullptr;
    TEST_ASSERT_FALSE(client.start(c));
    TEST_ASSERT_FALSE(client.running());
    ASSERT_STATUS(VenStatus::kStopped, client.status().status);
    c = config(); c.base_url = long_url;
    TEST_ASSERT_FALSE(client.start(c));
    c = config(); c.program_name = nullptr;
    TEST_ASSERT_FALSE(client.start(c));
    c = config(); c.program_name = long_name;
    TEST_ASSERT_FALSE(client.start(c));
    c = config(); c.program_id = long_id;
    TEST_ASSERT_FALSE(client.start(c));
    c = config(); c.targets = bad_target; c.target_count = 1;
    TEST_ASSERT_FALSE(client.start(c));
    c = config(); c.targets = null_target; c.target_count = 1;
    TEST_ASSERT_FALSE(client.start(c));
    c = config(); c.targets = nullptr; c.target_count = 1;
    TEST_ASSERT_FALSE(client.start(c));
    c = config(); c.targets = too_many; c.target_count = OA3_MAX_TARGETS + 1;
    TEST_ASSERT_FALSE(client.start(c));
    c = config(); c.payload_type = nullptr;
    TEST_ASSERT_FALSE(client.start(c));
    c = config(); c.payload_type = long_type;
    TEST_ASSERT_FALSE(client.start(c));
    c = config(); c.poll_period_s = 0;
    TEST_ASSERT_FALSE(client.start(c));
    c = config(); c.poll_period_s = 2000001;
    TEST_ASSERT_FALSE(client.start(c));
    c = config(); c.poll_period_s = 2000000;
    TEST_ASSERT_FALSE(client.start(c));
    c = config(); c.poll_period_s = 2000000; c.stale_after_polls = 0;
    TEST_ASSERT_TRUE(client.start(c));
    c = config(); c.poll_period_s = 715827; c.stale_after_polls = 3;
    TEST_ASSERT_TRUE(client.start(c));
    c = config(); c.poll_period_s = 715828; c.stale_after_polls = 3;
    TEST_ASSERT_FALSE(client.start(c));
    c = config(); c.poll_period_s = 86400; c.stale_after_polls = 50;
    TEST_ASSERT_FALSE(client.start(c));
    c = config(); c.program_id = "a b";
    TEST_ASSERT_FALSE(client.start(c));
    c = config(); c.backoff_min_s = 0;
    TEST_ASSERT_FALSE(client.start(c));
    c = config(); c.backoff_min_s = 20; c.backoff_max_s = 10;
    TEST_ASSERT_FALSE(client.start(c));
    c = config(); c.backoff_min_s = 10; c.backoff_max_s = 10;
    TEST_ASSERT_TRUE(client.start(c));
    c = config(); c.backoff_max_s = 2000001;
    TEST_ASSERT_FALSE(client.start(c));
    c = config(); c.horizon = oa3::Duration{};
    TEST_ASSERT_FALSE(client.start(c));
    c = config(); c.horizon.hours = -24;
    TEST_ASSERT_FALSE(client.start(c));
    c = config(); c.base_url = near_limit_url; c.targets = wide; c.target_count = OA3_MAX_TARGETS;
    TEST_ASSERT_FALSE(client.start(c));
    c = config(); c.base_url = near_limit_url;
    TEST_ASSERT_TRUE(client.start(c));

    oa3::VenClient bad_events(http, clk, oa3::VenStorage{nullptr, e2, 1, &small_arena});
    TEST_ASSERT_FALSE(bad_events.start(config()));
    oa3::VenClient single(http, clk, oa3::VenStorage{e1, nullptr, 1, &small_arena});
    TEST_ASSERT_TRUE(single.start(config()));
    oa3::VenClient bad_capacity(http, clk, oa3::VenStorage{e1, e2, 0, &small_arena});
    TEST_ASSERT_FALSE(bad_capacity.start(config()));
    oa3::VenClient bad_arena(http, clk, oa3::VenStorage{e1, e2, 1, nullptr});
    TEST_ASSERT_FALSE(bad_arena.start(config()));
    oa3::VenClient good(http, clk, oa3::VenStorage{e1, e2, 1, &small_arena});
    TEST_ASSERT_TRUE(good.start(config()));

    c = config(); c.program_name = nullptr; c.program_id = kProgramId;
    TEST_ASSERT_TRUE(client.start(c));
    TEST_ASSERT_EQUAL_size_t(0, rec.statuses);
    TEST_ASSERT_EQUAL_size_t(0, http.requests);
}

static fakes::FakeMqttSession mqtt;
static oa3::NotificationStorage notes;
static oa3::VenClient mqtt_client(http, clk, fakes::with_notifications(storage.storage(), &notes),
                                  &mqtt);

static oa3::VenConfig mqtt_config() {
    oa3::VenConfig c = config();
    c.notifications = oa3::NotificationMode::kMqtt;
    c.mqtt_client_id = "ven-1";
    return c;
}

static void test_mqtt_start_rejections() {
    mqtt.reset();
    TEST_ASSERT_TRUE(mqtt_client.start(mqtt_config()));
    TEST_ASSERT_FALSE(client.start(mqtt_config()));

    oa3::StaticVenStorage<1> small;
    oa3::VenClient bare(http, clk, small.storage(), &mqtt);
    TEST_ASSERT_FALSE(bare.start(mqtt_config()));

    oa3::VenConfig c = mqtt_config();
    c.mqtt_client_id = nullptr;
    TEST_ASSERT_FALSE(mqtt_client.start(c));
    c.mqtt_client_id = "";
    TEST_ASSERT_FALSE(mqtt_client.start(c));
    char id[OA3_MQTT_CLIENT_ID_MAX + 2];
    memset(id, 'v', sizeof(id) - 1);
    id[sizeof(id) - 1] = '\0';
    c.mqtt_client_id = id;
    TEST_ASSERT_FALSE(mqtt_client.start(c));
    id[OA3_MQTT_CLIENT_ID_MAX] = '\0';
    TEST_ASSERT_TRUE(mqtt_client.start(c));

    c = mqtt_config();
    c.poll_period_s = 900;
    c.notify_poll_period_s = 899;
    TEST_ASSERT_FALSE(mqtt_client.start(c));
    c.notify_poll_period_s = 900;
    TEST_ASSERT_TRUE(mqtt_client.start(c));
    c.notify_poll_period_s = 2000001;
    TEST_ASSERT_FALSE(mqtt_client.start(c));
    c.notify_poll_period_s = 1000000;
    c.stale_after_polls = 3;
    TEST_ASSERT_FALSE(mqtt_client.start(c));
    c.stale_after_polls = 0;
    TEST_ASSERT_TRUE(mqtt_client.start(c));

    c = mqtt_config();
    c.mqtt_uri = "mqtts://b:8883";
    TEST_ASSERT_TRUE(mqtt_client.start(c));
    c.mqtt_uri = "mqtt://b:1883";
    TEST_ASSERT_FALSE(mqtt_client.start(c));
    c.allow_plaintext = true;
    TEST_ASSERT_TRUE(mqtt_client.start(c));
    c.mqtt_uri = "mqtts://b:99999";
    TEST_ASSERT_FALSE(mqtt_client.start(c));
    c.mqtt_uri = "mqtts://";
    TEST_ASSERT_FALSE(mqtt_client.start(c));
    mqtt_client.stop();
}

static void test_mqtt_start_disconnects() {
    mqtt.reset();
    bool sp = false;
    oa3::MqttConnectOptions o;
    o.host = "b";
    o.client_id = "x";
    mqtt.connect(o, &sp);
    TEST_ASSERT_TRUE(mqtt.connected());
    TEST_ASSERT_TRUE(mqtt_client.start(mqtt_config()));
    TEST_ASSERT_FALSE(mqtt.connected());
    TEST_ASSERT_EQUAL_size_t(0, mqtt_client.subscription_count());
    TEST_ASSERT_EQUAL_STRING("OFF", oa3::to_string(mqtt_client.status().notify));
    mqtt_client.stop();
}

static void test_mqtt_copies_client_id_and_uri() {
    mqtt.reset();
    char id[] = "ven-1";
    char uri[] = "mqtts://override:8884";
    oa3::VenConfig c = mqtt_config();
    c.program_id = kProgramId;
    c.mqtt_client_id = id;
    c.mqtt_uri = uri;
    c.program_name = nullptr;
    TEST_ASSERT_TRUE(mqtt_client.start(c));
    id[0] = 'X';
    uri[8] = 'X';
    http.push_ok(fakes::kProgramBody);
    http.push_ok(events_fixture);
    http.push_ok(fakes::kEventTopicsBody);
    http.push_ok(fakes::kProgramTopicsBody);
    for (int i = 0; i < 5; ++i) {
        mqtt_client.loop();
    }
    TEST_ASSERT_EQUAL_size_t(1, mqtt.connects);
    TEST_ASSERT_EQUAL_STRING("ven-1", mqtt.last_client_id);
    TEST_ASSERT_EQUAL_STRING("override", mqtt.last_host);
    TEST_ASSERT_EQUAL_UINT16(8884, mqtt.last.port);
    mqtt_client.stop();
}

static void test_start_copies_strings() {
    char name[32];
    char base[80];
    strcpy(name, kProgramName);
    strcpy(base, kBaseUrl);
    oa3::VenConfig c = config();
    c.program_name = name;
    c.base_url = base;
    http.push_ok(programs_fixture, "programName=EELEC-013532223");
    TEST_ASSERT_TRUE(client.start(c));
    memset(name, 'x', sizeof(name) - 1);
    memset(base, 'x', sizeof(base) - 1);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(0, http.unexpected);
    TEST_ASSERT_TRUE(strncmp(http.last_url(), kBaseUrl, strlen(kBaseUrl)) == 0);
}

static void test_start_clears_data() {
    start_and_fetch();
    TEST_ASSERT_EQUAL_size_t(2, client.event_count());
    TEST_ASSERT_TRUE(client.start(config()));
    TEST_ASSERT_EQUAL_size_t(0, client.event_count());
    TEST_ASSERT_NULL(client.program());
    TEST_ASSERT_FALSE(client.price().active);
    oa3::DateTime t;
    TEST_ASSERT_FALSE(client.last_fetch(&t));
    TEST_ASSERT_FALSE(client.status().has_data);
}

static void test_stop_silences() {
    start_and_fetch();
    const size_t statuses = rec.statuses;
    const size_t prices = rec.prices;
    client.stop();
    TEST_ASSERT_FALSE(client.running());
    ASSERT_STATUS(VenStatus::kStopped, client.status().status);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, client.ms_until_due());
    clk.advance_s(7200);
    client.request_poll();
    client.loop();
    TEST_ASSERT_EQUAL_size_t(2, http.requests);
    TEST_ASSERT_EQUAL_size_t(statuses, rec.statuses);
    TEST_ASSERT_EQUAL_size_t(prices, rec.prices);
    TEST_ASSERT_EQUAL_size_t(2, client.event_count());
    TEST_ASSERT_TRUE(client.price().active);
    TEST_ASSERT_NOT_NULL(client.program());
}

static oa3::VenClient* stopping_client;
static void stop_on_status(void* ctx, const oa3::VenStatusInfo& s) {
    fakes::Recorder::on_status(ctx, s);
    stopping_client->stop();
}

static void test_stop_from_callback() {
    stopping_client = &client;
    oa3::VenConfig c = config();
    c.on_status = stop_on_status;
    http.push_ok(programs_fixture);
    TEST_ASSERT_TRUE(client.start(c));
    client.loop();
    TEST_ASSERT_EQUAL_size_t(1, rec.statuses);
    ASSERT_STATUS(VenStatus::kResolvingProgram, rec.last_status().status);
    TEST_ASSERT_FALSE(client.running());
    client.loop();
    TEST_ASSERT_EQUAL_size_t(0, http.requests);
}

static size_t requests_at_status[8];
static VenStatus status_seen[8];
static size_t status_calls;
static void record_requests(void*, const oa3::VenStatusInfo& s) {
    if (status_calls < 8) {
        requests_at_status[status_calls] = http.requests;
        status_seen[status_calls] = s.status;
    }
    ++status_calls;
}

static void test_resolving_reported_before_request() {
    status_calls = 0;
    oa3::VenConfig c = config();
    c.on_status = record_requests;
    http.push_ok(programs_fixture);
    http.push_ok(events_fixture);
    TEST_ASSERT_TRUE(client.start(c));
    client.loop();
    TEST_ASSERT_EQUAL_size_t(1, status_calls);
    ASSERT_STATUS(VenStatus::kResolvingProgram, status_seen[0]);
    TEST_ASSERT_EQUAL_size_t(0, requests_at_status[0]);
    client.loop();
    TEST_ASSERT_EQUAL_size_t(2, status_calls);
    ASSERT_STATUS(VenStatus::kOk, status_seen[1]);
    TEST_ASSERT_EQUAL_size_t(2, requests_at_status[1]);
}

static size_t log_count;
static char log_lines[16][OA3_LOG_LINE_MAX + 1];
static oa3::LogLevel log_levels[16];
static void capture_log(void*, oa3::LogLevel level, const char* line) {
    if (log_count < 16) {
        log_levels[log_count] = level;
        strcpy(log_lines[log_count], line);
    }
    ++log_count;
}

static bool logged(oa3::LogLevel level, const char* prefix) {
    for (size_t i = 0; i < log_count && i < 16; ++i) {
        if (log_levels[i] == level && strncmp(log_lines[i], prefix, strlen(prefix)) == 0) {
            return true;
        }
    }
    return false;
}

static void test_logging() {
    log_count = 0;
    oa3::VenConfig c = config();
    c.log = oa3::LogSink{capture_log, nullptr, oa3::LogLevel::kDebug};
    http.push_ok(programs_fixture);
    http.push_status(503);
    TEST_ASSERT_TRUE(client.start(c));
    client.loop();
    client.loop();
    TEST_ASSERT_TRUE(logged(oa3::LogLevel::kDebug, "GET https://price.grid-coordination.energy/"));
    TEST_ASSERT_TRUE(logged(oa3::LogLevel::kInfo, "program EELEC-013532223 resolved: id b54d3d1f"));
    TEST_ASSERT_TRUE(logged(oa3::LogLevel::kInfo, "status RESOLVING_PROGRAM"));
    TEST_ASSERT_TRUE(logged(oa3::LogLevel::kWarn, "HTTP_ERROR: http OK 503"));
    TEST_ASSERT_TRUE(logged(oa3::LogLevel::kInfo, "status HTTP_ERROR"));

    log_count = 0;
    c.log.max_level = oa3::LogLevel::kWarn;
    http.push_ok(programs_fixture);
    TEST_ASSERT_TRUE(client.start(c));
    client.loop();
    TEST_ASSERT_EQUAL_size_t(0, log_count);
}

static void test_to_string() {
    TEST_ASSERT_EQUAL_STRING("STOPPED", oa3::to_string(VenStatus::kStopped));
    TEST_ASSERT_EQUAL_STRING("WAITING_FOR_CLOCK", oa3::to_string(VenStatus::kWaitingForClock));
    TEST_ASSERT_EQUAL_STRING("PARSE_ERROR", oa3::to_string(VenStatus::kParseError));
}

int main() {
    fakes::load_file(OA3_FIXTURE_DIR "/price_server_programs.json", programs_fixture,
                     sizeof(programs_fixture));
    fakes::load_file(OA3_FIXTURE_DIR "/price_server_events.json", events_fixture,
                     sizeof(events_fixture));
    UNITY_BEGIN();
    RUN_TEST(test_waiting_for_clock);
    RUN_TEST(test_clock_lost_while_polling);
    RUN_TEST(test_stale_and_recovery);
    RUN_TEST(test_never_stale_with_zero);
    RUN_TEST(test_fire_on_change_only);
    RUN_TEST(test_fire_on_every_failure);
    RUN_TEST(test_start_rejections);
    RUN_TEST(test_mqtt_start_rejections);
    RUN_TEST(test_mqtt_start_disconnects);
    RUN_TEST(test_mqtt_copies_client_id_and_uri);
    RUN_TEST(test_start_copies_strings);
    RUN_TEST(test_start_clears_data);
    RUN_TEST(test_stop_silences);
    RUN_TEST(test_stop_from_callback);
    RUN_TEST(test_resolving_reported_before_request);
    RUN_TEST(test_logging);
    RUN_TEST(test_to_string);
    return UNITY_END();
}
