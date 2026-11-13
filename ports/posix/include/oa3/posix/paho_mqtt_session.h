#pragma once

#include <stddef.h>

#include <oa3/client/mqtt_session.h>

namespace oa3 {
namespace posix {

// MqttSession over the synchronous Paho MQTT C client with OpenSSL. No callbacks are set, so
// Paho runs no thread of its own and messages are taken in poll() with MQTTClient_receive.
// Each payload is copied into a caller-owned buffer; a larger one is delivered too_large.
// Paho sends the PUBACK of a QoS 1 message when it reads it, before poll() delivers it.
class PahoMqttSession final : public MqttSession {
public:
    PahoMqttSession(char* buffer, size_t size);
    ~PahoMqttSession();

    PahoMqttSession(const PahoMqttSession&) = delete;
    PahoMqttSession& operator=(const PahoMqttSession&) = delete;

    // A PEM bundle of trusted CAs, used instead of MqttConnectOptions::ca_pem. Borrowed.
    void set_ca_file(const char* path) { _ca_file = path; }

    MqttResult connect(const MqttConnectOptions& options, bool* session_present) override;
    void disconnect() override;
    MqttResult subscribe(const char* topic_filter, uint8_t qos, uint32_t timeout_ms) override;
    MqttResult unsubscribe(const char* topic_filter, uint32_t timeout_ms) override;
    MqttResult poll(uint32_t max_wait_ms, MqttMessageFn on_message, void* ctx) override;
    bool connected() const override;

private:
    static int on_ssl_error(const char* str, size_t len, void* self);
    const char* trust_store(const char* ca_pem);
    MqttResult command_result(int rc, uint64_t start, uint32_t timeout_ms) const;
    void destroy();

    char* _buffer;
    size_t _size;
    void* _client;  // MQTTClient
    const char* _ca_file;
    const char* _ca_pem_written;
    char _ca_tmp[64];
    bool _verify_failed;
};

}  // namespace posix
}  // namespace oa3
