#pragma once

#include <stddef.h>

#include <oa3/client/http_transport.h>

namespace oa3 {
namespace posix {

// HttpTransport over libcurl's easy API. Each get() opens and closes its own connection.
// The body is collected in a caller-owned buffer; a larger body gives kTooLarge. The program
// calls curl_global_init() before the first get() and curl_global_cleanup() at exit.
class CurlHttpTransport final : public HttpTransport {
public:
    CurlHttpTransport(char* buffer, size_t size);

    // A PEM bundle of trusted CAs; nullptr uses the system store.
    void set_ca_file(const char* path) { _ca_file = path; }
    // Turns off peer and host verification.
    void set_insecure(bool insecure) { _insecure = insecure; }

    HttpResult get(const HttpRequest& request, HttpResponseFn on_response, void* ctx) override;

private:
    static size_t on_write(char* data, size_t size, size_t count, void* self);

    char* _buffer;
    size_t _size;
    size_t _len;
    bool _overflow;
    const char* _ca_file;
    bool _insecure;
};

}  // namespace posix
}  // namespace oa3
