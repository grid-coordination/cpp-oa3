#include <oa3/model/intervals.h>

#include <stdint.h>
#include <string.h>

namespace oa3 {

namespace {

// 0001-01-01T00:00:00Z
constexpr int64_t kSequentialSentinel = -62135596800LL;

bool is_sequential_sentinel(const DateTime& dt) {
    return dt.seconds == kSequentialSentinel && dt.nanos == 0;
}

}  // namespace

IntervalResolver::IntervalResolver(const Event& event)
    : _event(&event),
      _event_period(event.has_interval_period ? &event.interval_period : nullptr),
      _next(0),
      _has_accumulated(false),
      _accumulated{},
      _has_prev(false),
      _prev_start{},
      _prev_duration{} {}

IntervalResolver::IntervalResolver(const IntervalPeriod* event_period)
    : _event(nullptr),
      _event_period(event_period),
      _next(0),
      _has_accumulated(false),
      _accumulated{},
      _has_prev(false),
      _prev_start{},
      _prev_duration{} {}

void IntervalResolver::reset() {
    _next = 0;
    _has_accumulated = false;
    _accumulated = Duration{};
    _has_prev = false;
    _prev_start = DateTime{};
    _prev_duration = Duration{};
}

bool IntervalResolver::next(ResolvedInterval* out) {
    if (_event == nullptr) return false;
    const Event& ev = *_event;
    if (_next >= ev.interval_count || _next >= OA3_MAX_INTERVALS) return false;
    resolve(ev.intervals[_next], out);
    return true;
}

void IntervalResolver::resolve(const Interval& iv, ResolvedInterval* out) {
    const IntervalPeriod* eip = _event_period;
    const bool own = iv.has_interval_period;

    ResolvedInterval r{};
    r.index = _next;
    r.id = iv.id;

    if (own && iv.has_duration) {
        r.has_duration = true;
        r.duration = to_duration(iv.duration);
    } else if (eip && eip->has_duration) {
        r.has_duration = true;
        r.duration = to_duration(eip->duration);
    }

    const bool sentinel = own && iv.has_start && is_sequential_sentinel(iv.start);
    if (sentinel && _next == 0 && eip && eip->has_start) {
        r.has_start = true;
        r.start = eip->start;
    } else if (sentinel && _next > 0) {
        if (_has_prev) r.has_start = add(_prev_start, _prev_duration, &r.start);
    } else if (own && iv.has_start) {
        r.has_start = true;
        r.start = iv.start;
    } else if (eip && eip->has_start) {
        if (_has_accumulated) {
            r.has_start = add(eip->start, _accumulated, &r.start);
        } else {
            r.has_start = true;
            r.start = eip->start;
        }
    }

#ifdef OA3_INTERVAL_RANDOMIZE_START
    if (own && iv.has_randomize_start) {
        r.has_randomize_start = true;
        r.randomize_start = to_duration(iv.randomize_start);
    }
#endif
    if (!r.has_randomize_start && eip && eip->has_randomize_start) {
        r.has_randomize_start = true;
        r.randomize_start = to_duration(eip->randomize_start);
    }

    r.has_period = r.has_start && r.has_duration && make_interval(r.start, r.duration, &r.period);

    if (r.has_duration) {
        if (!_has_accumulated) {
            _accumulated = r.duration;
            _has_accumulated = true;
        } else {
            Duration sum;
            // On int32 overflow the sum stays put; no real event reaches it.
            if (duration_sum(_accumulated, r.duration, &sum)) _accumulated = sum;
        }
    }

    _has_prev = r.has_start && r.has_duration;
    if (_has_prev) {
        _prev_start = r.start;
        _prev_duration = r.duration;
    }

    ++_next;
    *out = r;
}

size_t resolve_intervals(const Event& event, ResolvedInterval* out, size_t capacity) {
    IntervalResolver resolver(event);
    size_t n = 0;
    while (n < capacity && resolver.next(&out[n])) ++n;
    return n;
}

ExtentBuilder::ExtentBuilder() : _any(false), _all_periods(true), _first_start{}, _hull{} {}

void ExtentBuilder::add(const ResolvedInterval& r) {
    if (!r.has_period) {
        _all_periods = false;
        return;
    }
    if (!_any) {
        _any = true;
        _first_start = r.period.start;
        _hull = r.period;
        return;
    }
    switch (allen_relation(r.period, _hull)) {
        case AllenRelation::kBefore:
        case AllenRelation::kMeets:
        case AllenRelation::kOverlaps:
        case AllenRelation::kFinishedBy:
        case AllenRelation::kContains:
            _hull.start = r.period.start;
            break;
        default:
            break;
    }
    switch (allen_relation(r.period, _hull)) {
        case AllenRelation::kAfter:
        case AllenRelation::kMetBy:
        case AllenRelation::kOverlappedBy:
        case AllenRelation::kStartedBy:
        case AllenRelation::kContains:
            _hull.end = r.period.end;
            _hull.end_indefinite = r.period.end_indefinite;
            break;
        default:
            break;
    }
}

EventExtent ExtentBuilder::finish(bool has_duration, const CompactDuration& duration) const {
    EventExtent x{};
    x.has_period = _any;
    x.hull = _hull;
    x.span = _hull;
    // price_at shifts by whole seconds, so a hull shorter than one second cannot loop.
    if (!_any || !_all_periods || !has_duration || _hull.end_indefinite ||
        to_unix(_hull.end) - to_unix(_hull.start) <= 0) {
        return x;
    }
    TimeInterval bound;
    if (!make_interval(_first_start, to_duration(duration), &bound)) return x;
    x.looping = true;
    x.span.end = bound.end;
    x.span.end_indefinite = bound.end_indefinite;
    return x;
}

EventExtent event_extent(const Event& event) {
    ExtentBuilder builder;
    IntervalResolver resolver(event);
    ResolvedInterval r;
    while (resolver.next(&r)) builder.add(r);
    return builder.finish(event.has_duration, event.duration);
}

bool is_unitary_payload_type(const char* type) {
    if (!type) return false;
    return strcmp(type, "PRICE") == 0 || strcmp(type, "EXPORT_PRICE") == 0 ||
           strcmp(type, "GHG") == 0;
}

namespace {

int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

DateTime later_by(DateTime t, int64_t seconds) {
    t.seconds += seconds;
    return t;
}

TimeInterval shifted(const TimeInterval& iv, int64_t seconds) {
    TimeInterval r = iv;
    r.start = later_by(iv.start, seconds);
    if (!r.end_indefinite) r.end = later_by(iv.end, seconds);
    return r;
}

bool is_empty(const TimeInterval& iv) {
    return !iv.end_indefinite && compare(iv.start, iv.end) >= 0;
}

const Payload* payload_of(const Interval& iv, int type) {
    uint8_t n = iv.payload_count <= OA3_MAX_PAYLOADS ? iv.payload_count : OA3_MAX_PAYLOADS;
    for (uint8_t i = 0; i < n; ++i) {
        if (iv.payloads[i].type_index == type) return &iv.payloads[i];
    }
    return nullptr;
}

// True when a beats b: lower priority number, absent below present.
bool outranks(const Event& a, const Event& b) {
    if (a.has_priority != b.has_priority) return a.has_priority;
    return a.has_priority && a.priority < b.priority;
}

void consider_boundary(const DateTime& t, const DateTime& now, PriceAt* out) {
    if (compare(t, now) <= 0) return;
    if (!out->has_next_change || compare(t, out->next_change) < 0) {
        out->has_next_change = true;
        out->next_change = t;
    }
}

}  // namespace

bool price_at(const Event* events, size_t event_count, const DateTime& now,
              const char* payload_type, PriceAt* out) {
    *out = PriceAt{};
    if (!events || !payload_type) return false;
    const bool unitary = is_unitary_payload_type(payload_type);

    for (size_t e = 0; e < event_count; ++e) {
        const Event& ev = events[e];
        const int type = find_type(ev, payload_type);
        if (type < 0) continue;
        const EventExtent extent = event_extent(ev);

        int64_t cycle = 0;
        int64_t k_first = 0;
        int64_t k_last = 0;
        if (extent.looping) {
            cycle = to_unix(extent.hull.end) - to_unix(extent.hull.start);
            const int64_t since = to_unix(now) - to_unix(extent.hull.start);
            k_first = since < 0 ? 0 : floor_div(since, cycle);
            k_last = k_first + 1;
        }

        IntervalResolver resolver(ev);
        ResolvedInterval r;
        while (resolver.next(&r)) {
            if (!r.has_period || is_empty(r.period)) continue;
            const Payload* p = payload_of(ev.intervals[r.index], type);
            if (!p || p->value_count == 0 || p->first_value >= ev.value_count) continue;
            uint16_t n = p->value_count;
            if (n > ev.value_count - p->first_value) {
                n = static_cast<uint16_t>(ev.value_count - p->first_value);
            }

            const uint16_t subs = unitary && !r.period.end_indefinite ? n : 1;
            const int64_t width =
                r.period.end_indefinite ? 0 : (to_unix(r.period.end) - to_unix(r.period.start)) / subs;

            for (uint16_t s = 0; s < subs; ++s) {
                TimeInterval sub = r.period;
                sub.start = later_by(r.period.start, width * s);
                if (s + 1 < subs) sub.end = later_by(sub.start, width);
                if (is_empty(sub)) continue;

                for (int64_t k = k_first; k <= k_last; ++k) {
                    TimeInterval span = extent.looping ? shifted(sub, k * cycle) : sub;
                    if (extent.looping) {
                        switch (allen_relation(span, extent.span)) {
                            case AllenRelation::kBefore:
                            case AllenRelation::kMeets:
                            case AllenRelation::kMetBy:
                            case AllenRelation::kAfter:
                                continue;
                            case AllenRelation::kOverlappedBy:
                            case AllenRelation::kStartedBy:
                            case AllenRelation::kContains:
                                span.end = extent.span.end;
                                span.end_indefinite = extent.span.end_indefinite;
                                break;
                            default:
                                break;
                        }
                    }

                    consider_boundary(span.start, now, out);
                    if (!span.end_indefinite) consider_boundary(span.end, now, out);

                    if (!contains_instant(span, now)) continue;
                    // Iteration order is event index then interval index, so only a strictly
                    // better priority replaces the incumbent.
                    if (out->active && !outranks(ev, *out->event)) continue;
                    out->active = true;
                    out->event = &ev;
                    out->interval_index = r.index;
                    out->value_index = unitary ? s : 0;
                    out->value = &ev.values[p->first_value + out->value_index];
                    out->value_count = unitary ? 1 : n;
                    out->span = span;
                }
            }
        }
    }
    return out->active;
}

bool price_at(const Event* events, size_t event_count, int64_t now_unix,
              const char* payload_type, PriceAt* out) {
    DateTime now{};
    if (!from_unix(now_unix, &now)) {
        *out = PriceAt{};
        return false;
    }
    return price_at(events, event_count, now, payload_type, out);
}

size_t price_schedule(const Event* events, size_t event_count, const TimeInterval& window,
                      const char* payload_type, PriceStep* out, size_t max, bool* more) {
    if (more) *more = false;
    if (window.end_indefinite || compare(window.start, window.end) > 0) return 0;
    DateTime t = window.start;
    size_t n = 0;
    while (compare(t, window.end) < 0) {
        if (n == max) {
            if (more) *more = true;
            break;
        }
        PriceAt p;
        price_at(events, event_count, t, payload_type, &p);
        DateTime end = window.end;
        if (p.has_next_change && compare(p.next_change, window.end) < 0) end = p.next_change;
        if (compare(end, t) <= 0) break;
        PriceStep& s = out[n++];
        s.span.start = t;
        s.span.end = end;
        s.span.end_indefinite = false;
        s.event = p.active ? p.event : nullptr;
        s.value = p.active ? p.value : nullptr;
        t = end;
    }
    return n;
}

}  // namespace oa3
