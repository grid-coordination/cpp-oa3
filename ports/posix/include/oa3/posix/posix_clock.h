#pragma once

#include <stdint.h>

#include <oa3/client/clock.h>

namespace oa3 {
namespace posix {

class PosixClock final : public Clock {
public:
    // CLOCK_REALTIME; false before 2020-01-01T00:00:00Z, taken as a clock not yet set.
    bool now(DateTime* out) override;
    // CLOCK_MONOTONIC, truncated to 32 bits.
    uint32_t monotonic_ms() override;
};

}  // namespace posix
}  // namespace oa3
