#include <oa3/model/notifiers.h>

#include <string.h>

#include <ArduinoJson.h>

namespace oa3 {

const char* to_string(MqttAuthMethod m) {
    switch (m) {
        case MqttAuthMethod::kUnknown:
            return "UNKNOWN";
        case MqttAuthMethod::kAnonymous:
            return "ANONYMOUS";
        case MqttAuthMethod::kOauth2BearerToken:
            return "OAUTH2_BEARER_TOKEN";
        case MqttAuthMethod::kCertificate:
            return "CERTIFICATE";
    }
    return "UNKNOWN";
}

namespace {

using ArduinoJson::DeserializationError;
using ArduinoJson::JsonArrayConst;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObjectConst;
using ArduinoJson::JsonVariantConst;

// The CERTIFICATE method's caCert, clientCert and clientKey are left out by the filter, so a
// PEM never reaches the arena.
const char kObjectFilter[] =
    "{\"WEBHOOK\":true,\"webhook\":true,"
    "\"MQTT\":{\"URIS\":true,\"uris\":true,\"authentication\":{\"method\":true,\"username\":true}},"
    "\"mqtt\":{\"URIS\":true,\"uris\":true,\"authentication\":{\"method\":true,\"username\":true}}}";

const char kArrayFilter[] =
    "[{\"transport\":true,\"Transport\":true,\"url\":true,\"uri\":true,\"URL\":true,\"URI\":true,"
    "\"URIS\":true,\"uris\":true}]";

const char kTopicsFilter[] =
    "{\"topics\":{\"CREATE\":true,\"UPDATE\":true,\"DELETE\":true,\"ALL\":true}}";

// One byte of lookahead, so the response's shape picks the filter.
class PeekReader final : public ByteReader {
public:
    explicit PeekReader(ByteReader& in) : _in(in), _peeked(kNone) {}

