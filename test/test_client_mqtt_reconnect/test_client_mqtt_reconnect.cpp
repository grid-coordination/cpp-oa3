#include <string.h>

#include <oa3/client/ven_client.h>
#include <unity.h>

#include "../support/client_fakes.h"

using fakes::kEventsAll;
using fakes::kProgramAll;
using oa3::NotifyState;

static fakes::MqttRig rig;

#define ASSERT_NOTIFY(expected, actual) \
    TEST_ASSERT_EQUAL_STRING(oa3::to_string(expected), oa3::to_string(actual))

void setUp() { rig.reset(); }
void tearDown() {}

static void push_discovery(const char* program_topics = fakes::kProgramTopicsBody) {
    rig.http.push_ok(fakes::kNotifiersBody, "/notifiers");
    rig.http.push_ok(fakes::kEventTopicsBody, "/topics/programs/");
    rig.http.push_ok(program_topics, "/topics/programs/");
}

static void loop_until(NotifyState s, int max = 8) {
    for (int i = 0; i < max && rig.client.status().notify != s; ++i) {
        rig.client.loop();
    }
    ASSERT_NOTIFY(s, rig.client.status().notify);
}

static void test_snapshot_after_subscribe() {
    rig.start(rig.config());
    push_discovery();
    static char snapshot[1024];
    char e[512];
    fakes::event_json(e, sizeof(e), "e1", "2026-11-13T20:00:00Z", "0.7");
    snprintf(snapshot, sizeof(snapshot), "[%s]", e);
    rig.http.push_ok(snapshot, "/events?");
    for (int i = 0; i < 4; ++i) {
        rig.client.loop();
    }
    ASSERT_NOTIFY(NotifyState::kConnecting, rig.client.status().notify);
    TEST_ASSERT_EQUAL_size_t(3, rig.mqtt.calls);
    TEST_ASSERT_EQUAL_size_t(5, rig.http.requests);
    rig.client.loop();
    ASSERT_NOTIFY(NotifyState::kConnected, rig.client.status().notify);
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
    TEST_ASSERT_NOT_NULL(strstr(rig.http.last_url(), "/events?"));
    TEST_ASSERT_EQUAL_size_t(1, rig.http.gets_while_connected);
    TEST_ASSERT_EQUAL_STRING("0.7", rig.client.price().text);
}

static void test_slow_period_while_live() {
    rig.connect(rig.config());
    TEST_ASSERT_EQUAL_UINT32(3600000, rig.client.status().retry_in_ms);
    TEST_ASSERT_EQUAL_UINT32(100, rig.client.ms_until_due());
    rig.clk.advance_s(3599);
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
    TEST_ASSERT_EQUAL_STRING("OK", oa3::to_string(rig.client.status().status));
    rig.clk.advance_s(1);
    rig.http.push_ok(rig.events, "/events?");
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(7, rig.http.requests);
    TEST_ASSERT_EQUAL_UINT32(3600000, rig.client.status().retry_in_ms);
    ASSERT_NOTIFY(NotifyState::kConnected, rig.client.status().notify);
}

static void test_poll_cap_configurable() {
    oa3::VenConfig c = rig.config();
    c.mqtt_poll_ms = 250;
    rig.connect(c);
    TEST_ASSERT_EQUAL_UINT32(250, rig.client.ms_until_due());
}

// stale_after_polls 3: 3 x 900 s would be stale; 3 x 3600 s is not.
static void test_staleness_uses_period_in_force() {
    rig.connect(rig.config());
    rig.clk.advance_s(2700);
    rig.client.loop();
    TEST_ASSERT_EQUAL_STRING("OK", oa3::to_string(rig.client.status().status));
}

