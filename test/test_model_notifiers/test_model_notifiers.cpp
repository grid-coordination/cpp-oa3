#include <stdio.h>
#include <string.h>

#include <oa3/model/notifiers.h>
#include <unity.h>

using oa3::BrokerAddress;
using oa3::MqttAuthMethod;
using oa3::Notifiers;
using oa3::OperationTopics;
using oa3::ParseResult;

static oa3::StaticJsonArena<> arena;
static Notifiers n;
static OperationTopics t;

void setUp() {}
void tearDown() {}

static ParseResult notifiers(const char* json) {
    return oa3::parse_notifiers(json, strlen(json), arena, &n);
}

static ParseResult topics(const char* json) {
    return oa3::parse_topics(json, strlen(json), arena, &t);
}

// Live price-server bodies (2026-11-12).
static const char kLiveNotifiers[] =
    R"({"WEBHOOK":false,"MQTT":{"URIS":["mqtt://mqtt.grid-coordination.energy:1883","mqtts://mqtt.grid-coordination.energy:8883"],"serialization":"JSON","authentication":{"method":"ANONYMOUS"}}})";

static const char kLiveEventTopics[] =
    R"({"topics":{"UPDATE":"OpenADR\/3.1.0\/programs\/b54d3d1f-bc87-4e47-bbc1-2b95958283fc\/events\/update","DELETE":"OpenADR\/3.1.0\/programs\/b54d3d1f-bc87-4e47-bbc1-2b95958283fc\/events\/delete","ALL":"OpenADR\/3.1.0\/programs\/b54d3d1f-bc87-4e47-bbc1-2b95958283fc\/events\/+","CREATE":"OpenADR\/3.1.0\/programs\/b54d3d1f-bc87-4e47-bbc1-2b95958283fc\/events\/create"}})";

static const char kLiveProgramTopics[] =
    R"({"topics":{"UPDATE":"OpenADR\/3.1.0\/programs\/b54d3d1f-bc87-4e47-bbc1-2b95958283fc\/update","DELETE":"OpenADR\/3.1.0\/programs\/b54d3d1f-bc87-4e47-bbc1-2b95958283fc\/delete","ALL":"OpenADR\/3.1.0\/programs\/b54d3d1f-bc87-4e47-bbc1-2b95958283fc\/+"}})";

static void test_auth_strings() {
    TEST_ASSERT_EQUAL_STRING("ANONYMOUS", oa3::to_string(MqttAuthMethod::kAnonymous));
    TEST_ASSERT_EQUAL_STRING("OAUTH2_BEARER_TOKEN",
                             oa3::to_string(MqttAuthMethod::kOauth2BearerToken));
    TEST_ASSERT_EQUAL_STRING("CERTIFICATE", oa3::to_string(MqttAuthMethod::kCertificate));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", oa3::to_string(MqttAuthMethod::kUnknown));
}

static void test_live_notifiers() {
    TEST_ASSERT_EQUAL(ParseResult::kOk, notifiers(kLiveNotifiers));
    TEST_ASSERT_FALSE(n.webhook);
    TEST_ASSERT_TRUE(n.mqtt.present);
    TEST_ASSERT_EQUAL_UINT8(2, n.mqtt.uri_count);
    TEST_ASSERT_EQUAL_UINT8(0, n.mqtt.uris_dropped);
    TEST_ASSERT_EQUAL_STRING("mqtt://mqtt.grid-coordination.energy:1883", n.mqtt.uris[0]);
    TEST_ASSERT_EQUAL_STRING("mqtts://mqtt.grid-coordination.energy:8883", n.mqtt.uris[1]);
    TEST_ASSERT_TRUE(n.mqtt.auth == MqttAuthMethod::kAnonymous);
}

