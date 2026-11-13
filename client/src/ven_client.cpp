#include <oa3/client/ven_client.h>

#include <stdint.h>
#include <string.h>

#include <oa3/client/url.h>
#include <oa3/model/intervals.h>
#include <oa3/model/targets.h>

namespace oa3 {

const char* to_string(VenStatus s) {
    switch (s) {
        case VenStatus::kStopped:
            return "STOPPED";
        case VenStatus::kWaitingForClock:
            return "WAITING_FOR_CLOCK";
        case VenStatus::kResolvingProgram:
            return "RESOLVING_PROGRAM";
        case VenStatus::kProgramNotFound:
            return "PROGRAM_NOT_FOUND";
        case VenStatus::kOk:
            return "OK";
        case VenStatus::kTruncated:
            return "TRUNCATED";
        case VenStatus::kStale:
            return "STALE";
        case VenStatus::kTransportError:
            return "TRANSPORT_ERROR";
        case VenStatus::kHttpError:
            return "HTTP_ERROR";
        case VenStatus::kParseError:
            return "PARSE_ERROR";
    }
    return "UNKNOWN";
}

const char* to_string(NotifyState s) {
    switch (s) {
        case NotifyState::kOff:
            return "OFF";
        case NotifyState::kDiscovering:
            return "DISCOVERING";
        case NotifyState::kUnavailable:
            return "UNAVAILABLE";
        case NotifyState::kConnecting:
            return "CONNECTING";
        case NotifyState::kConnected:
            return "CONNECTED";
        case NotifyState::kSuspended:
            return "SUSPENDED";
        case NotifyState::kDisconnected:
            return "DISCONNECTED";
    }
    return "UNKNOWN";
}

namespace {

// Keeps every millisecond delay below 2^31, so (int32_t)(a - b) orders monotonic times.
constexpr uint32_t kMaxPeriodS = 2000000;
constexpr uint64_t kMaxDelayMs = 0x7FFFFFFF;

// A wall clock that moves this much more or less than the monotonic one has been stepped.
constexpr int64_t kClockStepMs = 5000;

const HttpHeader kHeaders[] = {{"Accept", "application/json"}};

bool is_2xx(int status) { return status >= 200 && status <= 299; }

bool copy_string(const char* s, char* dst, size_t size) {
    const size_t n = strlen(s);
    if (n >= size) {
        return false;
    }
    memcpy(dst, s, n + 1);
    return true;
}

void copy_bounded(const char* s, char* dst, size_t size) {
    size_t n = strlen(s);
    if (n >= size) {
        n = size - 1;
    }
    memcpy(dst, s, n);
    dst[n] = '\0';
}

bool same_span(const TimeInterval& a, const TimeInterval& b) {
    return a.end_indefinite == b.end_indefinite && compare(a.start, b.start) == 0 &&
           (a.end_indefinite || compare(a.end, b.end) == 0);
}

// True when both times are known and a is later.
bool newer(bool has_a, const DateTime& a, bool has_b, const DateTime& b) {
    return has_a && has_b && compare(a, b) > 0;
}

bool scheme_is(const char* url, const char* scheme) {
    const size_t n = strlen(scheme);
    for (size_t i = 0; i < n; ++i) {
        char c = url[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != scheme[i]) {
            return false;
        }
    }
    return strncmp(url + n, "://", 3) == 0;
}

void warn_plaintext(const LogSink& log, const char* what) {
    log_printf(log, LogLevel::kWarn, "%s without TLS: unencrypted, and not conformant to OpenADR 3",
               what);
}

bool reached(uint32_t now_ms, uint32_t at_ms) { return static_cast<int32_t>(now_ms - at_ms) >= 0; }

bool in_list(const char (&list)[OA3_MQTT_TOPICS_MAX][OA3_TOPIC_MAX + 1], size_t count,
             const char* s) {
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(list[i], s) == 0) {
            return true;
        }
    }
    return false;
}

// Where an event meets the window, by the rule parse_events applies (parse.cpp fit_event):
// first is its earliest instant in the window.
struct WindowFit {
    bool in_window;
    DateTime first;
};

WindowFit window_fit(const Event& ev, const TimeInterval& window) {
    ExtentBuilder extent;
    bool hit = false;
    DateTime first{};
    IntervalResolver resolver(ev);
    ResolvedInterval r;
    while (resolver.next(&r)) {
        extent.add(r);
        if (r.has_period && intersects(r.period, window) &&
            (!hit || compare(r.period.start, first) < 0)) {
            hit = true;
            first = r.period.start;
        }
    }
    const EventExtent x = extent.finish(ev.has_duration, ev.duration);
    WindowFit f{true, DateTime{INT64_MAX, 0, 0}};
    if (!x.has_period) {
        return f;
    }
    if (x.looping) {
        f.in_window = intersects(x.span, window);
        f.first = x.span.start;
    } else {
        f.in_window = hit;
        f.first = first;
    }
    if (compare(f.first, window.start) < 0) {
        f.first = window.start;
    }
    return f;
}

}  // namespace

VenClient::VenClient(HttpTransport& http, Clock& clock, const VenStorage& storage,
                     MqttSession* mqtt)
    : _http(&http),
      _clock(&clock),
      _storage(storage),
      _config(),
      _base_url(),
      _program_name(),
      _program_id(),
      _payload_type(),
      _targets(),
      _target_ptrs(),
      _phase(Phase::kStopped),
      _program(),
      _has_program(false),
      _skip(0),
      _pages(0),
      _name_query_rejected(false),
      _event_skip(0),
      _event_pages(0),
      _event_truncated(false),
      _events(storage.events),
      _scratch(storage.scratch),
      _event_count(0),
      _scratch_count(0),
      _due_now(false),
      _stale(false),
      _has_last_loop(false),
      _last_loop_ms(0),
      _last_loop_wall(),
      _next_request_ms(0),
      _last_success_ms(0),
      _last_fetch(),
      _has_fetch(false),
      _data_changed(false),
      _status(),
      _reported_status(VenStatus::kStopped),
      _status_before_clock(VenStatus::kStopped),
      _price(),
      _price_reported(false),
      _evaluated(false),
      _evaluated_at(),
      _response_status(0),
      _response_parse(ParseResult::kOk),
      _response_seen(false),
      _response_parsed(false),
      _response_found(false),
      _response_scanned(0),
      _window(),
      _options(),
      _mqtt(mqtt),
      _client_id(),
      _mqtt_uri(),
      _mqtt_step(MqttStep::kIdle),
      _mqtt_next_ms(0),
      _mqtt_open(false),
      _mqtt_no_session(false),
      _broker(),
      _has_broker(false),
      _plain_mqtt_warned(false),
      _wanted_count(0),
      _subscribed_count(0),
      _reported_notify(NotifyState::kOff),
      _program_deleted(false),
      _has_msg_window(false),
      _msg_window() {}

