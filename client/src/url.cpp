#include <oa3/client/url.h>

#include <stdio.h>
#include <string.h>

namespace oa3 {

namespace {

const char kHex[] = "0123456789ABCDEF";

bool unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

}  // namespace

bool percent_encode(const char* in, char* out, size_t size, size_t* len) {
    if (len != nullptr) {
        *len = 0;
    }
    if (size == 0) {
        return false;
    }
    size_t n = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(in ? in : ""); *p; ++p) {
        const size_t need = unreserved(*p) ? 1 : 3;
        if (n + need >= size) {
            out[0] = '\0';
            return false;
        }
        if (need == 1) {
            out[n++] = static_cast<char>(*p);
        } else {
            out[n++] = '%';
            out[n++] = kHex[*p >> 4];
            out[n++] = kHex[*p & 0x0F];
        }
    }
    out[n] = '\0';
    if (len != nullptr) {
        *len = n;
    }
    return true;
}

UrlBuilder::UrlBuilder(char* buffer, size_t size)
    : _buffer(buffer), _size(size), _len(0), _has_query(false), _ok(buffer != nullptr && size > 0) {
    if (_ok) {
        _buffer[0] = '\0';
    }
}

UrlBuilder& UrlBuilder::append(const char* raw) {
    if (!_ok) {
        return *this;
    }
    const char* s = raw ? raw : "";
    const size_t n = strlen(s);
    if (_len + n >= _size) {
        _ok = false;
        return *this;
    }
    memcpy(_buffer + _len, s, n + 1);
    _len += n;
    if (strchr(s, '?') != nullptr) {
        _has_query = true;
    }
    return *this;
}

UrlBuilder& UrlBuilder::append_path(const char* segment) {
    if (!_ok) {
        return *this;
    }
    const size_t start = _len;
    if (_len == 0 || _buffer[_len - 1] != '/') {
        append("/");
    }
    size_t n = 0;
    if (!_ok || !percent_encode(segment, _buffer + _len, _size - _len, &n)) {
        _len = start;
        _buffer[_len] = '\0';
        _ok = false;
        return *this;
    }
    _len += n;
    return *this;
}

UrlBuilder& UrlBuilder::append_query(const char* name, const char* value) {
    if (!_ok) {
        return *this;
    }
    const size_t start = _len;
    const bool had_query = _has_query;
    append(_has_query ? "&" : "?");
    append(name);
    append("=");
    size_t n = 0;
    if (!_ok || !percent_encode(value, _buffer + _len, _size - _len, &n)) {
        _len = start;
        _buffer[_len] = '\0';
        _has_query = had_query;
        _ok = false;
        return *this;
    }
    _len += n;
    _has_query = true;
    return *this;
}

UrlBuilder& UrlBuilder::append_query(const char* name, uint32_t value) {
    char digits[11];
    snprintf(digits, sizeof(digits), "%lu", static_cast<unsigned long>(value));
    return append_query(name, digits);
}

namespace {

void append_targets(UrlBuilder& b, const TargetList* targets) {
    if (targets == nullptr) {
        return;
    }
    for (uint8_t i = 0; i < targets->count && i < OA3_MAX_TARGETS; ++i) {
        b.append_query("targets", targets->items[i]);
    }
}

}  // namespace

bool build_programs_url(char* buffer, size_t size, const char* base_url,
                        const char* program_name, const TargetList* targets, uint32_t skip,
                        uint32_t limit) {
    if (base_url == nullptr) {
        return false;
    }
    UrlBuilder b(buffer, size);
    b.append(base_url).append_path("programs");
    if (program_name != nullptr) {
        b.append_query("programName", program_name);
    }
    append_targets(b, targets);
    b.append_query("skip", skip).append_query("limit", limit);
    return b.ok();
}

bool build_program_url(char* buffer, size_t size, const char* base_url, const char* program_id) {
    if (base_url == nullptr || program_id == nullptr) {
        return false;
    }
    UrlBuilder b(buffer, size);
    b.append(base_url).append_path("programs").append_path(program_id);
    return b.ok();
}

bool build_events_url(char* buffer, size_t size, const char* base_url, const char* program_id,
                      const TargetList* targets, uint32_t skip, uint32_t limit) {
    if (base_url == nullptr || program_id == nullptr) {
        return false;
    }
    UrlBuilder b(buffer, size);
    b.append(base_url).append_path("events").append_query("programID", program_id);
    append_targets(b, targets);
    b.append_query("skip", skip).append_query("limit", limit);
    return b.ok();
}

bool build_notifiers_url(char* buffer, size_t size, const char* base_url) {
    if (base_url == nullptr) {
        return false;
    }
    UrlBuilder b(buffer, size);
    b.append(base_url).append_path("notifiers");
    return b.ok();
}

bool build_program_topics_url(char* buffer, size_t size, const char* base_url,
                              const char* program_id) {
    if (base_url == nullptr || program_id == nullptr) {
        return false;
    }
    UrlBuilder b(buffer, size);
    b.append(base_url)
        .append_path("notifiers")
        .append_path("mqtt")
        .append_path("topics")
        .append_path("programs")
        .append_path(program_id);
    return b.ok();
}

bool build_program_events_topics_url(char* buffer, size_t size, const char* base_url,
                                     const char* program_id) {
    if (base_url == nullptr || program_id == nullptr) {
        return false;
    }
    UrlBuilder b(buffer, size);
    b.append(base_url)
        .append_path("notifiers")
        .append_path("mqtt")
        .append_path("topics")
        .append_path("programs")
        .append_path(program_id)
        .append_path("events");
    return b.ok();
}

bool is_object_id(const char* s) {
    if (s == nullptr || s[0] == '\0') {
        return false;
    }
    size_t n = 0;
    for (; s[n] != '\0'; ++n) {
        const char c = s[n];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '-';
        if (!ok || n >= 128) {
            return false;
        }
    }
    return true;
}

}  // namespace oa3
