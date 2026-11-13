#pragma once

#include <stdint.h>

#include <oa3/model/time.h>

namespace oa3 {

class Clock {
public:
    // The current UTC instant; false until the port's wall clock is set (e.g. by SNTP).
    virtual bool now(DateTime* out) = 0;
    // Milliseconds since an arbitrary start, unaffected by wall-clock steps. Wraps at 2^32;
    // compare differences.
    virtual uint32_t monotonic_ms() = 0;

protected:
    ~Clock() = default;
};

}  // namespace oa3