bool VenClient::start(const VenConfig& config) {
    mqtt_reset();
    _phase = Phase::kStopped;
    _status.status = VenStatus::kStopped;
    _status.notify = NotifyState::kOff;
    _reported_status = VenStatus::kStopped;
    _reported_notify = NotifyState::kOff;

    if (config.base_url == nullptr || strlen(config.base_url) > OA3_URL_MAX) {
        return false;
    }
    const bool plain_http = scheme_is(config.base_url, "http");
    if (!scheme_is(config.base_url, "https") && !(plain_http && config.allow_plaintext)) {
        return false;
    }
    if (config.program_name == nullptr && config.program_id == nullptr) {
        return false;
    }
    if (config.payload_type == nullptr || config.poll_period_s == 0 ||
        config.poll_period_s > kMaxPeriodS || config.backoff_min_s == 0 ||
        config.backoff_max_s < config.backoff_min_s || config.backoff_max_s > kMaxPeriodS) {
        return false;
    }
    if (static_cast<uint64_t>(config.stale_after_polls) * config.poll_period_s * 1000 >
        kMaxDelayMs) {
        return false;
    }
    if (is_negative(config.horizon) || is_zero(config.horizon)) {
        return false;
    }
    if (_storage.events == nullptr || _storage.scratch == nullptr || _storage.arena == nullptr ||
        _storage.capacity == 0) {
        return false;
    }
    if (config.target_count > OA3_MAX_TARGETS ||
        (config.target_count > 0 && config.targets == nullptr)) {
        return false;
    }
    const bool mqtt = config.notifications == NotificationMode::kMqtt;
    if (mqtt) {
        if (_mqtt == nullptr || _storage.notifications == nullptr || config.mqtt_client_id == nullptr) {
            return false;
        }
        const size_t id_len = strlen(config.mqtt_client_id);
        if (id_len == 0 || id_len > OA3_MQTT_CLIENT_ID_MAX) {
            return false;
        }
        if (config.notify_poll_period_s < config.poll_period_s ||
            config.notify_poll_period_s > kMaxPeriodS) {
            return false;
        }
        if (static_cast<uint64_t>(config.stale_after_polls) * config.notify_poll_period_s * 1000 >
            kMaxDelayMs) {
            return false;
        }
        if (config.mqtt_uri != nullptr) {
            BrokerAddress b;
            if (strlen(config.mqtt_uri) > OA3_NOTIFIER_URI_MAX ||
                !parse_broker_uri(config.mqtt_uri, &b) || (!b.tls && !config.allow_plaintext)) {
                return false;
            }
        }
    }

    _base_url[0] = '\0';
    _program_name[0] = '\0';
    _program_id[0] = '\0';
    if (!copy_string(config.base_url, _base_url, sizeof(_base_url)) ||
        !copy_string(config.payload_type, _payload_type, sizeof(_payload_type))) {
        return false;
    }
    if (config.program_name != nullptr &&
        !copy_string(config.program_name, _program_name, sizeof(_program_name))) {
        return false;
    }
    if (config.program_id != nullptr &&
        (!is_object_id(config.program_id) ||
         !copy_string(config.program_id, _program_id, sizeof(_program_id)))) {
        return false;
    }
    memset(&_targets, 0, sizeof(_targets));
    for (size_t i = 0; i < config.target_count; ++i) {
        if (config.targets[i] == nullptr ||
            !copy_string(config.targets[i], _targets.items[i], sizeof(_targets.items[i]))) {
            return false;
        }
        _target_ptrs[i] = _targets.items[i];
    }
    _targets.count = static_cast<uint8_t>(config.target_count);
    _client_id[0] = '\0';
    _mqtt_uri[0] = '\0';
    if (mqtt) {
        copy_string(config.mqtt_client_id, _client_id, sizeof(_client_id));
        if (config.mqtt_uri != nullptr) {
            copy_string(config.mqtt_uri, _mqtt_uri, sizeof(_mqtt_uri));
        }
    }

    _config = config;
    _config.base_url = _base_url;
    _config.program_name = config.program_name != nullptr ? _program_name : nullptr;
    _config.program_id = config.program_id != nullptr ? _program_id : nullptr;
    _config.payload_type = _payload_type;
    _config.targets = config.target_count > 0 ? _target_ptrs : nullptr;
    _config.mqtt_client_id = mqtt ? _client_id : nullptr;
    _config.mqtt_uri = mqtt && config.mqtt_uri != nullptr ? _mqtt_uri : nullptr;

    const TargetList* targets = _targets.count > 0 ? &_targets : nullptr;
    char probe_id[OA3_ID_MAX + 1];
    memset(probe_id, 'x', OA3_ID_MAX);
    probe_id[OA3_ID_MAX] = '\0';
    if (!build_events_url(_url, sizeof(_url), _base_url, probe_id, targets, UINT32_MAX,
                          OA3_EVENTS_PAGE_LIMIT)) {
        return false;
    }
    if (mqtt && (!build_notifiers_url(_url, sizeof(_url), _base_url) ||
                 !build_program_events_topics_url(_url, sizeof(_url), _base_url, probe_id) ||
                 !build_program_topics_url(_url, sizeof(_url), _base_url, probe_id))) {
        return false;
    }
    if (_config.program_id != nullptr &&
        !build_program_url(_url, sizeof(_url), _base_url, _config.program_id)) {
        return false;
    }
    if (_config.program_id == nullptr &&
        !build_programs_url(_url, sizeof(_url), _base_url,
                            _config.send_program_name ? _config.program_name : nullptr, targets,
                            UINT32_MAX, OA3_PROGRAMS_PAGE_LIMIT)) {
        return false;
    }

    memset(&_program, 0, sizeof(_program));
    _has_program = false;
    _skip = 0;
    _pages = 0;
    _name_query_rejected = false;
    _event_skip = 0;
    _event_pages = 0;
    _event_truncated = false;
    _events = _storage.events;
    _scratch = _storage.scratch;
    _event_count = 0;
    _scratch_count = 0;
    _due_now = true;
    _stale = false;
    _has_last_loop = false;
    _next_request_ms = 0;
    _last_success_ms = 0;
    _last_fetch = DateTime{};
    _has_fetch = false;
    _data_changed = false;
    _status = VenStatusInfo{};
    _status.status = VenStatus::kResolvingProgram;
    _reported_status = VenStatus::kStopped;
    _status_before_clock = VenStatus::kResolvingProgram;
    _price = PriceInfo{};
    _price_reported = false;
    _evaluated = false;
    _evaluated_at = DateTime{};
    _phase = Phase::kResolving;
    _plain_mqtt_warned = false;
    if (plain_http) {
        warn_plaintext(_config.log, "HTTP");
    }
    return true;
}

