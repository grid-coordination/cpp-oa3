#pragma once

#include <stddef.h>
#include <stdint.h>

#include <oa3/model/byte_reader.h>
#include <oa3/model/json_arena.h>
#include <oa3/model/limits.h>
#include <oa3/model/parse.h>

namespace oa3 {

enum class MqttAuthMethod : uint8_t { kUnknown, kAnonymous, kOauth2BearerToken, kCertificate };

const char* to_string(MqttAuthMethod m);

// The MQTT member of a /notifiers response (openadr3.yaml mqttNotifierBindingObject). The
// CERTIFICATE method's caCert, clientCert and clientKey are not kept.
struct MqttNotifier {
    bool present;
    uint8_t uri_count;
    uint8_t uris_dropped;  // URIs beyond OA3_NOTIFIER_URIS_MAX or OA3_NOTIFIER_URI_MAX
    char uris[OA3_NOTIFIER_URIS_MAX][OA3_NOTIFIER_URI_MAX + 1];
    MqttAuthMethod auth;
    char username[OA3_MQTT_USERNAME_MAX + 1];  // kOauth2BearerToken
};

struct Notifiers {
    bool webhook;
    MqttNotifier mqtt;
};

// A /notifiers response: the spec's object ({"WEBHOOK":..,"MQTT":{"URIS":[..],..}}), or the
// VTN-RI array ([{"transport":"MQTT","url":".."}]), whose MQTT entries count as ANONYMOUS.
// Keys other than MQTT (WEBSOCKET, PUSH-MQTT) are ignored. No MQTT binding is kOk with
// mqtt.present false.
ParseResult parse_notifiers(const char* json, size_t len, JsonArena& arena, Notifiers* out);
ParseResult parse_notifiers(ByteReader& in, JsonArena& arena, Notifiers* out);

// A /notifiers/mqtt/topics/... response (openadr3.yaml notifierTopicsResponse). "" for an
// operation the VTN did not list.
struct OperationTopics {
    char create[OA3_TOPIC_MAX + 1];
    char update[OA3_TOPIC_MAX + 1];
    char remove[OA3_TOPIC_MAX + 1];  // DELETE
    char all[OA3_TOPIC_MAX + 1];
};

// kCapacityExceeded when a topic is longer than OA3_TOPIC_MAX; kInvalidField without a
// topics object.
ParseResult parse_topics(const char* json, size_t len, JsonArena& arena, OperationTopics* out);
ParseResult parse_topics(ByteReader& in, JsonArena& arena, OperationTopics* out);

struct BrokerAddress {
    char host[OA3_HOST_MAX + 1];
    uint16_t port;
    bool tls;
};

// mqtts:// and ssl:// are TLS (default port 8883); mqtt:// and tcp:// are plain (1883).
// Schemes are case-insensitive. An IPv6 literal is kept without its brackets. False for any
// other or a missing scheme, an empty or too long host, or a port that is not 1..65535.
bool parse_broker_uri(const char* uri, BrokerAddress* out);

// The first TLS URI that parses; failing that, when allow_plain, the first plain one. False
// when none qualifies.
bool select_broker(const MqttNotifier& mqtt, bool allow_plain, BrokerAddress* out);

}  // namespace oa3
