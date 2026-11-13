#pragma once

#include <stddef.h>
#include <stdint.h>

namespace oa3 {

enum class MqttResult : uint8_t {
    kOk,
    kInvalidArgument,  // e.g. an empty host or client id
    kNetworkDown,      // the port knows it has no network (link down, no address)
    kDnsFailed,
    kConnectFailed,    // TCP refused or unreachable
    kTlsFailed,        // the handshake failed for a reason other than verification
    kTlsVerifyFailed,  // the broker's certificate or host name did not verify
    kRefused,          // CONNACK return code 1, 2 or 3 (version, identifier, unavailable)
    kNotAuthorized,    // CONNACK return code 4 or 5
    kSubscribeFailed,  // SUBACK 0x80
    kTimeout,
    kNotConnected,     // the call needs a session and there is none
    kConnectionLost,   // the session ended without disconnect()
    kIoError,          // any other failure
};

const char* to_string(MqttResult r);

// Everything is borrowed for the duration of connect().
struct MqttConnectOptions {
    const char* host = nullptr;
    uint16_t port = 0;
    bool tls = false;
    const char* client_id = nullptr;
    const char* username = nullptr;  // nullptr: none
    const char* password = nullptr;  // nullptr: none; sent only with a username
    bool verify = true;              // tls: verify the certificate chain and host name
    const char* ca_pem = nullptr;    // tls: trusted roots as PEM; nullptr: the port's default
    uint16_t keepalive_s = 60;
    bool clean_session = true;
    uint32_t timeout_ms = 15000;  // connect: TCP, TLS and CONNACK together
};

// payload is nullptr and too_large is true when the payload exceeded the port's buffer; len
// is then the size on the wire when known, else 0. Valid only during the call.
struct MqttMessage {
    const char* topic;
    const char* payload;
    size_t len;
    uint8_t qos;
    bool retained;
    bool too_large;
};

typedef void (*MqttMessageFn)(void* ctx, const MqttMessage& message);

// A port's MQTT 3.1.1 client, driven from the task that runs VenClient::loop(); it needs no
// thread of its own. One session at a time. Every call blocks at most its timeout.
class MqttSession {
public:
    // Opens a session, ending any open one first. On kOk, *session_present is the CONNACK flag.
    virtual MqttResult connect(const MqttConnectOptions& options, bool* session_present) = 0;
    // Sends DISCONNECT when connected and closes the connection. Callable at any time, it frees
    // everything a previous connect() allocated, the TLS session included, even after a failed
    // connect() or a lost session. A broker keeps a session opened with clean_session false.
    virtual void disconnect() = 0;
    // Each returns when the SUBACK or UNSUBACK arrives, or at timeout_ms.
    virtual MqttResult subscribe(const char* topic_filter, uint8_t qos, uint32_t timeout_ms) = 0;
    virtual MqttResult unsubscribe(const char* topic_filter, uint32_t timeout_ms) = 0;
    // Delivers every message already received, waiting up to max_wait_ms for the first, and
    // sends PINGREQ when keepalive requires it. Acknowledges each QoS 1 message after
    // on_message returns. kConnectionLost when the session ended, before or during the call;
    // messages received before the loss are still delivered.
    virtual MqttResult poll(uint32_t max_wait_ms, MqttMessageFn on_message, void* ctx) = 0;
    virtual bool connected() const = 0;

protected:
    ~MqttSession() = default;
};

}  // namespace oa3
