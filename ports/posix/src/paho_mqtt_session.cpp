#include <oa3/posix/paho_mqtt_session.h>

#include <MQTTClient.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace oa3 {
namespace posix {

static uint64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000u + static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
}

static unsigned long command_timeout(uint32_t timeout_ms) {
    return timeout_ms < 5000 ? 5000ul : timeout_ms;  // Paho's minimum
}

PahoMqttSession::PahoMqttSession(char* buffer, size_t size)
    : _buffer(buffer), _size(size), _client(nullptr), _ca_file(nullptr),
      _ca_pem_written(nullptr), _verify_failed(false) {
    _ca_tmp[0] = '\0';
}

PahoMqttSession::~PahoMqttSession() {
    disconnect();
    if (_ca_tmp[0]) unlink(_ca_tmp);
}

int PahoMqttSession::on_ssl_error(const char* str, size_t len, void* self) {
    static const char kVerify[] = "certificate verify failed";
    size_t n = sizeof(kVerify) - 1;
    for (size_t i = 0; i + n <= len; ++i) {
        if (memcmp(str + i, kVerify, n) == 0) {
            static_cast<PahoMqttSession*>(self)->_verify_failed = true;
            break;
        }
    }
    return 0;
}

const char* PahoMqttSession::trust_store(const char* ca_pem) {
    if (_ca_file) return _ca_file;
    if (!ca_pem) return nullptr;
    if (_ca_tmp[0] && _ca_pem_written == ca_pem) return _ca_tmp;
    if (_ca_tmp[0]) unlink(_ca_tmp);
    _ca_pem_written = nullptr;
    snprintf(_ca_tmp, sizeof(_ca_tmp), "/tmp/oa3-mqtt-ca-XXXXXX");
    int fd = mkstemp(_ca_tmp);
    if (fd < 0) {
        _ca_tmp[0] = '\0';
        return nullptr;
    }
    size_t len = strlen(ca_pem);
    bool ok = write(fd, ca_pem, len) == static_cast<ssize_t>(len);
    close(fd);
    if (!ok) {
        unlink(_ca_tmp);
        _ca_tmp[0] = '\0';
        return nullptr;
    }
    _ca_pem_written = ca_pem;
    return _ca_tmp;
}

void PahoMqttSession::destroy() {
    if (!_client) return;
    MQTTClient client = static_cast<MQTTClient>(_client);
    MQTTClient_destroy(&client);
    _client = nullptr;
}

