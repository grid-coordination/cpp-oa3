#include <oa3/posix/curl_transport.h>

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

#include <oa3/model/byte_reader.h>

namespace oa3 {
namespace posix {

static HttpResult map_error(CURLcode code) {
    switch (code) {
        case CURLE_URL_MALFORMAT:
        case CURLE_UNSUPPORTED_PROTOCOL:
            return HttpResult::kInvalidUrl;
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
            return HttpResult::kDnsFailed;
        case CURLE_COULDNT_CONNECT:
            return HttpResult::kConnectFailed;
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_SSL_CERTPROBLEM:
            return HttpResult::kTlsFailed;
        case CURLE_OPERATION_TIMEDOUT:
            return HttpResult::kTimeout;
        default:
            return HttpResult::kIoError;
    }
}

CurlHttpTransport::CurlHttpTransport(char* buffer, size_t size)
    : _buffer(buffer), _size(size), _len(0), _overflow(false), _ca_file(nullptr),
      _insecure(false) {}

size_t CurlHttpTransport::on_write(char* data, size_t size, size_t count, void* self) {
    CurlHttpTransport* t = static_cast<CurlHttpTransport*>(self);
    size_t n = size * count;
    if (n > t->_size - t->_len) {
        t->_overflow = true;
        return 0;  // aborts the transfer with CURLE_WRITE_ERROR
    }
    memcpy(t->_buffer + t->_len, data, n);
    t->_len += n;
    return n;
}

HttpResult CurlHttpTransport::get(const HttpRequest& request, HttpResponseFn on_response,
                                  void* ctx) {
    _len = 0;
    _overflow = false;

    CURL* curl = curl_easy_init();
    if (!curl) return HttpResult::kIoError;

    struct curl_slist* headers = nullptr;
    char line[512];
    for (size_t i = 0; i < request.header_count; ++i) {
        snprintf(line, sizeof(line), "%s: %s", request.headers[i].name, request.headers[i].value);
        headers = curl_slist_append(headers, line);
    }
    headers = curl_slist_append(headers, "Connection: close");

    curl_easy_setopt(curl, CURLOPT_URL, request.url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
    curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &CurlHttpTransport::on_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, _insecure ? 0L : 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, _insecure ? 0L : 2L);
    if (_ca_file) curl_easy_setopt(curl, CURLOPT_CAINFO, _ca_file);
    if (request.timeout_ms) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout_ms));
    }

    CURLcode code = curl_easy_perform(curl);
    long status = 0;
    if (code == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (code != CURLE_OK) return _overflow ? HttpResult::kTooLarge : map_error(code);

    BufferReader body(_buffer, _len);
    on_response(ctx, static_cast<int>(status), body);
    return HttpResult::kOk;
}

}  // namespace posix
}  // namespace oa3