static void test_lowercase_keys() {
    TEST_ASSERT_EQUAL(ParseResult::kOk,
                      notifiers(R"({"webhook":true,"mqtt":{"uris":["mqtts://b"],
                                   "authentication":{"method":"OAUTH2_BEARER_TOKEN","username":"{clientID}"}}})"));
    TEST_ASSERT_TRUE(n.webhook);
    TEST_ASSERT_TRUE(n.mqtt.present);
    TEST_ASSERT_EQUAL_UINT8(1, n.mqtt.uri_count);
    TEST_ASSERT_TRUE(n.mqtt.auth == MqttAuthMethod::kOauth2BearerToken);
    TEST_ASSERT_EQUAL_STRING("{clientID}", n.mqtt.username);
}

static void test_vtn_ri_array() {
    TEST_ASSERT_EQUAL(ParseResult::kOk,
                      notifiers(R"( [{"transport":"WEBHOOK","url":"https://x"},
                                     {"transport":"mqtt","url":"mqtt://ri:1883"},
                                     {"transport":"MQTT","uri":"mqtts://ri:8883","URIS":["ssl://ri2"]}])"));
    TEST_ASSERT_TRUE(n.mqtt.present);
    TEST_ASSERT_TRUE(n.mqtt.auth == MqttAuthMethod::kAnonymous);
    TEST_ASSERT_EQUAL_UINT8(3, n.mqtt.uri_count);
    TEST_ASSERT_EQUAL_STRING("mqtt://ri:1883", n.mqtt.uris[0]);
    TEST_ASSERT_EQUAL_STRING("mqtts://ri:8883", n.mqtt.uris[1]);
    TEST_ASSERT_EQUAL_STRING("ssl://ri2", n.mqtt.uris[2]);
}

