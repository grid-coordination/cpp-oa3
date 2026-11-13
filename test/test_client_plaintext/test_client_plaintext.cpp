#include <string.h>

#include <oa3/client/ven_client.h>
#include <unity.h>

#include "../support/client_fakes.h"

using oa3::NotifyState;

static fakes::MqttRig rig;

#define ASSERT_NOTIFY(expected, actual) \
    TEST_ASSERT_EQUAL_STRING(oa3::to_string(expected), oa3::to_string(actual))

static size_t log_count;
static char log_lines[64][OA3_LOG_LINE_MAX + 1];
static oa3::LogLevel log_levels[64];

static void capture_log(void*, oa3::LogLevel level, const char* line) {
    if (log_count < 64) {
        snprintf(log_lines[log_count], sizeof(log_lines[0]), "%s", line);
        log_levels[log_count] = level;
    }
    ++log_count;
}

static size_t count_logged(oa3::LogLevel level, const char* prefix) {
    size_t n = 0;
    for (size_t i = 0; i < log_count && i < 64; ++i) {
        if (log_levels[i] == level && strncmp(log_lines[i], prefix, strlen(prefix)) == 0) {
            ++n;
        }
    }
    return n;
}

static const char kHttpWarning[] = "HTTP without TLS: unencrypted, and not conformant to OpenADR 3";
static const char kMqttWarning[] = "MQTT without TLS: unencrypted, and not conformant to OpenADR 3";

void setUp() {
    rig.reset();
    log_count = 0;
}
void tearDown() {}

static oa3::VenConfig config() {
    oa3::VenConfig c = rig.config();
    c.log = oa3::LogSink{capture_log, nullptr, oa3::LogLevel::kInfo};
    return c;
}

static oa3::VenConfig polling_config(const char* base_url) {
    oa3::VenConfig c = config();
    c.notifications = oa3::NotificationMode::kOff;
    c.base_url = base_url;
    return c;
}

static void loop_until(NotifyState state) {
    for (int i = 0; i < 10 && rig.client.status().notify != state; ++i) {
        rig.client.loop();
    }
    ASSERT_NOTIFY(state, rig.client.status().notify);
}

static void test_default_requires_https_base_url() {
    TEST_ASSERT_FALSE(rig.client.start(polling_config("http://vtn.local/openadr3/3.1.0")));
    TEST_ASSERT_FALSE(rig.client.running());
    TEST_ASSERT_FALSE(rig.client.start(polling_config("HTTP://vtn.local/openadr3/3.1.0")));
    TEST_ASSERT_FALSE(rig.client.start(polling_config("ftp://vtn.local/openadr3/3.1.0")));
    TEST_ASSERT_FALSE(rig.client.start(polling_config("httpss://vtn.local/openadr3/3.1.0")));
    TEST_ASSERT_FALSE(rig.client.start(polling_config("https:vtn.local/openadr3/3.1.0")));
    TEST_ASSERT_FALSE(rig.client.start(polling_config("vtn.local/openadr3/3.1.0")));
    TEST_ASSERT_FALSE(rig.client.start(polling_config("")));
    TEST_ASSERT_TRUE(rig.client.start(polling_config("https://vtn.local/openadr3/3.1.0")));
    TEST_ASSERT_TRUE(rig.client.start(polling_config("HtTpS://vtn.local/openadr3/3.1.0")));
    TEST_ASSERT_EQUAL_size_t(0, count_logged(oa3::LogLevel::kWarn, kHttpWarning));
}

static void test_allow_plaintext_admits_http_only() {
    oa3::VenConfig c = polling_config("ftp://vtn.local/openadr3/3.1.0");
    c.allow_plaintext = true;
    TEST_ASSERT_FALSE(rig.client.start(c));
    c.base_url = "https://vtn.local/openadr3/3.1.0";
    TEST_ASSERT_TRUE(rig.client.start(c));
    TEST_ASSERT_EQUAL_size_t(0, count_logged(oa3::LogLevel::kWarn, kHttpWarning));
    c.base_url = "Http://vtn.local/openadr3/3.1.0";
    TEST_ASSERT_TRUE(rig.client.start(c));
    TEST_ASSERT_EQUAL_size_t(1, count_logged(oa3::LogLevel::kWarn, kHttpWarning));
}