void VenClient::stop() {
    mqtt_reset();
    _phase = Phase::kStopped;
    _status.status = VenStatus::kStopped;
    _status.notify = NotifyState::kOff;
    _reported_status = VenStatus::kStopped;
    _reported_notify = NotifyState::kOff;
}

void VenClient::request_poll() {
    if (_phase == Phase::kPolling) {
        _due_now = true;
    }
}

bool VenClient::running() const { return _phase != Phase::kStopped; }

bool VenClient::last_fetch(DateTime* out) const {
    if (!_has_fetch) {
        return false;
    }
    *out = _last_fetch;
    return true;
}

uint32_t VenClient::ms_until_request(uint32_t now_ms) const {
    if (_due_now) {
        return 0;
    }
    const int32_t d = static_cast<int32_t>(_next_request_ms - now_ms);
    return d <= 0 ? 0 : static_cast<uint32_t>(d);
}

uint32_t VenClient::ms_until_due() const {
    if (_phase == Phase::kStopped) {
        return UINT32_MAX;
    }
    DateTime now;
    if (!_clock->now(&now)) {
        return 1000;
    }
    const uint32_t now_ms = _clock->monotonic_ms();
    uint32_t due = ms_until_request(now_ms);
    if (mqtt_connected_step() && _config.mqtt_poll_ms < due) {
        due = _config.mqtt_poll_ms;
    }
    if (mqtt_timed_step()) {
        const int32_t d = static_cast<int32_t>(_mqtt_next_ms - now_ms);
        const uint32_t m = d <= 0 ? 0 : static_cast<uint32_t>(d);
        if (m < due) {
            due = m;
        }
    }
    if (!_has_fetch || due == 0) {
        return due;
    }
    if (_data_changed || !_evaluated || compare(now, _evaluated_at) < 0) {
        return 0;
    }
    if (_price.has_next_change) {
        const int64_t secs = _price.next_change.seconds - now.seconds;
        if (secs < static_cast<int64_t>(UINT32_MAX / 1000)) {
            const int64_t ns = secs * 1000000000 + static_cast<int64_t>(_price.next_change.nanos) -
                               static_cast<int64_t>(now.nanos);
            const int64_t ms = ns <= 0 ? 0 : (ns + 999999) / 1000000;
            if (ms < due) {
                due = static_cast<uint32_t>(ms);
            }
        }
    }
    return due;
}

HttpResult VenClient::request(HttpResponseFn on_response) {
    // A failed connect() or a dropped session may still hold its TLS session.
    if (_mqtt_open && _config.exclusive_tls && _broker.tls) {
        if (mqtt_connected_step()) {
            mqtt_suspend();
        } else {
            mqtt_close();
        }
    }
    _response_seen = false;
    _response_status = 0;
    _response_parse = ParseResult::kOk;
    _response_parsed = false;
    _response_found = false;
    _response_scanned = 0;
    log_printf(_config.log, LogLevel::kDebug, "GET %s", _url);
    const HttpRequest req{_url, kHeaders, sizeof(kHeaders) / sizeof(kHeaders[0]),
                          _config.http_timeout_ms};
    HttpResult r = _http->get(req, on_response, this);
    if (r == HttpResult::kOk && !_response_seen) {
        r = HttpResult::kIoError;
    }
    _status.http_result = r;
    _status.http_status = _response_seen ? _response_status : 0;
    if (_response_parsed) {
        _status.parse_result = _response_parse;
    }
    return r;
}

void VenClient::on_program_response(void* ctx, int status, ByteReader& body) {
    VenClient* self = static_cast<VenClient*>(ctx);
    self->_response_seen = true;
    self->_response_status = status;
    if (is_2xx(status)) {
        self->_response_parsed = true;
        self->_response_parse =
            parse_program(body, *self->_storage.arena, &self->_program, &self->_options);
    }
}

void VenClient::on_programs_response(void* ctx, int status, ByteReader& body) {
    VenClient* self = static_cast<VenClient*>(ctx);
    self->_response_seen = true;
    self->_response_status = status;
    if (is_2xx(status)) {
        self->_response_parsed = true;
        self->_response_parse = find_program(
            body, *self->_storage.arena, self->_config.program_name, &self->_program,
            &self->_response_found, &self->_response_scanned, &self->_options);
    }
}

void VenClient::on_events_response(void* ctx, int status, ByteReader& body) {
    VenClient* self = static_cast<VenClient*>(ctx);
    self->_response_seen = true;
    self->_response_status = status;
    if (is_2xx(status)) {
        self->_response_parsed = true;
        self->_response_parse =
            append_events(body, *self->_storage.arena, self->_scratch, self->_storage.capacity,
                          &self->_scratch_count, &self->_response_scanned, &self->_options);
    }
}

uint32_t VenClient::backoff_ms(uint16_t failures) const {
    uint64_t delay_s = _config.backoff_min_s;
    for (uint16_t i = 1; i < failures && delay_s < _config.backoff_max_s; ++i) {
        delay_s *= 2;
    }
    if (delay_s > _config.backoff_max_s) {
        delay_s = _config.backoff_max_s;
    }
    return static_cast<uint32_t>(delay_s * 1000);
}

bool VenClient::fail(VenStatus kind, uint32_t now_ms) {
    _status.status = kind;
    if (_status.consecutive_failures < UINT16_MAX) {
        ++_status.consecutive_failures;
    }
    const uint32_t delay_s = backoff_ms(_status.consecutive_failures) / 1000;
    _next_request_ms = now_ms + delay_s * 1000;
    log_printf(_config.log, LogLevel::kWarn, "%s: http %s %d, parse %s, failures %u, retry in %lu s",
               to_string(kind), to_string(_status.http_result), _status.http_status,
               to_string(_status.parse_result),
               static_cast<unsigned>(_status.consecutive_failures),
               static_cast<unsigned long>(delay_s));
    return true;
}

void VenClient::not_found(uint32_t now_ms) {
    _status.status = VenStatus::kProgramNotFound;
    _status.consecutive_failures = 0;
    _skip = 0;
    _pages = 0;
    _next_request_ms = now_ms + _config.poll_period_s * 1000;
    log_printf(_config.log, LogLevel::kWarn, "program %s not found",
               _config.program_name != nullptr ? _config.program_name : _config.program_id);
}

void VenClient::resolved() {
    _has_program = true;
    _phase = Phase::kPolling;
    _due_now = true;
    _status.status = VenStatus::kResolvingProgram;
    _status.consecutive_failures = 0;
    log_printf(_config.log, LogLevel::kInfo, "program %s resolved: id %s", _program.program_name,
               _program.id);
}

