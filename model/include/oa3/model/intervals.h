#pragma once

#include <stddef.h>
#include <stdint.h>

#include <oa3/model/entities.h>
#include <oa3/model/time.h>
#include <oa3/model/time_interval.h>

namespace oa3 {

// An interval with event.intervalPeriod inheritance applied, as python-oa3
// Event.resolved_intervals() (models.py:272-350), plus two spec rules python lacks: an
// interval start of 0001-01-01T00:00:00 follows the previous interval (User_Guide.md:640-645),
// and randomizeStart is inherited from the event (User_Guide.md:635).
struct ResolvedInterval {
    uint16_t index;  // into event.intervals
    int32_t id;
    bool has_start;
    bool has_duration;
    bool has_randomize_start;
    DateTime start;
    Duration duration;
    Duration randomize_start;
    bool has_period;  // has_start and a non-negative duration
    TimeInterval period;
};

// Yields the event's intervals in order, resolved, without storage for all of them.
class IntervalResolver {
public:
    explicit IntervalResolver(const Event& event);
    // Without an Event: pass each interval's period, in order, to resolve().
    explicit IntervalResolver(const IntervalPeriod* event_period);
    bool next(ResolvedInterval* out);
    void resolve(const IntervalPeriod* interval_period, int32_t id, ResolvedInterval* out);
    void reset();

private:
    const Event* _event;
    const IntervalPeriod* _event_period;
    uint16_t _next;
    bool _has_accumulated;
    Duration _accumulated;
    bool _has_prev;  // the previous interval resolved a start and a duration
    DateTime _prev_start;
    Duration _prev_duration;
};

// Returns the number written: the event's interval count, at most capacity.
size_t resolve_intervals(const Event& event, ResolvedInterval* out, size_t capacity);

// Where an event lies in time. It loops (User_Guide 7.3 "event.duration") when it has a
// non-negative duration and every interval has a finite period: the intervals repeat every
// hull length from the hull's start until the first interval's start plus the duration.
struct EventExtent {
    bool has_period;  // some interval has a period
    bool looping;
    TimeInterval hull;  // of the intervals with a period
    TimeInterval span;  // looping: [hull.start, first start + duration); else the hull
};

class ExtentBuilder {
public:
    ExtentBuilder();
    void add(const ResolvedInterval& r);
    EventExtent finish(bool has_duration, const Duration& duration) const;

private:
    bool _any;
    bool _all_periods;
    DateTime _first_start;
    TimeInterval _hull;
};

EventExtent event_extent(const Event& event);

// PRICE, EXPORT_PRICE and GHG: one value per sub-interval when a payload packs several
// (User_Guide.md:674-677, 730).
bool is_unitary_payload_type(const char* type);

struct PriceAt {
    bool active;
    const Event* event;
    uint16_t interval_index;
    uint16_t value_index;  // within the payload
    const Value* value;
    uint16_t value_count;  // values the active span covers: 1, or the payload's count for a
                           // type that is not unitary
    TimeInterval span;     // the active span
    bool has_next_change;
    DateTime next_change;  // the first span boundary after now, active or not
};

// The value of payload_type in force at now across events. Returns out->active.
bool price_at(const Event* events, size_t event_count, const DateTime& now,
              const char* payload_type, PriceAt* out);
bool price_at(const Event* events, size_t event_count, int64_t now_unix,
              const char* payload_type, PriceAt* out);

// One step of the schedule price_schedule() walks: span is clipped to the window, and value
// is nullptr where no price is in force.
struct PriceStep {
    TimeInterval span;
    const Event* event;
    const Value* value;
};

// The prices in force across a bounded window, one step per span price_at() reports, in time
// order, for drawing a forecast. Writes at most max steps and returns how many; *more (when
// not null) is set when the window continues past the last one written. An indefinite or
// inverted window gives 0.
size_t price_schedule(const Event* events, size_t event_count, const TimeInterval& window,
                      const char* payload_type, PriceStep* out, size_t max, bool* more);

}  // namespace oa3
