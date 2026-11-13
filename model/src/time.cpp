#include <oa3/model/time.h>

#include <stdint.h>
#include <string.h>

namespace oa3 {

namespace {

constexpr int64_t kNanosPerSecond = 1000000000;
constexpr int64_t kSecondsPerDay = 86400;

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool read_fixed(const char* s, size_t end, size_t* pos, size_t n, unsigned* out) {
    if (end - *pos < n) {
        return false;
    }
    unsigned v = 0;
    for (size_t i = 0; i < n; ++i) {
        char c = s[*pos + i];
        if (!is_digit(c)) {
            return false;
        }
        v = v * 10 + static_cast<unsigned>(c - '0');
    }
    *pos += n;
    *out = v;
    return true;
}

// Reads 1 or more digits as a fraction: keeps the first 9 in nanos, truncates the rest.
bool read_fraction(const char* s, size_t end, size_t* pos, uint32_t* nanos, uint8_t* digits) {
    size_t p = *pos;
    uint32_t v = 0;
    uint8_t n = 0;
    while (p < end && is_digit(s[p])) {
        if (n < 9) {
            v = v * 10 + static_cast<uint32_t>(s[p] - '0');
            ++n;
        }
        ++p;
    }
    if (p == *pos) {
        return false;
    }
    for (uint8_t i = n; i < 9; ++i) {
        v *= 10;
    }
    *pos = p;
    *nanos = v;
    *digits = n;
    return true;
}

int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        --q;
    }
    return q;
}

int64_t floor_mod(int64_t a, int64_t b) { return a - floor_div(a, b) * b; }

uint8_t fewest_frac_digits(uint32_t nanos) {
    if (nanos == 0) {
        return 0;
    }
    uint8_t digits = 9;
    while (nanos % 10 == 0) {
        nanos /= 10;
        --digits;
    }
    return digits;
}

class Writer {
public:
    Writer(char* buf, size_t size) : _buf(buf), _size(size), _len(0), _ok(buf != nullptr && size > 0) {}

    void put(char c) {
        if (!_ok || _len + 1 >= _size) {
            _ok = false;
            return;
        }
        _buf[_len++] = c;
    }

    void put_padded(uint64_t v, unsigned width) {
        char tmp[20];
        unsigned n = 0;
        do {
            tmp[n++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v != 0 && n < sizeof(tmp));
        for (unsigned i = n; i < width; ++i) {
            put('0');
        }
        while (n > 0) {
            put(tmp[--n]);
        }
    }

    void put_fraction(uint32_t nanos, unsigned digits) {
        char tmp[9];
        for (int i = 8; i >= 0; --i) {
            tmp[i] = static_cast<char>('0' + nanos % 10);
            nanos /= 10;
        }
        put('.');
        for (unsigned i = 0; i < digits && i < 9; ++i) {
            put(tmp[i]);
        }
    }

    size_t fail() {
        _ok = false;
        return finish();
    }

    size_t finish() {
        if (!_ok) {
            if (_buf != nullptr && _size > 0) {
                _buf[0] = '\0';
            }
            return 0;
        }
        _buf[_len] = '\0';
        return _len;
    }

private:
    char* _buf;
    size_t _size;
    size_t _len;
    bool _ok;
};

}  // namespace

int64_t days_from_civil(int64_t year, unsigned month, unsigned day) {
    const int64_t y = year - (month <= 2 ? 1 : 0);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t mp = month > 2 ? month - 3 : month + 9;
    const int64_t doy = (153 * mp + 2) / 5 + static_cast<int64_t>(day) - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civil_from_days(int64_t days, int64_t* year, unsigned* month, unsigned* day) {
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const unsigned d = static_cast<unsigned>(doy - (153 * mp + 2) / 5 + 1);
    const unsigned m = static_cast<unsigned>(mp < 10 ? mp + 3 : mp - 9);
    *year = yoe + era * 400 + (m <= 2 ? 1 : 0);
    *month = m;
    *day = d;
}

unsigned days_in_month(int64_t year, unsigned month) {
    static const uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 0;
    }
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        return leap ? 29 : 28;
    }
    return kDays[month - 1];
}

namespace {

// The instants whose UTC year fits int16_t.
bool in_range(int64_t seconds) {
    static const int64_t kMin = days_from_civil(INT16_MIN, 1, 1) * kSecondsPerDay;
    static const int64_t kMax = days_from_civil(static_cast<int64_t>(INT16_MAX) + 1, 1, 1) * kSecondsPerDay - 1;
    return seconds >= kMin && seconds <= kMax;
}

}  // namespace

