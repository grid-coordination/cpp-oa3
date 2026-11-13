#include <stdio.h>
#include <time.h>

#include <oa3/posix/posix_clock.h>

int main() {
    oa3::posix::PosixClock clock;
    int failures = 0;

    oa3::DateTime now;
    time_t before = time(nullptr);
    bool valid = clock.now(&now);
    time_t after = time(nullptr);
    if (!valid || now.seconds < before || now.seconds > after) {
        fprintf(stderr, "FAIL now: valid %d seconds %lld time %lld..%lld\n", valid,
                static_cast<long long>(now.seconds), static_cast<long long>(before),
                static_cast<long long>(after));
        ++failures;
    }

    uint32_t prev = clock.monotonic_ms();
    for (int i = 0; i < 200; ++i) {
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, nullptr);
        uint32_t cur = clock.monotonic_ms();
        if (static_cast<int32_t>(cur - prev) < 0) {
            fprintf(stderr, "FAIL monotonic_ms went back: %u then %u\n", prev, cur);
            ++failures;
            break;
        }
        prev = cur;
    }

    printf("%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
