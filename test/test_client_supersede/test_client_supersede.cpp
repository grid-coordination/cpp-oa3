// A VTN that publishes a new event each hour, overlapping the last one, instead of updating
// it: two event slots, push only.

#include <stdio.h>
#include <string.h>

#include <oa3/client/ven_client.h>
#include <oa3/model/intervals.h>
#include <unity.h>

#include "../support/client_fakes.h"

using oa3::NotifyState;
using oa3::VenStatus;

static fakes::MqttRigN<2> rig;
static char obj[8][1536];
static char msg[8][2048];
static char body[2048];

#define ASSERT_STATUS(expected, actual) \
    TEST_ASSERT_EQUAL_STRING(oa3::to_string(expected), oa3::to_string(actual))

void setUp() { rig.reset(); }
void tearDown() {}

static oa3::VenConfig push_config() {
    oa3::VenConfig c = rig.config();
    c.notify_poll_period_s = 0;
    return c;
}

static void run(uint32_t total_s, uint32_t step_s = 60) {
    for (uint32_t t = 0; t < total_s; t += step_s) {
        rig.clk.advance_s(step_s);
        rig.client.loop();
    }
}

// Event E<k>, published at 20:00Z + k h: hourly intervals for hours 20 + k .. 20 + k + count - 1,
// each worth 100 * k + hour. mdt: the publication time plus 10 s, unless with_mdt is false.
static const char* rolling(char* buf, size_t size, int k, int count = 4, bool with_mdt = true) {
    const int first = 20 + k;
    size_t n = static_cast<size_t>(
        snprintf(buf, size, R"({"id":"E%d","programID":"%s","objectType":"EVENT",)", k, fakes::kProgramId));
    if (with_mdt) {
        n += static_cast<size_t>(snprintf(buf + n, size - n,
                                          R"("modificationDateTime":"2026-11-1%dT%02d:00:10Z",)",
                                          3 + first / 24, first % 24));
    }
    n += static_cast<size_t>(snprintf(buf + n, size - n, R"("intervals":[)"));
    for (int i = 0; i < count; ++i) {
        const int h = first + i;
        n += static_cast<size_t>(snprintf(
            buf + n, size - n,
            R"(%s{"id":%d,"intervalPeriod":{"start":"2026-11-1%dT%02d:00:00Z","duration":"PT1H"},)"
            R"("payloads":[{"type":"PRICE","values":[%d]}]})",
            i == 0 ? "" : ",", i, 3 + h / 24, h % 24, 100 * k + h));
    }
    snprintf(buf + n, size - n, "]}");
    return buf;
}

static void push_create(int slot, int k, int count = 4, bool with_mdt = true) {
    char e[1536];
    rolling(e, sizeof(e), k, count, with_mdt);
    snprintf(obj[slot], sizeof(obj[slot]), "%s", e);
    fakes::envelope(msg[slot], sizeof(msg[slot]), "EVENT", "CREATE", obj[slot]);
    rig.mqtt.push_message(fakes::kEventsTopic, msg[slot]);
}

// The value in force across [now, now + hours h), hour by hour, against expected.
static void assert_schedule(const int* expected, int hours) {
    oa3::DateTime now{};
    TEST_ASSERT_TRUE(rig.clk.now(&now));
    now.seconds -= now.seconds % 3600;
    oa3::Duration d{};
    d.hours = hours;
    oa3::TimeInterval w{};
    TEST_ASSERT_TRUE(oa3::make_interval(now, d, &w));
    oa3::PriceStep steps[8];
    const size_t n =
        oa3::price_schedule(rig.client.events(), rig.client.event_count(), w, "PRICE", steps, 8, nullptr);
    TEST_ASSERT_EQUAL_size_t(static_cast<size_t>(hours), n);
    for (int i = 0; i < hours; ++i) {
        TEST_ASSERT_NOT_NULL(steps[i].value);
        TEST_ASSERT_EQUAL_DOUBLE(expected[i], steps[i].value->number);
    }
}

