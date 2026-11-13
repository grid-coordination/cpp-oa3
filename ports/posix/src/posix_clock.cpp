#include <oa3/posix/posix_clock.h>

#include <time.h>

namespace oa3 {
namespace posix {

static const int64_t kEarliestValid = 1577836800;  // 2020-01-01T00:00:00Z

bool PosixClock::now(DateTime* out) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || ts.tv_sec < kEarliestValid) return false;
    return from_unix(static_cast<int64_t>(ts.tv_sec), static_cast<uint32_t>(ts.tv_nsec), out);
}

uint32_t PosixClock::monotonic_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ms = static_cast<uint64_t>(ts.tv_sec) * 1000u + static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
    return static_cast<uint32_t>(ms);
}

}  // namespace posix
}  // namespace oa3
