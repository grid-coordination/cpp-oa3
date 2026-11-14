#pragma once

#include <stddef.h>
#include <stdint.h>

#include <oa3/client/clock.h>
#include <oa3/client/http_transport.h>
#include <oa3/client/limits.h>
#include <oa3/client/log.h>
#include <oa3/client/mqtt_session.h>
#include <oa3/model/entities.h>
#include <oa3/model/json_arena.h>
#include <oa3/model/limits.h>
#include <oa3/model/notifiers.h>
#include <oa3/model/parse.h>
#include <oa3/model/time.h>
#include <oa3/model/time_interval.h>

namespace oa3 {

enum class VenStatus : uint8_t {
    kStopped,
    kWaitingForClock,
    kResolvingProgram,
    kProgramNotFound,
    kOk,
    kTruncated,  // the last fetch succeeded with more events in the window than fit
    kStale,      // holding data, but no successful fetch for stale_after_polls poll periods
    kTransportError,
    kHttpError,
    kParseError,
};

const char* to_string(VenStatus s);

// The MQTT side. Everything but kOff and kUnavailable comes only with NotificationMode::kMqtt
// once the program is resolved.
enum class NotifyState : uint8_t {
    kOff,
    kDiscovering,   // fetching /notifiers or topics
    kUnavailable,   // no MQTT binding, no usable URI, or an auth method the client lacks;
                    // discovery is retried every poll_period_s
    kConnecting,    // topics in hand; connecting, subscribing, or snapshotting /events
    kConnected,     // subscribed; events are polled every notify_poll_period_s
    kSuspended,     // exclusive_tls: closed for an HTTPS request, resumed after it
    kDisconnected,  // lost or failed; a new cycle starts after backoff
};

const char* to_string(NotifyState s);

struct VenStatusInfo {
    VenStatus status;
    HttpResult http_result;    // of the last request; kOk before the first
    int http_status;           // of the last response; 0 when none
    ParseResult parse_result;  // of the last parse; kOk before the first
    uint16_t consecutive_failures;
    uint32_t retry_in_ms;  // until the next request, as of this report
    bool has_data;         // events from a successful fetch are held
    NotifyState notify;
    MqttResult mqtt_result;  // of the last MQTT call; kOk before the first
    uint16_t mqtt_failures;  // consecutive failed connect cycles and losses
    uint32_t notifications;  // event and program notifications applied since start()
};

// The value of the configured payload type in force now. Text fields are copies.
struct PriceInfo {
    bool active;
    ValueKind kind;
    double number;                         // kNumber; kPoint: x
    char text[OA3_VALUE_TEXT_MAX + 1];     // kNumber: the wire literal; kString; kBool:
                                           // "true" or "false"; kPoint: ""
    char units[OA3_UNITS_MAX + 1];         // from the event's payloadDescriptors, else the
    char currency[OA3_CURRENCY_MAX + 1];   // program's; "" when neither has one
    char event_id[OA3_ID_MAX + 1];
    int32_t interval_id;
    TimeInterval span;
    bool has_next_change;
    DateTime next_change;  // the first span boundary after now, active or not
};

typedef void (*PriceChangeFn)(void* ctx, const PriceInfo& price);
typedef void (*VenStatusFn)(void* ctx, const VenStatusInfo& status);

enum class NotificationMode : uint8_t { kOff, kMqtt };

// start() copies the strings, so they need not outlive it. Set fields on a value-initialized
// config (VenConfig c{}; c.base_url = ...); fields added later keep this working.
struct VenConfig {
    const char* base_url = nullptr;  // up to the spec version segment, e.g.
                                     // https://price.grid-coordination.energy/openadr3/3.1.0
    // The spec requires HTTPS and MQTTS (Definition.md, HTTPS/TLS and MQTT). This admits an
    // http:// base URL and mqtt:// or tcp:// brokers, the latter only when no TLS URI is
    // offered; the client logs a warning the first time after start() that it uses each.
    bool allow_plaintext = false;
    // At least one. With program_id, GET /programs/<id>, and a program_name must then match;
    // otherwise program_name is resolved through /programs pages.
    const char* program_name = nullptr;
    const char* program_id = nullptr;
    // Adds the price server's programName query to /programs; a 4xx answer to it makes the
    // client drop it and page through /programs plainly.
    bool send_program_name = true;
    // Sent as targets query parameters; when any are given, events that targets_match()
    // rejects are dropped as well.
    const char* const* targets = nullptr;
    size_t target_count = 0;
    const char* payload_type = "PRICE";
    uint32_t poll_period_s = 900;
    Duration horizon = {0, 0, 0, 0, 24, 0, 0, 0, 0};  // parse window [now, now + horizon)
    uint32_t backoff_min_s = 10;
    uint32_t backoff_max_s = 600;
    uint16_t stale_after_polls = 3;  // 0: never stale; times poll_period_s below 2^31 ms
    uint32_t http_timeout_ms = 15000;

