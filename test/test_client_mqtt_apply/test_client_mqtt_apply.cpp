#include <string.h>

#include <oa3/client/ven_client.h>
#include <unity.h>

#include "../support/client_fakes.h"

using fakes::event_json;
using fakes::kEventsTopic;
using oa3::NotifyState;

static fakes::MqttRig rig;
static char obj[8][512];
static char msg[8][1024];

#define ASSERT_NOTIFY(expected, actual) \
    TEST_ASSERT_EQUAL_STRING(oa3::to_string(expected), oa3::to_string(actual))

void setUp() { rig.reset(); }
void tearDown() {}

static void connect() {
    rig.connect(rig.config());
    TEST_ASSERT_EQUAL_size_t(1, rig.client.event_count());
    TEST_ASSERT_EQUAL_STRING("0.1", rig.client.price().text);
}

// Queues an EVENT notification in slot i.
static void push_event(int i, const char* op, const char* id, const char* start, const char* value,
                       const char* mdt = nullptr, const char* targets = nullptr,
                       const char* program_id = fakes::kProgramId, bool retained = false) {
    event_json(obj[i], sizeof(obj[i]), id, start, value, mdt, targets, program_id);
    fakes::envelope(msg[i], sizeof(msg[i]), "EVENT", op, obj[i]);
    rig.mqtt.push_message(kEventsTopic, msg[i], retained);
}

static void assert_ids(const char* a, const char* b = nullptr, const char* c = nullptr,
                       const char* d = nullptr) {
    const char* ids[] = {a, b, c, d};
    size_t n = 0;
    while (n < 4 && ids[n] != nullptr) {
        ++n;
    }
    TEST_ASSERT_EQUAL_size_t(n, rig.client.event_count());
    for (size_t i = 0; i < n; ++i) {
        TEST_ASSERT_EQUAL_STRING(ids[i], rig.client.events()[i].id);
    }
}

static void test_create_appends() {
    connect();
    push_event(0, "CREATE", "e2", "2026-11-13T21:00:00Z", "0.2");
    rig.client.loop();
    assert_ids("e1", "e2");
    TEST_ASSERT_EQUAL_UINT32(1, rig.client.status().notifications);
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
}

static void test_update_fires_price_without_http() {
    connect();
    const size_t prices = rig.rec.prices;
    push_event(0, "UPDATE", "e1", "2026-11-13T20:00:00Z", "0.25", "2026-11-13T19:00:00Z");
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(prices + 1, rig.rec.prices);
    TEST_ASSERT_EQUAL_STRING("0.25", rig.rec.last_price().text);
    TEST_ASSERT_EQUAL_STRING("e1", rig.rec.last_price().event_id);
    assert_ids("e1");
}

static void test_post_and_put_read_as_create_and_update() {
    connect();
    push_event(0, "POST", "e2", "2026-11-13T21:00:00Z", "0.2");
    push_event(1, "PUT", "e1", "2026-11-13T20:00:00Z", "0.3");
    rig.client.loop();
    assert_ids("e1", "e2");
    TEST_ASSERT_EQUAL_STRING("0.3", rig.client.price().text);
    TEST_ASSERT_EQUAL_UINT32(2, rig.client.status().notifications);
}

static void test_create_of_stored_id_replaces() {
    connect();
    push_event(0, "CREATE", "e1", "2026-11-13T20:00:00Z", "0.4");
    rig.client.loop();
    assert_ids("e1");
    TEST_ASSERT_EQUAL_STRING("0.4", rig.client.price().text);
}

static void test_delete_by_id_keeps_order() {
    connect();
    push_event(0, "CREATE", "e2", "2026-11-13T21:00:00Z", "0.2");
    push_event(1, "CREATE", "e3", "2026-11-13T22:00:00Z", "0.3");
    rig.client.loop();
    assert_ids("e1", "e2", "e3");
    rig.mqtt.push_message(kEventsTopic, R"({"objectType":"EVENT","operation":"DELETE","object":{"id":"e2"}})");
    rig.client.loop();
    assert_ids("e1", "e3");
    rig.mqtt.push_message(kEventsTopic, R"({"objectType":"EVENT","operation":"DELETE","object":{"id":"e1"}})");
    rig.client.loop();
    assert_ids("e3");
    TEST_ASSERT_FALSE(rig.client.price().active);
    TEST_ASSERT_EQUAL_UINT32(4, rig.client.status().notifications);
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
}

