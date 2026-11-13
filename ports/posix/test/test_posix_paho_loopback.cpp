// PahoMqttSession against a mosquitto broker this test starts on a loopback port. argv[1] is
// the mosquitto executable.
#include <MQTTClient.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <oa3/posix/paho_mqtt_session.h>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        ++g_failures;
    }
}

void check_result(oa3::MqttResult got, oa3::MqttResult want, const char* what) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: %s, want %s\n", what, oa3::to_string(got), oa3::to_string(want));
        ++g_failures;
    }
}

unsigned free_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(a);
    bind(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof(a));
    getsockname(fd, reinterpret_cast<struct sockaddr*>(&a), &len);
    close(fd);
    return ntohs(a.sin_port);
}

bool port_open(unsigned port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<uint16_t>(port));
    bool ok = connect(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof(a)) == 0;
    close(fd);
    return ok;
}

void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

struct Received {
    int count;
    char topic[128];
    char payload[128];
    size_t len;
    bool retained;
    bool too_large;
    uint8_t qos;
};

void on_message(void* ctx, const oa3::MqttMessage& m) {
    Received* r = static_cast<Received*>(ctx);
    ++r->count;
    snprintf(r->topic, sizeof(r->topic), "%s", m.topic);
    r->len = m.len;
    r->retained = m.retained;
    r->too_large = m.too_large;
    r->qos = m.qos;
    r->payload[0] = '\0';
    if (m.payload && m.len < sizeof(r->payload)) {
        memcpy(r->payload, m.payload, m.len);
        r->payload[m.len] = '\0';
    }
}

// Polls until a message arrives or about two seconds pass.
void poll_for(oa3::posix::PahoMqttSession& s, Received* r) {
    for (int i = 0; i < 20 && r->count == 0; ++i) s.poll(100, on_message, r);
}

bool publish(MQTTClient pub, const char* topic, const char* payload, size_t len, bool retained) {
    MQTTClient_deliveryToken token;
    int rc = MQTTClient_publish(pub, topic, static_cast<int>(len), payload, 1, retained ? 1 : 0,
                                &token);
    return rc == MQTTCLIENT_SUCCESS && MQTTClient_waitForCompletion(pub, token, 2000) == 0;
}