bool parse_datetime(const char* s, size_t len, DateTime* out) {
    if (s == nullptr || out == nullptr) {
        return false;
    }
    size_t pos = 0;
    size_t end = len;
    while (pos < end && is_space(s[pos])) {
        ++pos;
    }
    while (end > pos && is_space(s[end - 1])) {
        --end;
    }

    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (!read_fixed(s, end, &pos, 4, &year) || pos >= end || s[pos++] != '-' ||
        !read_fixed(s, end, &pos, 2, &month) || pos >= end || s[pos++] != '-' ||
        !read_fixed(s, end, &pos, 2, &day)) {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > days_in_month(year, month)) {
        return false;
    }
    DateTime dt{};
    dt.seconds = days_from_civil(year, month, day) * kSecondsPerDay;

    if (pos == end) {
        *out = dt;
        return true;
    }

    const char sep = s[pos++];
    if (sep != 'T' && sep != 't' && sep != ' ') {
        return false;
    }
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    if (!read_fixed(s, end, &pos, 2, &hour) || pos >= end || s[pos++] != ':' ||
        !read_fixed(s, end, &pos, 2, &minute) || pos >= end || s[pos++] != ':' ||
        !read_fixed(s, end, &pos, 2, &second)) {
        return false;
    }
    if (hour > 23 || minute > 59 || second > 59) {
        return false;
    }
    dt.seconds += hour * 3600 + minute * 60 + second;

    if (pos < end && s[pos] == '.') {
        ++pos;
        if (!read_fraction(s, end, &pos, &dt.nanos, &dt.frac_digits)) {
            return false;
        }
    }

    if (pos < end) {
        const char z = s[pos++];
        if (z == '+' || z == '-') {
            unsigned oh = 0;
            unsigned om = 0;
            if (!read_fixed(s, end, &pos, 2, &oh) || pos >= end || s[pos++] != ':' ||
                !read_fixed(s, end, &pos, 2, &om)) {
                return false;
            }
            if (oh > 23 || om > 59) {
                return false;
            }
            const int64_t offset = static_cast<int64_t>(oh * 3600 + om * 60);
            dt.seconds -= z == '-' ? -offset : offset;
        } else if (z != 'Z' && z != 'z') {
            return false;
        }
    }
    if (pos != end) {
        return false;
    }
    *out = dt;
    return true;
}

bool parse_datetime(const char* s, DateTime* out) {
    if (s == nullptr) {
        return false;
    }
    return parse_datetime(s, strlen(s), out);
}

CivilTime to_civil(const DateTime& dt) {
    const int64_t days = floor_div(dt.seconds, kSecondsPerDay);
    const int64_t secs = floor_mod(dt.seconds, kSecondsPerDay);
    int64_t year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civil_from_days(days, &year, &month, &day);
    CivilTime c{};
    c.year = static_cast<int32_t>(year);
    c.month = static_cast<uint8_t>(month);
    c.day = static_cast<uint8_t>(day);
    c.hour = static_cast<uint8_t>(secs / 3600);
    c.minute = static_cast<uint8_t>((secs / 60) % 60);
    c.second = static_cast<uint8_t>(secs % 60);
    return c;
}

size_t format_datetime(const DateTime& dt, char* buf, size_t size) {
    Writer w(buf, size);
    if (!in_range(dt.seconds)) {
        return w.fail();
    }
    const CivilTime c = to_civil(dt);
    if (c.year < 0 || c.year > 9999) {
        return w.fail();
    }
    w.put_padded(static_cast<uint64_t>(c.year), 4);
    w.put('-');
    w.put_padded(c.month, 2);
    w.put('-');
    w.put_padded(c.day, 2);
    w.put('T');
    w.put_padded(c.hour, 2);
    w.put(':');
    w.put_padded(c.minute, 2);
    w.put(':');
    w.put_padded(c.second, 2);
    const unsigned digits = dt.frac_digits > 0 ? dt.frac_digits : fewest_frac_digits(dt.nanos);
    if (digits > 0) {
        w.put_fraction(dt.nanos, digits);
    }
    w.put('Z');
    return w.finish();
}

bool from_unix(int64_t unix_seconds, uint32_t nanos, DateTime* out) {
    if (out == nullptr || !in_range(unix_seconds)) {
        return false;
    }
    unix_seconds += static_cast<int64_t>(nanos / kNanosPerSecond);
    nanos = static_cast<uint32_t>(nanos % kNanosPerSecond);
    if (!in_range(unix_seconds)) {
        return false;
    }
    DateTime dt{};
    dt.seconds = unix_seconds;
    dt.nanos = nanos;
    dt.frac_digits = fewest_frac_digits(nanos);
    *out = dt;
    return true;
}

bool from_unix(int64_t unix_seconds, DateTime* out) {
    return from_unix(unix_seconds, 0, out);
}

int compare(const DateTime& a, const DateTime& b) {
    if (a.seconds != b.seconds) {
        return a.seconds < b.seconds ? -1 : 1;
    }
    if (a.nanos != b.nanos) {
        return a.nanos < b.nanos ? -1 : 1;
    }
    return 0;
}