static void test_stale_update_ignored() {
    connect();
    push_event(0, "UPDATE", "e1", "2026-11-13T20:00:00Z", "9.9", "2026-11-12T21:00:00Z");
    rig.client.loop();
    TEST_ASSERT_EQUAL_STRING("0.1", rig.client.price().text);
    TEST_ASSERT_EQUAL_UINT32(0, rig.client.status().notifications);
    push_event(1, "UPDATE", "e1", "2026-11-13T20:00:00Z", "0.5", "2026-11-12T22:00:00Z");
    rig.client.loop();
    TEST_ASSERT_EQUAL_STRING("0.5", rig.client.price().text);
}

static void test_other_program_ignored() {
    connect();
    push_event(0, "CREATE", "x1", "2026-11-13T20:00:00Z", "9.9", nullptr, nullptr, "other");
    push_event(1, "UPDATE", "e1", "2026-11-13T20:00:00Z", "9.9", nullptr, nullptr, "other");
    rig.client.loop();
    assert_ids("e1");
    TEST_ASSERT_EQUAL_STRING("0.1", rig.client.price().text);
    TEST_ASSERT_EQUAL_UINT32(0, rig.client.status().notifications);
}

static void test_target_mismatch() {
    oa3::VenConfig c = rig.config();
    const char* targets[] = {"T1"};
    c.targets = targets;
    c.target_count = 1;
    rig.connect(c);
    push_event(0, "CREATE", "x1", "2026-11-13T21:00:00Z", "9.9", nullptr, R"(["T2"])");
    push_event(1, "CREATE", "e2", "2026-11-13T21:00:00Z", "0.2", nullptr, R"(["T2","T1"])");
    rig.client.loop();
    assert_ids("e1", "e2");
    push_event(0, "UPDATE", "e2", "2026-11-13T21:00:00Z", "0.2", nullptr, R"(["T3"])");
    rig.client.loop();
    assert_ids("e1");
}

static void test_out_of_window_update_removes() {
    connect();
    push_event(0, "UPDATE", "e1", "2026-11-15T20:00:00Z", "0.1");
    rig.client.loop();
    assert_ids(nullptr);
    TEST_ASSERT_FALSE(rig.client.price().active);
    TEST_ASSERT_FALSE(rig.rec.last_price().active);
    push_event(0, "CREATE", "e9", "2026-11-13T19:00:00Z", "0.1");
    rig.client.loop();
    assert_ids(nullptr);
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
}

// OA3_MAX_EVENTS (4) on the host: the event reaching into the window latest gives way.
static void test_capacity_eviction_truncates() {
    connect();
    push_event(0, "CREATE", "e2", "2026-11-13T21:00:00Z", "0.2");
    push_event(1, "CREATE", "e3", "2026-11-13T22:00:00Z", "0.3");
    push_event(2, "CREATE", "e4", "2026-11-13T23:00:00Z", "0.4");
    rig.client.loop();
    assert_ids("e1", "e2", "e3", "e4");
    TEST_ASSERT_EQUAL_STRING("OK", oa3::to_string(rig.client.status().status));

    push_event(0, "CREATE", "late", "2026-11-14T05:00:00Z", "0.5");
    rig.client.loop();
    assert_ids("e1", "e2", "e3", "e4");
    TEST_ASSERT_EQUAL_STRING("TRUNCATED", oa3::to_string(rig.client.status().status));
    TEST_ASSERT_EQUAL_STRING("TRUNCATED", oa3::to_string(rig.rec.last_status().status));

    push_event(0, "CREATE", "early", "2026-11-13T20:30:00Z", "0.6");
    rig.client.loop();
    assert_ids("e1", "e2", "e3", "early");
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
}

