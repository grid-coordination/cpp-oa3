#pragma once

// Fakes for the oa3_client suites: a scripted HttpTransport, a scripted MqttSession and a
// settable Clock.

#include <stdio.h>
#include <string.h>

#include <oa3/client/clock.h>
#include <oa3/client/http_transport.h>
#include <oa3/client/limits.h>
#include <oa3/client/mqtt_session.h>
#include <oa3/client/ven_client.h>
#include <oa3/model/byte_reader.h>
#include <oa3/model/time.h>
#include <unity.h>

namespace fakes {

struct Response {
    const char* url_contains;  // nullptr: any URL
    oa3::HttpResult result;
    int status;
    const char* body;  // nullptr: ""
    bool call_handler;
    unsigned repeat;  // served this many times; 0 counts as 1
};

class FakeTransport final : public oa3::HttpTransport {
public:
    static constexpr size_t kMaxQueue = 32;
    static constexpr size_t kMaxUrls = 256;

    void reset() {
        _queued = 0;
        _next = 0;
        _served = 0;
        requests = 0;
        unexpected = 0;
        header_count = 0;
        timeout_ms = 0;
        gets_while_connected = 0;
        gets_while_tls_held = 0;
    }

    void push(const Response& r) {
        TEST_ASSERT_TRUE(_queued < kMaxQueue);
        _queue[_queued++] = r;
    }

    void push_ok(const char* body, const char* url_contains = nullptr, unsigned repeat = 1) {
        push(Response{url_contains, oa3::HttpResult::kOk, 200, body, true, repeat});
    }

    void push_status(int status, const char* body = "") {
        push(Response{nullptr, oa3::HttpResult::kOk, status, body, true, 1});
    }

    void push_result(oa3::HttpResult result) {
        push(Response{nullptr, result, 0, "", false, 1});
    }

    size_t pending() const { return _queued - _next; }

    const char* url(size_t i) const { return urls[i % kMaxUrls]; }
    const char* last_url() const { return requests > 0 ? url(requests - 1) : ""; }

    oa3::HttpResult get(const oa3::HttpRequest& request, oa3::HttpResponseFn on_response,
                        void* ctx) override {
        snprintf(urls[requests % kMaxUrls], sizeof(urls[0]), "%s", request.url);
        ++requests;
        if (mqtt != nullptr && mqtt->connected()) {
            ++gets_while_connected;
        }
        if (tls_held != nullptr && *tls_held) {
            ++gets_while_tls_held;
        }
        timeout_ms = request.timeout_ms;
        header_count = request.header_count < 4 ? request.header_count : 4;
        for (size_t i = 0; i < header_count; ++i) {
            snprintf(header_names[i], sizeof(header_names[i]), "%s", request.headers[i].name);
            snprintf(header_values[i], sizeof(header_values[i]), "%s", request.headers[i].value);
        }
        if (_next >= _queued) {
            ++unexpected;
            return oa3::HttpResult::kIoError;
        }
        const Response r = _queue[_next];
        const unsigned times = r.repeat == 0 ? 1 : r.repeat;
        if (++_served >= times) {
            ++_next;
            _served = 0;
        }
        if (r.url_contains != nullptr && strstr(request.url, r.url_contains) == nullptr) {
            ++unexpected;
        }
        if (r.call_handler) {
            const char* body = r.body ? r.body : "";
            oa3::BufferReader reader(body, strlen(body));
            on_response(ctx, r.status, reader);
        }
        return r.result;
    }

    size_t requests = 0;
    size_t unexpected = 0;
    char urls[kMaxUrls][OA3_URL_MAX + 1] = {};
    size_t header_count = 0;
    char header_names[4][32] = {};
    char header_values[4][64] = {};
    uint32_t timeout_ms = 0;
    // When set, get() counts the requests made while this session is connected.
    const oa3::MqttSession* mqtt = nullptr;
    size_t gets_while_connected = 0;
    // When set, get() counts the requests made while *tls_held.
    const bool* tls_held = nullptr;
    size_t gets_while_tls_held = 0;

private:
    Response _queue[kMaxQueue] = {};
    size_t _queued = 0;
    size_t _next = 0;
    unsigned _served = 0;
};

// An MqttSession whose broker keeps a session opened with clean_session false, unless
// keeps_sessions is false. Results are scripted per call kind; unscripted calls succeed.
class FakeMqttSession final : public oa3::MqttSession {
public:
    static constexpr size_t kMaxCalls = 128;
    static constexpr size_t kMaxQueue = 16;

