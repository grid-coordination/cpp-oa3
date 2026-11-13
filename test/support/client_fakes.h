#pragma once

// Fakes for the oa3_client suites: a scripted HttpTransport and a settable Clock.

#include <stdio.h>
#include <string.h>

#include <oa3/client/clock.h>
#include <oa3/client/http_transport.h>
#include <oa3/client/limits.h>
#include <oa3/client/ven_client.h>
#include <oa3/model/byte_reader.h>
#include <oa3/model/time.h>
#include <unity.h>

namespace fakes {

struct Response {
    const char* url_contains;  // nullptr: any URL
    oa3::HttpResult result;
    int status;
    const char* body;  // nullptr: ""
    bool call_handler;
    unsigned repeat;  // served this many times; 0 counts as 1
};

class FakeTransport final : public oa3::HttpTransport {
public:
    static constexpr size_t kMaxQueue = 32;
    static constexpr size_t kMaxUrls = 256;

    void reset() {
        _queued = 0;
        _next = 0;
        _served = 0;
        requests = 0;
        unexpected = 0;
        header_count = 0;
        timeout_ms = 0;
    }

    void push(const Response& r) {
        TEST_ASSERT_TRUE(_queued < kMaxQueue);
        _queue[_queued++] = r;
    }

    void push_ok(const char* body, const char* url_contains = nullptr, unsigned repeat = 1) {
        push(Response{url_contains, oa3::HttpResult::kOk, 200, body, true, repeat});
    }

    void push_status(int status, const char* body = "") {
        push(Response{nullptr, oa3::HttpResult::kOk, status, body, true, 1});
    }

    void push_result(oa3::HttpResult result) {
        push(Response{nullptr, result, 0, "", false, 1});
    }

    size_t pending() const { return _queued - _next; }

    const char* url(size_t i) const { return urls[i % kMaxUrls]; }
    const char* last_url() const { return requests > 0 ? url(requests - 1) : ""; }

    oa3::HttpResult get(const oa3::HttpRequest& request, oa3::HttpResponseFn on_response,
                        void* ctx) override {
        snprintf(urls[requests % kMaxUrls], sizeof(urls[0]), "%s", request.url);
        ++requests;
        timeout_ms = request.timeout_ms;
        header_count = request.header_count < 4 ? request.header_count : 4;
        for (size_t i = 0; i < header_count; ++i) {
            snprintf(header_names[i], sizeof(header_names[i]), "%s", request.headers[i].name);
            snprintf(header_values[i], sizeof(header_values[i]), "%s", request.headers[i].value);
        }
        if (_next >= _queued) {
            ++unexpected;
            return oa3::HttpResult::kIoError;
        }
        const Response r = _queue[_next];
        const unsigned times = r.repeat == 0 ? 1 : r.repeat;
        if (++_served >= times) {
            ++_next;
            _served = 0;
        }
        if (r.url_contains != nullptr && strstr(request.url, r.url_contains) == nullptr) {
            ++unexpected;
        }
        if (r.call_handler) {
            const char* body = r.body ? r.body : "";
            oa3::BufferReader reader(body, strlen(body));
            on_response(ctx, r.status, reader);
        }
        return r.result;
    }

    size_t requests = 0;
    size_t unexpected = 0;
    char urls[kMaxUrls][OA3_URL_MAX + 1] = {};
    size_t header_count = 0;
    char header_names[4][32] = {};
    char header_values[4][64] = {};
    uint32_t timeout_ms = 0;

private:
    Response _queue[kMaxQueue] = {};
    size_t _queued = 0;
    size_t _next = 0;
    unsigned _served = 0;
};

class FakeClock final : public oa3::Clock {
public:
    bool now(oa3::DateTime* out) override {
        if (!valid) {
            return false;
        }
        *out = wall;
        return true;
    }
    uint32_t monotonic_ms() override { return ms; }

    void set(const char* iso) {
        TEST_ASSERT_TRUE(oa3::parse_datetime(iso, &wall));
        valid = true;
    }
    // Advances both clocks.
    void advance_ms(uint32_t d) {
        ms += d;
        wall.seconds += d / 1000;
        uint32_t nanos = wall.nanos + (d % 1000) * 1000000u;
        if (nanos >= 1000000000u) {
            nanos -= 1000000000u;
            ++wall.seconds;
        }
        wall.nanos = nanos;
    }
    void advance_s(uint32_t s) { advance_ms(s * 1000u); }

    bool valid = false;
    oa3::DateTime wall = {};
    uint32_t ms = 0;
};

// Records the callbacks a VenClient fires.
struct Recorder {
    static constexpr size_t kMax = 64;
    size_t statuses = 0;
    oa3::VenStatusInfo status[kMax];
    size_t prices = 0;
    oa3::PriceInfo price[kMax];

    void reset() {
        statuses = 0;
        prices = 0;
    }
    const oa3::VenStatusInfo& last_status() const { return status[(statuses - 1) % kMax]; }
    const oa3::PriceInfo& last_price() const { return price[(prices - 1) % kMax]; }

    static void on_status(void* ctx, const oa3::VenStatusInfo& s) {
        Recorder* r = static_cast<Recorder*>(ctx);
        r->status[r->statuses++ % kMax] = s;
    }
    static void on_price(void* ctx, const oa3::PriceInfo& p) {
        Recorder* r = static_cast<Recorder*>(ctx);
        r->price[r->prices++ % kMax] = p;
    }
};

inline size_t load_file(const char* path, char* buf, size_t size) {
    FILE* f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    TEST_ASSERT_TRUE(n < size - 1);
    buf[n] = '\0';
    return n;
}

// The fixtures' program.
constexpr const char* kProgramName = "EELEC-013532223";
constexpr const char* kProgramId = "b54d3d1f-bc87-4e47-bbc1-2b95958283fc";
constexpr const char* kBaseUrl = "https://price.grid-coordination.energy/openadr3/3.1.0";

// A /programs page of count programs named <prefix>-<first + i>, ids p-<first + i>; when
// match_at < count, that element is named name instead. Returns buf.
inline const char* programs_page(char* buf, size_t size, const char* prefix, unsigned first,
                                 unsigned count, unsigned match_at = UINT32_MAX,
                                 const char* name = nullptr) {
    size_t n = 0;
    n += static_cast<size_t>(snprintf(buf + n, size - n, "["));
    for (unsigned i = 0; i < count; ++i) {
        char pname[64];
        if (i == match_at) {
            snprintf(pname, sizeof(pname), "%s", name);
        } else {
            snprintf(pname, sizeof(pname), "%s-%u", prefix, first + i);
        }
        n += static_cast<size_t>(snprintf(
            buf + n, size - n,
            "%s{\"id\":\"%s\",\"programName\":\"%s\",\"objectType\":\"PROGRAM\","
            "\"payloadDescriptors\":[{\"objectType\":\"EVENT_PAYLOAD_DESCRIPTOR\","
            "\"payloadType\":\"PRICE\",\"units\":\"KWH\",\"currency\":\"USD\"}]}",
            i == 0 ? "" : ",", i == match_at ? kProgramId : pname, pname));
        TEST_ASSERT_TRUE(n < size);
    }
    snprintf(buf + n, size - n, "]");
    TEST_ASSERT_TRUE(n + 1 < size);
    return buf;
}

}  // namespace fakes