MqttResult PahoMqttSession::connect(const MqttConnectOptions& options, bool* session_present) {
    disconnect();
    if (session_present) *session_present = false;
    if (!options.host || !options.host[0] || !options.client_id || !options.client_id[0] ||
        options.port == 0) {
        return MqttResult::kInvalidArgument;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* addrs = nullptr;
    if (getaddrinfo(options.host, nullptr, &hints, &addrs) != 0) return MqttResult::kDnsFailed;
    freeaddrinfo(addrs);

    char uri[300];
    bool v6 = strchr(options.host, ':') != nullptr;
    int n = snprintf(uri, sizeof(uri), "%s://%s%s%s:%u", options.tls ? "ssl" : "tcp",
                     v6 ? "[" : "", options.host, v6 ? "]" : "",
                     static_cast<unsigned>(options.port));
    if (n < 0 || static_cast<size_t>(n) >= sizeof(uri)) return MqttResult::kInvalidArgument;

    MQTTClient client = nullptr;
    int rc = MQTTClient_create(&client, uri, options.client_id, MQTTCLIENT_PERSISTENCE_NONE,
                               nullptr);
    if (rc != MQTTCLIENT_SUCCESS) {
        return rc == MQTTCLIENT_SSL_NOT_SUPPORTED ? MqttResult::kTlsFailed
                                                  : MqttResult::kInvalidArgument;
    }
    _client = client;
    MQTTClient_setCommandTimeout(client, command_timeout(options.timeout_ms));

    MQTTClient_connectOptions conn = MQTTClient_connectOptions_initializer;
    conn.MQTTVersion = MQTTVERSION_3_1_1;
    conn.keepAliveInterval = options.keepalive_s;
    conn.cleansession = options.clean_session ? 1 : 0;
    conn.reliable = 1;
    conn.connectTimeout = static_cast<int>((options.timeout_ms + 999) / 1000);
    if (options.username) {
        conn.username = options.username;
        conn.password = options.password;
    }

    MQTTClient_SSLOptions ssl = MQTTClient_SSLOptions_initializer;
    _verify_failed = false;
    if (options.tls) {
        ssl.enableServerCertAuth = options.verify ? 1 : 0;
        ssl.verify = options.verify ? 1 : 0;
        ssl.sslVersion = MQTT_SSL_VERSION_TLS_1_2;
        ssl.trustStore = trust_store(options.ca_pem);
        ssl.ssl_error_cb = on_ssl_error;
        ssl.ssl_error_context = this;
        conn.ssl = &ssl;
    }

    uint64_t start = now_ms();
    rc = MQTTClient_connect(client, &conn);
    if (rc == MQTTCLIENT_SUCCESS) {
        if (session_present) *session_present = conn.returned.sessionPresent != 0;
        return MqttResult::kOk;
    }
    destroy();
    switch (rc) {
        case 1:
        case 2:
        case 3:
            return MqttResult::kRefused;
        case 4:
        case 5:
            return MqttResult::kNotAuthorized;
        case MQTTCLIENT_SSL_NOT_SUPPORTED:
            return MqttResult::kTlsFailed;
        default:
            break;
    }
    if (_verify_failed) return MqttResult::kTlsVerifyFailed;
    if (now_ms() - start >= options.timeout_ms) return MqttResult::kTimeout;
    return options.tls ? MqttResult::kTlsFailed : MqttResult::kConnectFailed;
}

void PahoMqttSession::disconnect() {
    if (!_client) return;
    MQTTClient client = static_cast<MQTTClient>(_client);
    if (MQTTClient_isConnected(client)) MQTTClient_disconnect(client, 1000);
    destroy();
}

MqttResult PahoMqttSession::command_result(int rc, uint64_t start, uint32_t timeout_ms) const {
    if (rc == MQTTCLIENT_SUCCESS) return MqttResult::kOk;
    if (rc == MQTT_BAD_SUBSCRIBE) return MqttResult::kSubscribeFailed;
    if (rc == MQTTCLIENT_DISCONNECTED || !connected()) return MqttResult::kConnectionLost;
    if (now_ms() - start >= timeout_ms) return MqttResult::kTimeout;
    return MqttResult::kIoError;
}

MqttResult PahoMqttSession::subscribe(const char* topic_filter, uint8_t qos,
                                      uint32_t timeout_ms) {
    if (!_client) return MqttResult::kNotConnected;
    if (!topic_filter || !topic_filter[0] || qos > 2) return MqttResult::kInvalidArgument;
    MQTTClient client = static_cast<MQTTClient>(_client);
    MQTTClient_setCommandTimeout(client, command_timeout(timeout_ms));
    uint64_t start = now_ms();
    return command_result(MQTTClient_subscribe(client, topic_filter, qos), start, timeout_ms);
}

MqttResult PahoMqttSession::unsubscribe(const char* topic_filter, uint32_t timeout_ms) {
    if (!_client) return MqttResult::kNotConnected;
    if (!topic_filter || !topic_filter[0]) return MqttResult::kInvalidArgument;
    MQTTClient client = static_cast<MQTTClient>(_client);
    MQTTClient_setCommandTimeout(client, command_timeout(timeout_ms));
    uint64_t start = now_ms();
    return command_result(MQTTClient_unsubscribe(client, topic_filter), start, timeout_ms);
}

MqttResult PahoMqttSession::poll(uint32_t max_wait_ms, MqttMessageFn on_message, void* ctx) {
    if (!_client) return MqttResult::kNotConnected;
    MQTTClient client = static_cast<MQTTClient>(_client);
    unsigned long wait = max_wait_ms;
    for (;;) {
        char* topic = nullptr;
        int topic_len = 0;
        MQTTClient_message* msg = nullptr;
        int rc = MQTTClient_receive(client, &topic, &topic_len, &msg, wait);
        wait = 0;
        bool got = msg != nullptr;
        if (got) {
            MqttMessage m;
            m.topic = topic ? topic : "";
            m.qos = static_cast<uint8_t>(msg->qos);
            m.retained = msg->retained != 0;
            m.len = msg->payloadlen > 0 ? static_cast<size_t>(msg->payloadlen) : 0;
            m.too_large = m.len > _size;
            m.payload = m.too_large ? nullptr : _buffer;
            if (!m.too_large && m.len) memcpy(_buffer, msg->payload, m.len);
            if (on_message) on_message(ctx, m);
            MQTTClient_freeMessage(&msg);
        }
        if (topic) MQTTClient_free(topic);
        if (rc == MQTTCLIENT_DISCONNECTED || !MQTTClient_isConnected(client)) {
            return MqttResult::kConnectionLost;
        }
        if (rc != MQTTCLIENT_SUCCESS && rc != MQTTCLIENT_TOPICNAME_TRUNCATED) {
            return MqttResult::kIoError;
        }
        if (!got) return MqttResult::kOk;
    }
}

bool PahoMqttSession::connected() const {
    return _client && MQTTClient_isConnected(static_cast<MQTTClient>(_client));
}

}  // namespace posix
}  // namespace oa3
