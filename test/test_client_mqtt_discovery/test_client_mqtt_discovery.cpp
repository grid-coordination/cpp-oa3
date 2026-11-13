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

static const char kBase[] = "https://price.grid-coordination.energy/openadr3/3.1.0";

void setUp() { rig.reset(); }
void tearDown() {}

static void test_request_order_and_urls() {
    rig.connect(rig.config());
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
    char url[256];
    snprintf(url, sizeof(url), "%s/programs/%s", kBase, fakes::kProgramId);
    TEST_ASSERT_EQUAL_STRING(url, rig.http.url(0));
    TEST_ASSERT_NOT_NULL(strstr(rig.http.url(1), "/events?programID="));
    snprintf(url, sizeof(url), "%s/notifiers", kBase);
    TEST_ASSERT_EQUAL_STRING(url, rig.http.url(2));
    snprintf(url, sizeof(url), "%s/notifiers/mqtt/topics/programs/%s/events", kBase,
             fakes::kProgramId);
    TEST_ASSERT_EQUAL_STRING(url, rig.http.url(3));
    snprintf(url, sizeof(url), "%s/notifiers/mqtt/topics/programs/%s", kBase, fakes::kProgramId);
    TEST_ASSERT_EQUAL_STRING(url, rig.http.url(4));
    TEST_ASSERT_NOT_NULL(strstr(rig.http.url(5), "/events?programID="));

    TEST_ASSERT_EQUAL_size_t(3, rig.mqtt.calls);
    TEST_ASSERT_EQUAL_STRING("C", rig.mqtt.call(0));
    char s[256];
    snprintf(s, sizeof(s), "S:%s", kEventsAll);
    TEST_ASSERT_EQUAL_STRING(s, rig.mqtt.call(1));
    snprintf(s, sizeof(s), "S:%s", kProgramAll);
    TEST_ASSERT_EQUAL_STRING(s, rig.mqtt.call(2));
    TEST_ASSERT_EQUAL_UINT8(1, rig.mqtt.last_qos);

    const oa3::MqttConnectOptions& o = rig.mqtt.last;
    TEST_ASSERT_EQUAL_STRING("mqtt.grid-coordination.energy", o.host);
    TEST_ASSERT_EQUAL_UINT16(8883, o.port);
    TEST_ASSERT_TRUE(o.tls);
    TEST_ASSERT_EQUAL_STRING("ven-1", o.client_id);
    TEST_ASSERT_NULL(o.username);
    TEST_ASSERT_NULL(o.password);
    TEST_ASSERT_TRUE(o.verify);
    TEST_ASSERT_NULL(o.ca_pem);
    TEST_ASSERT_EQUAL_UINT16(60, o.keepalive_s);
    TEST_ASSERT_TRUE(o.clean_session);
    TEST_ASSERT_EQUAL_UINT32(15000, o.timeout_ms);

    const oa3::BrokerAddress* b = rig.client.broker();
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_TRUE(b->tls);
    TEST_ASSERT_EQUAL_size_t(2, rig.client.subscription_count());
    TEST_ASSERT_EQUAL_STRING(kEventsAll, rig.client.subscription(0));
    TEST_ASSERT_EQUAL_STRING(kProgramAll, rig.client.subscription(1));
    TEST_ASSERT_EQUAL_UINT16(0, rig.client.status().mqtt_failures);
    TEST_ASSERT_EQUAL_STRING("OK", oa3::to_string(rig.client.status().status));
}

static void test_notify_states_reported() {
    rig.connect(rig.config());
    bool discovering = false;
    bool connecting = false;
    for (size_t i = 0; i < rig.rec.statuses; ++i) {
        discovering = discovering || rig.rec.status[i].notify == NotifyState::kDiscovering;
        connecting = connecting || rig.rec.status[i].notify == NotifyState::kConnecting;
    }
    TEST_ASSERT_TRUE(discovering);
    TEST_ASSERT_TRUE(connecting);
    ASSERT_NOTIFY(NotifyState::kConnected, rig.rec.last_status().notify);
}