    // kMqtt needs an MqttSession and VenStorage::notification. After the program resolves:
    // GET /notifiers, the program's events and program topics, connect, subscribe, and
    // snapshot /events; again on every connect that does not resume a session.
    NotificationMode notifications = NotificationMode::kOff;
    // The poll period while subscribed; at least poll_period_s.
    uint32_t notify_poll_period_s = 3600;
    // At most one TLS session at a time (the classic ESP32): an MQTTS session is closed
    // before each HTTPS request and resumed after it, with clean_session false and QoS 1 so
    // the broker queues what arrives meanwhile. Until a resume finds the broker kept the
    // session, events are polled every poll_period_s.
    bool exclusive_tls = false;
    const char* mqtt_client_id = nullptr;  // required with kMqtt
    // Used instead of the /notifiers URIs, e.g. for a VTN that advertises an unreachable one.
    const char* mqtt_uri = nullptr;
    bool mqtt_verify = true;
    const char* mqtt_ca_pem = nullptr;  // not copied: must outlive the client
    uint16_t mqtt_keepalive_s = 60;
    uint32_t mqtt_timeout_ms = 15000;
    // While subscribed, ms_until_due() is at most this, so the port's loop() picks messages
    // up promptly.
    uint32_t mqtt_poll_ms = 100;
    // Also subscribes to the program's own topics: UPDATE refreshes the program, DELETE
    // resolves it again.
    bool follow_program = true;

    PriceChangeFn on_price_change = nullptr;
    VenStatusFn on_status = nullptr;
    void* ctx = nullptr;  // for both callbacks
    LogSink log = {};
};

// The client's working memory for NotificationMode::kMqtt. A notification is parsed into
// notification before it is applied to the events.
struct NotificationStorage {
    Notification notification;
    Notifiers notifiers;
    OperationTopics topics;
    char wanted[OA3_MQTT_TOPICS_MAX][OA3_TOPIC_MAX + 1];
    char subscribed[OA3_MQTT_TOPICS_MAX][OA3_TOPIC_MAX + 1];
};

// Caller-owned memory, so a port can place it (e.g. in PSRAM). A fetch parses into scratch
// and swaps it with events on success, so a failure keeps the last good events. With scratch
// nullptr, a fetch parses into events directly, saving capacity events of RAM: a fetch that
// fails once its response has begun to parse clears them, logs a warning, and reports has_data
// false and no price until a fetch succeeds. A request that fails before parsing leaves them.
struct VenStorage {
    Event* events;
    Event* scratch;   // may be nullptr
    size_t capacity;  // elements in each array
    JsonArena* arena;
    NotificationStorage* notifications = nullptr;  // required with NotificationMode::kMqtt
};

namespace detail {

template <size_t N, bool Present>
struct ScratchEvents {
    Event* get() { return _events; }
    Event _events[N];
};

template <size_t N>
struct ScratchEvents<N, false> {
    Event* get() { return nullptr; }
};

}  // namespace detail

// DoubleBuffer false gives the single-buffer form (VenStorage::scratch nullptr).
template <size_t N = OA3_MAX_EVENTS, size_t ArenaSize = OA3_JSON_ARENA_SIZE, bool DoubleBuffer = true>
class StaticVenStorage {
public:
    VenStorage storage() { return VenStorage{_events, _scratch.get(), N, &_arena}; }

private:
    Event _events[N];
    detail::ScratchEvents<N, DoubleBuffer> _scratch;
    StaticJsonArena<ArenaSize> _arena;
};

// A VEN for one program that polls and, optionally, follows MQTT notifications. No threads:
// the port calls loop() from one task, and every callback runs inside loop().
class VenClient {
public:
    VenClient(HttpTransport& http, Clock& clock, const VenStorage& storage,
              MqttSession* mqtt = nullptr);