bool VenClient::resolve_by_id(uint32_t now_ms) {
    _options = ParseOptions{};
    _options.payload_types = &_config.payload_type;
    _options.payload_type_count = 1;
    if (!build_program_url(_url, sizeof(_url), _base_url, _config.program_id)) {
        _status.http_result = HttpResult::kInvalidUrl;
        return fail(VenStatus::kTransportError, now_ms);
    }
    if (request(on_program_response) != HttpResult::kOk) {
        return fail(VenStatus::kTransportError, now_ms);
    }
    if (_response_status == 404) {
        not_found(now_ms);
        return false;
    }
    if (!is_2xx(_response_status)) {
        return fail(VenStatus::kHttpError, now_ms);
    }
    if (_response_parse != ParseResult::kOk) {
        return fail(VenStatus::kParseError, now_ms);
    }
    if (!is_object_id(_program.id)) {
        _status.parse_result = ParseResult::kInvalidField;
        return fail(VenStatus::kParseError, now_ms);
    }
    if (_config.program_name != nullptr && strcmp(_program.program_name, _config.program_name) != 0) {
        log_printf(_config.log, LogLevel::kWarn, "program %s is named %s", _config.program_id,
                   _program.program_name);
        not_found(now_ms);
        return false;
    }
    resolved();
    return false;
}

bool VenClient::resolve_by_name(uint32_t now_ms) {
    _options = ParseOptions{};
    _options.payload_types = &_config.payload_type;
    _options.payload_type_count = 1;
    _status.status = VenStatus::kResolvingProgram;
    const bool name_query = _config.send_program_name && !_name_query_rejected;
    if (!build_programs_url(_url, sizeof(_url), _base_url,
                            name_query ? _config.program_name : nullptr,
                            _targets.count > 0 ? &_targets : nullptr, _skip,
                            OA3_PROGRAMS_PAGE_LIMIT)) {
        _status.http_result = HttpResult::kInvalidUrl;
        return fail(VenStatus::kTransportError, now_ms);
    }
    if (request(on_programs_response) != HttpResult::kOk) {
        return fail(VenStatus::kTransportError, now_ms);
    }
    if (name_query && _response_status >= 400 && _response_status <= 499) {
        log_printf(_config.log, LogLevel::kWarn, "programName query refused with %d; paging",
                   _response_status);
        _name_query_rejected = true;
        _skip = 0;
        _pages = 0;
        _due_now = true;
        return false;
    }
    if (!is_2xx(_response_status)) {
        return fail(VenStatus::kHttpError, now_ms);
    }
    if (_response_parse != ParseResult::kOk) {
        return fail(VenStatus::kParseError, now_ms);
    }
    ++_pages;
    if (_response_found) {
        if (!is_object_id(_program.id)) {
            _status.parse_result = ParseResult::kInvalidField;
            return fail(VenStatus::kParseError, now_ms);
        }
        resolved();
        return false;
    }
    // More than the limit means the VTN ignores paging, so this was all of it.
    if (_response_scanned != OA3_PROGRAMS_PAGE_LIMIT || _pages >= OA3_PROGRAM_PAGES_MAX) {
        not_found(now_ms);
        return false;
    }
    _skip += static_cast<uint32_t>(_response_scanned);
    _due_now = true;
    _status.consecutive_failures = 0;
    return false;
}

bool VenClient::poll(const DateTime& now, uint32_t now_ms) {
    if (_event_skip == 0) {
        _options = ParseOptions{};
        _options.payload_types = &_config.payload_type;
        _options.payload_type_count = 1;
        if (make_interval(now, _config.horizon, &_window)) {
            _options.window = &_window;
        }
        _options.program_id = _program.id;
        if (_config.target_count > 0) {
            _options.ven_targets = _config.targets;
            _options.ven_target_count = _config.target_count;
        }
        _scratch_count = 0;
        _event_pages = 0;
        _event_truncated = false;
    }
    const TargetList* targets = _targets.count > 0 ? &_targets : nullptr;
    if (!build_events_url(_url, sizeof(_url), _base_url, _program.id, targets, _event_skip,
                          OA3_EVENTS_PAGE_LIMIT)) {
        _event_skip = 0;
        _status.http_result = HttpResult::kInvalidUrl;
        return fail(VenStatus::kTransportError, now_ms);
    }
    const HttpResult r = request(on_events_response);
    const uint32_t skip = _event_skip;
    _event_skip = 0;
    if (r != HttpResult::kOk) {
        return fail(VenStatus::kTransportError, now_ms);
    }
    if (!is_2xx(_response_status)) {
        return fail(VenStatus::kHttpError, now_ms);
    }
    if (_response_parse != ParseResult::kOk && _response_parse != ParseResult::kTruncated) {
        return fail(VenStatus::kParseError, now_ms);
    }
    _event_truncated = _event_truncated || _response_parse == ParseResult::kTruncated;
    ++_event_pages;
    // More than the limit means the VTN ignores paging, so this was all of it.
    if (_response_scanned == OA3_EVENTS_PAGE_LIMIT) {
        if (_event_pages < OA3_EVENT_PAGES_MAX) {
            _event_skip = skip + static_cast<uint32_t>(_response_scanned);
            _due_now = true;
            return false;
        }
        _event_truncated = true;
    }

    Event* swap = _events;
    _events = _scratch;
    _scratch = swap;
    _event_count = _scratch_count;
    _scratch_count = 0;

    _data_changed = true;
    _has_fetch = true;
    _last_fetch = now;
    _last_success_ms = now_ms;
    _stale = false;
    _status.consecutive_failures = 0;
    _status.status = _event_truncated ? VenStatus::kTruncated : VenStatus::kOk;
    _next_request_ms = now_ms + period_s() * 1000;
    log_printf(_config.log, LogLevel::kDebug, "%u events kept, pages %u",
               static_cast<unsigned>(_event_count), static_cast<unsigned>(_event_pages));
    mqtt_after_poll(now_ms);
    return false;
}

void VenClient::report(uint32_t now_ms, bool force) {
    _status.retry_in_ms = ms_until_request(now_ms);
    _status.has_data = _has_fetch;
    const bool status_changed = _status.status != _reported_status;
    const bool notify_changed = _status.notify != _reported_notify;
    if (!status_changed && !notify_changed && !force) {
        return;
    }
    if (status_changed) {
        log_printf(_config.log, LogLevel::kInfo, "status %s", to_string(_status.status));
    }
    if (notify_changed) {
        log_printf(_config.log, LogLevel::kInfo, "notify %s", to_string(_status.notify));
    }
    _reported_status = _status.status;
    _reported_notify = _status.notify;
    if (_config.on_status != nullptr) {
        _config.on_status(_config.ctx, _status);
    }
}