static void test_individual_topics_without_all() {
    rig.start(rig.config());
    rig.http.push_ok(fakes::kNotifiersBody);
    rig.http.push_ok(
        R"({"topics":{"CREATE":"p/events/create","UPDATE":"p/events/update","DELETE":"p/events/delete"}})");
    rig.http.push_ok(R"({"topics":{"UPDATE":"p/update","DELETE":"p/update"}})");
    rig.http.push_ok(rig.events);
    for (int i = 0; i < 6; ++i) {
        rig.client.loop();
    }
    ASSERT_NOTIFY(NotifyState::kConnected, rig.client.status().notify);
    TEST_ASSERT_EQUAL_size_t(4, rig.client.subscription_count());
    TEST_ASSERT_EQUAL_STRING("p/events/create", rig.client.subscription(0));
    TEST_ASSERT_EQUAL_STRING("p/events/update", rig.client.subscription(1));
    TEST_ASSERT_EQUAL_STRING("p/events/delete", rig.client.subscription(2));
    TEST_ASSERT_EQUAL_STRING("p/update", rig.client.subscription(3));
}

static void test_follow_program_off() {
    oa3::VenConfig c = rig.config();
    c.follow_program = false;
    rig.start(c);
    rig.http.push_ok(fakes::kNotifiersBody);
    rig.http.push_ok(fakes::kEventTopicsBody);
    rig.http.push_ok(rig.events);
    for (int i = 0; i < 6; ++i) {
        rig.client.loop();
    }
    ASSERT_NOTIFY(NotifyState::kConnected, rig.client.status().notify);
    TEST_ASSERT_EQUAL_size_t(5, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
    TEST_ASSERT_EQUAL_size_t(1, rig.client.subscription_count());
    TEST_ASSERT_EQUAL_STRING(kEventsAll, rig.client.subscription(0));
}

// Discovery stops at /notifiers, and is retried after poll_period_s.
static void assert_unavailable(const char* notifiers_body) {
    rig.start(rig.config());
    rig.http.push_ok(notifiers_body);
    rig.client.loop();
    ASSERT_NOTIFY(NotifyState::kUnavailable, rig.client.status().notify);
    ASSERT_NOTIFY(NotifyState::kUnavailable, rig.rec.last_status().notify);
    TEST_ASSERT_EQUAL_size_t(3, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(0, rig.mqtt.connects);
    TEST_ASSERT_NULL(rig.client.broker());
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(3, rig.http.requests);
    TEST_ASSERT_EQUAL_STRING("OK", oa3::to_string(rig.client.status().status));

    rig.clk.advance_s(900);
    rig.http.push_ok(rig.events, "/events?");
    rig.http.push_ok(notifiers_body, "/notifiers");
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(4, rig.http.requests);
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(5, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
    ASSERT_NOTIFY(NotifyState::kUnavailable, rig.client.status().notify);
}

static void test_unavailable_without_mqtt() { assert_unavailable(R"({"WEBHOOK":false})"); }

static void test_unavailable_without_uris() {
    assert_unavailable(R"({"WEBHOOK":false,"MQTT":{"URIS":[],"authentication":{"method":"ANONYMOUS"}}})");
}

static void test_unavailable_with_certificate() {
    assert_unavailable(
        R"({"WEBHOOK":true,"MQTT":{"URIS":["mqtts://b:8883"],"authentication":{"method":"CERTIFICATE","caCert":"x","clientCert":"y","clientKey":"z"}}})");
}

static void test_unavailable_with_oauth() {
    assert_unavailable(
        R"({"WEBHOOK":true,"MQTT":{"URIS":["mqtts://b:8883"],"authentication":{"method":"OAUTH2_BEARER_TOKEN","username":"{clientID}"}}})");
}

static const char kPlainOnly[] =
    R"({"WEBHOOK":false,"MQTT":{"URIS":["mqtt://plain.example:1883"],"authentication":{"method":"ANONYMOUS"}}})";

static void test_plain_only_without_allow_plain() { assert_unavailable(kPlainOnly); }

static void test_plain_only_with_allow_plain() {
    oa3::VenConfig c = rig.config();
    c.mqtt_allow_plain = true;
    rig.start(c);
    rig.http.push_ok(kPlainOnly);
    rig.http.push_ok(fakes::kEventTopicsBody);
    rig.http.push_ok(fakes::kProgramTopicsBody);
    rig.http.push_ok(rig.events);
    for (int i = 0; i < 6; ++i) {
        rig.client.loop();
    }
    ASSERT_NOTIFY(NotifyState::kConnected, rig.client.status().notify);
    TEST_ASSERT_EQUAL_STRING("plain.example", rig.mqtt.last.host);
    TEST_ASSERT_EQUAL_UINT16(1883, rig.mqtt.last.port);
    TEST_ASSERT_FALSE(rig.mqtt.last.tls);
}

static void test_mqtt_uri_override_skips_notifiers() {
    oa3::VenConfig c = rig.config();
    char uri[] = "mqtts://override.example:8884";
    c.mqtt_uri = uri;
    rig.start(c);
    uri[0] = 'x';
    rig.http.push_ok(fakes::kEventTopicsBody, "/topics/programs/");
    rig.http.push_ok(fakes::kProgramTopicsBody, "/topics/programs/");
    rig.http.push_ok(rig.events, "/events?");
    for (int i = 0; i < 6; ++i) {
        rig.client.loop();
    }
    ASSERT_NOTIFY(NotifyState::kConnected, rig.client.status().notify);
    TEST_ASSERT_EQUAL_size_t(5, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
    TEST_ASSERT_EQUAL_STRING("override.example", rig.mqtt.last.host);
    TEST_ASSERT_EQUAL_UINT16(8884, rig.mqtt.last.port);
    TEST_ASSERT_TRUE(rig.mqtt.last.tls);
}

// backoff_min_s 10: 10 s, 20 s, 40 s; VenStatus is untouched, the HTTP result is reported.
static void test_backoff_schedule() {
    rig.start(rig.config());
    rig.http.push_status(500);
    rig.client.loop();
    ASSERT_NOTIFY(NotifyState::kDisconnected, rig.client.status().notify);
    TEST_ASSERT_EQUAL_UINT16(1, rig.client.status().mqtt_failures);
    TEST_ASSERT_EQUAL_INT(500, rig.client.status().http_status);
    TEST_ASSERT_EQUAL_STRING("OK", oa3::to_string(rig.client.status().status));
    TEST_ASSERT_EQUAL_UINT32(10000, rig.client.ms_until_due());

    rig.clk.advance_ms(9999);
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(3, rig.http.requests);
    rig.clk.advance_ms(1);
    rig.http.push_result(oa3::HttpResult::kConnectFailed);
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(4, rig.http.requests);
    TEST_ASSERT_EQUAL_UINT16(2, rig.client.status().mqtt_failures);
    TEST_ASSERT_EQUAL_UINT32(20000, rig.client.ms_until_due());
    TEST_ASSERT_EQUAL_STRING("OK", oa3::to_string(rig.client.status().status));

    rig.clk.advance_ms(20000);
    rig.http.push_status(503);
    rig.client.loop();
    TEST_ASSERT_EQUAL_UINT16(3, rig.client.status().mqtt_failures);
    TEST_ASSERT_EQUAL_UINT32(40000, rig.client.ms_until_due());

    rig.clk.advance_ms(40000);
    rig.http.push_ok(fakes::kNotifiersBody);
    rig.http.push_result(oa3::HttpResult::kTimeout);
    rig.client.loop();
    rig.client.loop();
    TEST_ASSERT_EQUAL_UINT16(4, rig.client.status().mqtt_failures);
    TEST_ASSERT_EQUAL_UINT32(80000, rig.client.ms_until_due());

    rig.clk.advance_ms(80000);
    rig.http.push_ok(fakes::kNotifiersBody);
    rig.http.push_ok(fakes::kEventTopicsBody);
    rig.http.push_ok(fakes::kProgramTopicsBody);
    rig.http.push_ok(rig.events);
    for (int i = 0; i < 6; ++i) {
        rig.client.loop();
    }
    ASSERT_NOTIFY(NotifyState::kConnected, rig.client.status().notify);
    TEST_ASSERT_EQUAL_UINT16(0, rig.client.status().mqtt_failures);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
}

// A VTN without the route, or with a body the client cannot read, has no binding to retry on
// the backoff schedule.
static void assert_unavailable_on(int status, const char* body) {
    rig.reset();
    rig.start(rig.config());
    rig.http.push_status(status, body);
    rig.client.loop();
    ASSERT_NOTIFY(NotifyState::kUnavailable, rig.client.status().notify);
    TEST_ASSERT_EQUAL_UINT16(0, rig.client.status().mqtt_failures);
    TEST_ASSERT_EQUAL_UINT32(900000, rig.client.ms_until_due());
    TEST_ASSERT_EQUAL_STRING("OK", oa3::to_string(rig.client.status().status));
}

static void test_unavailable_on_missing_route() {
    assert_unavailable_on(404, "");
    assert_unavailable_on(405, "");
    assert_unavailable_on(501, "");
    assert_unavailable_on(200, "{\"WEBHOOK\":");
}

static void test_unavailable_on_missing_topics_route() {
    rig.start(rig.config());
    rig.http.push_ok(fakes::kNotifiersBody);
    rig.http.push_status(404);
    rig.client.loop();
    rig.client.loop();
    ASSERT_NOTIFY(NotifyState::kUnavailable, rig.client.status().notify);
    TEST_ASSERT_EQUAL_UINT16(0, rig.client.status().mqtt_failures);
    TEST_ASSERT_EQUAL_UINT32(900000, rig.client.ms_until_due());
}

static void test_connect_failure_backs_off() {
    rig.start(rig.config());
    rig.mqtt.push_connect(oa3::MqttResult::kTlsVerifyFailed);
    rig.http.push_ok(fakes::kNotifiersBody);
    rig.http.push_ok(fakes::kEventTopicsBody);
    rig.http.push_ok(fakes::kProgramTopicsBody);
    for (int i = 0; i < 4; ++i) {
        rig.client.loop();
    }
    TEST_ASSERT_EQUAL_size_t(1, rig.mqtt.connects);
    ASSERT_NOTIFY(NotifyState::kDisconnected, rig.client.status().notify);
    TEST_ASSERT_EQUAL_STRING("TLS_VERIFY_FAILED", oa3::to_string(rig.client.status().mqtt_result));
    TEST_ASSERT_EQUAL_UINT16(1, rig.client.status().mqtt_failures);
    TEST_ASSERT_EQUAL_UINT32(10000, rig.client.ms_until_due());
}

static void test_subscribe_failure_disconnects() {
    rig.start(rig.config());
    rig.mqtt.push_subscribe(oa3::MqttResult::kOk);
    rig.mqtt.push_subscribe(oa3::MqttResult::kSubscribeFailed);
    rig.http.push_ok(fakes::kNotifiersBody);
    rig.http.push_ok(fakes::kEventTopicsBody);
    rig.http.push_ok(fakes::kProgramTopicsBody);
    for (int i = 0; i < 4; ++i) {
        rig.client.loop();
    }
    TEST_ASSERT_FALSE(rig.mqtt.connected());
    ASSERT_NOTIFY(NotifyState::kDisconnected, rig.client.status().notify);
    TEST_ASSERT_EQUAL_STRING("SUBSCRIBE_FAILED", oa3::to_string(rig.client.status().mqtt_result));
}

static void test_off_mode_unchanged() {
    oa3::VenConfig c = rig.config();
    c.notifications = oa3::NotificationMode::kOff;
    rig.start(c);
    for (int i = 0; i < 4; ++i) {
        rig.client.loop();
    }
    TEST_ASSERT_EQUAL_size_t(2, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(0, rig.mqtt.calls);
    ASSERT_NOTIFY(NotifyState::kOff, rig.client.status().notify);
    TEST_ASSERT_EQUAL_UINT32(900000, rig.client.status().retry_in_ms);
}

static void test_stop_disconnects() {
    rig.connect(rig.config());
    rig.client.stop();
    TEST_ASSERT_FALSE(rig.mqtt.connected());
    TEST_ASSERT_EQUAL_size_t(0, rig.client.subscription_count());
    ASSERT_NOTIFY(NotifyState::kOff, rig.client.status().notify);
}

static void test_strings() {
    TEST_ASSERT_EQUAL_STRING("CONNECTED", oa3::to_string(NotifyState::kConnected));
    TEST_ASSERT_EQUAL_STRING("SUSPENDED", oa3::to_string(NotifyState::kSuspended));
    TEST_ASSERT_EQUAL_STRING("CONNECTION_LOST", oa3::to_string(oa3::MqttResult::kConnectionLost));
    TEST_ASSERT_EQUAL_STRING("NOT_AUTHORIZED", oa3::to_string(oa3::MqttResult::kNotAuthorized));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_request_order_and_urls);
    RUN_TEST(test_notify_states_reported);
    RUN_TEST(test_individual_topics_without_all);
    RUN_TEST(test_follow_program_off);
    RUN_TEST(test_unavailable_without_mqtt);
    RUN_TEST(test_unavailable_without_uris);
    RUN_TEST(test_unavailable_with_certificate);
    RUN_TEST(test_unavailable_with_oauth);
    RUN_TEST(test_plain_only_without_allow_plain);
    RUN_TEST(test_plain_only_with_allow_plain);
    RUN_TEST(test_mqtt_uri_override_skips_notifiers);
    RUN_TEST(test_backoff_schedule);
    RUN_TEST(test_unavailable_on_missing_route);
    RUN_TEST(test_unavailable_on_missing_topics_route);
    RUN_TEST(test_connect_failure_backs_off);
    RUN_TEST(test_subscribe_failure_disconnects);
    RUN_TEST(test_off_mode_unchanged);
    RUN_TEST(test_stop_disconnects);
    RUN_TEST(test_strings);
    return UNITY_END();
}