    VenClient(const VenClient&) = delete;
    VenClient& operator=(const VenClient&) = delete;

    // Validates and copies config, clears all state and data, and makes the first request due
    // at once. No I/O. False, leaving the client stopped, when the config is invalid.
    bool start(const VenConfig& config);
    // Does what is due: delivers MQTT messages, makes at most one HTTP request (with MQTT
    // connect and subscribe calls around it), then the price check. Cheap otherwise.
    void loop();
    // Disconnects MQTT; no further requests or callbacks. The data stays readable.
    void stop();
    // Makes the next loop() fetch events, ignoring the poll period and backoff. Before the
    // program is resolved it has no effect.
    void request_poll();

    bool running() const;
    // Until loop() next has work: 0 when due now, UINT32_MAX when stopped.
    uint32_t ms_until_due() const;

    const VenStatusInfo& status() const { return _status; }
    // As of the last loop(); active is false before the first successful fetch.
    const PriceInfo& price() const { return _price; }
    // nullptr until resolved.
    const Program* program() const { return _has_program ? &_program : nullptr; }
    // Valid until the next loop() or start().
    const Event* events() const { return _events; }
    size_t event_count() const { return _event_count; }
    // The wall-clock time of the last successful events fetch; false before one.
    bool last_fetch(DateTime* out) const;
    // The broker in use; nullptr before one is selected.
    const BrokerAddress* broker() const { return _has_broker ? &_broker : nullptr; }
    // The topic filters subscribed in the current session.
    size_t subscription_count() const { return _subscribed_count; }
    const char* subscription(size_t i) const { return _storage.notifications->subscribed[i]; }

private:
    enum class Phase : uint8_t { kStopped, kResolving, kPolling };

    static void on_programs_response(void* ctx, int status, ByteReader& body);
    static void on_program_response(void* ctx, int status, ByteReader& body);
    static void on_events_response(void* ctx, int status, ByteReader& body);
    static void on_notifiers_response(void* ctx, int status, ByteReader& body);
    static void on_topics_response(void* ctx, int status, ByteReader& body);
    static void on_message(void* ctx, const MqttMessage& message);

    enum class MqttStep : uint8_t {
        kIdle,
        kNotifiers,
        kEventTopics,
        kProgramTopics,
        kSnapshotBefore,  // exclusive_tls: /events while MQTT is closed, then connect
        kConnect,
        kSnapshotAfter,   // /events once subscribed
        kLive,
        kSuspended,
        kBackoff,
    };

