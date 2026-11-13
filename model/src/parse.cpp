#include <oa3/model/parse.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <ArduinoJson.h>

#include <oa3/model/intervals.h>
#include <oa3/model/time.h>

// Inlined into parse_one, the window-fit locals stay live under copy_event and add about
// 600 bytes to the deepest stack on xtensa.
#if defined(__GNUC__)
#define OA3_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define OA3_NOINLINE __declspec(noinline)
#else
#define OA3_NOINLINE
#endif

namespace oa3 {

const char* to_string(ParseResult r) {
    switch (r) {
        case ParseResult::kOk:
            return "OK";
        case ParseResult::kJsonError:
            return "JSON_ERROR";
        case ParseResult::kTooLarge:
            return "TOO_LARGE";
        case ParseResult::kCapacityExceeded:
            return "CAPACITY_EXCEEDED";
        case ParseResult::kInvalidField:
            return "INVALID_FIELD";
        case ParseResult::kTruncated:
            return "TRUNCATED";
    }
    return "UNKNOWN";
}

namespace {

using ArduinoJson::DeserializationError;
using ArduinoJson::JsonArrayConst;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObjectConst;
using ArduinoJson::JsonVariantConst;

// ArduinoJson keeps neither a number's text nor, for a short mantissa, its double (it stores a
// float). LiteralReader sits between the source and ArduinoJson and rewrites each number N in
// intervals[].payloads[].values[], and in a {x,y} point there, as the string "\x01N", so the
// document carries the wire literal. A literal longer than OA3_VALUE_TEXT_MAX becomes "\x02".
// Every string the source has in those places gets a leading "\x03", so the first byte of a
// value string is always the reader's and "\u0001" on the wire cannot pose as a literal.
constexpr char kLiteralMark = '\x01';
constexpr char kTooLongMark = '\x02';
constexpr char kStringMark = '\x03';

enum Tag : uint8_t {
    kTagOther,
    kTagIntervals,
    kTagInterval,
    kTagPayloads,
    kTagPayload,
    kTagValues,
    kTagPoint,
};

constexpr uint8_t kMaxLexDepth = 16;
constexpr size_t kKeyBufSize = 16;

bool is_number_char(int c) {
    return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
}

class LiteralReader {
public:
    explicit LiteralReader(ByteReader& src) : _src(src), _pushback(-1) { reset_lexer(); }

    void reset_lexer() {
        _depth = 0;
        _overflow_depth = 0;
        _in_string = false;
        _escape = false;
        _quote = 0;
        _have_key = false;
        _key_valid = false;
        _key[0] = '\0';
        _str_len = 0;
        _str_bad = false;
        _lit_len = 0;
        _lit_active = false;
        _lit_too_long = false;
        _out_len = 0;
        _out_pos = 0;
    }

    // A byte from the source, bypassing the lexer and the rewrite.
    int read_raw() {
        if (_pushback >= 0) {
            int c = _pushback;
            _pushback = -1;
            return c;
        }
        return _src.read();
    }

    void unread(int c) { _pushback = c; }

    int read() {
        if (_out_pos < _out_len) {
            return static_cast<unsigned char>(_out[_out_pos++]);
        }
        _out_pos = 0;
        _out_len = 0;
        for (;;) {
            int c = read_raw();
            if (_lit_active) {
                if (c >= 0 && is_number_char(c)) {
                    append_literal(static_cast<char>(c));
                    continue;
                }
                emit_literal();
                if (c >= 0) {
                    lex(static_cast<char>(c));
                    _out[_out_len++] = static_cast<char>(c);
                }
                _out_pos = 1;
                return static_cast<unsigned char>(_out[0]);
            }
            if (c < 0) {
                return -1;
            }
            uint8_t tag = top_tag();
            if (!_in_string && (tag == kTagValues || tag == kTagPoint) &&
                (c == '-' || (c >= '0' && c <= '9'))) {
                _lit_active = true;
                _lit_len = 0;
                _lit_too_long = false;
                append_literal(static_cast<char>(c));
                continue;
            }
            if (!_in_string && (c == '"' || c == '\'') &&
                (tag == kTagValues || (tag == kTagPoint && _have_key))) {
                lex(static_cast<char>(c));
                _out[0] = static_cast<char>(c);
                _out[1] = kStringMark;
                _out_len = 2;
                _out_pos = 1;
                return c;
            }
            lex(static_cast<char>(c));
            return c;
        }
    }

    size_t readBytes(char* buffer, size_t length) {
        size_t n = 0;
        while (n < length) {
            int c = read();
            if (c < 0) {
                break;
            }
            buffer[n++] = static_cast<char>(c);
        }
        return n;
    }

private:
    void append_literal(char c) {
        if (_lit_len < OA3_VALUE_TEXT_MAX) {
            _lit[_lit_len++] = c;
        } else {
            _lit_too_long = true;
        }
    }

    void emit_literal() {
        _lit_active = false;
        _out[_out_len++] = '"';
        if (_lit_too_long) {
            _out[_out_len++] = kTooLongMark;
        } else {
            _out[_out_len++] = kLiteralMark;
            memcpy(_out + _out_len, _lit, _lit_len);
            _out_len += _lit_len;
        }
        _out[_out_len++] = '"';
    }