static void test_plain_http_polls_and_warns_once() {
    oa3::VenConfig c = polling_config("http://vtn.local/openadr3/3.1.0");
    c.allow_plaintext = true;
    rig.http.push_ok(fakes::kProgramBody, "/programs/");
    rig.http.push_ok(rig.events, "/events?");
    TEST_ASSERT_TRUE(rig.client.start(c));
    rig.client.loop();
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(2, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
    TEST_ASSERT_EQUAL_STRING("OK", oa3::to_string(rig.client.status().status));
    TEST_ASSERT_EQUAL_INT(0, strncmp(rig.http.url(1), "http://vtn.local/openadr3/3.1.0/events?", 39));

    rig.clk.advance_s(900);
    rig.http.push_ok(rig.events, "/events?");
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(3, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(1, count_logged(oa3::LogLevel::kWarn, kHttpWarning));
}

static void test_default_rejects_plain_mqtt_uri() {
    oa3::VenConfig c = config();
    const char* const plain[] = {"mqtt://b:1883", "MQTT://b:1883", "tcp://b:1883",
                                 "Tcp://b:1883"};
    const char* const unknown[] = {"b:1883", "ws://b:80", "wss://b:443/mqtt", " mqtts://b"};
    for (const char* uri : plain) {
        c.mqtt_uri = uri;
        TEST_ASSERT_FALSE_MESSAGE(rig.client.start(c), uri);
    }
    for (const char* uri : unknown) {
        c.mqtt_uri = uri;
        TEST_ASSERT_FALSE_MESSAGE(rig.client.start(c), uri);
    }
    c.mqtt_uri = "MQTTS://b:8883";
    TEST_ASSERT_TRUE(rig.client.start(c));
    c.mqtt_uri = "SSL://b:8883";
    TEST_ASSERT_TRUE(rig.client.start(c));
    c.allow_plaintext = true;
    for (const char* uri : plain) {
        c.mqtt_uri = uri;
        TEST_ASSERT_TRUE_MESSAGE(rig.client.start(c), uri);
    }
    for (const char* uri : unknown) {
        c.mqtt_uri = uri;
        TEST_ASSERT_FALSE_MESSAGE(rig.client.start(c), uri);
    }
    rig.client.stop();
}

static const char kPlainOnly[] =
    R"({"WEBHOOK":false,"MQTT":{"URIS":["MQTT://plain.example:1883","Tcp://plain2.example"],"authentication":{"method":"ANONYMOUS"}}})";

static void test_plain_only_offer_is_unavailable_and_polling_continues() {
    rig.start(config());
    rig.http.push_ok(kPlainOnly, "/notifiers");
    rig.client.loop();
    ASSERT_NOTIFY(NotifyState::kUnavailable, rig.client.status().notify);
    TEST_ASSERT_EQUAL_size_t(0, rig.mqtt.connects);
    TEST_ASSERT_NULL(rig.client.broker());
    TEST_ASSERT_EQUAL_size_t(
        1, count_logged(oa3::LogLevel::kWarn,
                        "MQTT unavailable: only plaintext URIs, and allow_plaintext is off"));

    rig.clk.advance_s(900);
    rig.http.push_ok(rig.events, "/events?");
    rig.http.push_ok(kPlainOnly, "/notifiers");
    rig.client.loop();
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(5, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
    TEST_ASSERT_EQUAL_STRING("OK", oa3::to_string(rig.client.status().status));
    ASSERT_NOTIFY(NotifyState::kUnavailable, rig.client.status().notify);
    TEST_ASSERT_EQUAL_size_t(0, rig.mqtt.connects);
    TEST_ASSERT_EQUAL_size_t(0, count_logged(oa3::LogLevel::kWarn, kMqttWarning));
}

static void test_unusable_uri_reason_unchanged() {
    rig.start(config());
    rig.http.push_ok(
        R"({"WEBHOOK":false,"MQTT":{"URIS":["mqtts://:8883"],"authentication":{"method":"ANONYMOUS"}}})",
        "/notifiers");
    rig.client.loop();
    ASSERT_NOTIFY(NotifyState::kUnavailable, rig.client.status().notify);
    TEST_ASSERT_EQUAL_size_t(1, count_logged(oa3::LogLevel::kWarn, "MQTT unavailable: no usable URI"));
}

static void test_allow_plaintext_refuses_unknown_scheme_offer() {
    oa3::VenConfig c = config();
    c.allow_plaintext = true;
    rig.start(c);
    rig.http.push_ok(
        R"({"WEBHOOK":false,"MQTT":{"URIS":["wss://b.example:443/mqtt","ws://b.example/mqtt"],"authentication":{"method":"ANONYMOUS"}}})",
        "/notifiers");
    rig.client.loop();
    ASSERT_NOTIFY(NotifyState::kUnavailable, rig.client.status().notify);
    TEST_ASSERT_EQUAL_size_t(0, rig.mqtt.connects);
    TEST_ASSERT_EQUAL_size_t(1, count_logged(oa3::LogLevel::kWarn, "MQTT unavailable: no usable URI"));
}

static void test_allow_plaintext_still_prefers_tls() {
    oa3::VenConfig c = config();
    c.allow_plaintext = true;
    rig.connect(c);  // kNotifiersBody lists mqtt:// before mqtts://
    TEST_ASSERT_TRUE(rig.mqtt.last.tls);
    TEST_ASSERT_EQUAL_UINT16(8883, rig.mqtt.last.port);
    TEST_ASSERT_EQUAL_size_t(0, count_logged(oa3::LogLevel::kWarn, kMqttWarning));
    TEST_ASSERT_EQUAL_size_t(0, count_logged(oa3::LogLevel::kWarn, kHttpWarning));
}

static void test_allow_plaintext_connects_plain_and_warns_once() {
    oa3::VenConfig c = config();
    c.allow_plaintext = true;
    rig.start(c);
    rig.http.push_ok(kPlainOnly, "/notifiers");
    rig.http.push_ok(fakes::kEventTopicsBody, "/topics/programs/");
    rig.http.push_ok(fakes::kProgramTopicsBody, "/topics/programs/");
    rig.http.push_ok(rig.events, "/events?");
    loop_until(NotifyState::kConnected);
    TEST_ASSERT_FALSE(rig.mqtt.last.tls);
    TEST_ASSERT_EQUAL_STRING("plain.example", rig.mqtt.last.host);
    TEST_ASSERT_EQUAL_UINT16(1883, rig.mqtt.last.port);
    TEST_ASSERT_EQUAL_size_t(1, count_logged(oa3::LogLevel::kWarn, kMqttWarning));

    rig.mqtt.drop();
    rig.http.push_ok(kPlainOnly, "/notifiers");
    rig.http.push_ok(fakes::kEventTopicsBody, "/topics/programs/");
    rig.http.push_ok(fakes::kProgramTopicsBody, "/topics/programs/");
    rig.http.push_ok(rig.events, "/events?");
    rig.client.loop();
    loop_until(NotifyState::kConnected);
    TEST_ASSERT_EQUAL_size_t(2, rig.mqtt.connects);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
    TEST_ASSERT_EQUAL_size_t(1, count_logged(oa3::LogLevel::kWarn, kMqttWarning));

    log_count = 0;
    rig.reset();
    rig.connect(c);
    TEST_ASSERT_EQUAL_size_t(0, count_logged(oa3::LogLevel::kWarn, kMqttWarning));
}

static void test_plain_mqtt_uri_override_warns() {
    oa3::VenConfig c = config();
    c.allow_plaintext = true;
    c.mqtt_uri = "tcp://override.example";
    rig.start(c);
    rig.http.push_ok(fakes::kEventTopicsBody, "/topics/programs/");
    rig.http.push_ok(fakes::kProgramTopicsBody, "/topics/programs/");
    rig.http.push_ok(rig.events, "/events?");
    loop_until(NotifyState::kConnected);
    TEST_ASSERT_FALSE(rig.mqtt.last.tls);
    TEST_ASSERT_EQUAL_STRING("override.example", rig.mqtt.last.host);
    TEST_ASSERT_EQUAL_size_t(1, count_logged(oa3::LogLevel::kWarn, kMqttWarning));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_default_requires_https_base_url);
    RUN_TEST(test_allow_plaintext_admits_http_only);
    RUN_TEST(test_plain_http_polls_and_warns_once);
    RUN_TEST(test_default_rejects_plain_mqtt_uri);
    RUN_TEST(test_plain_only_offer_is_unavailable_and_polling_continues);
    RUN_TEST(test_unusable_uri_reason_unchanged);
    RUN_TEST(test_allow_plaintext_refuses_unknown_scheme_offer);
    RUN_TEST(test_allow_plaintext_still_prefers_tls);
    RUN_TEST(test_allow_plaintext_connects_plain_and_warns_once);
    RUN_TEST(test_plain_mqtt_uri_override_warns);
    return UNITY_END();
}
