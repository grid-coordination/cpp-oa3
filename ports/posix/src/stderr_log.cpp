#include <oa3/posix/stderr_log.h>

#include <stdio.h>
#include <time.h>

#include <oa3/model/time.h>

namespace oa3 {
namespace posix {

void stderr_log(void*, LogLevel level, const char* line) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    char when[kDateTimeStringSize] = "";
    DateTime dt;
    uint32_t millis = static_cast<uint32_t>(ts.tv_nsec / 1000000) * 1000000u;
    if (from_unix(static_cast<int64_t>(ts.tv_sec), millis, &dt)) {
        dt.frac_digits = 3;
        format_datetime(dt, when, sizeof(when));
    }
    fprintf(stderr, "%s oa3 %s %s\n", when, to_string(level), line);
}

}  // namespace posix
}  // namespace oa3