    uint8_t top_tag() const {
        if (_overflow_depth > 0 || _depth == 0) {
            return kTagOther;
        }
        return _tags[_depth - 1];
    }

    bool top_is_object() const {
        return _overflow_depth == 0 && _depth > 0 && !_is_array[_depth - 1];
    }

    bool key_is(const char* k) const { return _key_valid && strcmp(_key, k) == 0; }

    uint8_t tag_for_open(bool array) const {
        uint8_t parent = top_tag();
        if (array) {
            if (!top_is_object()) {
                return kTagOther;
            }
            if (key_is("intervals")) {
                return kTagIntervals;
            }
            if (parent == kTagInterval && key_is("payloads")) {
                return kTagPayloads;
            }
            if (parent == kTagPayload && key_is("values")) {
                return kTagValues;
            }
            return kTagOther;
        }
        if (parent == kTagIntervals) {
            return kTagInterval;
        }
        if (parent == kTagPayloads) {
            return kTagPayload;
        }
        if (parent == kTagValues) {
            return kTagPoint;
        }
        return kTagOther;
    }

    void lex(char c) {
        if (_in_string) {
            if (_escape) {
                _escape = false;
                _str_bad = true;
            } else if (c == '\\') {
                _escape = true;
            } else if (c == _quote) {
                _in_string = false;
                if (top_is_object() && !_have_key) {
                    _key_valid = !_str_bad;
                    memcpy(_key, _str, _str_len);
                    _key[_str_len] = '\0';
                }
            } else if (_str_len < kKeyBufSize - 1) {
                _str[_str_len++] = c;
            } else {
                _str_bad = true;
            }
            return;
        }
        switch (c) {
            case '"':
            case '\'':
                _in_string = true;
                _quote = c;
                _str_len = 0;
                _str_bad = false;
                break;
            case ':':
                _have_key = true;
                break;
            case ',':
                _have_key = false;
                break;
            case '{':
            case '[': {
                bool array = c == '[';
                uint8_t tag = tag_for_open(array);
                if (_overflow_depth > 0 || _depth == kMaxLexDepth) {
                    ++_overflow_depth;
                } else {
                    _tags[_depth] = tag;
                    _is_array[_depth] = array;
                    ++_depth;
                }
                _have_key = false;
                _key_valid = false;
                break;
            }
            case '}':
            case ']':
                if (_overflow_depth > 0) {
                    --_overflow_depth;
                } else if (_depth > 0) {
                    --_depth;
                }
                break;
            default:
                break;
        }
    }

    ByteReader& _src;
    int _pushback;

    uint8_t _depth;
    uint16_t _overflow_depth;
    uint8_t _tags[kMaxLexDepth];
    bool _is_array[kMaxLexDepth];
    bool _in_string;
    bool _escape;
    char _quote;
    bool _have_key;
    bool _key_valid;
    char _key[kKeyBufSize];
    char _str[kKeyBufSize];
    size_t _str_len;
    bool _str_bad;

    bool _lit_active;
    bool _lit_too_long;
    size_t _lit_len;
    char _lit[OA3_VALUE_TEXT_MAX];

    char _out[OA3_VALUE_TEXT_MAX + 4];
    size_t _out_len;
    size_t _out_pos;
};

// Each key in camelCase and in VTN-RI snake_case. One "e" filter serves programs and events;
// "n" wraps it in the notification envelope.
#define OA3_ENTITY_FILTER                                                                       \
    "{\"id\":true,\"programID\":true,\"program_id\":true,\"programName\":true,"                 \
    "\"program_name\":true,\"eventName\":true,\"event_name\":true,\"objectType\":true,"         \
    "\"object_type\":true,\"modificationDateTime\":true,\"modification_date_time\":true,"       \
    "\"priority\":true,\"duration\":true,\"intervalPeriod\":true,\"interval_period\":true,"     \
    "\"targets\":true,\"localPrice\":true,\"local_price\":true,"                                \
    "\"attributes\":[{\"type\":true,\"values\":true}],"                                         \
    "\"payloadDescriptors\":[{\"objectType\":true,\"object_type\":true,\"payloadType\":true,"   \
    "\"payload_type\":true,\"units\":true,\"currency\":true}],"                                 \
    "\"payload_descriptors\":[{\"objectType\":true,\"object_type\":true,\"payloadType\":true,"  \
    "\"payload_type\":true,\"units\":true,\"currency\":true}],"                                 \
    "\"intervals\":[{\"id\":true,\"intervalPeriod\":true,\"interval_period\":true,"             \
    "\"payloads\":[{\"type\":true,\"values\":true}]}]}"

const char kFilterJson[] =
    "{\"e\":" OA3_ENTITY_FILTER
    ",\"n\":{\"objectType\":true,\"object_type\":true,\"operation\":true,\"targets\":true,"
    "\"object\":" OA3_ENTITY_FILTER "}}";

#undef OA3_ENTITY_FILTER

// Two variant pools (one is copied when ArduinoJson shrinks it) and the key strings. A
// variant slot is two pointers wide.
constexpr size_t kFilterArenaSize = 2 * ARDUINOJSON_POOL_CAPACITY * 2 * sizeof(void*) + 3072;

struct Filters {
    StaticJsonArena<kFilterArenaSize> arena;
    JsonDocument doc;
    bool ok;