    void reset() {
        connected_ = false;
        tls_held = false;
        has_session = false;
        keeps_sessions = true;
        drop_next = false;
        calls = 0;
        connects = 0;
        disconnects = 0;
        polls = 0;
        _connect_q = _connect_next = 0;
        _sub_q = _sub_next = 0;
        _unsub_q = _unsub_next = 0;
        _messages = 0;
        last = oa3::MqttConnectOptions{};
        last_host[0] = '\0';
        last_client_id[0] = '\0';
    }

    void push_connect(oa3::MqttResult r) { _connect_r[_connect_q++ % kMaxQueue] = r; }
    void push_subscribe(oa3::MqttResult r) { _sub_r[_sub_q++ % kMaxQueue] = r; }
    void push_unsubscribe(oa3::MqttResult r) { _unsub_r[_unsub_q++ % kMaxQueue] = r; }

    // json must outlive the poll that delivers it.
    void push_message(const char* topic, const char* json, bool retained = false,
                      bool too_large = false) {
        TEST_ASSERT_TRUE(_messages < kMaxQueue);
        Message& m = _queue[_messages++];
        snprintf(m.topic, sizeof(m.topic), "%s", topic);
        m.json = json;
        m.retained = retained;
        m.too_large = too_large;
    }
    // The next poll delivers what is queued, then reports kConnectionLost.
    void drop() { drop_next = true; }

    oa3::MqttResult connect(const oa3::MqttConnectOptions& o, bool* session_present) override {
        record("C");
        ++connects;
        last = o;
        snprintf(last_host, sizeof(last_host), "%s", o.host ? o.host : "");
        snprintf(last_client_id, sizeof(last_client_id), "%s", o.client_id ? o.client_id : "");
        last.host = last_host;
        last.client_id = last_client_id;
        connected_ = false;
        tls_held = o.tls;
        const oa3::MqttResult r =
            _connect_next < _connect_q ? _connect_r[_connect_next++ % kMaxQueue] : oa3::MqttResult::kOk;
        if (r != oa3::MqttResult::kOk) {
            return r;
        }
        *session_present = !o.clean_session && keeps_sessions && has_session;
        has_session = !o.clean_session && keeps_sessions;
        connected_ = true;
        return r;
    }
    void disconnect() override {
        record("D");
        ++disconnects;
        connected_ = false;
        tls_held = false;
    }
    oa3::MqttResult subscribe(const char* filter, uint8_t qos, uint32_t) override {
        record("S:", filter);
        last_qos = qos;
        if (!connected_) {
            return oa3::MqttResult::kNotConnected;
        }
        const oa3::MqttResult r =
            _sub_next < _sub_q ? _sub_r[_sub_next++ % kMaxQueue] : oa3::MqttResult::kOk;
        if (r == oa3::MqttResult::kOk) {
            ++subscribed;
        }
        return r;
    }
    oa3::MqttResult unsubscribe(const char* filter, uint32_t) override {
        record("U:", filter);
        if (!connected_) {
            return oa3::MqttResult::kNotConnected;
        }
        return _unsub_next < _unsub_q ? _unsub_r[_unsub_next++ % kMaxQueue] : oa3::MqttResult::kOk;
    }
    oa3::MqttResult poll(uint32_t, oa3::MqttMessageFn on_message, void* ctx) override {
        ++polls;
        if (!connected_) {
            return oa3::MqttResult::kNotConnected;
        }
        const size_t n = _messages;
        _messages = 0;
        for (size_t i = 0; i < n; ++i) {
            const Message& q = _queue[i];
            oa3::MqttMessage m{q.topic, q.too_large ? nullptr : q.json,
                               q.json ? strlen(q.json) : 0, 1, q.retained, q.too_large};
            on_message(ctx, m);
        }
        if (drop_next) {
            drop_next = false;
            connected_ = false;
            return oa3::MqttResult::kConnectionLost;
        }
        return oa3::MqttResult::kOk;
    }
    bool connected() const override { return connected_; }