void VenClient::update_price(const DateTime& now) {
    const bool due = _data_changed || !_evaluated ||
                     (_price.has_next_change && compare(now, _price.next_change) >= 0) ||
                     compare(now, _evaluated_at) < 0;
    if (!due) {
        return;
    }
    PriceAt at;
    price_at(_events, _event_count, now, _config.payload_type, &at);

    PriceInfo p{};
    p.active = at.active && at.event != nullptr && at.value != nullptr;
    if (p.active) {
        const Value& v = *at.value;
        p.kind = v.kind;
        p.number = v.number;
        switch (v.kind) {
            case ValueKind::kNumber:
            case ValueKind::kString:
                copy_bounded(v.text, p.text, sizeof(p.text));
                break;
            case ValueKind::kBool:
                copy_bounded(v.boolean ? "true" : "false", p.text, sizeof(p.text));
                break;
            case ValueKind::kPoint:
                break;
        }
        const PayloadDescriptor* d = find_descriptor(*at.event, _config.payload_type);
        if (d == nullptr) {
            d = find_descriptor(_program, _config.payload_type);
        }
        if (d != nullptr) {
            copy_bounded(d->units, p.units, sizeof(p.units));
            copy_bounded(d->currency, p.currency, sizeof(p.currency));
        }
        copy_bounded(at.event->id, p.event_id, sizeof(p.event_id));
        p.interval_id = at.event->intervals[at.interval_index].id;
        p.span = at.span;
    }
    p.has_next_change = at.has_next_change;
    p.next_change = at.next_change;

    const bool fire = !_price_reported || p.active != _price.active ||
                      (p.active && (!same_span(p.span, _price.span) || p.kind != _price.kind ||
                                    strcmp(p.text, _price.text) != 0 || p.number != _price.number));
    _price = p;
    _data_changed = false;
    _evaluated = true;
    _evaluated_at = now;
    if (!fire) {
        return;
    }
    _price_reported = true;
    if (_price.active) {
        log_printf(_config.log, LogLevel::kInfo, "%s %s %s/%s, event %s interval %ld",
                   _config.payload_type, _price.text, _price.currency, _price.units,
                   _price.event_id, static_cast<long>(_price.interval_id));
    } else {
        log_printf(_config.log, LogLevel::kInfo, "no %s in force", _config.payload_type);
    }
    if (_config.on_price_change != nullptr) {
        _config.on_price_change(_config.ctx, _price);
    }
}

// The fetch window starts at the wall-clock time of the fetch, so after a step the data may
// not cover the new now: fetch again.
void VenClient::check_clock_step(const DateTime& now, uint32_t now_ms) {
    if (_has_last_loop && _phase == Phase::kPolling && _has_fetch) {
        const int64_t wall_ms = (now.seconds - _last_loop_wall.seconds) * 1000 +
                                (static_cast<int64_t>(now.nanos) -
                                 static_cast<int64_t>(_last_loop_wall.nanos)) / 1000000;
        const int64_t step_ms = wall_ms - static_cast<int64_t>(now_ms - _last_loop_ms);
        if (step_ms > kClockStepMs || step_ms < -kClockStepMs) {
            log_printf(_config.log, LogLevel::kInfo, "wall clock stepped %ld s; fetching",
                       static_cast<long>(step_ms / 1000));
            _due_now = true;
        }
    }
    _has_last_loop = true;
    _last_loop_ms = now_ms;
    _last_loop_wall = now;
}

void VenClient::loop() {
    if (_phase == Phase::kStopped) {
        return;
    }
    const uint32_t now_ms = _clock->monotonic_ms();
    DateTime now;
    if (!_clock->now(&now)) {
        if (_status.status != VenStatus::kWaitingForClock) {
            _status_before_clock = _status.status;
            _status.status = VenStatus::kWaitingForClock;
        }
        report(now_ms, false);
        return;
    }
    if (_status.status == VenStatus::kWaitingForClock) {
        _status.status = _status_before_clock;
    }
    check_clock_step(now, now_ms);

    // Between /events pages a notification would be lost in the swap, so it waits.
    if (mqtt_connected_step() && _event_skip == 0) {
        mqtt_service(now, now_ms);
        if (_phase == Phase::kStopped) {
            return;
        }
    }

    bool failed = false;
    if (_due_now || reached(now_ms, _next_request_ms)) {
        _due_now = false;
        report(now_ms, false);
        if (_phase == Phase::kStopped) {
            return;
        }
        if (_phase == Phase::kPolling) {
            failed = poll(now, now_ms);
        } else if (_config.program_id != nullptr) {
            failed = resolve_by_id(now_ms);
        } else {
            failed = resolve_by_name(now_ms);
        }
    } else if (_phase == Phase::kPolling) {
        mqtt_request(now_ms);
    }

    if (_phase == Phase::kPolling && _has_fetch && _config.stale_after_polls > 0) {
        const uint64_t limit_ms =
            static_cast<uint64_t>(_config.stale_after_polls) * period_s() * 1000;
        if (_stale || static_cast<uint32_t>(now_ms - _last_success_ms) >= limit_ms) {
            _stale = true;
            _status.status = VenStatus::kStale;
        }
    }

    report(now_ms, failed);
    if (_phase == Phase::kStopped || !_has_fetch) {
        return;
    }
    update_price(now);
}

// MQTT notifications.

uint32_t VenClient::period_s() const {
    const bool notified =
        _status.notify == NotifyState::kConnected || _status.notify == NotifyState::kSuspended;
    return notified && !_mqtt_no_session ? _config.notify_poll_period_s : _config.poll_period_s;
}

bool VenClient::mqtt_connected_step() const {
    return _mqtt_step == MqttStep::kLive || _mqtt_step == MqttStep::kSnapshotAfter;
}

bool VenClient::mqtt_timed_step() const {
    switch (_mqtt_step) {
        case MqttStep::kNotifiers:
        case MqttStep::kEventTopics:
        case MqttStep::kProgramTopics:
        case MqttStep::kConnect:
        case MqttStep::kSuspended:
        case MqttStep::kBackoff:
            return _phase == Phase::kPolling;
        default:
            return false;
    }
}

void VenClient::mqtt_reset() {
    mqtt_close();
    _mqtt_step = MqttStep::kIdle;
    _mqtt_no_session = false;
    _has_broker = false;
    _wanted_count = 0;
    _subscribed_count = 0;
    _program_deleted = false;
}