    int peek_non_space() {
        int c;
        do {
            c = read();
        } while (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        _peeked = c;
        return c;
    }

    int read() override {
        if (_peeked != kNone) {
            const int c = _peeked;
            _peeked = kNone;
            return c;
        }
        return _in.read();
    }

    size_t readBytes(char* buffer, size_t length) override {
        size_t n = 0;
        if (length > 0 && _peeked != kNone) {
            if (_peeked < 0) {
                return 0;
            }
            buffer[n++] = static_cast<char>(_peeked);
            _peeked = kNone;
        }
        return n + _in.readBytes(buffer + n, length - n);
    }

private:
    static constexpr int kNone = -2;
    ByteReader& _in;
    int _peeked;
};

ParseResult map_error(DeserializationError e, const JsonArena& arena) {
    if (e == DeserializationError::NoMemory || e == DeserializationError::TooDeep ||
        arena.exhausted()) {
        return ParseResult::kTooLarge;
    }
    if (e) {
        return ParseResult::kJsonError;
    }
    return ParseResult::kOk;
}

// The filter is built in the caller's arena ahead of the document; both are dropped at the
// next reset().
ParseResult deserialize(ByteReader& in, JsonArena& arena, const char* filter_json,
                        JsonDocument& filter, JsonDocument& doc) {
    if (deserializeJson(filter, filter_json) || arena.exhausted()) {
        return ParseResult::kTooLarge;
    }
    const DeserializationError e =
        deserializeJson(doc, in, ArduinoJson::DeserializationOption::Filter(filter));
    return map_error(e, arena);
}

JsonVariantConst either(JsonObjectConst o, const char* a, const char* b) {
    JsonVariantConst v = o[a];
    return v.isNull() ? o[b] : v;
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// a[0..n) equals b, ignoring ASCII case.
bool iequals_n(const char* a, size_t n, const char* b) {
    if (strlen(b) != n) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

bool iequals(const char* a, const char* b) { return iequals_n(a, strlen(a), b); }

void add_uri(MqttNotifier* m, JsonVariantConst v) {
    if (!v.is<const char*>()) {
        return;
    }
    const char* s = v.as<const char*>();
    const size_t n = strlen(s);
    if (n > OA3_NOTIFIER_URI_MAX || m->uri_count >= OA3_NOTIFIER_URIS_MAX) {
        if (m->uris_dropped < UINT8_MAX) {
            ++m->uris_dropped;
        }
        return;
    }
    memcpy(m->uris[m->uri_count++], s, n + 1);
}

void add_uris(MqttNotifier* m, JsonVariantConst v) {
    if (v.is<JsonArrayConst>()) {
        for (JsonVariantConst u : v.as<JsonArrayConst>()) {
            add_uri(m, u);
        }
    } else {
        add_uri(m, v);
    }
}

MqttAuthMethod parse_auth(const char* s) {
    if (strcmp(s, "ANONYMOUS") == 0) {
        return MqttAuthMethod::kAnonymous;
    }
    if (strcmp(s, "OAUTH2_BEARER_TOKEN") == 0) {
        return MqttAuthMethod::kOauth2BearerToken;
    }
    if (strcmp(s, "CERTIFICATE") == 0) {
        return MqttAuthMethod::kCertificate;
    }
    return MqttAuthMethod::kUnknown;
}

void read_object(JsonObjectConst o, Notifiers* out) {
    JsonVariantConst webhook = either(o, "WEBHOOK", "webhook");
    out->webhook = webhook.is<bool>() && webhook.as<bool>();
    JsonVariantConst mqtt = either(o, "MQTT", "mqtt");
    if (!mqtt.is<JsonObjectConst>()) {
        return;
    }
    MqttNotifier* m = &out->mqtt;
    m->present = true;
    add_uris(m, either(mqtt.as<JsonObjectConst>(), "URIS", "uris"));
    // authentication is required; a VTN that leaves it out is read as ANONYMOUS, like VTN-RI.
    m->auth = MqttAuthMethod::kAnonymous;
    JsonVariantConst auth = mqtt["authentication"];
    if (auth.is<JsonObjectConst>()) {
        JsonVariantConst method = auth["method"];
        if (method.is<const char*>()) {
            m->auth = parse_auth(method.as<const char*>());
        }
        JsonVariantConst user = auth["username"];
        if (user.is<const char*>() && strlen(user.as<const char*>()) <= OA3_MQTT_USERNAME_MAX) {
            strcpy(m->username, user.as<const char*>());
        }
    }
}

void read_array(JsonArrayConst a, Notifiers* out) {
    MqttNotifier* m = &out->mqtt;
    for (JsonVariantConst item : a) {
        if (!item.is<JsonObjectConst>()) {
            continue;
        }
        JsonObjectConst o = item.as<JsonObjectConst>();
        JsonVariantConst transport = either(o, "transport", "Transport");
        if (transport.is<const char*>() && transport.as<const char*>()[0] != '\0' &&
            !iequals(transport.as<const char*>(), "MQTT")) {
            continue;
        }
        m->present = true;
        m->auth = MqttAuthMethod::kAnonymous;
        add_uri(m, o["url"]);
        add_uri(m, o["uri"]);
        add_uri(m, o["URL"]);
        add_uri(m, o["URI"]);
        add_uris(m, either(o, "URIS", "uris"));
    }
}

ParseResult copy_topic(JsonObjectConst topics, const char* key, char* dst) {
    dst[0] = '\0';
    JsonVariantConst v = topics[key];
    if (v.isNull()) {
        return ParseResult::kOk;
    }
    if (!v.is<const char*>()) {
        return ParseResult::kInvalidField;
    }
    const char* s = v.as<const char*>();
    const size_t n = strlen(s);
    if (n > OA3_TOPIC_MAX) {
        return ParseResult::kCapacityExceeded;
    }
    memcpy(dst, s, n + 1);
    return ParseResult::kOk;
}

bool parse_port(const char* s, size_t n, uint16_t* out) {
    if (n == 0 || n > 5) {
        return false;
    }
    uint32_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10 + static_cast<uint32_t>(s[i] - '0');
    }
    if (v == 0 || v > 65535) {
        return false;
    }
    *out = static_cast<uint16_t>(v);
    return true;
}

}  // namespace

ParseResult parse_notifiers(ByteReader& in, JsonArena& arena, Notifiers* out) {
    memset(out, 0, sizeof(*out));
    PeekReader reader(in);
    const int c = reader.peek_non_space();
    if (c != '{' && c != '[') {
        const bool json_value = c == '"' || c == '-' || (c >= '0' && c <= '9') || c == 't' ||
                                c == 'f' || c == 'n';
        return json_value ? ParseResult::kInvalidField : ParseResult::kJsonError;
    }
    arena.reset();
    JsonDocument filter(&arena);
    JsonDocument doc(&arena);
    const ParseResult r =
        deserialize(reader, arena, c == '{' ? kObjectFilter : kArrayFilter, filter, doc);
    if (r != ParseResult::kOk) {
        return r;
    }
    if (doc.is<JsonObjectConst>()) {
        read_object(doc.as<JsonObjectConst>(), out);
    } else if (doc.is<JsonArrayConst>()) {
        read_array(doc.as<JsonArrayConst>(), out);
    } else {
        return ParseResult::kInvalidField;
    }
    return ParseResult::kOk;
}

ParseResult parse_notifiers(const char* json, size_t len, JsonArena& arena, Notifiers* out) {
    BufferReader in(json, len);
    return parse_notifiers(in, arena, out);
}

ParseResult parse_topics(ByteReader& in, JsonArena& arena, OperationTopics* out) {
    memset(out, 0, sizeof(*out));
    arena.reset();
    JsonDocument filter(&arena);
    JsonDocument doc(&arena);
    const ParseResult r = deserialize(in, arena, kTopicsFilter, filter, doc);
    if (r != ParseResult::kOk) {
        return r;
    }
    JsonVariantConst topics = doc["topics"];
    if (!topics.is<JsonObjectConst>()) {
        return ParseResult::kInvalidField;
    }
    JsonObjectConst t = topics.as<JsonObjectConst>();
    ParseResult e = copy_topic(t, "CREATE", out->create);
    if (e == ParseResult::kOk) {
        e = copy_topic(t, "UPDATE", out->update);
    }
    if (e == ParseResult::kOk) {
        e = copy_topic(t, "DELETE", out->remove);
    }
    if (e == ParseResult::kOk) {
        e = copy_topic(t, "ALL", out->all);
    }
    if (e != ParseResult::kOk) {
        memset(out, 0, sizeof(*out));
    }
    return e;
}

ParseResult parse_topics(const char* json, size_t len, JsonArena& arena, OperationTopics* out) {
    BufferReader in(json, len);
    return parse_topics(in, arena, out);
}

bool parse_broker_uri(const char* uri, BrokerAddress* out) {
    memset(out, 0, sizeof(*out));
    if (uri == nullptr) {
        return false;
    }
    const char* sep = strstr(uri, "://");
    if (sep == nullptr) {
        return false;
    }
    const size_t n = static_cast<size_t>(sep - uri);
    const bool tls = iequals_n(uri, n, "mqtts") || iequals_n(uri, n, "ssl");
    if (!tls && !iequals_n(uri, n, "mqtt") && !iequals_n(uri, n, "tcp")) {
        return false;
    }
    const char* rest = sep + 3;
    size_t end = strcspn(rest, "/?#");
    const char* auth = rest;
    for (size_t i = 0; i < end; ++i) {
        if (rest[i] == '@') {
            auth = rest + i + 1;
        }
    }
    end -= static_cast<size_t>(auth - rest);

    const char* host = auth;
    size_t host_len = 0;
    const char* port = nullptr;
    size_t port_len = 0;
    if (end > 0 && auth[0] == '[') {
        const char* close = static_cast<const char*>(memchr(auth, ']', end));
        if (close == nullptr) {
            return false;
        }
        host = auth + 1;
        host_len = static_cast<size_t>(close - host);
        const char* after = close + 1;
        const size_t left = end - static_cast<size_t>(after - auth);
        if (left > 0) {
            if (after[0] != ':') {
                return false;
            }
            port = after + 1;
            port_len = left - 1;
        }
    } else {
        const char* colon = static_cast<const char*>(memchr(auth, ':', end));
        if (colon != nullptr) {
            const size_t after = static_cast<size_t>(colon - auth) + 1;
            if (memchr(colon + 1, ':', end - after) != nullptr) {
                return false;
            }
            host_len = static_cast<size_t>(colon - auth);
            port = colon + 1;
            port_len = end - after;
        } else {
            host_len = end;
        }
    }
    if (host_len == 0 || host_len > OA3_HOST_MAX) {
        return false;
    }
    out->tls = tls;
    out->port = tls ? 8883 : 1883;
    if (port_len > 0 && !parse_port(port, port_len, &out->port)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    memcpy(out->host, host, host_len);
    out->host[host_len] = '\0';
    return true;
}

bool select_broker(const MqttNotifier& mqtt, bool allow_plain, BrokerAddress* out) {
    BrokerAddress b;
    for (uint8_t i = 0; i < mqtt.uri_count; ++i) {
        if (parse_broker_uri(mqtt.uris[i], &b) && b.tls) {
            *out = b;
            return true;
        }
    }
    if (!allow_plain) {
        return false;
    }
    for (uint8_t i = 0; i < mqtt.uri_count; ++i) {
        if (parse_broker_uri(mqtt.uris[i], &b)) {
            *out = b;
            return true;
        }
    }
    return false;
}

}  // namespace oa3
