#include <oa3/client/http_transport.h>

namespace oa3 {

const char* to_string(HttpResult r) {
    switch (r) {
        case HttpResult::kOk:
            return "OK";
        case HttpResult::kInvalidUrl:
            return "INVALID_URL";
        case HttpResult::kDnsFailed:
            return "DNS_FAILED";
        case HttpResult::kConnectFailed:
            return "CONNECT_FAILED";
        case HttpResult::kTlsFailed:
            return "TLS_FAILED";
        case HttpResult::kTimeout:
            return "TIMEOUT";
        case HttpResult::kTooLarge:
            return "TOO_LARGE";
        case HttpResult::kIoError:
            return "IO_ERROR";
        case HttpResult::kUnsupported:
            return "UNSUPPORTED";
    }
    return "UNKNOWN";
}

HttpResult HttpTransport::post(const HttpRequest&, const char*, const char*, size_t,
                               HttpResponseFn, void*) {
    return HttpResult::kUnsupported;
}

}  // namespace oa3