static void test_array_without_mqtt() {
    TEST_ASSERT_EQUAL(ParseResult::kOk, notifiers(R"([{"transport":"WEBHOOK","url":"https://x"}])"));
    TEST_ASSERT_FALSE(n.mqtt.present);
    TEST_ASSERT_EQUAL(ParseResult::kOk, notifiers("[]"));
    TEST_ASSERT_FALSE(n.mqtt.present);
}

static void test_webhook_only() {
    TEST_ASSERT_EQUAL(ParseResult::kOk, notifiers(R"({"WEBHOOK":false})"));
    TEST_ASSERT_FALSE(n.webhook);
    TEST_ASSERT_FALSE(n.mqtt.present);
    TEST_ASSERT_EQUAL_UINT8(0, n.mqtt.uri_count);
}

static void test_not_json_or_wrong_shape() {
    TEST_ASSERT_EQUAL(ParseResult::kInvalidField, notifiers("\"MQTT\""));
    TEST_ASSERT_EQUAL(ParseResult::kInvalidField, notifiers("42"));
    TEST_ASSERT_EQUAL(ParseResult::kJsonError, notifiers(""));
    TEST_ASSERT_EQUAL(ParseResult::kJsonError, notifiers("{\"MQTT\":"));
    TEST_ASSERT_EQUAL(ParseResult::kJsonError, notifiers("<html>"));
}

// A CERTIFICATE binding's PEMs are filtered out before they reach the arena.
static void test_certificate_with_large_pems() {
    static char body[60000];
    static char pem[16001];
    memset(pem, 'A', sizeof(pem) - 1);
    pem[sizeof(pem) - 1] = '\0';
    snprintf(body, sizeof(body),
             R"({"WEBHOOK":true,"MQTT":{"URIS":["mqtts://b:8883"],"serialization":"JSON",)"
             R"("authentication":{"method":"CERTIFICATE","caCert":"%s","clientCert":"%s","clientKey":"%s"}}})",
             pem, pem, pem);
    TEST_ASSERT_TRUE(strlen(body) > 48000);
    TEST_ASSERT_EQUAL(ParseResult::kOk, notifiers(body));
    TEST_ASSERT_TRUE(n.mqtt.auth == MqttAuthMethod::kCertificate);
    TEST_ASSERT_EQUAL_UINT8(1, n.mqtt.uri_count);
    TEST_ASSERT_EQUAL_STRING("", n.mqtt.username);
    TEST_ASSERT_TRUE(arena.used() < strlen(pem));
}

static void test_uri_overflow() {
    static char body[2048];
    char long_uri[OA3_NOTIFIER_URI_MAX + 2];
    memset(long_uri, 'h', sizeof(long_uri) - 1);
    long_uri[sizeof(long_uri) - 1] = '\0';
    snprintf(body, sizeof(body),
             R"({"WEBHOOK":true,"MQTT":{"URIS":["mqtts://a","%s","mqtts://b","mqtts://c","mqtts://d","mqtts://e"],)"
             R"("authentication":{"method":"ANONYMOUS"}}})",
             long_uri);
    TEST_ASSERT_EQUAL(ParseResult::kOk, notifiers(body));
    TEST_ASSERT_EQUAL_UINT8(OA3_NOTIFIER_URIS_MAX, n.mqtt.uri_count);
    TEST_ASSERT_EQUAL_UINT8(2, n.mqtt.uris_dropped);
    TEST_ASSERT_EQUAL_STRING("mqtts://a", n.mqtt.uris[0]);
    TEST_ASSERT_EQUAL_STRING("mqtts://d", n.mqtt.uris[3]);
}

static void test_unknown_and_missing_auth() {
    TEST_ASSERT_EQUAL(ParseResult::kOk,
                      notifiers(R"({"WEBHOOK":true,"MQTT":{"URIS":["mqtts://b"],"authentication":{"method":"KERBEROS"}}})"));
    TEST_ASSERT_TRUE(n.mqtt.auth == MqttAuthMethod::kUnknown);
    TEST_ASSERT_EQUAL(ParseResult::kOk, notifiers(R"({"WEBHOOK":true,"MQTT":{"URIS":["mqtts://b"]}})"));
    TEST_ASSERT_TRUE(n.mqtt.auth == MqttAuthMethod::kAnonymous);
}

static void test_live_event_topics() {
    TEST_ASSERT_EQUAL(ParseResult::kOk, topics(kLiveEventTopics));
    TEST_ASSERT_EQUAL_STRING(
        "OpenADR/3.1.0/programs/b54d3d1f-bc87-4e47-bbc1-2b95958283fc/events/create", t.create);
    TEST_ASSERT_EQUAL_STRING(
        "OpenADR/3.1.0/programs/b54d3d1f-bc87-4e47-bbc1-2b95958283fc/events/update", t.update);
    TEST_ASSERT_EQUAL_STRING(
        "OpenADR/3.1.0/programs/b54d3d1f-bc87-4e47-bbc1-2b95958283fc/events/delete", t.remove);
    TEST_ASSERT_EQUAL_STRING("OpenADR/3.1.0/programs/b54d3d1f-bc87-4e47-bbc1-2b95958283fc/events/+",
                             t.all);
    TEST_ASSERT_EQUAL_size_t(73, strlen(t.update));
}

static void test_live_program_topics() {
    TEST_ASSERT_EQUAL(ParseResult::kOk, topics(kLiveProgramTopics));
    TEST_ASSERT_EQUAL_STRING("", t.create);
    TEST_ASSERT_EQUAL_STRING("OpenADR/3.1.0/programs/b54d3d1f-bc87-4e47-bbc1-2b95958283fc/update",
                             t.update);
    TEST_ASSERT_EQUAL_STRING("OpenADR/3.1.0/programs/b54d3d1f-bc87-4e47-bbc1-2b95958283fc/+", t.all);
}

static void test_topics_without_all() {
    TEST_ASSERT_EQUAL(ParseResult::kOk,
                      topics(R"({"topics":{"UPDATE":"events/update","DELETE":"events/delete"}})"));
    TEST_ASSERT_EQUAL_STRING("", t.create);
    TEST_ASSERT_EQUAL_STRING("events/update", t.update);
    TEST_ASSERT_EQUAL_STRING("events/delete", t.remove);
    TEST_ASSERT_EQUAL_STRING("", t.all);
}

static void test_topics_errors() {
    TEST_ASSERT_EQUAL(ParseResult::kInvalidField, topics(R"({"UPDATE":"x"})"));
    TEST_ASSERT_EQUAL(ParseResult::kInvalidField, topics(R"({"topics":["x"]})"));
    TEST_ASSERT_EQUAL(ParseResult::kInvalidField, topics(R"({"topics":{"ALL":5}})"));
    TEST_ASSERT_EQUAL(ParseResult::kInvalidField, topics("[]"));
    TEST_ASSERT_EQUAL(ParseResult::kJsonError, topics("{"));
    char body[512];
    char topic[OA3_TOPIC_MAX + 2];
    memset(topic, 't', sizeof(topic) - 1);
    topic[sizeof(topic) - 1] = '\0';
    snprintf(body, sizeof(body), R"({"topics":{"UPDATE":"u","ALL":"%s"}})", topic);
    TEST_ASSERT_EQUAL(ParseResult::kCapacityExceeded, topics(body));
    TEST_ASSERT_EQUAL_STRING("", t.update);
}

static void assert_broker(const char* uri, const char* host, uint16_t port, bool tls) {
    BrokerAddress b;
    TEST_ASSERT_TRUE_MESSAGE(oa3::parse_broker_uri(uri, &b), uri);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(host, b.host, uri);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(port, b.port, uri);
    TEST_ASSERT_EQUAL_MESSAGE(tls, b.tls, uri);
}

static void assert_bad(const char* uri) {
    BrokerAddress b;
    TEST_ASSERT_FALSE_MESSAGE(oa3::parse_broker_uri(uri, &b), uri);
}

static void test_broker_schemes() {
    assert_broker("mqtts://mqtt.grid-coordination.energy:8883", "mqtt.grid-coordination.energy",
                  8883, true);
    assert_broker("mqtt://mqtt.grid-coordination.energy:1883", "mqtt.grid-coordination.energy",
                  1883, false);
    assert_broker("MQTTS://b", "b", 8883, true);
    assert_broker("Ssl://b", "b", 8883, true);
    assert_broker("MQTT://b", "b", 1883, false);
    assert_broker("TCP://b:1884", "b", 1884, false);
    assert_broker("mqtts://user:pw@b:9000/path?q#f", "b", 9000, true);
    assert_broker("mqtts://b/", "b", 8883, true);
    assert_broker("mqtts://b:", "b", 8883, true);
}

static void test_broker_ipv6() {
    assert_broker("mqtts://[2001:db8::1]:8884", "2001:db8::1", 8884, true);
    assert_broker("mqtt://[::1]", "::1", 1883, false);
    assert_bad("mqtt://[::1");
    assert_bad("mqtt://[::1]x");
    assert_bad("mqtt://2001:db8::1");
}

static void test_broker_bad() {
    assert_bad(nullptr);
    assert_bad("");
    assert_bad("ws://b");
    assert_bad("wss://b:443/mqtt");
    assert_bad("https://b");
    assert_bad("foo://b");
    assert_bad("://b");
    assert_bad(" mqtts://b");
    assert_bad("mqtts:/b");
    assert_bad("b.example.com");
    assert_bad("b.example.com:8883");
    assert_bad("mqtts://");
    assert_bad("mqtts://:8883");
    assert_bad("mqtts://b:0");
    assert_bad("mqtts://b:65536");
    assert_bad("mqtts://b:88a3");
    assert_bad("mqtts://b:123456");
    char uri[OA3_HOST_MAX + 16];
    snprintf(uri, sizeof(uri), "mqtts://");
    memset(uri + 8, 'h', OA3_HOST_MAX + 1);
    uri[8 + OA3_HOST_MAX + 1] = '\0';
    assert_bad(uri);
    uri[8 + OA3_HOST_MAX] = '\0';
    assert_broker(uri, uri + 8, 8883, true);
}

static void test_select_broker() {
    BrokerAddress b;
    TEST_ASSERT_EQUAL(ParseResult::kOk, notifiers(kLiveNotifiers));
    TEST_ASSERT_TRUE(oa3::select_broker(n.mqtt, false, &b));
    TEST_ASSERT_TRUE(b.tls);
    TEST_ASSERT_EQUAL_UINT16(8883, b.port);
    TEST_ASSERT_TRUE(oa3::select_broker(n.mqtt, true, &b));
    TEST_ASSERT_TRUE(b.tls);

    TEST_ASSERT_EQUAL(ParseResult::kOk,
                      notifiers(R"({"WEBHOOK":true,"MQTT":{"URIS":["mqtts://","tcp://p:1883"],"authentication":{"method":"ANONYMOUS"}}})"));
    TEST_ASSERT_FALSE(oa3::select_broker(n.mqtt, false, &b));
    TEST_ASSERT_TRUE(oa3::select_broker(n.mqtt, true, &b));
    TEST_ASSERT_FALSE(b.tls);
    TEST_ASSERT_EQUAL_STRING("p", b.host);

    TEST_ASSERT_EQUAL(ParseResult::kOk,
                      notifiers(R"({"WEBHOOK":true,"MQTT":{"URIS":["wss://b:443/mqtt","ws://b"]}})"));
    TEST_ASSERT_FALSE(oa3::select_broker(n.mqtt, true, &b));

    TEST_ASSERT_EQUAL(ParseResult::kOk, notifiers(R"({"WEBHOOK":true,"MQTT":{"URIS":[]}})"));
    TEST_ASSERT_FALSE(oa3::select_broker(n.mqtt, true, &b));
}

// A DELETE notification's object may be its id alone; other operations still need programID.
static void test_delete_notification_with_id_only() {
    static oa3::Notification note;
    const char ev[] = R"({"objectType":"EVENT","operation":"DELETE","object":{"id":"e1"}})";
    TEST_ASSERT_EQUAL(ParseResult::kOk, oa3::parse_notification(ev, strlen(ev), arena, &note));
    TEST_ASSERT_TRUE(note.has_object);
    TEST_ASSERT_TRUE(note.operation == oa3::Operation::kDelete);
    TEST_ASSERT_EQUAL_STRING("e1", note.event.id);
    TEST_ASSERT_EQUAL_STRING("", note.event.program_id);

    const char pr[] = R"({"objectType":"PROGRAM","operation":"DELETE","object":{"id":"p1"}})";
    TEST_ASSERT_EQUAL(ParseResult::kOk, oa3::parse_notification(pr, strlen(pr), arena, &note));
    TEST_ASSERT_EQUAL_STRING("p1", note.program.id);

    const char no_id[] = R"({"objectType":"EVENT","operation":"DELETE","object":{"programID":"p"}})";
    TEST_ASSERT_EQUAL(ParseResult::kInvalidField,
                      oa3::parse_notification(no_id, strlen(no_id), arena, &note));
    const char update[] = R"({"objectType":"EVENT","operation":"UPDATE","object":{"id":"e1"}})";
    TEST_ASSERT_EQUAL(ParseResult::kInvalidField,
                      oa3::parse_notification(update, strlen(update), arena, &note));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_auth_strings);
    RUN_TEST(test_live_notifiers);
    RUN_TEST(test_lowercase_keys);
    RUN_TEST(test_vtn_ri_array);
    RUN_TEST(test_array_without_mqtt);
    RUN_TEST(test_webhook_only);
    RUN_TEST(test_not_json_or_wrong_shape);
    RUN_TEST(test_certificate_with_large_pems);
    RUN_TEST(test_uri_overflow);
    RUN_TEST(test_unknown_and_missing_auth);
    RUN_TEST(test_live_event_topics);
    RUN_TEST(test_live_program_topics);
    RUN_TEST(test_topics_without_all);
    RUN_TEST(test_topics_errors);
    RUN_TEST(test_broker_schemes);
    RUN_TEST(test_broker_ipv6);
    RUN_TEST(test_broker_bad);
    RUN_TEST(test_select_broker);
    RUN_TEST(test_delete_notification_with_id_only);
    return UNITY_END();
}
