// notify_poll_period_s 0: no polling while subscribed, a REST snapshot on every connect, and
// a fetch when something left out beyond the window comes within it.

#include <string.h>

#include <oa3/client/ven_client.h>
#include <oa3/model/intervals.h>
#include <unity.h>

#include "../support/client_fakes.h"

using oa3::NotifyState;
using oa3::VenStatus;

static fakes::MqttRig rig;
static char later[2048];
static char obj[512];
static char msg[1024];

#define ASSERT_NOTIFY(expected, actual) \
    TEST_ASSERT_EQUAL_STRING(oa3::to_string(expected), oa3::to_string(actual))
#define ASSERT_STATUS(expected, actual) \
    TEST_ASSERT_EQUAL_STRING(oa3::to_string(expected), oa3::to_string(actual))

void setUp() { rig.reset(); }
void tearDown() {}

static oa3::VenConfig push_config(bool exclusive = false) {
    oa3::VenConfig c = rig.config();
    c.notify_poll_period_s = 0;
    c.exclusive_tls = exclusive;
    return c;
}

static void push_discovery() {
    rig.http.push_ok(fakes::kNotifiersBody, "/notifiers");
    rig.http.push_ok(fakes::kEventTopicsBody, "/topics/programs/");
    rig.http.push_ok(fakes::kProgramTopicsBody, "/topics/programs/");
}

static void loop_until(NotifyState s, int max = 8) {
    for (int i = 0; i < max && rig.client.status().notify != s; ++i) {
        rig.client.loop();
    }
    ASSERT_NOTIFY(s, rig.client.status().notify);
}

// Advances by step_s and loops, total_s in all.
static void run(uint32_t total_s, uint32_t step_s = 60) {
    for (uint32_t t = 0; t < total_s; t += step_s) {
        rig.clk.advance_s(step_s);
        rig.client.loop();
    }
}

// exclusive_tls: snapshot, connect, and the snapshot after a new session.
static void go_live_exclusive(const oa3::VenConfig& c) {
    rig.start(c);
    push_discovery();
    rig.http.push_ok(rig.events, "/events?");
    rig.http.push_ok(rig.events, "/events?");
    loop_until(NotifyState::kConnected);
    TEST_ASSERT_EQUAL_size_t(7, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
}

// The fixture event e1 at 20:00Z and e2 starting at start, beyond the 24 h window.
static void with_later_event(const char* start) {
    char e1[512], e2[512];
    fakes::event_json(e1, sizeof(e1), "e1", "2026-11-13T20:00:00Z", "0.1", "2026-11-12T22:00:00Z");
    fakes::event_json(e2, sizeof(e2), "e2", start, "0.2", "2026-11-12T22:00:00Z");
    snprintf(later, sizeof(later), "[%s,%s]", e1, e2);
}

static void test_start_accepts_zero() {
    oa3::VenConfig c = push_config();
    TEST_ASSERT_TRUE(rig.client.start(c));
    c.notify_poll_period_s = 1;
    TEST_ASSERT_FALSE(rig.client.start(c));
    c.notify_poll_period_s = 899;
    TEST_ASSERT_FALSE(rig.client.start(c));
}

static void test_no_requests_while_connected_72h() {
    rig.connect(push_config());
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, rig.client.status().retry_in_ms);
    TEST_ASSERT_EQUAL_UINT32(100, rig.client.ms_until_due());
    run(72 * 3600);
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(1, rig.mqtt.connects);
    ASSERT_NOTIFY(NotifyState::kConnected, rig.client.status().notify);
    ASSERT_STATUS(VenStatus::kOk, rig.client.status().status);
    for (size_t i = 0; i < rig.rec.statuses; ++i) {
        TEST_ASSERT_TRUE(rig.rec.status[i].status != VenStatus::kStale);
    }
}

