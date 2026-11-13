#include <string.h>

#include <oa3/client/ven_client.h>
#include <unity.h>

#include "../support/client_fakes.h"

using fakes::kEventsAll;
using oa3::NotifyState;

static fakes::MqttRig rig;
static char page[16384];

#define ASSERT_NOTIFY(expected, actual) \
    TEST_ASSERT_EQUAL_STRING(oa3::to_string(expected), oa3::to_string(actual))

void setUp() { rig.reset(); }
void tearDown() {}

static oa3::VenConfig config() {
    oa3::VenConfig c = rig.config();
    c.exclusive_tls = true;
    return c;
}

static void push_discovery() {
    rig.http.push_ok(fakes::kNotifiersBody, "/notifiers");
    rig.http.push_ok(fakes::kEventTopicsBody, "/topics/programs/");
    rig.http.push_ok(fakes::kProgramTopicsBody, "/topics/programs/");
}

static void go_live() {
    rig.start(config());
    push_discovery();
    rig.http.push_ok(rig.events, "/events?");
    for (int i = 0; i < 8 && rig.client.status().notify != NotifyState::kConnected; ++i) {
        rig.client.loop();
    }
    ASSERT_NOTIFY(NotifyState::kConnected, rig.client.status().notify);
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
}

static void test_snapshot_before_connect() {
    rig.start(config());
    push_discovery();
    rig.http.push_ok(rig.events, "/events?");
    for (int i = 0; i < 3; ++i) {
        rig.client.loop();
    }
    ASSERT_NOTIFY(NotifyState::kConnecting, rig.client.status().notify);
    TEST_ASSERT_EQUAL_size_t(0, rig.mqtt.connects);
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
    TEST_ASSERT_NOT_NULL(strstr(rig.http.last_url(), "/events?"));
    TEST_ASSERT_EQUAL_size_t(0, rig.mqtt.connects);
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(1, rig.mqtt.connects);
    TEST_ASSERT_FALSE(rig.mqtt.last.clean_session);
    TEST_ASSERT_TRUE(rig.mqtt.last.tls);
    ASSERT_NOTIFY(NotifyState::kConnected, rig.client.status().notify);
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(2, rig.client.subscription_count());
    TEST_ASSERT_EQUAL_size_t(0, rig.http.gets_while_connected);
    // A new session holds nothing published before it, so the fast period stays until a
    // resume finds the session kept.
    TEST_ASSERT_EQUAL_UINT32(900000, rig.client.status().retry_in_ms);
}

static void test_suspend_and_resume_around_fallback_poll() {
    go_live();
    const size_t calls = rig.mqtt.calls;
    rig.clk.advance_s(3600);
    rig.http.push_ok(rig.events, "/events?");
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(7, rig.http.requests);
    TEST_ASSERT_FALSE(rig.mqtt.connected());
    ASSERT_NOTIFY(NotifyState::kSuspended, rig.client.status().notify);
    TEST_ASSERT_EQUAL_STRING("D", rig.mqtt.call(calls));
    TEST_ASSERT_EQUAL_UINT32(0, rig.client.ms_until_due());

    rig.client.loop();
    TEST_ASSERT_TRUE(rig.mqtt.connected());
    TEST_ASSERT_EQUAL_STRING("C", rig.mqtt.call(calls + 1));
    TEST_ASSERT_EQUAL_size_t(calls + 2, rig.mqtt.calls);
    TEST_ASSERT_FALSE(rig.mqtt.last.clean_session);
    ASSERT_NOTIFY(NotifyState::kConnected, rig.client.status().notify);
    TEST_ASSERT_EQUAL_UINT32(3600000, rig.client.status().retry_in_ms);
    TEST_ASSERT_EQUAL_size_t(7, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.gets_while_connected);
}

static void test_request_poll_suspends() {
    go_live();
    rig.client.request_poll();
    rig.http.push_ok(rig.events, "/events?");
    rig.client.loop();
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(7, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.gets_while_connected);
    ASSERT_NOTIFY(NotifyState::kConnected, rig.client.status().notify);
}

// A full /events page is followed by the next page before MQTT is resumed.
static void test_no_resume_between_pages() {
    go_live();
    size_t n = 0;
    n += static_cast<size_t>(snprintf(page + n, sizeof(page) - n, "["));
    for (int i = 0; i < OA3_EVENTS_PAGE_LIMIT; ++i) {
        n += static_cast<size_t>(snprintf(page + n, sizeof(page) - n,
                                          "%s{\"id\":\"x%d\",\"programID\":\"other\"}",
                                          i == 0 ? "" : ",", i));
    }
    snprintf(page + n, sizeof(page) - n, "]");
    const size_t connects = rig.mqtt.connects;
    rig.clk.advance_s(3600);
    rig.http.push_ok(page, "skip=0");
    rig.http.push_ok(rig.events, "skip=50");
    rig.client.loop();
    ASSERT_NOTIFY(NotifyState::kSuspended, rig.client.status().notify);
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(8, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(connects, rig.mqtt.connects);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(connects + 1, rig.mqtt.connects);
    ASSERT_NOTIFY(NotifyState::kConnected, rig.client.status().notify);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.gets_while_connected);
}

