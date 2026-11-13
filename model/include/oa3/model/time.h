#pragma once

#include <stddef.h>
#include <stdint.h>

namespace oa3 {

// An instant in UTC. Offsets on the wire are applied at parse time and not kept; local time
// is the port's concern. Proleptic Gregorian, years -32768..32767.
struct DateTime {
    int64_t seconds;      // since 1970-01-01T00:00:00Z
    uint32_t nanos;       // 0..999999999
    uint8_t frac_digits;  // digits of fraction on the wire, 0..9, kept for formatting
};

// "YYYY-MM-DDTHH:MM:SS.fffffffffZ" and the NUL.
constexpr size_t kDateTimeStringSize = 31;

// RFC 3339 with "Z" or a "+hh:mm"/"-hh:mm" offset, converted to UTC. A string without a zone
// (VTN-RI "YYYY-MM-DD HH:MM:SS", or a date alone) is read as UTC.
bool parse_datetime(const char* s, size_t len, DateTime* out);
bool parse_datetime(const char* s, DateTime* out);

// Writes the NUL-terminated UTC form with "Z" and returns its length, or 0 if it does not fit
// or the year is outside 0..9999.
size_t format_datetime(const DateTime& dt, char* buf, size_t size);

inline int64_t to_unix(const DateTime& dt) { return dt.seconds; }

// frac_digits is the fewest digits that represent nanos. False outside the year range.
bool from_unix(int64_t unix_seconds, uint32_t nanos, DateTime* out);
bool from_unix(int64_t unix_seconds, DateTime* out);

struct CivilTime {
    int32_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
};

CivilTime to_civil(const DateTime& dt);

// Orders by instant, fraction included; <0, 0, >0.
int compare(const DateTime& a, const DateTime& b);

int64_t days_from_civil(int64_t year, unsigned month, unsigned day);
void civil_from_days(int64_t days, int64_t* year, unsigned* month, unsigned* day);
unsigned days_in_month(int64_t year, unsigned month);

// ISO 8601 duration as calendar fields, as the spec's pattern allows:
// [-]P[nY][nM][nD|nW][T[nH][nM][n[.f]S]]. A leading '-' negates every field.
struct Duration {
    int32_t years;
    int32_t months;
    int32_t weeks;
    int32_t days;
    int32_t hours;
    int32_t minutes;
    int32_t seconds;
    int32_t nanos;        // same sign as seconds
    uint8_t frac_digits;  // digits of the seconds fraction on the wire, 0..9
};

// Worst case of every field at INT32 range, the sign and the fraction, and the NUL.
constexpr size_t kDurationStringSize = 96;

bool parse_duration(const char* s, size_t len, Duration* out);
bool parse_duration(const char* s, Duration* out);

// Writes fields as they are, without normalizing (PT36H stays PT36H); the zero duration
// is "PT0S". Returns the length, or 0 if it does not fit or the fields mix signs.
size_t format_duration(const Duration& d, char* buf, size_t size);

bool is_zero(const Duration& d);
bool is_negative(const Duration& d);

// "P9999Y" means indefinite (User_Guide 7.3).
bool is_indefinite(const Duration& d);

// Field by field, without normalizing. False on int32 overflow.
bool duration_sum(const Duration& a, const Duration& b, Duration* out);

// Calendar-aware add on the UTC date, as pendulum DateTime.add() in UTC: years and months
// first, clamping the day to the target month's length (Jan 31 + P1M is Feb 29 or 28), then
// weeks and days, then hours, minutes, seconds and fraction as elapsed time. False if the
// year leaves the DateTime range.
bool add(const DateTime& dt, const Duration& d, DateTime* out);

}  // namespace oa3
