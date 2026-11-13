#include <oa3/model/time_interval.h>

namespace oa3 {

namespace {

int compare_ends(const TimeInterval& a, const TimeInterval& b) {
    if (a.end_indefinite || b.end_indefinite) {
        return a.end_indefinite == b.end_indefinite ? 0 : (a.end_indefinite ? 1 : -1);
    }
    return compare(a.end, b.end);
}

// a's end against b's start; an indefinite end is later than any start.
int compare_end_start(const TimeInterval& a, const TimeInterval& b) {
    return a.end_indefinite ? 1 : compare(a.end, b.start);
}

}  // namespace

bool make_interval(const DateTime& start, const Duration& d, TimeInterval* out) {
    if (out == nullptr || is_negative(d)) {
        return false;
    }
    TimeInterval r{};
    r.start = start;
    r.end_indefinite = is_indefinite(d) || !add(start, d, &r.end);
    if (r.end_indefinite) {
        r.end = DateTime{};
    }
    *out = r;
    return true;
}

AllenRelation allen_relation(const TimeInterval& a, const TimeInterval& b) {
    const int ab = compare_end_start(a, b);  // a.end vs b.start
    const int ba = compare_end_start(b, a);  // b.end vs a.start
    const int starts = compare(a.start, b.start);
    if (ab < 0) {
        return AllenRelation::kBefore;
    }
    if (ba < 0) {
        return AllenRelation::kAfter;
    }
    if (ab == 0 && starts < 0) {
        return AllenRelation::kMeets;
    }
    if (ba == 0 && starts > 0) {
        return AllenRelation::kMetBy;
    }
    const int ends = compare_ends(a, b);
    if (starts == 0) {
        return ends == 0 ? AllenRelation::kEquals : (ends < 0 ? AllenRelation::kStarts : AllenRelation::kStartedBy);
    }
    if (ends == 0) {
        return starts > 0 ? AllenRelation::kFinishes : AllenRelation::kFinishedBy;
    }
    if (starts > 0) {
        return ends < 0 ? AllenRelation::kDuring : AllenRelation::kOverlappedBy;
    }
    return ends > 0 ? AllenRelation::kContains : AllenRelation::kOverlaps;
}

AllenRelation inverse(AllenRelation r) {
    switch (r) {
        case AllenRelation::kBefore:
            return AllenRelation::kAfter;
        case AllenRelation::kMeets:
            return AllenRelation::kMetBy;
        case AllenRelation::kOverlaps:
            return AllenRelation::kOverlappedBy;
        case AllenRelation::kStarts:
            return AllenRelation::kStartedBy;
        case AllenRelation::kDuring:
            return AllenRelation::kContains;
        case AllenRelation::kFinishes:
            return AllenRelation::kFinishedBy;
        case AllenRelation::kEquals:
            return AllenRelation::kEquals;
        case AllenRelation::kAfter:
            return AllenRelation::kBefore;
        case AllenRelation::kMetBy:
            return AllenRelation::kMeets;
        case AllenRelation::kOverlappedBy:
            return AllenRelation::kOverlaps;
        case AllenRelation::kStartedBy:
            return AllenRelation::kStarts;
        case AllenRelation::kContains:
            return AllenRelation::kDuring;
        case AllenRelation::kFinishedBy:
            return AllenRelation::kFinishes;
    }
    return r;
}

const char* to_string(AllenRelation r) {
    switch (r) {
        case AllenRelation::kBefore:
            return "before";
        case AllenRelation::kMeets:
            return "meets";
        case AllenRelation::kOverlaps:
            return "overlaps";
        case AllenRelation::kStarts:
            return "starts";
        case AllenRelation::kDuring:
            return "during";
        case AllenRelation::kFinishes:
            return "finishes";
        case AllenRelation::kEquals:
            return "equals";
        case AllenRelation::kAfter:
            return "after";
        case AllenRelation::kMetBy:
            return "met_by";
        case AllenRelation::kOverlappedBy:
            return "overlapped_by";
        case AllenRelation::kStartedBy:
            return "started_by";
        case AllenRelation::kContains:
            return "contains";
        case AllenRelation::kFinishedBy:
            return "finished_by";
    }
    return "unknown";
}

bool intersects(const TimeInterval& a, const TimeInterval& b) {
    const AllenRelation r = allen_relation(a, b);
    return r != AllenRelation::kBefore && r != AllenRelation::kMeets && r != AllenRelation::kMetBy &&
           r != AllenRelation::kAfter;
}

bool contains_instant(const TimeInterval& interval, const DateTime& t) {
    const TimeInterval point{t, t, false};
    const AllenRelation r = allen_relation(point, interval);
    return r == AllenRelation::kStarts || r == AllenRelation::kDuring;
}

}  // namespace oa3
