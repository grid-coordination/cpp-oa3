#pragma once

#include <stddef.h>
#include <stdint.h>

#include <oa3/client/clock.h>
#include <oa3/client/http_transport.h>
#include <oa3/client/limits.h>
#include <oa3/client/log.h>
#include <oa3/model/entities.h>
#include <oa3/model/json_arena.h>
#include <oa3/model/limits.h>
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

struct VenStatusInfo {
    VenStatus status;
    HttpResult http_result;    // of the last request; kOk before the first
    int http_status;           // of the last response; 0 when none
    ParseResult parse_result;  // of the last parse; kOk before the first
    uint16_t consecutive_failures;
    uint32_t retry_in_ms;  // until the next request, as of this report
    bool has_data;         // events from a successful fetch are held
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

// start() copies the strings, so they need not outlive it. Set fields on a value-initialized
// config (VenConfig c{}; c.base_url = ...); fields added later keep this working.
struct VenConfig {
    const char* base_url = nullptr;  // up to the spec version segment, e.g.
                                     // https://price.grid-coordination.energy/openadr3/3.1.0
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
    PriceChangeFn on_price_change = nullptr;
    VenStatusFn on_status = nullptr;
    void* ctx = nullptr;  // for both callbacks
    LogSink log = {};
};

// Caller-owned memory, so a port can place it (e.g. in PSRAM). A fetch parses into scratch
// and swaps it with events on success, so a failure keeps the last good events.
struct VenStorage {
    Event* events;
    Event* scratch;
    size_t capacity;  // elements in each array
    JsonArena* arena;
};

template <size_t N = OA3_MAX_EVENTS, size_t ArenaSize = OA3_JSON_ARENA_SIZE>
class StaticVenStorage {
public:
    VenStorage storage() { return VenStorage{_events, _scratch, N, &_arena}; }

private:
    Event _events[N];
    Event _scratch[N];
    StaticJsonArena<ArenaSize> _arena;
};

// A polling VEN for one program. No threads: the port calls loop() from one task, and
// every callback runs inside loop().
class VenClient {
public:
    VenClient(HttpTransport& http, Clock& clock, const VenStorage& storage);

    VenClient(const VenClient&) = delete;
    VenClient& operator=(const VenClient&) = delete;

    // Validates and copies config, clears all state and data, and makes the first request due
    // at once. No I/O. False, leaving the client stopped, when the config is invalid.
    bool start(const VenConfig& config);
    // Does what is due: at most one HTTP request, then the price check. Cheap otherwise.
    void loop();
    // No further requests or callbacks; the data stays readable.
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

private:
    enum class Phase : uint8_t { kStopped, kResolving, kPolling };

    static void on_programs_response(void* ctx, int status, ByteReader& body);
    static void on_program_response(void* ctx, int status, ByteReader& body);
    static void on_events_response(void* ctx, int status, ByteReader& body);

    // Each request step returns true when the request failed.
    HttpResult request(HttpResponseFn on_response);
    bool resolve_by_id(uint32_t now_ms);
    bool resolve_by_name(uint32_t now_ms);
    bool poll(const DateTime& now, uint32_t now_ms);
    bool fail(VenStatus kind, uint32_t now_ms);
    void not_found(uint32_t now_ms);
    void resolved();
    uint32_t ms_until_request(uint32_t now_ms) const;
    void check_clock_step(const DateTime& now, uint32_t now_ms);
    void update_price(const DateTime& now);
    void report(uint32_t now_ms, bool force);

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
    Event* _scratch;
    size_t _event_count;
    size_t _scratch_count;

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
};

}  // namespace oa3