    Filters() : arena(), doc(&arena), ok(false) {
        ok = !deserializeJson(doc, kFilterJson) && !arena.exhausted();
    }
};

const Filters& filters() {
    static const Filters f;
    return f;
}

ParseResult map_error(DeserializationError e, const JsonArena& arena) {
    if (e == DeserializationError::NoMemory || e == DeserializationError::TooDeep ||
        arena.exhausted()) {
        return ParseResult::kTooLarge;
    }
    if (e) {
        return ParseResult::kJsonError;
    }
    return ParseResult::kOk;
}

JsonVariantConst get(JsonObjectConst o, const char* camel, const char* snake) {
    JsonVariantConst v = o[camel];
    if (v.isNull() && snake != nullptr) {
        v = o[snake];
    }
    return v;
}

bool type_kept(const ParseOptions* opt, const char* type) {
    if (opt == nullptr || opt->payload_types == nullptr) {
        return true;
    }
    for (size_t i = 0; i < opt->payload_type_count; ++i) {
        if (opt->payload_types[i] != nullptr && strcmp(opt->payload_types[i], type) == 0) {
            return true;
        }
    }
    return false;
}

ParseResult copy_string(JsonVariantConst v, char* dst, size_t size, bool required) {
    dst[0] = '\0';
    if (v.isNull()) {
        return required ? ParseResult::kInvalidField : ParseResult::kOk;
    }
    if (!v.is<const char*>()) {
        return ParseResult::kInvalidField;
    }
    const char* s = v.as<const char*>();
    size_t len = strlen(s);
    if (len >= size) {
        return ParseResult::kCapacityExceeded;
    }
    memcpy(dst, s, len + 1);
    return ParseResult::kOk;
}

// Truncates at a UTF-8 character boundary.
ParseResult copy_truncated(JsonVariantConst v, char* dst, size_t size) {
    dst[0] = '\0';
    if (v.isNull()) {
        return ParseResult::kOk;
    }
    if (!v.is<const char*>()) {
        return ParseResult::kInvalidField;
    }
    const char* s = v.as<const char*>();
    size_t len = strlen(s);
    if (len >= size) {
        len = size - 1;
        while (len > 0 && (static_cast<unsigned char>(s[len]) & 0xC0) == 0x80) {
            --len;
        }
    }
    memcpy(dst, s, len);
    dst[len] = '\0';
    return ParseResult::kOk;
}

// python-oa3 and clj-oa3 read a blank datetime or duration as absent.
bool is_blank(JsonVariantConst v) {
    if (!v.is<const char*>()) {
        return false;
    }
    for (const char* s = v.as<const char*>(); *s != '\0'; ++s) {
        if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') {
            return false;
        }
    }
    return true;
}

ParseResult copy_datetime(JsonVariantConst v, bool* has, DateTime* out) {
    *has = false;
    if (v.isNull() || is_blank(v)) {
        return ParseResult::kOk;
    }
    if (!v.is<const char*>() || !parse_datetime(v.as<const char*>(), out)) {
        return ParseResult::kInvalidField;
    }
    *has = true;
    return ParseResult::kOk;
}

ParseResult copy_duration(JsonVariantConst v, bool* has, Duration* out) {
    *has = false;
    if (v.isNull() || is_blank(v)) {
        return ParseResult::kOk;
    }
    if (!v.is<const char*>() || !parse_duration(v.as<const char*>(), out)) {
        return ParseResult::kInvalidField;
    }
    *has = true;
    return ParseResult::kOk;
}

#define OA3_TRY(expr)                        \
    do {                                     \
        ParseResult _r = (expr);             \
        if (_r != ParseResult::kOk) {        \
            return _r;                       \
        }                                    \
    } while (0)

ParseResult copy_period(JsonVariantConst v, bool* has, IntervalPeriod* out) {
    *has = false;
    if (v.isNull()) {
        return ParseResult::kOk;
    }
    if (!v.is<JsonObjectConst>()) {
        return ParseResult::kInvalidField;
    }
    JsonObjectConst o = v.as<JsonObjectConst>();
    OA3_TRY(copy_datetime(o["start"], &out->has_start, &out->start));
    OA3_TRY(copy_duration(o["duration"], &out->has_duration, &out->duration));
    OA3_TRY(copy_duration(get(o, "randomizeStart", "randomize_start"), &out->has_randomize_start,
                          &out->randomize_start));
    *has = true;
    return ParseResult::kOk;
}

ParseResult copy_targets(JsonVariantConst v, TargetList* out) {
    out->count = 0;
    out->non_string_count = 0;
    if (v.isNull()) {
        return ParseResult::kOk;
    }
    if (!v.is<JsonArrayConst>()) {
        return ParseResult::kInvalidField;
    }
    for (JsonVariantConst t : v.as<JsonArrayConst>()) {
        if (!t.is<const char*>()) {
            if (out->non_string_count < UINT8_MAX) {
                ++out->non_string_count;
            }
            continue;
        }
        if (out->count >= OA3_MAX_TARGETS) {
            return ParseResult::kCapacityExceeded;
        }
        OA3_TRY(copy_string(t, out->items[out->count], sizeof(out->items[0]), true));
        ++out->count;
    }
    return ParseResult::kOk;
}

ParseResult copy_descriptors(JsonVariantConst v, PayloadDescriptor* out, uint8_t* count,
                             const ParseOptions* opt) {
    *count = 0;
    if (v.isNull()) {
        return ParseResult::kOk;
    }
    if (!v.is<JsonArrayConst>()) {
        return ParseResult::kInvalidField;
    }
    for (JsonVariantConst d : v.as<JsonArrayConst>()) {
        if (!d.is<JsonObjectConst>()) {
            return ParseResult::kInvalidField;
        }
        JsonObjectConst o = d.as<JsonObjectConst>();
        JsonVariantConst object_type = get(o, "objectType", "object_type");
        if (object_type.is<const char*>() &&
            strcmp(object_type.as<const char*>(), "REPORT_PAYLOAD_DESCRIPTOR") == 0) {
            continue;
        }
        JsonVariantConst payload_type = get(o, "payloadType", "payload_type");
        if (!payload_type.is<const char*>()) {
            return ParseResult::kInvalidField;
        }
        if (!type_kept(opt, payload_type.as<const char*>())) {
            continue;
        }
        if (*count >= OA3_MAX_DESCRIPTORS) {
            return ParseResult::kCapacityExceeded;
        }
        PayloadDescriptor* pd = &out[*count];
        OA3_TRY(copy_string(payload_type, pd->payload_type, sizeof(pd->payload_type), true));
        OA3_TRY(copy_string(o["units"], pd->units, sizeof(pd->units), false));
        OA3_TRY(copy_string(o["currency"], pd->currency, sizeof(pd->currency), false));
        ++*count;
    }
    return ParseResult::kOk;
}

// A JSON number literal to a double. Correctly rounded when the significand has at most 15
// digits and the decimal exponent is within 22 (both factors are exact doubles).
bool number_from_literal(const char* s, double* out) {
    static const double kPow10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                    1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                    1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    bool negative = *s == '-';
    if (negative) {
        ++s;
    }
    if (*s < '0' || *s > '9' || (s[0] == '0' && s[1] >= '0' && s[1] <= '9')) {
        return false;
    }
    uint64_t mantissa = 0;
    int digits = 0;
    int32_t exp10 = 0;
    for (; *s >= '0' && *s <= '9'; ++s) {
        if (digits < 19) {
            mantissa = mantissa * 10 + static_cast<uint64_t>(*s - '0');
            if (mantissa != 0) {
                ++digits;
            }
        } else {
            ++exp10;
        }
    }
    if (*s == '.') {
        ++s;
        if (*s < '0' || *s > '9') {
            return false;
        }
        for (; *s >= '0' && *s <= '9'; ++s) {
            if (digits < 19) {
                mantissa = mantissa * 10 + static_cast<uint64_t>(*s - '0');
                if (mantissa != 0) {
                    ++digits;
                }
                --exp10;
            }
        }
    }
    if (*s == 'e' || *s == 'E') {
        ++s;
        bool exp_negative = *s == '-';
        if (*s == '-' || *s == '+') {
            ++s;
        }
        if (*s < '0' || *s > '9') {
            return false;
        }
        int32_t e = 0;
        for (; *s >= '0' && *s <= '9'; ++s) {
            if (e < 100000) {
                e = e * 10 + (*s - '0');
            }
        }
        exp10 += exp_negative ? -e : e;
    }
    if (*s != '\0') {
        return false;
    }
    double value = static_cast<double>(mantissa);
    if (mantissa != 0) {
        while (exp10 > 22 && value < 1e300) {
            value *= 1e22;
            exp10 -= 22;
        }
        while (exp10 < -22 && value > 1e-300) {
            value /= 1e22;
            exp10 += 22;
        }
        if (exp10 > 22) {
            value = HUGE_VAL;
        } else if (exp10 < -22) {
            value = 0;
        } else if (exp10 >= 0) {
            value *= kPow10[exp10];
        } else {
            value /= kPow10[-exp10];
        }
    }
    *out = negative ? -value : value;
    return true;
}

// kOk with *is_number set when v is a rewritten number literal.
ParseResult marked_number(JsonVariantConst v, bool* is_number, double* number, const char** text) {
    *is_number = false;
    if (!v.is<const char*>()) {
        return ParseResult::kOk;
    }
    const char* s = v.as<const char*>();
    if (s[0] == kTooLongMark) {
        return ParseResult::kCapacityExceeded;
    }
    if (s[0] != kLiteralMark) {
        return ParseResult::kOk;
    }
    if (strlen(s + 1) > OA3_VALUE_TEXT_MAX) {
        return ParseResult::kCapacityExceeded;
    }
    if (!number_from_literal(s + 1, number)) {
        return ParseResult::kJsonError;
    }
    *is_number = true;
    *text = s + 1;
    return ParseResult::kOk;
}

ParseResult copy_values(JsonVariantConst v, Event* ev, Payload* p) {
    p->first_value = ev->value_count;
    p->value_count = 0;
    if (v.isNull()) {
        return ParseResult::kOk;
    }
    if (!v.is<JsonArrayConst>()) {
        return ParseResult::kInvalidField;
    }
    for (JsonVariantConst e : v.as<JsonArrayConst>()) {
        if (e.isNull()) {
            continue;
        }
        if (ev->value_count >= OA3_MAX_VALUES) {
            return ParseResult::kCapacityExceeded;
        }
        Value* out = &ev->values[ev->value_count];
        memset(out, 0, sizeof(*out));
        bool is_number = false;
        const char* text = nullptr;
        OA3_TRY(marked_number(e, &is_number, &out->number, &text));
        if (is_number) {
            out->kind = ValueKind::kNumber;
            memcpy(out->text, text, strlen(text) + 1);
        } else if (e.is<const char*>()) {
            out->kind = ValueKind::kString;
            const char* s = e.as<const char*>();
            if (s[0] == kStringMark) {
                ++s;
            }
            const size_t len = strlen(s);
            if (len >= sizeof(out->text)) {
                return ParseResult::kCapacityExceeded;
            }
            memcpy(out->text, s, len + 1);
        } else if (e.is<bool>()) {
            out->kind = ValueKind::kBool;
            out->boolean = e.as<bool>();
        } else if (e.is<JsonObjectConst>()) {
            bool x_ok = false;
            bool y_ok = false;
            OA3_TRY(marked_number(e["x"], &x_ok, &out->number, &text));
            OA3_TRY(marked_number(e["y"], &y_ok, &out->y, &text));
            if (!x_ok || !y_ok) {
                return ParseResult::kInvalidField;
            }
            out->kind = ValueKind::kPoint;
        } else {
            return ParseResult::kInvalidField;
        }
        ++ev->value_count;
        ++p->value_count;
    }
    return ParseResult::kOk;
}

ParseResult copy_payloads(JsonVariantConst payloads, Event* ev, Interval* out, const ParseOptions* opt) {
    out->payload_count = 0;
    if (payloads.isNull()) {
        return ParseResult::kOk;
    }
    if (!payloads.is<JsonArrayConst>()) {
        return ParseResult::kInvalidField;
    }
    for (JsonVariantConst pv : payloads.as<JsonArrayConst>()) {
        if (!pv.is<JsonObjectConst>()) {
            return ParseResult::kInvalidField;
        }
        JsonVariantConst type = pv["type"];
        if (!type.is<const char*>()) {
            return ParseResult::kInvalidField;
        }
        if (!type_kept(opt, type.as<const char*>())) {
            continue;
        }
        if (out->payload_count >= OA3_MAX_PAYLOADS) {
            return ParseResult::kCapacityExceeded;
        }
        Payload* p = &out->payloads[out->payload_count];
        OA3_TRY(copy_string(type, p->type, sizeof(p->type), true));
        OA3_TRY(copy_values(pv["values"], ev, p));
        ++out->payload_count;
    }
    return ParseResult::kOk;
}

// The resolved period becomes the interval's own, so it no longer depends on dropped ones.
void make_explicit(const ResolvedInterval& r, Interval* out) {
    IntervalPeriod& ip = out->interval_period;
    out->has_interval_period = true;
    ip.has_start = r.has_start;
    ip.start = r.start;
    ip.has_duration = r.has_duration;
    ip.duration = r.duration;
    ip.has_randomize_start = r.has_randomize_start;
    ip.randomize_start = r.randomize_start;
}

const TimeInterval* window_of(const ParseOptions* opt) {
    return opt != nullptr ? opt->window : nullptr;
}

bool window_inverted(const ParseOptions* opt) {
    const TimeInterval* w = window_of(opt);
    return w != nullptr && !w->end_indefinite && compare(w->end, w->start) < 0;
}

// False when the interval lies outside the window. Out of line so that the ResolvedInterval
// is off the stack while payloads are copied.
OA3_NOINLINE bool keep_in_window(const TimeInterval& window, bool dropped,
                                 IntervalResolver* resolver, Interval* iv) {
    ResolvedInterval r;
    resolver->resolve(iv->has_interval_period ? &iv->interval_period : nullptr, iv->id, &r);
    if (r.has_period && !intersects(r.period, window)) {
        return false;
    }
    if (dropped) {
        make_explicit(r, iv);
    }
    return true;
}

ParseResult copy_intervals(JsonVariantConst v, Event* ev, const ParseOptions* opt) {
    ev->interval_count = 0;
    ev->value_count = 0;
    if (v.isNull()) {
        return ParseResult::kOk;
    }
    if (!v.is<JsonArrayConst>()) {
        return ParseResult::kInvalidField;
    }
    const TimeInterval* window = window_of(opt);
    // An event duration may repeat the intervals, so none is outside the window on its own.
    if (ev->has_duration) {
        window = nullptr;
    }
    IntervalResolver resolver(ev->has_interval_period ? &ev->interval_period : nullptr);
    bool dropped = false;
    Interval spare;
    for (JsonVariantConst iv : v.as<JsonArrayConst>()) {
        if (!iv.is<JsonObjectConst>()) {
            return ParseResult::kInvalidField;
        }
        const bool full = ev->interval_count >= OA3_MAX_INTERVALS;
        if (full && window == nullptr) {
            return ParseResult::kCapacityExceeded;
        }
        JsonObjectConst o = iv.as<JsonObjectConst>();
        Interval* out = full ? &spare : &ev->intervals[ev->interval_count];
        *out = Interval{};
        JsonVariantConst id = o["id"];
        if (!id.is<int32_t>()) {
            return ParseResult::kInvalidField;
        }
        out->id = id.as<int32_t>();
        OA3_TRY(copy_period(get(o, "intervalPeriod", "interval_period"), &out->has_interval_period,
                            &out->interval_period));
        if (window != nullptr) {
            if (!keep_in_window(*window, dropped, &resolver, out)) {
                dropped = true;
                continue;
            }
            if (full) {
                return ParseResult::kCapacityExceeded;
            }
        }
        OA3_TRY(copy_payloads(o["payloads"], ev, out, opt));
        ++ev->interval_count;
    }
    return ParseResult::kOk;
}

ParseResult check_object_type(JsonObjectConst o, const char* inherited, const char* expected) {
    JsonVariantConst t = get(o, "objectType", "object_type");
    const char* s = inherited;
    if (!t.isNull()) {
        if (!t.is<const char*>()) {
            return ParseResult::kInvalidField;
        }
        s = t.as<const char*>();
    }
    if (s != nullptr && strcmp(s, expected) != 0) {
        return ParseResult::kInvalidField;
    }
    return ParseResult::kOk;
}

ParseResult copy_event(JsonVariantConst v, Event* out, const ParseOptions* opt,
                       const char* inherited_type) {
    if (!v.is<JsonObjectConst>()) {
        return ParseResult::kInvalidField;
    }
    JsonObjectConst o = v.as<JsonObjectConst>();
    OA3_TRY(check_object_type(o, inherited_type, "EVENT"));
    OA3_TRY(copy_string(o["id"], out->id, sizeof(out->id), false));
    OA3_TRY(copy_string(get(o, "programID", "program_id"), out->program_id,
                        sizeof(out->program_id), true));
    OA3_TRY(copy_truncated(get(o, "eventName", "event_name"), out->event_name,
                           sizeof(out->event_name)));
    OA3_TRY(copy_datetime(get(o, "modificationDateTime", "modification_date_time"),
                          &out->has_modification_date_time, &out->modification_date_time));
    JsonVariantConst priority = o["priority"];
    if (!priority.isNull()) {
        if (!priority.is<uint32_t>()) {
            return ParseResult::kInvalidField;
        }
        out->has_priority = true;
        out->priority = priority.as<uint32_t>();
    }
    OA3_TRY(copy_duration(o["duration"], &out->has_duration, &out->duration));
    OA3_TRY(copy_period(get(o, "intervalPeriod", "interval_period"), &out->has_interval_period,
                        &out->interval_period));
    OA3_TRY(copy_targets(o["targets"], &out->targets));
    OA3_TRY(copy_descriptors(get(o, "payloadDescriptors", "payload_descriptors"), out->descriptors,
                             &out->descriptor_count, opt));
    return copy_intervals(o["intervals"], out, opt);
}

bool attributes_say_local(JsonVariantConst v) {
    if (!v.is<JsonArrayConst>()) {
        return false;
    }
    for (JsonVariantConst a : v.as<JsonArrayConst>()) {
        JsonVariantConst type = a["type"];
        if (!type.is<const char*>() || strcmp(type.as<const char*>(), "LOCAL") != 0) {
            continue;
        }
        bool local = true;
        JsonVariantConst values = a["values"];
        if (values.is<JsonArrayConst>()) {
            for (JsonVariantConst x : values.as<JsonArrayConst>()) {
                if (x.is<bool>() && !x.as<bool>()) {
                    local = false;
                }
            }
        }
        if (local) {
            return true;
        }
    }
    return false;
}

ParseResult copy_program(JsonVariantConst v, Program* out, const ParseOptions* opt,
                         const char* inherited_type) {
    if (!v.is<JsonObjectConst>()) {
        return ParseResult::kInvalidField;
    }
    JsonObjectConst o = v.as<JsonObjectConst>();
    OA3_TRY(check_object_type(o, inherited_type, "PROGRAM"));
    OA3_TRY(copy_string(o["id"], out->id, sizeof(out->id), true));
    OA3_TRY(copy_string(get(o, "programName", "program_name"), out->program_name,
                        sizeof(out->program_name), true));
    OA3_TRY(copy_datetime(get(o, "modificationDateTime", "modification_date_time"),
                          &out->has_modification_date_time, &out->modification_date_time));
    JsonVariantConst local_price = get(o, "localPrice", "local_price");
    out->local = (local_price.is<bool>() && local_price.as<bool>()) ||
                 attributes_say_local(o["attributes"]);
    OA3_TRY(copy_targets(o["targets"], &out->targets));
    return copy_descriptors(get(o, "payloadDescriptors", "payload_descriptors"), out->descriptors,
                            &out->descriptor_count, opt);
}

ParseResult copy_notification(JsonVariantConst v, Notification* out, const ParseOptions* opt) {
    if (!v.is<JsonObjectConst>()) {
        return ParseResult::kInvalidField;
    }
    JsonObjectConst o = v.as<JsonObjectConst>();
    JsonVariantConst type = get(o, "objectType", "object_type");
    JsonVariantConst op = o["operation"];
    JsonVariantConst object = o["object"];
    if (!type.is<const char*>() || !op.is<const char*>() || object.isNull()) {
        return ParseResult::kInvalidField;
    }
    out->object_type = parse_object_type(type.as<const char*>());
    out->operation = parse_operation(op.as<const char*>());
    OA3_TRY(copy_targets(o["targets"], &out->targets));
    if (out->object_type == ObjectType::kProgram) {
        OA3_TRY(copy_program(object, &out->program, opt, type.as<const char*>()));
        out->has_object = true;
    } else if (out->object_type == ObjectType::kEvent) {
        OA3_TRY(copy_event(object, &out->event, opt, type.as<const char*>()));
        out->has_object = true;
    }
    return ParseResult::kOk;
}

enum class Kind : uint8_t { kProgram, kEvent, kNotification };

// Where an event meets the window: first is its earliest instant in the window.
struct WindowFit {
    bool in_window;
    DateTime first;
};

class WindowFitBuilder {
public:
    explicit WindowFitBuilder(const TimeInterval& window) : _window(window), _hit(false), _first{} {}

