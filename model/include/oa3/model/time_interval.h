#pragma once

#include <stdint.h>

#include <oa3/model/time.h>

namespace oa3 {

// A half-open span [start, end) of UTC instants. end is ignored when end_indefinite.
struct TimeInterval {
    DateTime start;
    DateTime end;
    bool end_indefinite;
};

// [start, start + d). P9999Y (User_Guide 7.3), or an end past the DateTime range, gives an
// indefinite end. False when d is negative.
bool make_interval(const DateTime& start, const Duration& d, TimeInterval* out);

// Allen's thirteen interval relations, of a to b. Endpoints compare as instants, so a zero-length
// [t, t) at b's start starts b, and at b's end is met by b.
enum class AllenRelation : uint8_t {
    kBefore,
    kMeets,
    kOverlaps,
    kStarts,
    kDuring,
    kFinishes,
    kEquals,
    kAfter,
    kMetBy,
    kOverlappedBy,
    kStartedBy,
    kContains,
    kFinishedBy,
};

AllenRelation allen_relation(const TimeInterval& a, const TimeInterval& b);
AllenRelation inverse(AllenRelation r);
const char* to_string(AllenRelation r);

// Any relation but before, meets, met_by and after: a and b share an instant, or a zero-length
// one lies in (or equals) the other.
bool intersects(const TimeInterval& a, const TimeInterval& b);

// start <= t < end.
bool contains_instant(const TimeInterval& interval, const DateTime& t);

}  // namespace oa3