bool parse_duration(const char* s, size_t len, Duration* out) {
    if (s == nullptr || out == nullptr) {
        return false;
    }
    size_t pos = 0;
    bool negative = false;
    if (pos < len && s[pos] == '-') {
        negative = true;
        ++pos;
    }
    if (pos >= len || s[pos++] != 'P') {
        return false;
    }
    // The spec's lookahead (?=\d|T\d).
    if (pos >= len || !(is_digit(s[pos]) || (s[pos] == 'T' && pos + 1 < len && is_digit(s[pos + 1])))) {
        return false;
    }

    int64_t f[7] = {0, 0, 0, 0, 0, 0, 0};  // years, months, weeks, days, hours, minutes, seconds
    uint32_t nanos = 0;
    uint8_t frac_digits = 0;

    int rank = 0;  // next allowed date designator: 0 Y, 1 M, 2 D/W, 3 none
    while (pos < len && s[pos] != 'T') {
        int64_t v = 0;
        const size_t start = pos;
        while (pos < len && is_digit(s[pos])) {
            v = v * 10 + (s[pos] - '0');
            if (v > INT32_MAX) {
                return false;
            }
            ++pos;
        }
        if (pos == start || pos >= len) {
            return false;
        }
        const char c = s[pos++];
        if (c == 'Y' && rank <= 0) {
            f[0] = v;
            rank = 1;
        } else if (c == 'M' && rank <= 1) {
            f[1] = v;
            rank = 2;
        } else if ((c == 'D' || c == 'W') && rank <= 2) {
            f[c == 'W' ? 2 : 3] = v;
            rank = 3;
        } else {
            return false;
        }
    }

    if (pos < len) {
        ++pos;  // 'T'
        if (pos >= len) {
            return false;
        }
        rank = 0;  // 0 H, 1 M, 2 S, 3 none
        while (pos < len) {
            int64_t v = 0;
            const size_t start = pos;
            while (pos < len && is_digit(s[pos])) {
                v = v * 10 + (s[pos] - '0');
                if (v > INT32_MAX) {
                    return false;
                }
                ++pos;
            }
            if (pos == start || pos >= len) {
                return false;
            }
            bool has_frac = false;
            if (s[pos] == '.') {
                ++pos;
                if (!read_fraction(s, len, &pos, &nanos, &frac_digits) || pos >= len) {
                    return false;
                }
                has_frac = true;
            }
            const char c = s[pos++];
            if (c == 'H' && rank <= 0 && !has_frac) {
                f[4] = v;
                rank = 1;
            } else if (c == 'M' && rank <= 1 && !has_frac) {
                f[5] = v;
                rank = 2;
            } else if (c == 'S' && rank <= 2) {
                f[6] = v;
                rank = 3;
            } else {
                return false;
            }
        }
    }

    const int32_t sign = negative ? -1 : 1;
    Duration d{};
    d.years = sign * static_cast<int32_t>(f[0]);
    d.months = sign * static_cast<int32_t>(f[1]);
    d.weeks = sign * static_cast<int32_t>(f[2]);
    d.days = sign * static_cast<int32_t>(f[3]);
    d.hours = sign * static_cast<int32_t>(f[4]);
    d.minutes = sign * static_cast<int32_t>(f[5]);
    d.seconds = sign * static_cast<int32_t>(f[6]);
    d.nanos = sign * static_cast<int32_t>(nanos);
    d.frac_digits = frac_digits;
    *out = d;
    return true;
}

bool parse_duration(const char* s, Duration* out) {
    if (s == nullptr) {
        return false;
    }
    return parse_duration(s, strlen(s), out);
}

size_t format_duration(const Duration& d, char* buf, size_t size) {
    const int32_t fields[8] = {d.years, d.months, d.weeks, d.days, d.hours, d.minutes, d.seconds, d.nanos};
    bool any_pos = false;
    bool any_neg = false;
    for (int32_t v : fields) {
        any_pos = any_pos || v > 0;
        any_neg = any_neg || v < 0;
    }
    Writer w(buf, size);
    if (any_pos && any_neg) {
        return w.fail();
    }
    uint64_t a[8];
    for (int i = 0; i < 8; ++i) {
        const int64_t v = fields[i];
        a[i] = static_cast<uint64_t>(v < 0 ? -v : v);
    }
    if (any_neg) {
        w.put('-');
    }
    w.put('P');
    if (!any_pos && !any_neg) {
        w.put('T');
        w.put('0');
        w.put('S');
        return w.finish();
    }
    static const char kDate[4] = {'Y', 'M', 'W', 'D'};
    for (int i = 0; i < 4; ++i) {
        if (a[i] != 0) {
            w.put_padded(a[i], 1);
            w.put(kDate[i]);
        }
    }
    if (a[4] != 0 || a[5] != 0 || a[6] != 0 || a[7] != 0) {
        w.put('T');
        if (a[4] != 0) {
            w.put_padded(a[4], 1);
            w.put('H');
        }
        if (a[5] != 0) {
            w.put_padded(a[5], 1);
            w.put('M');
        }
        if (a[6] != 0 || a[7] != 0) {
            w.put_padded(a[6], 1);
            const uint32_t nanos = static_cast<uint32_t>(a[7]);
            const unsigned digits = d.frac_digits > 0 ? d.frac_digits : fewest_frac_digits(nanos);
            if (digits > 0) {
                w.put_fraction(nanos, digits);
            }
            w.put('S');
        }
    }
    return w.finish();
}