    const char* call(size_t i) const { return _calls[i % kMaxCalls]; }
    // The number of calls recorded that equal s.
    size_t count(const char* s) const {
        size_t n = 0;
        for (size_t i = 0; i < calls && i < kMaxCalls; ++i) {
            n += strcmp(_calls[i], s) == 0 ? 1 : 0;
        }
        return n;
    }
    size_t queued() const { return _messages; }

    bool connected_ = false;
    // Like a port that frees its TLS session only in disconnect(), even after a failure.
    bool tls_held = false;
    bool has_session = false;
    bool keeps_sessions = true;
    bool drop_next = false;
    size_t calls = 0;
    size_t connects = 0;
    size_t disconnects = 0;
    size_t polls = 0;
    size_t subscribed = 0;
    uint8_t last_qos = 0;
    oa3::MqttConnectOptions last = {};
    char last_host[128] = {};
    char last_client_id[64] = {};

private:
    struct Message {
        char topic[200];
        const char* json;
        bool retained;
        bool too_large;
    };

    void record(const char* kind, const char* topic = "") {
        snprintf(_calls[calls++ % kMaxCalls], sizeof(_calls[0]), "%s%s", kind, topic);
    }

    char _calls[kMaxCalls][200] = {};
    oa3::MqttResult _connect_r[kMaxQueue] = {};
    size_t _connect_q = 0, _connect_next = 0;
    oa3::MqttResult _sub_r[kMaxQueue] = {};
    size_t _sub_q = 0, _sub_next = 0;
    oa3::MqttResult _unsub_r[kMaxQueue] = {};
    size_t _unsub_q = 0, _unsub_next = 0;
    Message _queue[kMaxQueue] = {};
    size_t _messages = 0;
};

class FakeClock final : public oa3::Clock {
public:
    bool now(oa3::DateTime* out) override {
        if (!valid) {
            return false;
        }
        *out = wall;
        return true;
    }
    uint32_t monotonic_ms() override { return ms; }

    void set(const char* iso) {
        TEST_ASSERT_TRUE(oa3::parse_datetime(iso, &wall));
        valid = true;
    }
    // Advances both clocks.
    void advance_ms(uint32_t d) {
        ms += d;
        wall.seconds += d / 1000;
        uint32_t nanos = wall.nanos + (d % 1000) * 1000000u;
        if (nanos >= 1000000000u) {
            nanos -= 1000000000u;
            ++wall.seconds;
        }
        wall.nanos = nanos;
    }
    void advance_s(uint32_t s) { advance_ms(s * 1000u); }

    bool valid = false;
    oa3::DateTime wall = {};
    uint32_t ms = 0;
};

// Records the callbacks a VenClient fires.
struct Recorder {
    static constexpr size_t kMax = 64;
    size_t statuses = 0;
    oa3::VenStatusInfo status[kMax];
    size_t prices = 0;
    oa3::PriceInfo price[kMax];

    void reset() {
        statuses = 0;
        prices = 0;
    }
    const oa3::VenStatusInfo& last_status() const { return status[(statuses - 1) % kMax]; }
    const oa3::PriceInfo& last_price() const { return price[(prices - 1) % kMax]; }