oa3::MqttConnectOptions options(unsigned port, const char* client_id, bool clean) {
    oa3::MqttConnectOptions o;
    o.host = "127.0.0.1";
    o.port = static_cast<uint16_t>(port);
    o.client_id = client_id;
    o.clean_session = clean;
    o.timeout_ms = 5000;
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s MOSQUITTO\n", argv[0]);
        return 2;
    }
    unsigned port = free_port();
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%u", port);
    pid_t broker = fork();
    if (broker == 0) {
        execl(argv[1], "mosquitto", "-p", port_text, static_cast<char*>(nullptr));
        _exit(127);
    }
    bool up = false;
    for (int i = 0; i < 100 && !up; ++i) {
        up = port_open(port);
        if (!up) sleep_ms(50);
    }
    if (!up) {
        fprintf(stderr, "FAIL mosquitto did not listen on %u\n", port);
        kill(broker, SIGTERM);
        waitpid(broker, nullptr, 0);
        return 1;
    }

    char small[64];
    oa3::posix::PahoMqttSession s(small, sizeof(small));
    bool present = true;
    Received r;

    check_result(s.poll(0, on_message, &r), oa3::MqttResult::kNotConnected, "poll before connect");
    check_result(s.subscribe("x", 1, 1000), oa3::MqttResult::kNotConnected,
                 "subscribe before connect");

    oa3::MqttConnectOptions bad = options(port, "oa3-test", true);
    bad.host = "oa3-test.invalid";
    check_result(s.connect(bad, &present), oa3::MqttResult::kDnsFailed, "unknown host");
    check_result(s.connect(options(free_port(), "oa3-test", true), &present),
                 oa3::MqttResult::kConnectFailed, "closed port");
    check_result(s.connect(options(port, "", true), &present), oa3::MqttResult::kInvalidArgument,
                 "empty client id");

    char pub_uri[64];
    snprintf(pub_uri, sizeof(pub_uri), "tcp://127.0.0.1:%u", port);
    MQTTClient pub = nullptr;
    MQTTClient_create(&pub, pub_uri, "oa3-test-pub", MQTTCLIENT_PERSISTENCE_NONE, nullptr);
    MQTTClient_connectOptions pub_opts = MQTTClient_connectOptions_initializer;
    check(MQTTClient_connect(pub, &pub_opts) == MQTTCLIENT_SUCCESS, "publisher connect");

    check_result(s.connect(options(port, "oa3-test", true), &present), oa3::MqttResult::kOk,
                 "connect");
    check(!present, "clean session has no session present");
    check(s.connected(), "connected");
    check_result(s.subscribe("oa3/test/#", 1, 5000), oa3::MqttResult::kOk, "subscribe");

    memset(&r, 0, sizeof(r));
    check(publish(pub, "oa3/test/a", "hello", 5, false), "publish hello");
    poll_for(s, &r);
    check(r.count == 1 && strcmp(r.topic, "oa3/test/a") == 0 && strcmp(r.payload, "hello") == 0 &&
              r.len == 5 && !r.retained && !r.too_large && r.qos == 1,
          "hello delivered");

    char big[100];
    memset(big, 'x', sizeof(big));
    memset(&r, 0, sizeof(r));
    check(publish(pub, "oa3/test/big", big, sizeof(big), false), "publish big");
    poll_for(s, &r);
    check(r.count == 1 && r.too_large && r.len == sizeof(big), "big delivered too_large");

    memset(&r, 0, sizeof(r));
    check(publish(pub, "oa3/retained", "kept", 4, true), "publish retained");
    check_result(s.subscribe("oa3/retained", 1, 5000), oa3::MqttResult::kOk, "subscribe retained");
    poll_for(s, &r);
    check(r.count == 1 && r.retained && strcmp(r.payload, "kept") == 0, "retained delivered");
    publish(pub, "oa3/retained", "", 0, true);  // clears it

    check_result(s.unsubscribe("oa3/test/#", 5000), oa3::MqttResult::kOk, "unsubscribe");
    check_result(s.unsubscribe("oa3/retained", 5000), oa3::MqttResult::kOk,
                 "unsubscribe retained");
    for (int i = 0; i < 3; ++i) s.poll(100, on_message, &r);
    memset(&r, 0, sizeof(r));
    publish(pub, "oa3/test/a", "after", 5, false);
    for (int i = 0; i < 3; ++i) s.poll(100, on_message, &r);
    check(r.count == 0, "nothing after unsubscribe");

    // A session kept by the broker queues QoS 1 messages while the client is away.
    check_result(s.connect(options(port, "oa3-test-persist", false), &present),
                 oa3::MqttResult::kOk, "persistent connect");
    check_result(s.subscribe("oa3/p", 1, 5000), oa3::MqttResult::kOk, "persistent subscribe");
    s.disconnect();
    check(!s.connected(), "disconnected");
    check(publish(pub, "oa3/p", "queued", 6, false), "publish queued");
    check_result(s.connect(options(port, "oa3-test-persist", false), &present),
                 oa3::MqttResult::kOk, "persistent reconnect");
    check(present, "session present on reconnect");
    memset(&r, 0, sizeof(r));
    poll_for(s, &r);
    check(r.count == 1 && strcmp(r.payload, "queued") == 0, "queued message delivered");

    MQTTClient_disconnect(pub, 1000);
    MQTTClient_destroy(&pub);

    kill(broker, SIGTERM);
    waitpid(broker, nullptr, 0);
    oa3::MqttResult lost = oa3::MqttResult::kOk;
    for (int i = 0; i < 30 && lost == oa3::MqttResult::kOk; ++i) {
        lost = s.poll(100, on_message, &r);
    }
    check_result(lost, oa3::MqttResult::kConnectionLost, "poll after broker exit");
    check(!s.connected(), "not connected after loss");

    printf("%s\n", g_failures ? "FAILED" : "OK");
    return g_failures ? 1 : 0;
}