static void test_resume_without_session_resubscribes_without_snapshot() {
    go_live();
    rig.clk.advance_s(3600);
    rig.http.push_ok(rig.events, "/events?");
    rig.client.loop();
    rig.mqtt.keeps_sessions = false;
    const size_t calls = rig.mqtt.calls;
    rig.client.loop();
    rig.client.loop();
    TEST_ASSERT_EQUAL_STRING("C", rig.mqtt.call(calls));
    char s[256];
    snprintf(s, sizeof(s), "S:%s", kEventsAll);
    TEST_ASSERT_EQUAL_STRING(s, rig.mqtt.call(calls + 1));
    TEST_ASSERT_EQUAL_size_t(calls + 3, rig.mqtt.calls);
    ASSERT_NOTIFY(NotifyState::kConnected, rig.client.status().notify);
    TEST_ASSERT_EQUAL_size_t(7, rig.http.requests);
    TEST_ASSERT_EQUAL_UINT32(900000, rig.client.status().retry_in_ms);
}

// The broker sends retained messages on every subscribe, after the /events snapshot here.
static void test_retained_after_snapshot_ignored() {
    go_live();
    char gone[512], e1[512], e1_newer[512];
    char m1[1024], m2[1024], m3[1024];
    fakes::event_json(gone, sizeof(gone), "gone", "2026-11-13T21:00:00Z", "0.7",
                      "2026-11-12T23:00:00Z");
    fakes::event_json(e1, sizeof(e1), "e1", "2026-11-13T20:00:00Z", "0.9");
    fakes::envelope(m1, sizeof(m1), "EVENT", "CREATE", gone);
    fakes::envelope(m2, sizeof(m2), "EVENT", "UPDATE", e1);
    rig.mqtt.push_message(fakes::kEventsTopic, m1, true);
    rig.mqtt.push_message(fakes::kEventsTopic, m2, true);
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(1, rig.client.event_count());
    TEST_ASSERT_EQUAL_STRING("0.1", rig.client.price().text);
    TEST_ASSERT_EQUAL_UINT32(0, rig.client.status().notifications);

    fakes::event_json(e1_newer, sizeof(e1_newer), "e1", "2026-11-13T20:00:00Z", "0.3",
                      "2026-11-13T19:00:00Z");
    fakes::envelope(m3, sizeof(m3), "EVENT", "UPDATE", e1_newer);
    rig.mqtt.push_message(fakes::kEventsTopic, m3, true);
    rig.client.loop();
    TEST_ASSERT_EQUAL_STRING("0.3", rig.client.price().text);
    TEST_ASSERT_EQUAL_UINT32(1, rig.client.status().notifications);
}

// A port that cannot report the retained flag (arduino-mqtt): an event the snapshot lacks and
// that was modified before it is dropped; one modified after it is created.
static void test_unflagged_create_before_snapshot_ignored() {
    go_live();
    char gone[512], fresh[512], m1[1024], m2[1024];
    fakes::event_json(gone, sizeof(gone), "gone", "2026-11-13T21:00:00Z", "0.7",
                      "2026-11-12T23:00:00Z");
    fakes::event_json(fresh, sizeof(fresh), "fresh", "2026-11-13T21:00:00Z", "0.6",
                      "2026-11-13T20:00:01Z");
    fakes::envelope(m1, sizeof(m1), "EVENT", "CREATE", gone);
    fakes::envelope(m2, sizeof(m2), "EVENT", "CREATE", fresh);
    rig.mqtt.push_message(fakes::kEventsTopic, m1);
    rig.mqtt.push_message(fakes::kEventsTopic, m2);
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(2, rig.client.event_count());
    TEST_ASSERT_EQUAL_STRING("fresh", rig.client.events()[1].id);
    TEST_ASSERT_EQUAL_UINT32(1, rig.client.status().notifications);
}