void VenClient::on_notifiers_response(void* ctx, int status, ByteReader& body) {
    VenClient* self = static_cast<VenClient*>(ctx);
    self->_response_seen = true;
    self->_response_status = status;
    if (is_2xx(status)) {
        self->_response_parse =
            parse_notifiers(body, *self->_storage.arena, &self->_storage.notifications->notifiers);
    }
}

void VenClient::on_topics_response(void* ctx, int status, ByteReader& body) {
    VenClient* self = static_cast<VenClient*>(ctx);
    self->_response_seen = true;
    self->_response_status = status;
    if (is_2xx(status)) {
        self->_response_parse =
            parse_topics(body, *self->_storage.arena, &self->_storage.notifications->topics);
    }
}

void VenClient::on_message(void* ctx, const MqttMessage& message) {
    static_cast<VenClient*>(ctx)->handle_message(message);
}

void VenClient::mqtt_close() {
    if (_mqtt != nullptr && (_mqtt_open || _mqtt->connected())) {
        _mqtt->disconnect();
        _mqtt_open = false;
    }
}

// A failed discovery request leaves VenStatus alone. A VTN without the route, or with an
// answer the client cannot read, has no usable binding; anything else backs off.
bool VenClient::mqtt_discover(HttpResponseFn on_response, const char* what, uint32_t now_ms) {
    const HttpResult r = request(on_response);
    const int s = _response_status;
    if (r == HttpResult::kOk && is_2xx(s) && _response_parse == ParseResult::kOk) {
        return true;
    }
    log_printf(_config.log, LogLevel::kWarn, "%s: http %s %d, parse %s", what, to_string(r), s,
               to_string(_response_parse));
    if (r == HttpResult::kOk && (is_2xx(s) || s == 404 || s == 405 || s == 501)) {
        mqtt_unavailable(now_ms, what);
    } else {
        mqtt_retry(now_ms, false);
    }
    return false;
}

void VenClient::mqtt_unavailable(uint32_t now_ms, const char* why) {
    log_printf(_config.log, LogLevel::kWarn, "MQTT unavailable: %s", why);
    _status.notify = NotifyState::kUnavailable;
    _mqtt_step = MqttStep::kBackoff;
    _mqtt_next_ms = now_ms + _config.poll_period_s * 1000;
}

void VenClient::broker_selected() {
    if (!_broker.tls && !_plain_mqtt_warned) {
        _plain_mqtt_warned = true;
        warn_plaintext(_config.log, "MQTT");
    }
}

void VenClient::mqtt_retry(uint32_t now_ms, bool at_once) {
    if (_status.mqtt_failures < UINT16_MAX) {
        ++_status.mqtt_failures;
    }
    _status.notify = NotifyState::kDisconnected;
    _mqtt_step = MqttStep::kBackoff;
    _mqtt_next_ms = now_ms + (at_once ? 0 : backoff_ms(_status.mqtt_failures));
    if (_has_fetch) {
        const uint32_t due = _last_success_ms + _config.poll_period_s * 1000;
        if (static_cast<int32_t>(due - _next_request_ms) < 0) {
            _next_request_ms = due;
        }
    }
}

void VenClient::mqtt_fail(MqttResult r, uint32_t now_ms) {
    _status.mqtt_result = r;
    mqtt_close();
    mqtt_retry(now_ms, false);
    log_printf(_config.log, LogLevel::kWarn, "MQTT %s, failures %u, retry in %lu s", to_string(r),
               static_cast<unsigned>(_status.mqtt_failures),
               static_cast<unsigned long>(backoff_ms(_status.mqtt_failures) / 1000));
}

void VenClient::add_wanted(const char* topic) {
    char (&wanted)[OA3_MQTT_TOPICS_MAX][OA3_TOPIC_MAX + 1] = _storage.notifications->wanted;
    if (topic[0] == '\0' || in_list(wanted, _wanted_count, topic)) {
        return;
    }
    if (_wanted_count >= OA3_MQTT_TOPICS_MAX) {
        log_printf(_config.log, LogLevel::kWarn, "topic %s left out: OA3_MQTT_TOPICS_MAX", topic);
        return;
    }
    copy_string(topic, wanted[_wanted_count++], sizeof(wanted[0]));
}

void VenClient::add_wanted(const OperationTopics& topics) {
    if (topics.all[0] != '\0') {
        add_wanted(topics.all);
        return;
    }
    add_wanted(topics.create);
    add_wanted(topics.update);
    add_wanted(topics.remove);
}

void VenClient::mqtt_discovered(uint32_t now_ms) {
    _status.notify = NotifyState::kConnecting;
    _mqtt_next_ms = now_ms;
    if (_config.exclusive_tls && _broker.tls) {
        _mqtt_step = MqttStep::kSnapshotBefore;
        _due_now = true;
    } else {
        _mqtt_step = MqttStep::kConnect;
    }
}

void VenClient::mqtt_request(uint32_t now_ms) {
    if (_config.notifications != NotificationMode::kMqtt) {
        return;
    }
    if (_mqtt_step == MqttStep::kSuspended) {
        mqtt_resume(now_ms);
        return;
    }
    if (!mqtt_timed_step() || !reached(now_ms, _mqtt_next_ms)) {
        return;
    }
    if (_mqtt_step == MqttStep::kBackoff) {
        _mqtt_step = MqttStep::kNotifiers;
    }
    if (_mqtt_step == MqttStep::kNotifiers) {
        _status.notify = NotifyState::kDiscovering;
        _wanted_count = 0;
        if (_config.mqtt_uri != nullptr) {
            _has_broker = parse_broker_uri(_mqtt_uri, &_broker);
            _mqtt_step = MqttStep::kEventTopics;
            broker_selected();
        } else {
            _has_broker = false;
            build_notifiers_url(_url, sizeof(_url), _base_url);
            if (!mqtt_discover(on_notifiers_response, "notifiers", now_ms)) {
                return;
            }
            const MqttNotifier& m = _storage.notifications->notifiers.mqtt;
            if (!m.present) {
                mqtt_unavailable(now_ms, "no MQTT binding");
            } else if (m.auth != MqttAuthMethod::kAnonymous) {
                mqtt_unavailable(now_ms, to_string(m.auth));
            } else if (!select_broker(m, _config.allow_plaintext, &_broker)) {
                BrokerAddress plain;
                mqtt_unavailable(now_ms, m.uri_count == 0 ? "no URI"
                                         : !_config.allow_plaintext && select_broker(m, true, &plain)
                                             ? "only plaintext URIs, and allow_plaintext is off"
                                             : "no usable URI");
            } else {
                _has_broker = true;
                _mqtt_step = MqttStep::kEventTopics;
                log_printf(_config.log, LogLevel::kInfo, "broker %s:%u%s", _broker.host,
                           static_cast<unsigned>(_broker.port), _broker.tls ? " tls" : "");
                broker_selected();
            }
            return;
        }
    }
    if (_mqtt_step == MqttStep::kEventTopics) {
        build_program_events_topics_url(_url, sizeof(_url), _base_url, _program.id);
        if (!mqtt_discover(on_topics_response, "event topics", now_ms)) {
            return;
        }
        add_wanted(_storage.notifications->topics);
        if (_wanted_count == 0) {
            mqtt_unavailable(now_ms, "no event topics");
        } else if (_config.follow_program) {
            _mqtt_step = MqttStep::kProgramTopics;
        } else {
            mqtt_discovered(now_ms);
        }
        return;
    }
    if (_mqtt_step == MqttStep::kProgramTopics) {
        build_program_topics_url(_url, sizeof(_url), _base_url, _program.id);
        if (!mqtt_discover(on_topics_response, "program topics", now_ms)) {
            return;
        }
        add_wanted(_storage.notifications->topics);
        mqtt_discovered(now_ms);
        return;
    }
    if (_mqtt_step == MqttStep::kConnect) {
        const bool clean = !(_config.exclusive_tls && _broker.tls);
        bool session_present = false;
        if (!mqtt_connect(clean, &session_present, now_ms) ||
            !mqtt_subscribe(session_present, now_ms)) {
            return;
        }
        if (clean) {
            _mqtt_step = MqttStep::kSnapshotAfter;
            _due_now = true;
        } else {
            _mqtt_no_session = !session_present;
            mqtt_go_live();
        }
    }
}