bool is_zero(const Duration& d) {
    return d.years == 0 && d.months == 0 && d.weeks == 0 && d.days == 0 && d.hours == 0 && d.minutes == 0 &&
           d.seconds == 0 && d.nanos == 0;
}

bool is_negative(const Duration& d) {
    return d.years < 0 || d.months < 0 || d.weeks < 0 || d.days < 0 || d.hours < 0 || d.minutes < 0 ||
           d.seconds < 0 || d.nanos < 0;
}

bool is_indefinite(const Duration& d) { return d.years >= 9999 && !is_negative(d); }

bool duration_sum(const Duration& a, const Duration& b, Duration* out) {
    if (out == nullptr) {
        return false;
    }
    int64_t nanos = static_cast<int64_t>(a.nanos) + b.nanos;
    int64_t seconds = static_cast<int64_t>(a.seconds) + b.seconds;
    seconds += nanos / kNanosPerSecond;
    nanos %= kNanosPerSecond;
    if (seconds > 0 && nanos < 0) {
        --seconds;
        nanos += kNanosPerSecond;
    } else if (seconds < 0 && nanos > 0) {
        ++seconds;
        nanos -= kNanosPerSecond;
    }
    const int64_t sums[7] = {
        static_cast<int64_t>(a.years) + b.years,     static_cast<int64_t>(a.months) + b.months,
        static_cast<int64_t>(a.weeks) + b.weeks,     static_cast<int64_t>(a.days) + b.days,
        static_cast<int64_t>(a.hours) + b.hours,     static_cast<int64_t>(a.minutes) + b.minutes,
        seconds,
    };
    for (int64_t v : sums) {
        if (v > INT32_MAX || v < INT32_MIN) {
            return false;
        }
    }
    Duration r{};
    r.years = static_cast<int32_t>(sums[0]);
    r.months = static_cast<int32_t>(sums[1]);
    r.weeks = static_cast<int32_t>(sums[2]);
    r.days = static_cast<int32_t>(sums[3]);
    r.hours = static_cast<int32_t>(sums[4]);
    r.minutes = static_cast<int32_t>(sums[5]);
    r.seconds = static_cast<int32_t>(sums[6]);
    r.nanos = static_cast<int32_t>(nanos);
    r.frac_digits = a.frac_digits > b.frac_digits ? a.frac_digits : b.frac_digits;
    *out = r;
    return true;
}

bool add(const DateTime& dt, const Duration& d, DateTime* out) {
    if (out == nullptr || !in_range(dt.seconds)) {
        return false;
    }
    const CivilTime c = to_civil(dt);
    const int64_t month_index = static_cast<int64_t>(c.year) * 12 + (static_cast<int64_t>(c.month) - 1) +
                                static_cast<int64_t>(d.years) * 12 + d.months;
    const int64_t year = floor_div(month_index, 12);
    const unsigned month = static_cast<unsigned>(floor_mod(month_index, 12) + 1);
    unsigned day = c.day;
    const unsigned dim = days_in_month(year, month);
    if (day > dim) {
        day = dim;
    }
    if (year < INT16_MIN || year > INT16_MAX) {
        return false;
    }

    const int64_t days = days_from_civil(year, month, day) + static_cast<int64_t>(d.weeks) * 7 + d.days;
    int64_t nanos = static_cast<int64_t>(dt.nanos) + d.nanos;
    const int64_t secs = days * kSecondsPerDay + floor_mod(dt.seconds, kSecondsPerDay) +
                         static_cast<int64_t>(d.hours) * 3600 + static_cast<int64_t>(d.minutes) * 60 + d.seconds +
                         floor_div(nanos, kNanosPerSecond);
    nanos = floor_mod(nanos, kNanosPerSecond);
    if (!in_range(secs)) {
        return false;
    }

    DateTime r{};
    r.seconds = secs;
    r.nanos = static_cast<uint32_t>(nanos);
    r.frac_digits = dt.frac_digits > d.frac_digits ? dt.frac_digits : d.frac_digits;
    *out = r;
    return true;
}

}  // namespace oa3