static void test_program_update() {
    connect();
    rig.mqtt.push_message(
        "OpenADR/3.1.0/programs/b54d3d1f-bc87-4e47-bbc1-2b95958283fc/update",
        R"({"objectType":"PROGRAM","operation":"UPDATE","object":{"id":"b54d3d1f-bc87-4e47-bbc1-2b95958283fc",)"
        R"("programName":"EELEC-013532223","payloadDescriptors":[{"payloadType":"PRICE","units":"MWH","currency":"EUR"}]}})");
    rig.client.loop();
    TEST_ASSERT_NOT_NULL(rig.client.program());
    TEST_ASSERT_EQUAL_STRING("MWH", rig.client.program()->descriptors[0].units);
    TEST_ASSERT_EQUAL_STRING(fakes::kProgramId, rig.client.program()->id);
    TEST_ASSERT_EQUAL_UINT32(1, rig.client.status().notifications);
}

static char program_msg[4][512];

static void push_program(int i, const char* mdt, const char* units, bool retained = false) {
    snprintf(program_msg[i], sizeof(program_msg[i]),
             R"({"objectType":"PROGRAM","operation":"UPDATE","object":{"id":"%s",)"
             R"("programName":"EELEC-013532223","modificationDateTime":"%s",)"
             R"("payloadDescriptors":[{"payloadType":"PRICE","units":"%s","currency":"USD"}]}})",
             fakes::kProgramId, mdt, units);
    rig.mqtt.push_message("OpenADR/3.1.0/programs/b54d3d1f-bc87-4e47-bbc1-2b95958283fc/update",
                          program_msg[i], retained);
}

static void test_program_update_ordering() {
    connect();
    push_program(0, "2026-11-12T00:00:00Z", "MWH");
    push_program(1, "2026-11-01T00:00:00Z", "KWH");
    rig.client.loop();
    TEST_ASSERT_EQUAL_STRING("MWH", rig.client.program()->descriptors[0].units);
    TEST_ASSERT_EQUAL_UINT32(1, rig.client.status().notifications);

    push_program(2, "2026-11-12T00:00:00Z", "GWH", true);
    rig.client.loop();
    TEST_ASSERT_EQUAL_STRING("MWH", rig.client.program()->descriptors[0].units);
    push_program(3, "2026-11-13T00:00:00Z", "GWH", true);
    rig.client.loop();
    TEST_ASSERT_EQUAL_STRING("GWH", rig.client.program()->descriptors[0].units);
    TEST_ASSERT_EQUAL_UINT32(2, rig.client.status().notifications);
}

// Retained messages may predate the /events snapshot: only a newer UPDATE of a stored event
// applies.
static void test_retained_event_rules() {
    connect();
    push_event(0, "CREATE", "e2", "2026-11-13T21:00:00Z", "0.2", "2026-11-13T19:00:00Z", nullptr,
               fakes::kProgramId, true);
    push_event(1, "UPDATE", "e1", "2026-11-13T20:00:00Z", "0.9", nullptr, nullptr,
               fakes::kProgramId, true);
    push_event(2, "UPDATE", "e1", "2026-11-13T20:00:00Z", "0.8", "2026-11-12T22:00:00Z", nullptr,
               fakes::kProgramId, true);
    rig.client.loop();
    assert_ids("e1");
    TEST_ASSERT_EQUAL_STRING("0.1", rig.client.price().text);
    TEST_ASSERT_EQUAL_UINT32(0, rig.client.status().notifications);
    push_event(3, "UPDATE", "e1", "2026-11-13T20:00:00Z", "0.3", "2026-11-13T19:00:00Z", nullptr,
               fakes::kProgramId, true);
    rig.client.loop();
    TEST_ASSERT_EQUAL_STRING("0.3", rig.client.price().text);
    TEST_ASSERT_EQUAL_UINT32(1, rig.client.status().notifications);
}

static void test_other_program_update_ignored() {
    connect();
    rig.mqtt.push_message("x",
                          R"({"objectType":"PROGRAM","operation":"UPDATE","object":{"id":"other","programName":"N"}})");
    rig.client.loop();
    TEST_ASSERT_EQUAL_STRING("EELEC-013532223", rig.client.program()->program_name);
    TEST_ASSERT_EQUAL_UINT32(0, rig.client.status().notifications);
}