bool VenClient::mqtt_connect(bool clean_session, bool* session_present, uint32_t now_ms) {
    MqttConnectOptions o;
    o.host = _broker.host;
    o.port = _broker.port;
    o.tls = _broker.tls;
    o.client_id = _client_id;
    o.verify = _config.mqtt_verify;
    o.ca_pem = _config.mqtt_ca_pem;
    o.keepalive_s = _config.mqtt_keepalive_s;
    o.clean_session = clean_session;
    o.timeout_ms = _config.mqtt_timeout_ms;
    log_printf(_config.log, LogLevel::kDebug, "MQTT connect %s:%u clean %d", _broker.host,
               static_cast<unsigned>(_broker.port), clean_session ? 1 : 0);
    *session_present = false;
    _mqtt_open = true;
    const MqttResult r = _mqtt->connect(o, session_present);
    _status.mqtt_result = r;
    if (r != MqttResult::kOk) {
        mqtt_fail(r, now_ms);
        return false;
    }
    return true;
}

bool VenClient::mqtt_subscribe(bool session_present, uint32_t now_ms) {
    NotificationStorage& ns = *_storage.notifications;
    if (!session_present) {
        _subscribed_count = 0;
    }
    for (size_t i = _subscribed_count; i-- > 0;) {
        if (in_list(ns.wanted, _wanted_count, ns.subscribed[i])) {
            continue;
        }
        const MqttResult r = _mqtt->unsubscribe(ns.subscribed[i], _config.mqtt_timeout_ms);
        if (r != MqttResult::kOk) {
            mqtt_fail(r, now_ms);
            return false;
        }
        --_subscribed_count;
        memmove(ns.subscribed[i], ns.subscribed[i + 1],
                (_subscribed_count - i) * sizeof(ns.subscribed[0]));
    }
    for (size_t i = 0; i < _wanted_count; ++i) {
        if (in_list(ns.subscribed, _subscribed_count, ns.wanted[i])) {
            continue;
        }
        const MqttResult r = _mqtt->subscribe(ns.wanted[i], 1, _config.mqtt_timeout_ms);
        if (r != MqttResult::kOk) {
            mqtt_fail(r, now_ms);
            return false;
        }
        memcpy(ns.subscribed[_subscribed_count++], ns.wanted[i], sizeof(ns.wanted[0]));
    }
    return true;
}

void VenClient::mqtt_go_live() {
    _mqtt_step = MqttStep::kLive;
    _status.notify = NotifyState::kConnected;
    _status.mqtt_failures = 0;
    if (_has_fetch && _status.consecutive_failures == 0) {
        _next_request_ms = _last_success_ms + period_s() * 1000;
    }
    log_printf(_config.log, LogLevel::kInfo, "MQTT subscribed to %u topics",
               static_cast<unsigned>(_subscribed_count));
}

void VenClient::mqtt_after_poll(uint32_t now_ms) {
    if (_config.notifications != NotificationMode::kMqtt) {
        return;
    }
    if (_mqtt_step == MqttStep::kIdle) {
        _mqtt_step = MqttStep::kNotifiers;
        _mqtt_next_ms = now_ms;
        _status.notify = NotifyState::kDiscovering;
    } else if (_mqtt_step == MqttStep::kSnapshotBefore) {
        _mqtt_step = MqttStep::kConnect;
        _mqtt_next_ms = now_ms;
    } else if (_mqtt_step == MqttStep::kSnapshotAfter) {
        mqtt_go_live();
    }
}

void VenClient::mqtt_suspend() {
    log_printf(_config.log, LogLevel::kDebug, "MQTT closed for an HTTPS request");
    mqtt_close();
    _mqtt_step = MqttStep::kSuspended;
    _mqtt_next_ms = _clock->monotonic_ms();
    _status.notify = NotifyState::kSuspended;
}

void VenClient::mqtt_resume(uint32_t now_ms) {
    bool session_present = false;
    if (!mqtt_connect(false, &session_present, now_ms)) {
        return;
    }
    // A snapshot here would loop on a broker that keeps no sessions; polling every
    // poll_period_s bounds what the gap loses instead.
    if (!session_present) {
        log_printf(_config.log, LogLevel::kInfo, "MQTT session not kept by the broker; subscribing");
    }
    if (!mqtt_subscribe(session_present, now_ms)) {
        return;
    }
    _mqtt_no_session = !session_present;
    mqtt_go_live();
}

void VenClient::mqtt_service(const DateTime& now, uint32_t now_ms) {
    _has_msg_window = make_interval(now, _config.horizon, &_msg_window);
    const MqttResult r = _mqtt->poll(0, on_message, this);
    if (_program_deleted) {
        log_printf(_config.log, LogLevel::kInfo, "program %s deleted; resolving again", _program.id);
        mqtt_reset();
        _event_count = 0;
        _data_changed = true;
        update_price(now);
        _has_fetch = false;
        _stale = false;
        _status.notify = NotifyState::kOff;
        _phase = Phase::kResolving;
        _has_program = false;
        _skip = 0;
        _pages = 0;
        _event_skip = 0;
        _due_now = true;
        _status.status = VenStatus::kResolvingProgram;
        return;
    }
    const bool lost = r == MqttResult::kConnectionLost || !_mqtt->connected();
    if (r == MqttResult::kOk && !lost) {
        return;
    }
    _status.mqtt_result = r == MqttResult::kOk ? MqttResult::kConnectionLost : r;
    if (!lost) {
        log_printf(_config.log, LogLevel::kWarn, "MQTT poll %s", to_string(r));
        return;
    }
    log_printf(_config.log, LogLevel::kWarn, "MQTT connection lost");
    const bool at_once = _status.mqtt_failures == 0;
    mqtt_close();
    mqtt_retry(now_ms, at_once);
    report(now_ms, false);
}

