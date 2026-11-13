// CurlHttpTransport against an HTTP/1.1 server on an ephemeral loopback port.
#include <arpa/inet.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

#include <oa3/posix/curl_transport.h>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                  \
    do {                                                             \
        if (!(cond)) {                                               \
            fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                            \
        }                                                            \
    } while (0)

class Server {
public:
    bool start() {
        _fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        if (listen(_fd, 8) != 0) return false;
        socklen_t len = sizeof(addr);
        getsockname(_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        _port = ntohs(addr.sin_port);
        _thread = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        _stop = true;
        _thread.join();
        close(_fd);
    }

    int port() const { return _port; }
    int connections() const { return _connections; }
    std::string last_request() {
        std::lock_guard<std::mutex> lock(_mutex);
        return _last_request;
    }

private:
    void run() {
        while (!_stop) {
            pollfd p{_fd, POLLIN, 0};
            if (poll(&p, 1, 50) <= 0) continue;
            int c = accept(_fd, nullptr, nullptr);
            if (c < 0) continue;
            ++_connections;
            serve(c);
            close(c);
        }
    }

    void serve(int c) {
        std::string req;
        char buf[1024];
        while (req.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = recv(c, buf, sizeof(buf), 0);
            if (n <= 0) return;
            req.append(buf, static_cast<size_t>(n));
        }
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _last_request = req;
        }
        std::string path = req.substr(4, req.find(' ', 4) - 4);
        std::string resp;
        if (path == "/ok") {
            resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 13\r\n"
                   "Connection: close\r\n\r\n{\"hello\":\"x\"}";
        } else if (path == "/missing") {
            resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nnot found";
        } else if (path == "/chunked") {
            resp = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                   "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
        } else if (path == "/big") {
            resp = "HTTP/1.1 200 OK\r\nContent-Length: 4096\r\nConnection: close\r\n\r\n" +
                   std::string(4096, 'x');
        } else if (path == "/stall") {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            return;
        } else {
            resp = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
        }
        send(c, resp.data(), resp.size(), 0);
    }

    int _fd = -1;
    int _port = 0;
    std::thread _thread;
    std::atomic<bool> _stop{false};
    std::atomic<int> _connections{0};
    std::mutex _mutex;
    std::string _last_request;
};

struct Captured {
    int calls = 0;
    int status = 0;
    std::string body;
};

void capture(void* ctx, int status, oa3::ByteReader& body) {
    Captured* c = static_cast<Captured*>(ctx);
    ++c->calls;
    c->status = status;
    int ch;
    while ((ch = body.read()) >= 0) c->body.push_back(static_cast<char>(ch));
}

char g_buffer[1024];

oa3::HttpResult get(const std::string& url, Captured* out, uint32_t timeout_ms = 5000) {
    static const oa3::HttpHeader headers[] = {{"Accept", "application/json"},
                                              {"X-Oa3-Test", "yes"}};
    oa3::posix::CurlHttpTransport http(g_buffer, sizeof(g_buffer));
    oa3::HttpRequest req{url.c_str(), headers, 2, timeout_ms};
    return http.get(req, capture, out);
}

}  // namespace

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    Server server;
    if (!server.start()) {
        fprintf(stderr, "cannot start the loopback server\n");
        return 1;
    }
    std::string base = "http://127.0.0.1:" + std::to_string(server.port());

    {
        Captured c;
        CHECK(get(base + "/ok", &c) == oa3::HttpResult::kOk);
        CHECK(c.calls == 1);
        CHECK(c.status == 200);
        CHECK(c.body == "{\"hello\":\"x\"}");
        std::string req = server.last_request();
        CHECK(req.find("Accept: application/json\r\n") != std::string::npos);
        CHECK(req.find("X-Oa3-Test: yes\r\n") != std::string::npos);
        CHECK(req.find("Connection: close\r\n") != std::string::npos);
    }
    {
        Captured c;
        CHECK(get(base + "/missing", &c) == oa3::HttpResult::kOk);
        CHECK(c.calls == 1);
        CHECK(c.status == 404);
        CHECK(c.body == "not found");
    }
    {
        Captured c;
        CHECK(get(base + "/chunked", &c) == oa3::HttpResult::kOk);
        CHECK(c.status == 200);
        CHECK(c.body == "hello world");
    }
    {
        Captured c;
        CHECK(get(base + "/big", &c) == oa3::HttpResult::kTooLarge);
        CHECK(c.calls == 0);
    }
    {
        int before = server.connections();
        Captured a, b;
        get(base + "/ok", &a);
        get(base + "/ok", &b);
        CHECK(server.connections() - before == 2);
    }
    {
        Captured c;
        auto t0 = std::chrono::steady_clock::now();
        CHECK(get(base + "/stall", &c, 200) == oa3::HttpResult::kTimeout);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 900);
        CHECK(c.calls == 0);
    }
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        socklen_t len = sizeof(addr);
        getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
        int closed_port = ntohs(addr.sin_port);
        close(fd);
        Captured c;
        CHECK(get("http://127.0.0.1:" + std::to_string(closed_port) + "/", &c) ==
              oa3::HttpResult::kConnectFailed);
    }
    {
        Captured c;
        CHECK(get("http://[::1/x", &c) == oa3::HttpResult::kInvalidUrl);
        CHECK(get("gopherz://example.com/", &c) == oa3::HttpResult::kInvalidUrl);
        CHECK(c.calls == 0);
    }

    server.stop();
    curl_global_cleanup();
    printf("%s\n", g_failures ? "FAILED" : "OK");
    return g_failures ? 1 : 0;
}