    // Each request step returns true when the request failed.
    HttpResult request(HttpResponseFn on_response);
    bool resolve_by_id(uint32_t now_ms);
    bool resolve_by_name(uint32_t now_ms);
    bool poll(const DateTime& now, uint32_t now_ms);
    bool fail(VenStatus kind, uint32_t now_ms);
    bool fetch_failed(VenStatus kind, const DateTime& now, uint32_t now_ms);
    void not_found(uint32_t now_ms);
    void resolved();
    uint32_t ms_until_request(uint32_t now_ms) const;
    void check_clock_step(const DateTime& now, uint32_t now_ms);
    void update_price(const DateTime& now);
    void report(uint32_t now_ms, bool force);
    uint32_t period_s() const;
    uint32_t backoff_ms(uint16_t failures) const;
    bool mqtt_connected_step() const;
    bool mqtt_timed_step() const;
    void mqtt_request(uint32_t now_ms);
    void mqtt_service(const DateTime& now, uint32_t now_ms);
    bool mqtt_discover(HttpResponseFn on_response, const char* what, uint32_t now_ms);
    void mqtt_discovered(uint32_t now_ms);
    void mqtt_after_poll(uint32_t now_ms);
    bool mqtt_connect(bool clean_session, bool* session_present, uint32_t now_ms);
    bool mqtt_subscribe(bool session_present, uint32_t now_ms);
    void mqtt_go_live();
    void mqtt_suspend();
    void mqtt_resume(uint32_t now_ms);
    void mqtt_fail(MqttResult r, uint32_t now_ms);
    void mqtt_retry(uint32_t now_ms, bool at_once);
    void mqtt_unavailable(uint32_t now_ms, const char* why);
    void broker_selected();
    void mqtt_reset();
    void mqtt_close();
    void add_wanted(const char* topic);
    void add_wanted(const OperationTopics& topics);
    void handle_message(const MqttMessage& message);
    void apply_notification(bool retained);
    bool upsert_event(const Event& ev);
    void remove_event(size_t i);
    size_t find_event(const char* id) const;

    HttpTransport* _http;
    Clock* _clock;
    VenStorage _storage;

    VenConfig _config;  // its strings point at the copies below
    char _base_url[OA3_URL_MAX + 1];
    char _program_name[OA3_NAME_MAX + 1];
    char _program_id[OA3_ID_MAX + 1];
    char _payload_type[OA3_TYPE_MAX + 1];
    TargetList _targets;
    const char* _target_ptrs[OA3_MAX_TARGETS];

    Phase _phase;
    Program _program;
    bool _has_program;
    uint32_t _skip;
    uint16_t _pages;
    bool _name_query_rejected;
    uint32_t _event_skip;
    uint16_t _event_pages;
    bool _event_truncated;

    Event* _events;
    Event* _scratch;  // nullptr with a single buffer
    size_t _event_count;
    size_t _scratch_count;
    bool _events_dirty;  // single buffer: a fetch has begun to parse into _events

    bool _due_now;
    bool _stale;
    bool _has_last_loop;
    uint32_t _last_loop_ms;
    DateTime _last_loop_wall;
    uint32_t _next_request_ms;
    uint32_t _last_success_ms;
    DateTime _last_fetch;
    bool _has_fetch;
    bool _data_changed;

    VenStatusInfo _status;
    VenStatus _reported_status;
    VenStatus _status_before_clock;
    PriceInfo _price;
    bool _price_reported;
    bool _evaluated;
    DateTime _evaluated_at;

    int _response_status;
    ParseResult _response_parse;
    bool _response_seen;
    bool _response_parsed;
    bool _response_found;
    size_t _response_scanned;
    TimeInterval _window;
    ParseOptions _options;
    char _url[OA3_URL_MAX + 1];

    MqttSession* _mqtt;
    char _client_id[OA3_MQTT_CLIENT_ID_MAX + 1];
    char _mqtt_uri[OA3_NOTIFIER_URI_MAX + 1];
    MqttStep _mqtt_step;
    uint32_t _mqtt_next_ms;
    bool _mqtt_open;        // connect() called and disconnect() not since
    bool _mqtt_no_session;  // exclusive_tls: the last connect found no session on the broker
    BrokerAddress _broker;
    bool _has_broker;
    bool _plain_mqtt_warned;
    uint8_t _wanted_count;
    uint8_t _subscribed_count;
    NotifyState _reported_notify;
    bool _program_deleted;  // set by a notification, acted on once poll() returns
    bool _has_msg_window;
    TimeInterval _msg_window;  // [now, now + horizon) of the loop() delivering messages
};

}  // namespace oa3