void VenClient::handle_message(const MqttMessage& m) {
    if (_phase != Phase::kPolling || _program_deleted) {
        return;
    }
    if (m.too_large || m.payload == nullptr) {
        log_printf(_config.log, LogLevel::kWarn, "notification on %s too large (%lu bytes)%s",
                   m.topic, static_cast<unsigned long>(m.len), m.retained ? ", retained" : "");
        if (!m.retained) {
            request_poll();
        }
        return;
    }
    ParseOptions o{};
    o.payload_types = &_config.payload_type;
    o.payload_type_count = 1;
    o.window = _has_msg_window ? &_msg_window : nullptr;
    o.program_id = _program.id;
    BufferReader in(m.payload, m.len);
    const ParseResult r =
        parse_notification(in, *_storage.arena, &_storage.notifications->notification, &o);
    if (r != ParseResult::kOk) {
        log_printf(_config.log, LogLevel::kWarn, "notification on %s: parse %s%s", m.topic,
                   to_string(r), m.retained ? ", retained" : "");
        if (!m.retained) {
            request_poll();
        }
        return;
    }
    apply_notification(m.retained);
}

size_t VenClient::find_event(const char* id) const {
    for (size_t i = 0; i < _event_count; ++i) {
        if (strcmp(_events[i].id, id) == 0) {
            return i;
        }
    }
    return _event_count;
}

void VenClient::remove_event(size_t i) {
    --_event_count;
    memmove(&_events[i], &_events[i + 1], (_event_count - i) * sizeof(Event));
}

// False when the stored copy is newer, so nothing changed.
bool VenClient::upsert_event(const Event& ev) {
    const size_t i = find_event(ev.id);
    const bool stored = i < _event_count;
    if (stored && ev.has_modification_date_time && _events[i].has_modification_date_time &&
        compare(ev.modification_date_time, _events[i].modification_date_time) < 0) {
        log_printf(_config.log, LogLevel::kDebug, "event %s: older than the stored copy", ev.id);
        return false;
    }
    const bool targeted = _config.target_count == 0 ||
                          targets_match(ev.targets, _config.targets, _config.target_count);
    // The parse drops intervals outside the window, so an event with none left lies outside.
    WindowFit fit{true, DateTime{}};
    if (_has_msg_window) {
        fit = ev.interval_count == 0 ? WindowFit{false, DateTime{}} : window_fit(ev, _msg_window);
    }
    if (!targeted || !fit.in_window) {
        if (stored) {
            remove_event(i);
        }
        return stored;
    }
    if (stored) {
        _events[i] = ev;
        return true;
    }
    if (_event_count < _storage.capacity) {
        _events[_event_count++] = ev;
        return true;
    }
    _event_truncated = true;
    _status.status = VenStatus::kTruncated;
    size_t latest = 0;
    DateTime latest_first{};
    for (size_t k = 0; k < _event_count; ++k) {
        const WindowFit f = window_fit(_events[k], _msg_window);
        if (k == 0 || compare(f.first, latest_first) > 0) {
            latest = k;
            latest_first = f.first;
        }
    }
    if (compare(fit.first, latest_first) >= 0) {
        return false;
    }
    remove_event(latest);
    _events[_event_count++] = ev;
    return true;
}

// A retained message may predate the /events snapshot, so it applies only as an update newer
// than the stored copy. So does an event the snapshot lacks that was modified before it was
// taken, for a port that cannot report the retained flag.
void VenClient::apply_notification(bool retained) {
    const Notification& n = _storage.notifications->notification;
    if (!n.has_object) {
        return;
    }
    if (n.object_type == ObjectType::kEvent) {
        const Event& ev = n.event;
        if (ev.id[0] == '\0' ||
            (ev.program_id[0] != '\0' && strcmp(ev.program_id, _program.id) != 0)) {
            return;
        }
        bool changed = false;
        if (n.operation == Operation::kCreate || n.operation == Operation::kUpdate) {
            const size_t i = find_event(ev.id);
            const bool stored = i < _event_count;
            const bool superseded = !stored && _has_fetch &&
                                    newer(true, _last_fetch, ev.has_modification_date_time,
                                          ev.modification_date_time);
            if (superseded ||
                (retained && (!stored || !newer(ev.has_modification_date_time,
                                                ev.modification_date_time,
                                                _events[i].has_modification_date_time,
                                                _events[i].modification_date_time)))) {
                log_printf(_config.log, LogLevel::kDebug, "%s event %s predates the fetch%s",
                           to_string(n.operation), ev.id, retained ? ", retained" : "");
                return;
            }
            changed = upsert_event(ev);
        } else if (n.operation == Operation::kDelete) {
            const size_t i = find_event(ev.id);
            if (i < _event_count) {
                remove_event(i);
                changed = true;
            }
        } else {
            return;
        }
        log_printf(_config.log, LogLevel::kInfo, "notification %s event %s%s", to_string(n.operation),
                   ev.id, changed ? "" : ", no change");
        if (changed) {
            _data_changed = true;
            ++_status.notifications;
        }
        return;
    }
    if (n.object_type == ObjectType::kProgram && strcmp(n.program.id, _program.id) == 0) {
        if (n.operation == Operation::kUpdate) {
            const Program& p = n.program;
            const bool older = newer(_program.has_modification_date_time,
                                     _program.modification_date_time,
                                     p.has_modification_date_time, p.modification_date_time);
            if (older || (retained && !newer(p.has_modification_date_time, p.modification_date_time,
                                             _program.has_modification_date_time,
                                             _program.modification_date_time))) {
                log_printf(_config.log, LogLevel::kDebug, "%s program UPDATE ignored",
                           older ? "older" : "retained");
                return;
            }
            _program = n.program;
            _data_changed = true;
        } else if (n.operation == Operation::kDelete) {
            _program_deleted = true;
        } else {
            return;
        }
        log_printf(_config.log, LogLevel::kInfo, "notification %s program %s",
                   to_string(n.operation), n.program.id);
        ++_status.notifications;
    }
}

}  // namespace oa3
