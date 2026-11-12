#pragma once

#include <stddef.h>
#include <stdint.h>

#include <oa3/model/entities.h>
#include <oa3/model/time.h>

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
    bool has_period;  // has_start and has_duration
    int64_t start_unix;
    int64_t end_unix;  // to_unix(add(start, duration))
};

// Yields the event's intervals in order, resolved, without storage for all of them.
class IntervalResolver {
public:
    explicit IntervalResolver(const Event& event);
    bool next(ResolvedInterval* out);
    void reset();

private:
    const Event* _event;
    uint16_t _next;
    bool _has_accumulated;
    Duration _accumulated;
    bool _has_prev;  // the previous interval resolved a start and a duration
    DateTime _prev_start;
    Duration _prev_duration;
};

// Returns the number written: the event's interval count, at most capacity.
size_t resolve_intervals(const Event& event, ResolvedInterval* out, size_t capacity);

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
    int64_t start_unix;    // the active span
    int64_t end_unix;
    bool has_next_change;
    int64_t next_change_unix;  // the first span boundary after now, active or not
};

// The value of payload_type in force at now across events. Returns out->active.
bool price_at(const Event* events, size_t event_count, int64_t now_unix,
              const char* payload_type, PriceAt* out);
bool price_at(const Event* events, size_t event_count, const DateTime& now,
              const char* payload_type, PriceAt* out);

}  // namespace oa3
