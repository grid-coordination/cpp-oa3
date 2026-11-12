#include <oa3/model/intervals.h>

#include <stdint.h>
#include <string.h>

namespace oa3 {

namespace {

constexpr int64_t kNoEnd = INT64_MAX;

int64_t end_of(const DateTime& start, const Duration& d) {
    DateTime end;
    if (!add(start, d, &end)) return kNoEnd;
    return to_unix(end);
}

bool is_sequential_sentinel(const DateTime& dt) {
    return dt.year == 1 && dt.month == 1 && dt.day == 1 && dt.hour == 0 && dt.minute == 0 &&
           dt.second == 0 && dt.nanos == 0;
}

}  // namespace

IntervalResolver::IntervalResolver(const Event& event)
    : _event(&event),
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
    const Event& ev = *_event;
    if (_next >= ev.interval_count || _next >= OA3_MAX_INTERVALS) return false;
    const Interval& iv = ev.intervals[_next];
    const IntervalPeriod* ip = iv.has_interval_period ? &iv.interval_period : nullptr;
    const IntervalPeriod* eip = ev.has_interval_period ? &ev.interval_period : nullptr;

    ResolvedInterval r{};
    r.index = _next;
    r.id = iv.id;

    if (ip && ip->has_duration) {
        r.has_duration = true;
        r.duration = ip->duration;
    } else if (eip && eip->has_duration) {
        r.has_duration = true;
        r.duration = eip->duration;
    }

    const bool sentinel = ip && ip->has_start && is_sequential_sentinel(ip->start);
    if (sentinel && _next == 0 && eip && eip->has_start) {
        r.has_start = true;
        r.start = eip->start;
    } else if (sentinel && _next > 0) {
        if (_has_prev) r.has_start = add(_prev_start, _prev_duration, &r.start);
    } else if (ip && ip->has_start) {
        r.has_start = true;
        r.start = ip->start;
    } else if (eip && eip->has_start) {
        if (_has_accumulated) {
            r.has_start = add(eip->start, _accumulated, &r.start);
        } else {
            r.has_start = true;
            r.start = eip->start;
        }
    }

    if (ip && ip->has_randomize_start) {
        r.has_randomize_start = true;
        r.randomize_start = ip->randomize_start;
    } else if (eip && eip->has_randomize_start) {
        r.has_randomize_start = true;
        r.randomize_start = eip->randomize_start;
    }

    if (r.has_start && r.has_duration) {
        r.has_period = true;
        r.start_unix = to_unix(r.start);
        r.end_unix = end_of(r.start, r.duration);
    }

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
    return true;
}

size_t resolve_intervals(const Event& event, ResolvedInterval* out, size_t capacity) {
    IntervalResolver resolver(event);
    size_t n = 0;
    while (n < capacity && resolver.next(&out[n])) ++n;
    return n;
}

bool is_unitary_payload_type(const char* type) {
    if (!type) return false;
    return strcmp(type, "PRICE") == 0 || strcmp(type, "EXPORT_PRICE") == 0 ||
           strcmp(type, "GHG") == 0;
}