static void test_no_suspend_cycles_exclusive_72h() {
    go_live_exclusive(push_config(true));
    const size_t connects = rig.mqtt.connects;
    run(72 * 3600);
    TEST_ASSERT_EQUAL_size_t(7, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(connects, rig.mqtt.connects);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.gets_while_tls_held);
    ASSERT_NOTIFY(NotifyState::kConnected, rig.client.status().notify);
    ASSERT_STATUS(VenStatus::kOk, rig.client.status().status);
}

// Failed polls while subscribed are not staleness; once the channel is down, staleness counts
// from when it went down.
static void test_stale_only_while_down() {
    oa3::VenConfig c = rig.config();
    rig.connect(c);
    rig.http.push(fakes::Response{nullptr, oa3::HttpResult::kOk, 500, "", true, 1000});
    run(5 * 3600);
    ASSERT_NOTIFY(NotifyState::kConnected, rig.client.status().notify);
    ASSERT_STATUS(VenStatus::kHttpError, rig.client.status().status);

    rig.mqtt.drop();
    rig.clk.advance_s(1);
    rig.client.loop();
    TEST_ASSERT_TRUE(rig.client.status().notify != NotifyState::kConnected);
    ASSERT_STATUS(VenStatus::kHttpError, rig.client.status().status);
    run(2640);
    ASSERT_STATUS(VenStatus::kHttpError, rig.client.status().status);
    run(60);
    ASSERT_STATUS(VenStatus::kStale, rig.client.status().status);
}

// Every loss: discovery again and a snapshot once subscribed.
static void test_break_snapshot_after_subscribe() {
    rig.connect(push_config());
    run(10 * 3600);
    rig.mqtt.drop();
    rig.http.push_ok(rig.events, "/events?");  // overdue by poll_period_s
    push_discovery();
    rig.http.push_ok(rig.events, "/events?");
    const size_t before = rig.http.gets_while_connected;
    rig.clk.advance_s(1);
    rig.client.loop();
    loop_until(NotifyState::kConnected);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.pending());
    TEST_ASSERT_NOT_NULL(strstr(rig.http.last_url(), "/events?"));
    TEST_ASSERT_EQUAL_size_t(before + 1, rig.http.gets_while_connected);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, rig.client.status().retry_in_ms);
    const size_t n = rig.http.requests;
    run(24 * 3600);
    TEST_ASSERT_EQUAL_size_t(n, rig.http.requests);
}

// The broker resumed the session, and the snapshot before the connect still came.
static void test_break_exclusive_session_kept() {
    go_live_exclusive(push_config(true));
    run(10 * 3600);
    rig.mqtt.drop();
    rig.http.push_ok(rig.events, "/events?");
    push_discovery();
    rig.http.push_ok(rig.events, "/events?");
    const size_t connects = rig.mqtt.connects;
    rig.clk.advance_s(1);
    rig.client.loop();
    loop_until(NotifyState::kConnected);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.pending());
    TEST_ASSERT_EQUAL_size_t(12, rig.http.requests);
    TEST_ASSERT_NOT_NULL(strstr(rig.http.last_url(), "/events?"));
    TEST_ASSERT_EQUAL_size_t(connects + 1, rig.mqtt.connects);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.gets_while_tls_held);
    run(24 * 3600);
    TEST_ASSERT_EQUAL_size_t(12, rig.http.requests);
}

// The broker lost the session: a snapshot before the connect and one after subscribing.
static void test_break_exclusive_session_lost() {
    go_live_exclusive(push_config(true));
    run(10 * 3600);
    rig.mqtt.drop();
    rig.mqtt.has_session = false;
    rig.http.push_ok(rig.events, "/events?");
    push_discovery();
    rig.http.push_ok(rig.events, "/events?");
    rig.http.push_ok(rig.events, "/events?");
    const size_t connects = rig.mqtt.connects;
    rig.clk.advance_s(1);
    rig.client.loop();
    loop_until(NotifyState::kConnected);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.pending());
    TEST_ASSERT_EQUAL_size_t(13, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(connects + 2, rig.mqtt.connects);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.gets_while_tls_held);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, rig.client.status().retry_in_ms);
    run(24 * 3600);
    TEST_ASSERT_EQUAL_size_t(13, rig.http.requests);
}