static void test_program_delete_resolves_again() {
    connect();
    const size_t while_connected = rig.http.gets_while_connected;
    rig.mqtt.push_message("x",
                          R"({"objectType":"PROGRAM","operation":"DELETE","object":{"id":"b54d3d1f-bc87-4e47-bbc1-2b95958283fc"}})");
    rig.http.push_status(404);
    rig.client.loop();
    TEST_ASSERT_FALSE(rig.mqtt.connected());
    TEST_ASSERT_EQUAL_size_t(0, rig.client.subscription_count());
    ASSERT_NOTIFY(NotifyState::kOff, rig.client.status().notify);
    TEST_ASSERT_EQUAL_size_t(7, rig.http.requests);
    TEST_ASSERT_NOT_NULL(strstr(rig.http.last_url(), "/programs/b54d3d1f"));
    TEST_ASSERT_EQUAL_STRING("PROGRAM_NOT_FOUND", oa3::to_string(rig.client.status().status));
    TEST_ASSERT_EQUAL_size_t(while_connected, rig.http.gets_while_connected);
    TEST_ASSERT_EQUAL_size_t(0, rig.client.event_count());
    TEST_ASSERT_FALSE(rig.client.status().has_data);
    TEST_ASSERT_FALSE(rig.client.price().active);
    TEST_ASSERT_FALSE(rig.rec.last_price().active);

    rig.http.push(fakes::Response{nullptr, oa3::HttpResult::kOk, 404, "", true, 3});
    for (int i = 0; i < 3; ++i) {
        rig.clk.advance_s(900);
        rig.client.loop();
    }
    TEST_ASSERT_EQUAL_size_t(10, rig.http.requests);
    TEST_ASSERT_EQUAL_STRING("PROGRAM_NOT_FOUND", oa3::to_string(rig.client.status().status));
    TEST_ASSERT_FALSE(rig.client.price().active);
}

static void test_too_large_live_polls() {
    connect();
    rig.mqtt.push_message(kEventsTopic, nullptr, false, true);
    rig.http.push_ok(rig.events, "/events?");
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(7, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
}

static void test_too_large_retained_only_logged() {
    connect();
    rig.mqtt.push_message(kEventsTopic, nullptr, true, true);
    rig.client.loop();
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
}

static void test_parse_error_live_vs_retained() {
    connect();
    rig.mqtt.push_message(kEventsTopic, "{\"objectType\":\"EVENT\",", true);
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
    rig.mqtt.push_message(kEventsTopic, R"({"objectType":"EVENT","operation":"UPDATE","object":{"id":"e1"}})");
    rig.http.push_ok(rig.events, "/events?");
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(7, rig.http.requests);
    TEST_ASSERT_EQUAL_STRING("OK", oa3::to_string(rig.client.status().status));
}

static void test_other_objects_and_operations_ignored() {
    connect();
    rig.mqtt.push_message("x", R"({"objectType":"REPORT","operation":"CREATE","object":{"id":"r1"}})");
    push_event(0, "GET", "e1", "2026-11-13T20:00:00Z", "9.9");
    push_event(1, "MERGE", "e1", "2026-11-13T20:00:00Z", "9.9");
    rig.client.loop();
    assert_ids("e1");
    TEST_ASSERT_EQUAL_STRING("0.1", rig.client.price().text);
    TEST_ASSERT_EQUAL_UINT32(0, rig.client.status().notifications);
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_create_appends);
    RUN_TEST(test_update_fires_price_without_http);
    RUN_TEST(test_post_and_put_read_as_create_and_update);
    RUN_TEST(test_create_of_stored_id_replaces);
    RUN_TEST(test_delete_by_id_keeps_order);
    RUN_TEST(test_stale_update_ignored);
    RUN_TEST(test_other_program_ignored);
    RUN_TEST(test_target_mismatch);
    RUN_TEST(test_out_of_window_update_removes);
    RUN_TEST(test_capacity_eviction_truncates);
    RUN_TEST(test_program_update);
    RUN_TEST(test_program_update_ordering);
    RUN_TEST(test_retained_event_rules);
    RUN_TEST(test_other_program_update_ignored);
    RUN_TEST(test_program_delete_resolves_again);
    RUN_TEST(test_too_large_live_polls);
    RUN_TEST(test_too_large_retained_only_logged);
    RUN_TEST(test_parse_error_live_vs_retained);
    RUN_TEST(test_other_objects_and_operations_ignored);
    return UNITY_END();
}