// Five hours of new events by notification, each superseding the last.
static void test_hourly_rolling_by_notification() {
    rolling(body + 1, sizeof(body) - 2, 0);
    body[0] = '[';
    strcat(body, "]");
    snprintf(rig.events, sizeof(rig.events), "%s", body);
    rig.connect(push_config());
    TEST_ASSERT_EQUAL_size_t(1, rig.client.event_count());
    for (int k = 1; k <= 5; ++k) {
        run(3540);
        push_create(k % 8, k);
        run(60);
        TEST_ASSERT_EQUAL_size_t(1, rig.client.event_count());
        char id[8];
        snprintf(id, sizeof(id), "E%d", k);
        TEST_ASSERT_EQUAL_STRING(id, rig.client.events()[0].id);
        ASSERT_STATUS(VenStatus::kOk, rig.client.status().status);
        const int h = 20 + k;
        const int expected[] = {100 * k + h, 100 * k + h + 1, 100 * k + h + 2, 100 * k + h + 3};
        assert_schedule(expected, 4);
    }
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
}

// The new event is published before the old one's remaining hours are covered: a partial
// overlap keeps the old one for its tail.
static void test_partial_overlap_keeps_tail() {
    char e0[1536];
    snprintf(rig.events, sizeof(rig.events), "[%s]", rolling(e0, sizeof(e0), 0, 6));
    rig.connect(push_config());
    run(3540);
    push_create(0, 1, 2);
    run(60);
    TEST_ASSERT_EQUAL_size_t(2, rig.client.event_count());
    const int expected[] = {121, 122, 23, 24, 25};
    assert_schedule(expected, 5);
}

// Both slots hold overlapping events, neither superseded; a third that covers both makes
// room for itself instead of truncating.
static void test_capacity_evicts_superseded() {
    char e0[1536], e1[1536];
    snprintf(rig.events, sizeof(rig.events), "[%s,%s]", rolling(e0, sizeof(e0), 0, 6),
             rolling(e1, sizeof(e1), 1, 3));
    rig.connect(push_config());
    TEST_ASSERT_EQUAL_size_t(2, rig.client.event_count());
    run(7140);
    push_create(0, 2, 4);
    run(60);
    TEST_ASSERT_EQUAL_size_t(1, rig.client.event_count());
    TEST_ASSERT_EQUAL_STRING("E2", rig.client.events()[0].id);
    ASSERT_STATUS(VenStatus::kOk, rig.client.status().status);
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
}

// The same rolling events by fetch, three in two slots, in either order.
static void test_rolling_by_fetch(bool newest_first) {
    char e[3][1024];
    for (int k = 0; k < 3; ++k) {
        rolling(e[k], sizeof(e[k]), k);
    }
    rig.clk.set("2026-11-13T22:00:00Z");
    if (newest_first) {
        snprintf(rig.events, sizeof(rig.events), "[%s,%s,%s]", e[2], e[1], e[0]);
    } else {
        snprintf(rig.events, sizeof(rig.events), "[%s,%s,%s]", e[0], e[1], e[2]);
    }
    rig.connect(push_config());
    TEST_ASSERT_EQUAL_size_t(1, rig.client.event_count());
    TEST_ASSERT_EQUAL_STRING("E2", rig.client.events()[0].id);
    ASSERT_STATUS(VenStatus::kOk, rig.client.status().status);
    const int expected[] = {222, 223, 224, 225};
    assert_schedule(expected, 4);
}

static void test_rolling_by_fetch_oldest_first() { test_rolling_by_fetch(false); }
static void test_rolling_by_fetch_newest_first() { test_rolling_by_fetch(true); }

// Without modificationDateTime, the later arrival wins.
static void test_rolling_without_mdt() {
    char e0[1536];
    snprintf(rig.events, sizeof(rig.events), "[%s]", rolling(e0, sizeof(e0), 0, 4, false));
    rig.connect(push_config());
    run(3540);
    push_create(0, 1, 4, false);
    run(60);
    TEST_ASSERT_EQUAL_size_t(1, rig.client.event_count());
    TEST_ASSERT_EQUAL_STRING("E1", rig.client.events()[0].id);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_hourly_rolling_by_notification);
    RUN_TEST(test_partial_overlap_keeps_tail);
    RUN_TEST(test_capacity_evicts_superseded);
    RUN_TEST(test_rolling_by_fetch_oldest_first);
    RUN_TEST(test_rolling_by_fetch_newest_first);
    RUN_TEST(test_rolling_without_mdt);
    return UNITY_END();
}