// A broker that keeps no sessions: one snapshot after the subscription, then the resume finds
// no session either, and polling every poll_period_s takes over instead of another snapshot.
static void test_gap_snapshot_once_per_connect() {
    rig.mqtt.keeps_sessions = false;
    oa3::VenConfig c = push_config(true);
    rig.start(c);
    push_discovery();
    rig.http.push_ok(rig.events, "/events?");
    rig.http.push_ok(rig.events, "/events?");
    loop_until(NotifyState::kConnected);
    TEST_ASSERT_EQUAL_size_t(7, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(2, rig.mqtt.connects);
    TEST_ASSERT_EQUAL_UINT32(900000, rig.client.status().retry_in_ms);
    run(840);
    TEST_ASSERT_EQUAL_size_t(7, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
}

// The gap close applies with a nonzero period too, once.
static void test_gap_snapshot_nonzero_period() {
    oa3::VenConfig c = rig.config();
    c.exclusive_tls = true;
    go_live_exclusive(c);
    TEST_ASSERT_EQUAL_size_t(2, rig.mqtt.connects);
    TEST_ASSERT_EQUAL_UINT32(3600000, rig.client.status().retry_in_ms);
}

static void test_window_entry_from_events() {
    with_later_event("2026-11-14T22:00:00Z");
    rig.http.push_ok(fakes::kProgramBody, "/programs/");
    rig.http.push_ok(later, "/events?");
    TEST_ASSERT_TRUE(rig.client.start(push_config()));
    rig.client.loop();
    rig.client.loop();
    push_discovery();
    rig.http.push_ok(later, "/events?");
    loop_until(NotifyState::kConnected);
    TEST_ASSERT_EQUAL_size_t(1, rig.client.event_count());
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
    // 22:00Z tomorrow comes within [now, now + 24 h) just after 22:00Z today.
    run(7200);
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
    rig.http.push_ok(later, "/events?");
    rig.clk.advance_ms(1);
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(7, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(1, rig.client.event_count());
    TEST_ASSERT_EQUAL_STRING("e2", rig.client.events()[0].id);
    run(24 * 3600);
    TEST_ASSERT_EQUAL_size_t(7, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
}

// An interval of an in-window event that starts beyond the window counts too.
static void test_window_entry_from_interval() {
    static const char body[] =
        R"([{"id":"e1","programID":"b54d3d1f-bc87-4e47-bbc1-2b95958283fc","objectType":"EVENT",)"
        R"("intervalPeriod":{"start":"2026-11-13T20:00:00Z","duration":"PT1H"},"intervals":[)"
        R"({"id":0,"payloads":[{"type":"PRICE","values":[0.1]}]},)"
        R"({"id":1,"intervalPeriod":{"start":"2026-11-14T20:30:00Z","duration":"PT1H"},)"
        R"("payloads":[{"type":"PRICE","values":[0.3]}]}]}])";
    snprintf(rig.events, sizeof(rig.events), "%s", body);
    rig.connect(push_config());
    TEST_ASSERT_EQUAL_UINT8(1, rig.client.events()[0].interval_count);
    run(1800);
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
    rig.http.push_ok(body, "/events?");
    run(60);
    TEST_ASSERT_EQUAL_size_t(7, rig.http.requests);
    TEST_ASSERT_EQUAL_UINT8(2, rig.client.events()[0].interval_count);
}

static void test_window_entry_from_notification(bool exclusive) {
    if (exclusive) {
        go_live_exclusive(push_config(true));
    } else {
        rig.connect(push_config());
    }
    const size_t n = rig.http.requests;
    const size_t connects = rig.mqtt.connects;
    fakes::event_json(obj, sizeof(obj), "e2", "2026-11-14T21:00:00Z", "0.2",
                      "2026-11-13T20:00:30Z");
    fakes::envelope(msg, sizeof(msg), "EVENT", "CREATE", obj);
    rig.mqtt.push_message(fakes::kEventsTopic, msg);
    run(60);
    TEST_ASSERT_EQUAL_size_t(1, rig.client.event_count());
    run(3540);
    TEST_ASSERT_EQUAL_size_t(n, rig.http.requests);
    with_later_event("2026-11-14T21:00:00Z");
    rig.http.push_ok(later, "/events?");
    run(60);
    if (exclusive) {
        rig.client.loop();
        TEST_ASSERT_EQUAL_size_t(connects + 1, rig.mqtt.connects);
        TEST_ASSERT_EQUAL_size_t(0, rig.http.gets_while_tls_held);
    }
    ASSERT_NOTIFY(NotifyState::kConnected, rig.client.status().notify);
    TEST_ASSERT_EQUAL_size_t(n + 1, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(1, rig.client.event_count());
    TEST_ASSERT_EQUAL_STRING("e2", rig.client.events()[0].id);
    run(24 * 3600);
    TEST_ASSERT_EQUAL_size_t(n + 1, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
}

static void test_window_entry_from_notification_shared() {
    test_window_entry_from_notification(false);
}
static void test_window_entry_from_notification_exclusive() {
    test_window_entry_from_notification(true);
}

// A start that comes within the window before the next poll is fetched then.
static void test_window_entry_before_next_poll() {
    with_later_event("2026-11-14T20:30:00Z");
    snprintf(rig.events, sizeof(rig.events), "%s", later);
    rig.connect(rig.config());
    TEST_ASSERT_EQUAL_size_t(1, rig.client.event_count());
    run(1800);
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
    rig.http.push_ok(later, "/events?");
    run(60);
    TEST_ASSERT_EQUAL_size_t(7, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(2, rig.client.event_count());
    rig.http.push_ok(later, "/events?");
    run(3600);
    TEST_ASSERT_EQUAL_size_t(8, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
}

// Without notifications polling alone covers it, as before.
static void test_no_window_entry_fetch_without_notifications() {
    with_later_event("2026-11-14T20:05:00Z");
    rig.http.push_ok(fakes::kProgramBody, "/programs/");
    rig.http.push_ok(later, "/events?");
    oa3::VenConfig c = rig.config();
    c.notifications = oa3::NotificationMode::kOff;
    TEST_ASSERT_TRUE(rig.client.start(c));
    rig.client.loop();
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(2, rig.http.requests);
    run(840);
    TEST_ASSERT_EQUAL_size_t(2, rig.http.requests);
    TEST_ASSERT_EQUAL_UINT32(60000, rig.client.ms_until_due());
}

// ms_until_due() wakes the port for the window fetch, here with MQTT unavailable.
static void test_window_entry_wakes_port() {
    with_later_event("2026-11-14T20:05:00Z");
    rig.http.push_ok(fakes::kProgramBody, "/programs/");
    rig.http.push_ok(later, "/events?");
    rig.http.push_status(404);
    TEST_ASSERT_TRUE(rig.client.start(rig.config()));
    for (int i = 0; i < 3; ++i) {
        rig.client.loop();
    }
    ASSERT_NOTIFY(NotifyState::kUnavailable, rig.client.status().notify);
    TEST_ASSERT_EQUAL_UINT32(300001, rig.client.ms_until_due());
    rig.clk.advance_ms(300000);
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(3, rig.http.requests);
    rig.http.push_ok(later, "/events?");
    rig.clk.advance_ms(1);
    rig.client.loop();
    TEST_ASSERT_EQUAL_size_t(4, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(2, rig.client.event_count());
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
}

// Revisions of future prices.

static char rev_obj[8][768];
static char rev_msg[8][1024];

// e1 at 20:00Z with two one-hour intervals, values v0 and v1. mdt may be nullptr.
static const char* two_intervals(char* buf, size_t size, const char* id, const char* start0,
                                 const char* start1, const char* v0, const char* v1,
                                 const char* mdt) {
    char m[96] = "";
    if (mdt != nullptr) {
        snprintf(m, sizeof(m), R"("modificationDateTime":"%s",)", mdt);
    }
    snprintf(buf, size,
             R"({"id":"%s","programID":"%s","objectType":"EVENT",%s)"
             R"("intervalPeriod":{"start":"%s","duration":"PT1H"},"intervals":[)"
             R"({"id":0,"payloads":[{"type":"PRICE","values":[%s]}]},)"
             R"({"id":1,"intervalPeriod":{"start":"%s","duration":"PT1H"},)"
             R"("payloads":[{"type":"PRICE","values":[%s]}]}]})",
             id, fakes::kProgramId, m, start0, v0, start1, v1);
    return buf;
}

static void push_note(int slot, const char* op, const char* object) {
    snprintf(rev_obj[slot], sizeof(rev_obj[slot]), "%s", object);
    fakes::envelope(rev_msg[slot], sizeof(rev_msg[slot]), "EVENT", op, rev_obj[slot]);
    rig.mqtt.push_message(fakes::kEventsTopic, rev_msg[slot]);
}

static void push_revision(int slot, const char* v0, const char* v1, const char* mdt) {
    char e[768];
    two_intervals(e, sizeof(e), "e1", "2026-11-13T20:00:00Z", "2026-11-13T21:00:00Z", v0, v1, mdt);
    push_note(slot, "UPDATE", e);
}

static void connect_two_intervals() {
    char e[768];
    two_intervals(e, sizeof(e), "e1", "2026-11-13T20:00:00Z", "2026-11-13T21:00:00Z", "0.1", "0.2",
                  "2026-11-12T22:00:00Z");
    snprintf(rig.events, sizeof(rig.events), "[%s]", e);
    rig.connect(push_config());
}

// The value of the second step of the schedule over [20:00Z, 22:00Z).
static const char* second_step() {
    static oa3::PriceStep steps[4];
    oa3::DateTime s{};
    oa3::Duration d{};
    oa3::TimeInterval w{};
    TEST_ASSERT_TRUE(oa3::parse_datetime("2026-11-13T20:00:00Z", &s));
    TEST_ASSERT_TRUE(oa3::parse_duration("PT2H", &d));
    TEST_ASSERT_TRUE(oa3::make_interval(s, d, &w));
    const size_t n = oa3::price_schedule(rig.client.events(), rig.client.event_count(), w, "PRICE",
                                         steps, 4, nullptr);
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_NOT_NULL(steps[1].value);
    return steps[1].value->text;
}

// (a) A future interval revised: stored, revision bumped, forecast changed, no price callback.
static void test_revision_of_future_interval() {
    connect_two_intervals();
    TEST_ASSERT_EQUAL_STRING("0.2", second_step());
    const uint32_t rev = rig.client.events_revision();
    const size_t prices = rig.rec.prices;
    push_revision(0, "0.1", "0.25", "2026-11-13T20:00:10Z");
    run(60);
    TEST_ASSERT_TRUE(rig.client.events_revision() != rev);
    TEST_ASSERT_EQUAL_STRING("0.25", second_step());
    TEST_ASSERT_EQUAL_size_t(prices, rig.rec.prices);
    TEST_ASSERT_EQUAL_STRING("0.1", rig.client.price().text);

    push_revision(1, "0.15", "0.25", "2026-11-13T20:00:20Z");
    run(60);
    TEST_ASSERT_EQUAL_size_t(prices + 1, rig.rec.prices);
    TEST_ASSERT_EQUAL_STRING("0.15", rig.rec.last_price().text);
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
}

// (b) Revisions apply in modificationDateTime order; an older or equal copy is ignored; one
// without modificationDateTime applies, as nothing orders it.
static void test_revisions_in_order() {
    connect_two_intervals();
    push_revision(0, "0.1", "0.3", "2026-11-13T20:00:30Z");
    push_revision(1, "0.1", "0.29", "2026-11-13T20:00:20Z");
    run(60);
    TEST_ASSERT_EQUAL_STRING("0.3", second_step());
    const uint32_t rev = rig.client.events_revision();
    push_revision(2, "0.1", "0.31", "2026-11-13T20:00:30Z");
    run(60);
    TEST_ASSERT_EQUAL_STRING("0.3", second_step());
    TEST_ASSERT_EQUAL_UINT32(rev, rig.client.events_revision());
    push_revision(3, "0.1", "0.4", nullptr);
    run(60);
    TEST_ASSERT_EQUAL_STRING("0.4", second_step());
    push_revision(4, "0.1", "0.5", "2026-11-13T20:00:40Z");
    run(60);
    TEST_ASSERT_EQUAL_STRING("0.5", second_step());
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
}

static void push_e2(int slot, const char* op, const char* start, const char* mdt) {
    char e[512];
    fakes::event_json(e, sizeof(e), "e2", start, "0.2", mdt);
    push_note(slot, op, e);
}

// Queues body for the next fetch and checks that it comes in the last 60 s of after_s,
// making the request count requests.
static void expect_fetch_at(const char* body, uint32_t after_s, size_t requests) {
    rig.http.push_ok(body, "/events?");
    run(after_s - 60);
    TEST_ASSERT_EQUAL_size_t(requests - 1, rig.http.requests);
    run(60);
    TEST_ASSERT_EQUAL_size_t(requests, rig.http.requests);
}

// (c) A revision moving a beyond-window start earlier makes the refresh earlier.
static void test_revision_moves_start_earlier() {
    rig.connect(push_config());
    push_e2(0, "CREATE", "2026-11-14T22:00:00Z", "2026-11-13T20:00:10Z");
    push_e2(1, "UPDATE", "2026-11-14T21:00:00Z", "2026-11-13T20:00:20Z");
    with_later_event("2026-11-14T21:00:00Z");
    expect_fetch_at(later, 3660, 7);
    TEST_ASSERT_EQUAL_STRING("e2", rig.client.events()[rig.client.event_count() - 1].id);
    run(24 * 3600);
    TEST_ASSERT_EQUAL_size_t(7, rig.http.requests);
}

// A revision moving it later: a fetch at the old start finds it still beyond the window, and
// the next refresh comes at the new start.
static void test_revision_moves_start_later() {
    rig.connect(push_config());
    push_e2(0, "CREATE", "2026-11-14T21:00:00Z", "2026-11-13T20:00:10Z");
    push_e2(1, "UPDATE", "2026-11-14T22:00:00Z", "2026-11-13T20:00:20Z");
    with_later_event("2026-11-14T22:00:00Z");
    expect_fetch_at(later, 3660, 7);
    TEST_ASSERT_EQUAL_size_t(0, rig.client.event_count());
    expect_fetch_at(later, 3600, 8);
    TEST_ASSERT_EQUAL_STRING("e2", rig.client.events()[0].id);
    run(24 * 3600);
    TEST_ASSERT_EQUAL_size_t(8, rig.http.requests);
}

// A stored event revised to start beyond the window leaves the events and is refreshed.
static void test_revision_moves_stored_event_beyond() {
    rig.connect(push_config());
    char e[512];
    fakes::event_json(e, sizeof(e), "e1", "2026-11-14T21:00:00Z", "0.1", "2026-11-13T20:00:10Z");
    push_note(0, "UPDATE", e);
    run(60);
    TEST_ASSERT_EQUAL_size_t(0, rig.client.event_count());
    static char body[1024];
    snprintf(body, sizeof(body), "[%s]", e);
    expect_fetch_at(body, 3600, 7);
    TEST_ASSERT_EQUAL_size_t(1, rig.client.event_count());
}

// A DELETE of the event that set the earliest start leaves the refresh in place: at worst one
// fetch more, never one missed.
static void test_delete_keeps_refresh() {
    rig.connect(push_config());
    push_e2(0, "CREATE", "2026-11-14T21:00:00Z", "2026-11-13T20:00:10Z");
    push_e2(1, "CREATE", "2026-11-14T23:00:00Z", "2026-11-13T20:00:10Z");
    run(60);
    char e3[512];
    fakes::event_json(e3, sizeof(e3), "e3", "2026-11-14T23:00:00Z", "0.3", "2026-11-13T20:00:10Z");
    push_note(2, "CREATE", e3);
    push_note(3, "DELETE", R"({"id":"e2"})");
    run(60);
    static char body[1100];
    char e1[512];
    fakes::event_json(e1, sizeof(e1), "e1", "2026-11-13T20:00:00Z", "0.1", "2026-11-12T22:00:00Z");
    snprintf(body, sizeof(body), "[%s,%s]", e1, e3);
    expect_fetch_at(body, 3540, 7);
    expect_fetch_at(body, 7200, 8);
    TEST_ASSERT_EQUAL_STRING("e3", rig.client.events()[0].id);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
}

// (d) A revision as DELETE of the old id and CREATE of a new one.
static void test_revision_as_delete_and_create() {
    rig.connect(push_config());
    char e[512];
    fakes::event_json(e, sizeof(e), "e1b", "2026-11-13T20:00:00Z", "0.4", "2026-11-13T20:00:10Z");
    push_note(0, "DELETE", R"({"id":"e1"})");
    push_note(1, "CREATE", e);
    run(60);
    TEST_ASSERT_EQUAL_size_t(1, rig.client.event_count());
    TEST_ASSERT_EQUAL_STRING("e1b", rig.client.events()[0].id);
    TEST_ASSERT_EQUAL_STRING("0.4", rig.client.price().text);
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
}

// Truncation: OA3_MAX_EVENTS (4) slots, and hourly events from 20:00Z.

static char hourly[2][2048];

// count one-hour events h<i> at 20:00Z + i hours, i from first.
static const char* hourly_body(int which, int first, int count) {
    char* buf = hourly[which];
    size_t n = static_cast<size_t>(snprintf(buf, sizeof(hourly[0]), "["));
    for (int i = first; i < first + count; ++i) {
        char id[16], start[48], e[512];
        snprintf(id, sizeof(id), "h%d", i);
        snprintf(start, sizeof(start), "2026-11-1%dT%02d:00:00Z", 3 + (20 + i) / 24, (20 + i) % 24);
        fakes::event_json(e, sizeof(e), id, start, "0.1", "2026-11-12T22:00:00Z");
        n += static_cast<size_t>(snprintf(buf + n, sizeof(hourly[0]) - n, "%s%s",
                                          i == first ? "" : ",", e));
    }
    snprintf(buf + n, sizeof(hourly[0]) - n, "]");
    TEST_ASSERT_TRUE(n + 1 < sizeof(hourly[0]));
    return buf;
}

static void test_truncated_fetch_refreshed(bool exclusive) {
    TEST_ASSERT_EQUAL_INT(4, OA3_MAX_EVENTS);
    snprintf(rig.events, sizeof(rig.events), "%s", hourly_body(0, 0, 5));
    if (exclusive) {
        go_live_exclusive(push_config(true));
    } else {
        rig.connect(push_config());
    }
    const size_t n = rig.http.requests;
    const size_t connects = rig.mqtt.connects;
    ASSERT_STATUS(VenStatus::kTruncated, rig.client.status().status);
    TEST_ASSERT_EQUAL_size_t(4, rig.client.event_count());
    // h0 ends at 21:00Z, freeing a slot for h4.
    run(3540);
    TEST_ASSERT_EQUAL_size_t(n, rig.http.requests);
    rig.http.push_ok(hourly_body(1, 0, 5), "/events?");
    run(60);
    if (exclusive) {
        rig.client.loop();
        TEST_ASSERT_EQUAL_size_t(connects + 1, rig.mqtt.connects);
        TEST_ASSERT_EQUAL_size_t(0, rig.http.gets_while_tls_held);
    }
    ASSERT_NOTIFY(NotifyState::kConnected, rig.client.status().notify);
    TEST_ASSERT_EQUAL_size_t(n + 1, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(4, rig.client.event_count());
    TEST_ASSERT_EQUAL_STRING("h4", rig.client.events()[3].id);
    ASSERT_STATUS(VenStatus::kOk, rig.client.status().status);
    run(24 * 3600);
    TEST_ASSERT_EQUAL_size_t(n + 1, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
}

static void test_truncated_fetch_refreshed_shared() { test_truncated_fetch_refreshed(false); }
static void test_truncated_fetch_refreshed_exclusive() { test_truncated_fetch_refreshed(true); }

static void push_hourly_create(int slot, int i) {
    char e[512], start[48], id[16];
    snprintf(id, sizeof(id), "h%d", i);
    snprintf(start, sizeof(start), "2026-11-1%dT%02d:00:00Z", 3 + (20 + i) / 24, (20 + i) % 24);
    fakes::event_json(e, sizeof(e), id, start, "0.1", "2026-11-13T20:00:10Z");
    push_note(slot, "CREATE", e);
}

// A CREATE that finds no slot is refreshed when the earliest held event ends.
static void test_truncating_notification_refreshed() {
    snprintf(rig.events, sizeof(rig.events), "%s", hourly_body(0, 0, 4));
    rig.connect(push_config());
    ASSERT_STATUS(VenStatus::kOk, rig.client.status().status);
    push_hourly_create(0, 4);
    run(60);
    ASSERT_STATUS(VenStatus::kTruncated, rig.client.status().status);
    TEST_ASSERT_EQUAL_size_t(4, rig.client.event_count());
    run(3480);
    TEST_ASSERT_EQUAL_size_t(6, rig.http.requests);
    rig.http.push_ok(hourly_body(1, 0, 5), "/events?");
    run(60);
    TEST_ASSERT_EQUAL_size_t(7, rig.http.requests);
    TEST_ASSERT_EQUAL_STRING("h4", rig.client.events()[3].id);
    run(24 * 3600);
    TEST_ASSERT_EQUAL_size_t(7, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
}

// While truncated, a DELETE frees a slot at once.
static void test_delete_while_truncated_refreshed() {
    snprintf(rig.events, sizeof(rig.events), "%s", hourly_body(0, 0, 5));
    rig.connect(push_config());
    push_note(0, "DELETE", R"({"id":"h2"})");
    rig.http.push_ok(hourly_body(1, 0, 5), "/events?");
    run(60);
    TEST_ASSERT_EQUAL_size_t(7, rig.http.requests);
    TEST_ASSERT_EQUAL_size_t(4, rig.client.event_count());
    TEST_ASSERT_EQUAL_size_t(0, rig.http.unexpected);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_start_accepts_zero);
    RUN_TEST(test_no_requests_while_connected_72h);
    RUN_TEST(test_no_suspend_cycles_exclusive_72h);
    RUN_TEST(test_stale_only_while_down);
    RUN_TEST(test_break_snapshot_after_subscribe);
    RUN_TEST(test_break_exclusive_session_kept);
    RUN_TEST(test_break_exclusive_session_lost);
    RUN_TEST(test_gap_snapshot_once_per_connect);
    RUN_TEST(test_gap_snapshot_nonzero_period);
    RUN_TEST(test_window_entry_from_events);
    RUN_TEST(test_window_entry_from_interval);
    RUN_TEST(test_window_entry_from_notification_shared);
    RUN_TEST(test_window_entry_from_notification_exclusive);
    RUN_TEST(test_window_entry_before_next_poll);
    RUN_TEST(test_no_window_entry_fetch_without_notifications);
    RUN_TEST(test_window_entry_wakes_port);
    RUN_TEST(test_revision_of_future_interval);
    RUN_TEST(test_revisions_in_order);
    RUN_TEST(test_revision_moves_start_earlier);
    RUN_TEST(test_revision_moves_start_later);
    RUN_TEST(test_revision_moves_stored_event_beyond);
    RUN_TEST(test_delete_keeps_refresh);
    RUN_TEST(test_revision_as_delete_and_create);
    RUN_TEST(test_truncated_fetch_refreshed_shared);
    RUN_TEST(test_truncated_fetch_refreshed_exclusive);
    RUN_TEST(test_truncating_notification_refreshed);
    RUN_TEST(test_delete_while_truncated_refreshed);
    return UNITY_END();
}