    static void on_status(void* ctx, const oa3::VenStatusInfo& s) {
        Recorder* r = static_cast<Recorder*>(ctx);
        r->status[r->statuses++ % kMax] = s;
    }
    static void on_price(void* ctx, const oa3::PriceInfo& p) {
        Recorder* r = static_cast<Recorder*>(ctx);
        r->price[r->prices++ % kMax] = p;
    }
};

inline size_t load_file(const char* path, char* buf, size_t size) {
    FILE* f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    TEST_ASSERT_TRUE(n < size - 1);
    buf[n] = '\0';
    return n;
}

// The fixtures' program.
constexpr const char* kProgramName = "EELEC-013532223";
constexpr const char* kProgramId = "b54d3d1f-bc87-4e47-bbc1-2b95958283fc";
constexpr const char* kBaseUrl = "https://price.grid-coordination.energy/openadr3/3.1.0";

// A /programs page of count programs named <prefix>-<first + i>, ids p-<first + i>; when
// match_at < count, that element is named name instead. Returns buf.
inline const char* programs_page(char* buf, size_t size, const char* prefix, unsigned first,
                                 unsigned count, unsigned match_at = UINT32_MAX,
                                 const char* name = nullptr) {
    size_t n = 0;
    n += static_cast<size_t>(snprintf(buf + n, size - n, "["));
    for (unsigned i = 0; i < count; ++i) {
        char pname[64];
        if (i == match_at) {
            snprintf(pname, sizeof(pname), "%s", name);
        } else {
            snprintf(pname, sizeof(pname), "%s-%u", prefix, first + i);
        }
        n += static_cast<size_t>(snprintf(
            buf + n, size - n,
            "%s{\"id\":\"%s\",\"programName\":\"%s\",\"objectType\":\"PROGRAM\","
            "\"payloadDescriptors\":[{\"objectType\":\"EVENT_PAYLOAD_DESCRIPTOR\","
            "\"payloadType\":\"PRICE\",\"units\":\"KWH\",\"currency\":\"USD\"}]}",
            i == 0 ? "" : ",", i == match_at ? kProgramId : pname, pname));
        TEST_ASSERT_TRUE(n < size);
    }
    snprintf(buf + n, size - n, "]");
    TEST_ASSERT_TRUE(n + 1 < size);
    return buf;
}

// MQTT discovery bodies as the price server sends them (2026-11-12).
constexpr const char* kProgramBody =
    R"({"id":"b54d3d1f-bc87-4e47-bbc1-2b95958283fc","programName":"EELEC-013532223","objectType":"PROGRAM",)"
    R"("payloadDescriptors":[{"payloadType":"PRICE","units":"KWH","currency":"USD"}]})";
constexpr const char* kNotifiersBody =
    R"({"WEBHOOK":false,"MQTT":{"URIS":["mqtt://mqtt.grid-coordination.energy:1883","mqtts://mqtt.grid-coordination.energy:8883"],"serialization":"JSON","authentication":{"method":"ANONYMOUS"}}})";
constexpr const char* kEventTopicsBody =
    R"({"topics":{"UPDATE":"OpenADR\/3.1.0\/programs\/b54d3d1f-bc87-4e47-bbc1-2b95958283fc\/events\/update","DELETE":"OpenADR\/3.1.0\/programs\/b54d3d1f-bc87-4e47-bbc1-2b95958283fc\/events\/delete","ALL":"OpenADR\/3.1.0\/programs\/b54d3d1f-bc87-4e47-bbc1-2b95958283fc\/events\/+","CREATE":"OpenADR\/3.1.0\/programs\/b54d3d1f-bc87-4e47-bbc1-2b95958283fc\/events\/create"}})";
constexpr const char* kProgramTopicsBody =
    R"({"topics":{"UPDATE":"OpenADR\/3.1.0\/programs\/b54d3d1f-bc87-4e47-bbc1-2b95958283fc\/update","DELETE":"OpenADR\/3.1.0\/programs\/b54d3d1f-bc87-4e47-bbc1-2b95958283fc\/delete","ALL":"OpenADR\/3.1.0\/programs\/b54d3d1f-bc87-4e47-bbc1-2b95958283fc\/+"}})";
constexpr const char* kEventsAll = "OpenADR/3.1.0/programs/b54d3d1f-bc87-4e47-bbc1-2b95958283fc/events/+";
constexpr const char* kProgramAll = "OpenADR/3.1.0/programs/b54d3d1f-bc87-4e47-bbc1-2b95958283fc/+";
constexpr const char* kEventsTopic =
    "OpenADR/3.1.0/programs/b54d3d1f-bc87-4e47-bbc1-2b95958283fc/events/update";

// One one-hour PRICE event of the fixture program, starting at start. mdt and targets
// (a JSON array) may be nullptr.
inline const char* event_json(char* buf, size_t size, const char* id, const char* start,
                              const char* value, const char* mdt = nullptr,
                              const char* targets = nullptr, const char* program_id = kProgramId) {
    char extra[256] = "";
    size_t n = 0;
    if (mdt != nullptr) {
        n += static_cast<size_t>(
            snprintf(extra + n, sizeof(extra) - n, R"("modificationDateTime":"%s",)", mdt));
    }
    if (targets != nullptr) {
        snprintf(extra + n, sizeof(extra) - n, R"("targets":%s,)", targets);
    }
    snprintf(buf, size,
             R"({"id":"%s","programID":"%s","objectType":"EVENT",%s"intervalPeriod":{"start":"%s","duration":"PT1H"},)"
             R"("intervals":[{"id":0,"payloads":[{"type":"PRICE","values":[%s]}]}]})",
             id, program_id, extra, start, value);
    return buf;
}

inline const char* envelope(char* buf, size_t size, const char* object_type, const char* operation,
                            const char* object) {
    snprintf(buf, size, R"({"objectType":"%s","operation":"%s","object":%s})", object_type,
             operation, object);
    return buf;
}

inline oa3::VenStorage with_notifications(oa3::VenStorage s, oa3::NotificationStorage* n) {
    s.notifications = n;
    return s;
}

// The client under test, its fakes and a flow that takes it to NotifyState::kConnected.
struct MqttRig {
    FakeTransport http;
    FakeClock clk;
    Recorder rec;
    FakeMqttSession mqtt;
    oa3::StaticVenStorage<> storage;
    oa3::NotificationStorage notes;
    oa3::VenClient client;
    char events[2048];

