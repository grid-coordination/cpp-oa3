#pragma once

#include <stddef.h>
#include <stdint.h>

#include <oa3/model/byte_reader.h>

namespace oa3 {

enum class HttpResult : uint8_t {
    kOk,             // a status line arrived and the handler ran
    kInvalidUrl,
    kDnsFailed,
    kConnectFailed,
    kTlsFailed,      // handshake or certificate verification
    kTimeout,
    kTooLarge,       // the body exceeds what the transport can deliver
    kIoError,        // any other failure, including a body cut short
    kUnsupported,    // the transport does not implement the method
};

const char* to_string(HttpResult r);

struct HttpHeader {
    const char* name;
    const char* value;
};

struct HttpRequest {
    const char* url;  // absolute, already percent-encoded
    const HttpHeader* headers;
    size_t header_count;
    uint32_t timeout_ms;  // the whole request; 0 leaves it to the transport
};

// Called at most once per get(), with any status, before get() returns. body yields the
// response body (chunked transfer decoded) and -1 at its end. The handler may stop reading
// early; the transport discards the rest. body is valid only during the call.
typedef void (*HttpResponseFn)(void* ctx, int status, ByteReader& body);

// A port's HTTP(S) client. One request at a time, and no connection outlives get(): on the
// classic ESP32 a second TLS session does not fit.
class HttpTransport {
public:
    // Performs a GET and returns when the connection is closed. kOk means on_response ran and
    // the body was delivered in full or abandoned by the handler. On any other result the
    // handler may or may not have run; the caller discards what it parsed.
    virtual HttpResult get(const HttpRequest& request, HttpResponseFn on_response, void* ctx) = 0;
    // As get(), with a request body, for the VTN token and /vens requests of a later release;
    // the client sends none yet. The default returns kUnsupported, so a port that overrides
    // only get() keeps compiling when the client starts to use it.
    virtual HttpResult post(const HttpRequest& request, const char* content_type, const char* body,
                            size_t body_len, HttpResponseFn on_response, void* ctx);

protected:
    ~HttpTransport() = default;
};

}  // namespace oa3