static void test_loss_reverts_and_cycle_refetches_topics() {
    rig.connect(rig.config());
    rig.clk.advance_s(600);
    rig.mqtt.drop();
    push_discovery();
    rig.http.push_ok(rig.events, "/events?");
    rig.client.loop();
    TEST_ASSERT_FALSE(rig.mqtt.connected());
    TEST_ASSERT_EQUAL_STRING("CONNECTION_LOST", oa3::to_string(rig.client.status().mqtt_result));
    TEST_ASSERT_EQUAL_UINT16(1, rig.client.status().mqtt_failures);
    ASSERT_NOTIFY(NotifyState::kDiscovering, rig.client.status().notify);
    TEST_ASSERT_EQUAL_size_t(7, rig.http.requests);
    TEST_ASSERT_NOT_NULL(strstr(rig.http.last_url(), "/notifiers"));
    TEST_ASSERT_EQUAL_UINT32(300000, rig.client.status().retry_in_ms);
    bool disconnected = false;
    for (size_t i = 0; i < rig.rec.statuses; ++i) {
        disconnected = disconnected || rig.rec.status[i].notify == NotifyState::kDisconnected;
    }
    TEST_ASSERT_TRUE(disconnected);

    loop_until(NotifyState::kConnected);
    TEST_ASSERT_EQUAL_size_t(10, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
    TEST_ASSERT_EQUAL_size_t(2, rig.mqtt.connects);
    TEST_ASSERT_EQUAL_UINT16(0, rig.client.status().mqtt_failures);
    char s[256];
    snprintf(s, sizeof(s), "S:%s", kEventsAll);
    TEST_ASSERT_EQUAL_size_t(2, rig.mqtt.count(s));
    TEST_ASSERT_EQUAL_UINT32(3600000, rig.client.status().retry_in_ms);
}

static void test_loss_polls_when_overdue() {
    rig.connect(rig.config());
    rig.clk.advance_s(1200);
    rig.mqtt.drop();
    rig.http.push_ok(rig.events, "/events?");
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(7, rig.http.requests);
    TEST_ASSERT_NOT_NULL(strstr(rig.http.last_url(), "/events?"));
    TEST_ASSERT_EQUAL_UINT32(900000, rig.client.status().retry_in_ms);
    ASSERT_NOTIFY(NotifyState::kDisconnected, rig.client.status().notify);
}

static void test_later_cycles_back_off() {
    rig.connect(rig.config());
    rig.mqtt.drop();
    push_discovery();
    rig.mqtt.push_connect(oa3::MqttResult::kConnectFailed);
    for (int i = 0; i < 4; ++i) {
        rig.client.loop();
    }
    TEST_ASSERT_EQUAL_size_t(2, rig.mqtt.connects);
    TEST_ASSERT_EQUAL_UINT16(2, rig.client.status().mqtt_failures);
    TEST_ASSERT_EQUAL_UINT32(20000, rig.client.ms_until_due());
    rig.clk.advance_ms(19999);
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(9, rig.http.requests);
}

// The broker is unreachable: events are still polled every poll_period_s.
static void test_broker_down_polling_continues() {
    rig.start(rig.config());
    for (int i = 0; i < 8; ++i) {
        rig.mqtt.push_connect(oa3::MqttResult::kConnectFailed);
    }
    push_discovery();
    for (int i = 0; i < 4; ++i) {
        rig.client.loop();
    }
    ASSERT_NOTIFY(NotifyState::kDisconnected, rig.client.status().notify);
    const size_t before = rig.http.requests;
    rig.clk.advance_s(900);
    rig.http.push_ok(rig.events, "/events?");
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(before + 1, rig.http.requests);
    TEST_ASSERT_NOT_NULL(strstr(rig.http.last_url(), "/events?"));
    TEST_ASSERT_EQUAL_STRING("OK", oa3::to_string(rig.client.status().status));
    TEST_ASSERT_EQUAL_UINT32(900000, rig.client.status().retry_in_ms);
}

// With clean_session false the broker keeps the subscriptions: only the difference is sent.
static void test_set_diff_with_session_present() {
    oa3::VenConfig c = rig.config();
    c.exclusive_tls = true;
    rig.start(c);
    push_discovery();
    rig.http.push_ok(rig.events, "/events?");
    loop_until(NotifyState::kConnected);
    TEST_ASSERT_FALSE(rig.mqtt.last.clean_session);
    TEST_ASSERT_EQUAL_size_t(2, rig.client.subscription_count());

    rig.mqtt.drop();
    push_discovery(R"({"topics":{"UPDATE":"p/update","DELETE":"p/delete"}})");
    rig.http.push_ok(rig.events, "/events?");
    const size_t calls = rig.mqtt.calls;
    rig.client.loop();
    loop_until(NotifyState::kConnected);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
    char s[256];
    snprintf(s, sizeof(s), "U:%s", kProgramAll);
    TEST_ASSERT_EQUAL_STRING(s, rig.mqtt.call(calls + 2));
    TEST_ASSERT_EQUAL_STRING("S:p/update", rig.mqtt.call(calls + 3));
    TEST_ASSERT_EQUAL_STRING("S:p/delete", rig.mqtt.call(calls + 4));
    TEST_ASSERT_EQUAL_size_t(calls + 5, rig.mqtt.calls);
    snprintf(s, sizeof(s), "S:%s", kEventsAll);
    TEST_ASSERT_EQUAL_size_t(1, rig.mqtt.count(s));
    TEST_ASSERT_EQUAL_size_t(3, rig.client.subscription_count());
    TEST_ASSERT_EQUAL_STRING(kEventsAll, rig.client.subscription(0));
}

static void test_no_session_subscribes_all() {
    oa3::VenConfig c = rig.config();
    c.exclusive_tls = true;
    rig.start(c);
    push_discovery();
    rig.http.push_ok(rig.events, "/events?");
    loop_until(NotifyState::kConnected);
    rig.mqtt.drop();
    rig.mqtt.keeps_sessions = false;
    push_discovery();
    rig.http.push_ok(rig.events, "/events?");
    rig.client.loop();
    loop_until(NotifyState::kConnected);
    char s[256];
    snprintf(s, sizeof(s), "S:%s", kEventsAll);
    TEST_ASSERT_EQUAL_size_t(2, rig.mqtt.count(s));
}

static char page[16384];
static char update_obj[512];
static char update_msg[1024];

// A notification between two /events pages applies to the fetched set, not the replaced one.
static void test_update_between_pages_kept() {
    rig.connect(rig.config());
    char e1[512];
    fakes::event_json(e1, sizeof(e1), "e1", "2026-11-13T20:00:00Z", "0.1", "2026-11-12T22:00:00Z");
    size_t n = static_cast<size_t>(snprintf(page, sizeof(page), "[%s", e1));
    for (int i = 1; i < OA3_EVENTS_PAGE_LIMIT; ++i) {
        n += static_cast<size_t>(snprintf(page + n, sizeof(page) - n,
                                          ",{\"id\":\"x%d\",\"programID\":\"other\"}", i));
    }
    snprintf(page + n, sizeof(page) - n, "]");
    rig.client.request_poll();
    rig.http.push_ok(page, "skip=0");
    rig.http.push_ok("[]", "skip=50");
    rig.client.loop();
    fakes::event_json(update_obj, sizeof(update_obj), "e1", "2026-11-13T20:00:00Z", "0.5",
                      "2026-11-13T19:00:00Z");
    fakes::envelope(update_msg, sizeof(update_msg), "EVENT", "UPDATE", update_obj);
    rig.mqtt.push_message(fakes::kEventsTopic, update_msg);
    rig.client.loop();
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.pending());
    TEST_ASSERT_EQUAL_size_t(1, rig.client.event_count());
    TEST_ASSERT_EQUAL_STRING("0.5", rig.client.price().text);
    TEST_ASSERT_EQUAL_UINT32(1, rig.client.status().notifications);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_snapshot_after_subscribe);
    RUN_TEST(test_slow_period_while_live);
    RUN_TEST(test_poll_cap_configurable);
    RUN_TEST(test_staleness_uses_period_in_force);
    RUN_TEST(test_loss_reverts_and_cycle_refetches_topics);
    RUN_TEST(test_loss_polls_when_overdue);
    RUN_TEST(test_later_cycles_back_off);
    RUN_TEST(test_broker_down_polling_continues);
    RUN_TEST(test_set_diff_with_session_present);
    RUN_TEST(test_no_session_subscribes_all);
    RUN_TEST(test_update_between_pages_kept);
    return UNITY_END();
}