    MqttRig() : client(http, clk, with_notifications(storage.storage(), &notes), &mqtt) {}

    // The default /events body: one event e1 at 20:00Z, price 0.1.
    void reset() {
        client.stop();
        http.reset();
        mqtt.reset();
        http.mqtt = &mqtt;
        http.tls_held = &mqtt.tls_held;
        clk = FakeClock{};
        clk.set("2026-11-13T20:00:00Z");
        clk.ms = 5000;
        rec.reset();
        char e[512];
        event_json(e, sizeof(e), "e1", "2026-11-13T20:00:00Z", "0.1",
                   "2026-11-12T22:00:00Z");
        snprintf(events, sizeof(events), "[%s]", e);
    }

    oa3::VenConfig config() const {
        oa3::VenConfig c{};
        c.base_url = kBaseUrl;
        c.program_id = kProgramId;
        c.poll_period_s = 900;
        c.notify_poll_period_s = 3600;
        c.notifications = oa3::NotificationMode::kMqtt;
        c.mqtt_client_id = "ven-1";
        c.on_status = Recorder::on_status;
        c.on_price_change = Recorder::on_price;
        c.ctx = const_cast<Recorder*>(&rec);
        return c;
    }

    // Resolves the program and makes the first /events fetch: two requests.
    void start(const oa3::VenConfig& c) {
        http.push_ok(kProgramBody, "/programs/");
        http.push_ok(events, "/events?");
        TEST_ASSERT_TRUE(client.start(c));
        client.loop();
        client.loop();
        TEST_ASSERT_EQUAL_size_t(2, http.requests);
        TEST_ASSERT_TRUE(client.status().has_data);
    }

    // From start() to kConnected with the price server's discovery bodies.
    void connect(const oa3::VenConfig& c) {
        start(c);
        http.push_ok(kNotifiersBody, "/notifiers");
        http.push_ok(kEventTopicsBody, "/notifiers/mqtt/topics/programs/");
        http.push_ok(kProgramTopicsBody, "/notifiers/mqtt/topics/programs/");
        http.push_ok(events, "/events?");
        for (int i = 0; i < 8 && client.status().notify != oa3::NotifyState::kConnected; ++i) {
            client.loop();
        }
        TEST_ASSERT_EQUAL_STRING("CONNECTED", oa3::to_string(client.status().notify));
        TEST_ASSERT_EQUAL_size_t(0, http.pending());
        TEST_ASSERT_EQUAL_size_t(0, http.unexpected);
    }
};

}  // namespace fakes