    void add(const ResolvedInterval& r) {
        _extent.add(r);
        if (r.has_period && intersects(r.period, _window) && (!_hit || compare(r.period.start, _first) < 0)) {
            _hit = true;
            _first = r.period.start;
        }
    }

    // An event without a resolvable period is kept, and is the first to give way.
    WindowFit finish(bool has_duration, const Duration& duration) const {
        const EventExtent x = _extent.finish(has_duration, duration);
        WindowFit f{true, DateTime{INT64_MAX, 0, 0}};
        if (!x.has_period) {
            return f;
        }
        if (x.looping) {
            f.in_window = intersects(x.span, _window);
            f.first = x.span.start;
        } else {
            f.in_window = _hit;
            f.first = _first;
        }
        if (compare(f.first, _window.start) < 0) {
            f.first = _window.start;
        }
        return f;
    }

private:
    TimeInterval _window;
    ExtentBuilder _extent;
    bool _hit;
    DateTime _first;
};

OA3_NOINLINE ParseResult fit_json(JsonObjectConst o, const TimeInterval& window, WindowFit* out) {
    bool has_duration = false;
    Duration duration{};
    OA3_TRY(copy_duration(o["duration"], &has_duration, &duration));
    bool has_period = false;
    IntervalPeriod period{};
    OA3_TRY(copy_period(get(o, "intervalPeriod", "interval_period"), &has_period, &period));
    IntervalResolver resolver(has_period ? &period : nullptr);
    WindowFitBuilder fit(window);
    JsonVariantConst intervals = o["intervals"];
    if (intervals.is<JsonArrayConst>()) {
        for (JsonVariantConst iv : intervals.as<JsonArrayConst>()) {
            bool has = false;
            IntervalPeriod ip{};
            OA3_TRY(copy_period(get(iv.as<JsonObjectConst>(), "intervalPeriod", "interval_period"), &has, &ip));
            ResolvedInterval r;
            resolver.resolve(has ? &ip : nullptr, 0, &r);
            fit.add(r);
        }
    }
    *out = fit.finish(has_duration, duration);
    return ParseResult::kOk;
}

OA3_NOINLINE WindowFit fit_event(const Event& ev, const TimeInterval& window) {
    WindowFitBuilder fit(window);
    IntervalResolver resolver(ev);
    ResolvedInterval r;
    while (resolver.next(&r)) {
        fit.add(r);
    }
    return fit.finish(ev.has_duration, ev.duration);
}

// parse_events with a window: the caller's array and whether an event was left out for room.
struct EventSlots {
    Event* events;
    size_t capacity;
    size_t* count;
    bool truncated;
};

ParseResult place_event(JsonVariantConst v, EventSlots* slots, const ParseOptions* opt) {
    if (!v.is<JsonObjectConst>()) {
        return ParseResult::kInvalidField;
    }
    const TimeInterval& window = *opt->window;
    WindowFit fit;
    OA3_TRY(fit_json(v.as<JsonObjectConst>(), window, &fit));
    if (!fit.in_window) {
        return ParseResult::kOk;
    }
    size_t index = *slots->count;
    if (index >= slots->capacity) {
        slots->truncated = true;
        size_t latest = 0;
        DateTime latest_first{};
        for (size_t i = 0; i < *slots->count; ++i) {
            const WindowFit f = fit_event(slots->events[i], window);
            if (i == 0 || compare(f.first, latest_first) > 0) {
                latest = i;
                latest_first = f.first;
            }
        }
        if (*slots->count == 0 || compare(fit.first, latest_first) >= 0) {
            return ParseResult::kOk;
        }
        // Closing the gap keeps the kept events in arrival order, which price_at's
        // equal-priority tie-break depends on.
        memmove(&slots->events[latest], &slots->events[latest + 1],
                (*slots->count - 1 - latest) * sizeof(Event));
        index = --*slots->count;
    }
    Event* slot = &slots->events[index];
    memset(slot, 0, sizeof(*slot));
    OA3_TRY(copy_event(v, slot, opt, nullptr));
    ++*slots->count;
    return ParseResult::kOk;
}

ParseResult parse_one(LiteralReader& reader, JsonArena& arena, Kind kind, void* out,
                      const ParseOptions* opt, EventSlots* slots = nullptr) {
    if (window_inverted(opt)) {
        return ParseResult::kInvalidField;
    }
    const Filters& f = filters();
    if (!f.ok) {
        return ParseResult::kTooLarge;
    }
    arena.reset();
    reader.reset_lexer();
    JsonDocument doc(&arena);
    JsonVariantConst filter = kind == Kind::kNotification ? f.doc["n"] : f.doc["e"];
    DeserializationError e =
        deserializeJson(doc, reader, ArduinoJson::DeserializationOption::Filter(filter));
    ParseResult r = map_error(e, arena);
    if (r != ParseResult::kOk) {
        return r;
    }
    if (slots != nullptr) {
        return place_event(doc.as<JsonVariantConst>(), slots, opt);
    }
    switch (kind) {
        case Kind::kProgram:
            return copy_program(doc.as<JsonVariantConst>(), static_cast<Program*>(out), opt,
                                nullptr);
        case Kind::kEvent:
            return copy_event(doc.as<JsonVariantConst>(), static_cast<Event*>(out), opt, nullptr);
        case Kind::kNotification:
            return copy_notification(doc.as<JsonVariantConst>(), static_cast<Notification*>(out),
                                     opt);
    }
    return ParseResult::kJsonError;
}

bool is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

int next_non_space(LiteralReader& reader) {
    int c;
    do {
        c = reader.read_raw();
    } while (is_space(c));
    return c;
}

ParseResult parse_array(ByteReader& in, JsonArena& arena, Kind kind, void* out, size_t stride,
                        size_t capacity, size_t* count, const ParseOptions* opt) {
    *count = 0;
    if (window_inverted(opt)) {
        return ParseResult::kInvalidField;
    }
    const bool windowed = kind == Kind::kEvent && window_of(opt) != nullptr;
    EventSlots slots{static_cast<Event*>(out), capacity, count, false};
    LiteralReader reader(in);
    int c = next_non_space(reader);
    if (c != '[') {
        return ParseResult::kJsonError;
    }
    c = next_non_space(reader);
    if (c == ']') {
        return ParseResult::kOk;
    }
    for (;;) {
        if (c < 0) {
            return ParseResult::kJsonError;
        }
        if (c != '{') {
            return ParseResult::kInvalidField;
        }
        reader.unread(c);
        ParseResult r;
        if (windowed) {
            r = parse_one(reader, arena, kind, nullptr, opt, &slots);
        } else {
            if (*count >= capacity) {
                return ParseResult::kCapacityExceeded;
            }
            void* item = static_cast<unsigned char*>(out) + *count * stride;
            memset(item, 0, stride);
            r = parse_one(reader, arena, kind, item, opt);
            if (r == ParseResult::kOk) {
                ++*count;
            }
        }
        if (r != ParseResult::kOk) {
            return r;
        }
        c = next_non_space(reader);
        if (c == ']') {
            return slots.truncated ? ParseResult::kTruncated : ParseResult::kOk;
        }
        if (c != ',') {
            return ParseResult::kJsonError;
        }
        c = next_non_space(reader);
    }
}

}  // namespace

