#pragma once

#include <stddef.h>
#include <stdint.h>

#include <oa3/model/entities.h>

namespace oa3 {

// Copies in to out, leaving RFC 3986 unreserved characters (ALPHA DIGIT - . _ ~) and
// writing every other byte as %XX, uppercase. False when the result and its NUL do not fit;
// out is then "" (when size > 0). *len, when given, receives the length written.
bool percent_encode(const char* in, char* out, size_t size, size_t* len = nullptr);

// Appends to a caller-owned buffer, always NUL-terminated. The first append that does not
// fit makes the builder fail: the buffer keeps what was written before it and every later
// append is ignored.
class UrlBuilder {
public:
    UrlBuilder(char* buffer, size_t size);

    // Verbatim. A '?' in it makes the next query parameter start with '&'.
    UrlBuilder& append(const char* raw);
    // '/' (unless the buffer already ends in one) and the segment, percent-encoded.
    UrlBuilder& append_path(const char* segment);
    // '?' or '&', name verbatim, '=', value percent-encoded.
    UrlBuilder& append_query(const char* name, const char* value);
    UrlBuilder& append_query(const char* name, uint32_t value);

    bool ok() const { return _ok; }
    size_t length() const { return _len; }
    const char* c_str() const { return _buffer; }

private:
    char* _buffer;
    size_t _size;
    size_t _len;
    bool _has_query;
    bool _ok;
};

// Each writes the URL for a VTN whose base URL ends in the spec version segment (e.g.
// https://price.grid-coordination.energy/openadr3/3.1.0) and returns false when it does not
// fit. targets may be null; each target is a separate "targets" parameter
// (openadr3.yaml, form style, exploded).

// base/programs?programName=<name>&targets=..&skip=<skip>&limit=<limit>; programName is
// omitted when program_name is null. programName is the price server's extension
// (oadr3-org/specification#418); a VTN that ignores it returns full pages.
bool build_programs_url(char* buffer, size_t size, const char* base_url,
                        const char* program_name, const TargetList* targets, uint32_t skip,
                        uint32_t limit);

// base/programs/<id>
bool build_program_url(char* buffer, size_t size, const char* base_url, const char* program_id);

// base/events?programID=<id>&targets=..&skip=<skip>&limit=<limit>
bool build_events_url(char* buffer, size_t size, const char* base_url, const char* program_id,
                      const TargetList* targets, uint32_t skip, uint32_t limit);

// base/notifiers
bool build_notifiers_url(char* buffer, size_t size, const char* base_url);

// base/notifiers/mqtt/topics/programs/<id>/events
bool build_program_events_topics_url(char* buffer, size_t size, const char* base_url,
                                     const char* program_id);

// base/notifiers/mqtt/topics/programs/<id>
bool build_program_topics_url(char* buffer, size_t size, const char* base_url,
                              const char* program_id);

// The spec's objectID: 1 to 128 of ALPHA DIGIT _ - (openadr3.yaml), so it needs no encoding.
bool is_object_id(const char* s);

}  // namespace oa3