namespace {

// How an event's spans repeat in time (User_Guide 7.3 "event.duration").
struct Loop {
    bool looping;
    int64_t origin;  // S, the earliest start
    int64_t cycle;   // C, latest end minus S
    int64_t end;     // span end, kNoEnd when indefinite
};

Loop event_loop(const Event& ev) {
    Loop loop{false, 0, 0, kNoEnd};
    if (!ev.has_duration || is_negative(ev.duration)) return loop;

    IntervalResolver resolver(ev);
    ResolvedInterval r;
    bool any = false;
    int64_t earliest = 0;
    int64_t latest = 0;
    DateTime first_start{};
    while (resolver.next(&r)) {
        if (!r.has_period) return loop;
        if (!any) {
            first_start = r.start;
            earliest = r.start_unix;
            latest = r.end_unix;
            any = true;
        } else {
            if (r.start_unix < earliest) earliest = r.start_unix;
            if (r.end_unix > latest) latest = r.end_unix;
        }
    }
    if (!any || latest == kNoEnd || latest <= earliest) return loop;

    loop.looping = true;
    loop.origin = earliest;
    loop.cycle = latest - earliest;
    loop.end = is_indefinite(ev.duration) ? kNoEnd : end_of(first_start, ev.duration);
    return loop;
}

int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

const Payload* payload_of(const Interval& iv, const char* type) {
    uint8_t n = iv.payload_count <= OA3_MAX_PAYLOADS ? iv.payload_count : OA3_MAX_PAYLOADS;
    for (uint8_t i = 0; i < n; ++i) {
        if (strcmp(iv.payloads[i].type, type) == 0) return &iv.payloads[i];
    }
    return nullptr;
}

// True when a beats b: lower priority number, absent below present.
bool outranks(const Event& a, const Event& b) {
    if (a.has_priority != b.has_priority) return a.has_priority;
    return a.has_priority && a.priority < b.priority;
}

void consider_boundary(int64_t t, int64_t now, PriceAt* out) {
    if (t <= now || t == kNoEnd) return;
    if (!out->has_next_change || t < out->next_change_unix) {
        out->has_next_change = true;
        out->next_change_unix = t;
    }
}

}  // namespace

bool price_at(const Event* events, size_t event_count, int64_t now_unix,
              const char* payload_type, PriceAt* out) {
    *out = PriceAt{};
    if (!events || !payload_type) return false;

    for (size_t e = 0; e < event_count; ++e) {
        const Event& ev = events[e];
        const Loop loop = event_loop(ev);

        int64_t k_first = 0;
        int64_t k_last = 0;
        if (loop.looping) {
            int64_t k_now = now_unix < loop.origin
                                ? 0
                                : floor_div(now_unix - loop.origin, loop.cycle);
            k_first = k_now;
            k_last = k_now + 1;
        }

        IntervalResolver resolver(ev);
        ResolvedInterval r;
        while (resolver.next(&r)) {
            if (!r.has_period || r.end_unix <= r.start_unix) continue;
            const Payload* p = payload_of(ev.intervals[r.index], payload_type);
            if (!p || p->value_count == 0 || p->first_value >= ev.value_count) continue;
            uint16_t n = p->value_count;
            if (n > ev.value_count - p->first_value) {
                n = static_cast<uint16_t>(ev.value_count - p->first_value);
            }

            const bool unitary = is_unitary_payload_type(payload_type);
            const uint16_t subs = unitary ? n : 1;
            const int64_t width = (r.end_unix - r.start_unix) / subs;

            for (uint16_t s = 0; s < subs; ++s) {
                const int64_t base_start = r.start_unix + width * s;
                const int64_t base_end = (s + 1 == subs) ? r.end_unix : base_start + width;
                if (base_end <= base_start) continue;

                for (int64_t k = k_first; k <= k_last; ++k) {
                    const int64_t shift = loop.looping ? k * loop.cycle : 0;
                    const int64_t start = base_start + shift;
                    if (start >= loop.end) continue;
                    int64_t end = base_end + shift;
                    if (end > loop.end) end = loop.end;

                    consider_boundary(start, now_unix, out);
                    consider_boundary(end, now_unix, out);

                    if (start > now_unix || now_unix >= end) continue;
                    if (out->active) {
                        const Event& cur = *out->event;
                        // Iteration order is event index then interval index, so only a
                        // strictly better priority replaces the incumbent.
                        if (!outranks(ev, cur)) continue;
                    }
                    out->active = true;
                    out->event = &ev;
                    out->interval_index = r.index;
                    out->value_index = unitary ? s : 0;
                    out->value = &ev.values[p->first_value + out->value_index];
                    out->value_count = unitary ? 1 : n;
                    out->start_unix = start;
                    out->end_unix = end;
                }
            }
        }
    }
    return out->active;
}

bool price_at(const Event* events, size_t event_count, const DateTime& now,
              const char* payload_type, PriceAt* out) {
    return price_at(events, event_count, to_unix(now), payload_type, out);
}

}  // namespace oa3