ParseResult parse_program(ByteReader& in, JsonArena& arena, Program* out,
                          const ParseOptions* options) {
    memset(out, 0, sizeof(*out));
    LiteralReader reader(in);
    return parse_one(reader, arena, Kind::kProgram, out, options);
}

ParseResult parse_program(const char* json, size_t len, JsonArena& arena, Program* out,
                          const ParseOptions* options) {
    BufferReader in(json, len);
    return parse_program(in, arena, out, options);
}

ParseResult parse_programs(ByteReader& in, JsonArena& arena, Program* out, size_t capacity,
                           size_t* count, const ParseOptions* options) {
    return parse_array(in, arena, Kind::kProgram, out, sizeof(Program), capacity, count, options);
}

ParseResult parse_programs(const char* json, size_t len, JsonArena& arena, Program* out,
                           size_t capacity, size_t* count, const ParseOptions* options) {
    BufferReader in(json, len);
    return parse_programs(in, arena, out, capacity, count, options);
}

ParseResult parse_event(ByteReader& in, JsonArena& arena, Event* out,
                        const ParseOptions* options) {
    memset(out, 0, sizeof(*out));
    LiteralReader reader(in);
    return parse_one(reader, arena, Kind::kEvent, out, options);
}

ParseResult parse_event(const char* json, size_t len, JsonArena& arena, Event* out,
                        const ParseOptions* options) {
    BufferReader in(json, len);
    return parse_event(in, arena, out, options);
}

ParseResult parse_events(ByteReader& in, JsonArena& arena, Event* out, size_t capacity,
                         size_t* count, const ParseOptions* options) {
    return parse_array(in, arena, Kind::kEvent, out, sizeof(Event), capacity, count, options);
}

ParseResult parse_events(const char* json, size_t len, JsonArena& arena, Event* out,
                         size_t capacity, size_t* count, const ParseOptions* options) {
    BufferReader in(json, len);
    return parse_events(in, arena, out, capacity, count, options);
}

ParseResult parse_notification(ByteReader& in, JsonArena& arena, Notification* out,
                               const ParseOptions* options) {
    memset(out, 0, sizeof(*out));
    LiteralReader reader(in);
    return parse_one(reader, arena, Kind::kNotification, out, options);
}

ParseResult parse_notification(const char* json, size_t len, JsonArena& arena,
                               Notification* out, const ParseOptions* options) {
    BufferReader in(json, len);
    return parse_notification(in, arena, out, options);
}

}  // namespace oa3