// A port that frees its TLS session only in disconnect() must get that call after a failed
// connect or subscribe, and after a silent drop, before the next HTTPS request.
static void test_tls_freed_before_https_after_failures() {
    rig.start(config());
    push_discovery();
    rig.http.push_ok(rig.events, "/events?");
    rig.mqtt.push_connect(oa3::MqttResult::kRefused);
    for (int i = 0; i < 5; ++i) {
        rig.client.loop();
    }
    ASSERT_NOTIFY(NotifyState::kDisconnected, rig.client.status().notify);
    TEST_ASSERT_FALSE(rig.mqtt.tls_held);

    rig.clk.advance_s(10);
    push_discovery();
    rig.http.push_ok(rig.events, "/events?");
    rig.mqtt.push_subscribe(oa3::MqttResult::kConnectionLost);
    for (int i = 0; i < 5; ++i) {
        rig.client.loop();
    }
    ASSERT_NOTIFY(NotifyState::kDisconnected, rig.client.status().notify);
    TEST_ASSERT_FALSE(rig.mqtt.tls_held);

    rig.clk.advance_s(20);
    push_discovery();
    rig.http.push_ok(rig.events, "/events?");
    for (int i = 0; i < 8 && rig.client.status().notify != NotifyState::kConnected; ++i) {
        rig.client.loop();
    }
    ASSERT_NOTIFY(NotifyState::kConnected, rig.client.status().notify);

    rig.mqtt.connected_ = false;
    rig.clk.advance_s(900);
    rig.http.push_ok(rig.events, "/events?");
    push_discovery();
    rig.http.push_ok(rig.events, "/events?");
    for (int i = 0; i < 8; ++i) {
        rig.client.loop();
    }
    TEST_ASSERT_TRUE(rig.http.requests > 16);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.gets_while_tls_held);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
}

static void test_failed_resume_starts_full_cycle() {
    go_live();
    rig.clk.advance_s(3600);
    rig.http.push_ok(rig.events, "/events?");
    rig.client.loop();
    rig.mqtt.push_connect(oa3::MqttResult::kTimeout);
    rig.client.loop();
    ASSERT_NOTIFY(NotifyState::kDisconnected, rig.client.status().notify);
    TEST_ASSERT_EQUAL_UINT16(1, rig.client.status().mqtt_failures);
    TEST_ASSERT_EQUAL_UINT32(10000, rig.client.ms_until_due());
    TEST_ASSERT_EQUAL_UINT32(900000, rig.client.status().retry_in_ms);

    rig.clk.advance_s(10);
    push_discovery();
    rig.http.push_ok(rig.events, "/events?");
    for (int i = 0; i < 8 && rig.client.status().notify != NotifyState::kConnected; ++i) {
        rig.client.loop();
    }
    ASSERT_NOTIFY(NotifyState::kConnected, rig.client.status().notify);
    TEST_ASSERT_EQUAL_size_t(11, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.gets_while_connected);
}

static void test_no_get_while_connected_over_a_day() {
    go_live();
    for (int h = 0; h < 24; ++h) {
        rig.http.push_ok(rig.events, "/events?");
        for (int s = 0; s < 3600; s += 60) {
            rig.clk.advance_s(60);
            rig.client.loop();
        }
        rig.client.loop();
    }
    TEST_ASSERT_EQUAL_size_t(30, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.gets_while_connected);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.gets_while_tls_held);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
    ASSERT_NOTIFY(NotifyState::kConnected, rig.client.status().notify);
}

// exclusive_tls applies to MQTTS only.
static void test_plain_broker_is_not_suspended() {
    oa3::VenConfig c = config();
    c.mqtt_uri = "mqtt://plain.example";
    c.allow_plaintext = true;
    rig.start(c);
    rig.http.push_ok(fakes::kEventTopicsBody);
    rig.http.push_ok(fakes::kProgramTopicsBody);
    rig.http.push_ok(rig.events);
    for (int i = 0; i < 4; ++i) {
        rig.client.loop();
    }
    ASSERT_NOTIFY(NotifyState::kConnected, rig.client.status().notify);
    TEST_ASSERT_TRUE(rig.mqtt.last.clean_session);
    TEST_ASSERT_EQUAL_size_t(1, rig.http.gets_while_connected);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_snapshot_before_connect);
    RUN_TEST(test_suspend_and_resume_around_fallback_poll);
    RUN_TEST(test_request_poll_suspends);
    RUN_TEST(test_no_resume_between_pages);
    RUN_TEST(test_resume_without_session_resubscribes_without_snapshot);
    RUN_TEST(test_failed_resume_starts_full_cycle);
    RUN_TEST(test_retained_after_snapshot_ignored);
    RUN_TEST(test_unflagged_create_before_snapshot_ignored);
    RUN_TEST(test_tls_freed_before_https_after_failures);
    RUN_TEST(test_no_get_while_connected_over_a_day);
    RUN_TEST(test_plain_broker_is_not_suspended);
    return UNITY_END();
}
