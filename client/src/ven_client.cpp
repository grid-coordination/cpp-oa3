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

}  // namespace

VenClient::VenClient(HttpTransport& http, Clock& clock, const VenStorage& storage)
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
      _options() {}

bool VenClient::start(const VenConfig& config) {
    _phase = Phase::kStopped;
    _status.status = VenStatus::kStopped;
    _reported_status = VenStatus::kStopped;

    if (config.base_url == nullptr || strlen(config.base_url) > OA3_URL_MAX) {
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

    _config = config;
    _config.base_url = _base_url;
    _config.program_name = config.program_name != nullptr ? _program_name : nullptr;
    _config.program_id = config.program_id != nullptr ? _program_id : nullptr;
    _config.payload_type = _payload_type;
    _config.targets = config.target_count > 0 ? _target_ptrs : nullptr;

    const TargetList* targets = _targets.count > 0 ? &_targets : nullptr;
    char probe_id[OA3_ID_MAX + 1];
    memset(probe_id, 'x', OA3_ID_MAX);
    probe_id[OA3_ID_MAX] = '\0';
    if (!build_events_url(_url, sizeof(_url), _base_url, probe_id, targets, UINT32_MAX,
                          OA3_EVENTS_PAGE_LIMIT)) {
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
    return true;
}

void VenClient::stop() {
    _phase = Phase::kStopped;
    _status.status = VenStatus::kStopped;
    _reported_status = VenStatus::kStopped;
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
    uint32_t due = ms_until_request(_clock->monotonic_ms());
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

bool VenClient::fail(VenStatus kind, uint32_t now_ms) {
    _status.status = kind;
    if (_status.consecutive_failures < UINT16_MAX) {
        ++_status.consecutive_failures;
    }
    uint64_t delay_s = _config.backoff_min_s;
    for (uint16_t i = 1; i < _status.consecutive_failures && delay_s < _config.backoff_max_s; ++i) {
        delay_s *= 2;
    }
    if (delay_s > _config.backoff_max_s) {
        delay_s = _config.backoff_max_s;
    }
    _next_request_ms = now_ms + static_cast<uint32_t>(delay_s * 1000);
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
    _next_request_ms = now_ms + _config.poll_period_s * 1000;
    log_printf(_config.log, LogLevel::kDebug, "%u events kept, pages %u",
               static_cast<unsigned>(_event_count), static_cast<unsigned>(_event_pages));
    return false;
}

void VenClient::report(uint32_t now_ms, bool force) {
    _status.retry_in_ms = ms_until_request(now_ms);
    _status.has_data = _has_fetch;
    const bool changed = _status.status != _reported_status;
    if (!changed && !force) {
        return;
    }
    if (changed) {
        log_printf(_config.log, LogLevel::kInfo, "status %s", to_string(_status.status));
    }
    _reported_status = _status.status;
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

    bool failed = false;
    if (_due_now || static_cast<int32_t>(now_ms - _next_request_ms) >= 0) {
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
    }

    if (_has_fetch && _config.stale_after_polls > 0) {
        const uint64_t limit_ms =
            static_cast<uint64_t>(_config.stale_after_polls) * _config.poll_period_s * 1000;
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

}  // namespace oa3
