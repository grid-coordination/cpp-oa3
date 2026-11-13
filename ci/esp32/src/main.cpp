#include <Arduino.h>
#include <oa3/client/ven_client.h>
#include <oa3/model/time.h>
#include <oa3/model/version.h>

// Stubs so the client is compiled and linked for xtensa; the port supplies the real ones.
class StubTransport : public oa3::HttpTransport {
public:
    oa3::HttpResult get(const oa3::HttpRequest&, oa3::HttpResponseFn, void*) override {
        return oa3::HttpResult::kConnectFailed;
    }
};

class StubClock : public oa3::Clock {
public:
    bool now(oa3::DateTime* out) override {
        return oa3::parse_datetime("2026-11-13T20:00:00Z", out);
    }
    uint32_t monotonic_ms() override { return millis(); }
};

class StubMqtt : public oa3::MqttSession {
public:
    oa3::MqttResult connect(const oa3::MqttConnectOptions&, bool*) override {
        return oa3::MqttResult::kConnectFailed;
    }
    void disconnect() override {}
    oa3::MqttResult subscribe(const char*, uint8_t, uint32_t) override {
        return oa3::MqttResult::kNotConnected;
    }
    oa3::MqttResult unsubscribe(const char*, uint32_t) override {
        return oa3::MqttResult::kNotConnected;
    }
    oa3::MqttResult poll(uint32_t, oa3::MqttMessageFn, void*) override {
        return oa3::MqttResult::kNotConnected;
    }
    bool connected() const override { return false; }
};

static StubTransport transport;
static StubClock clock_;
static StubMqtt mqtt;
static oa3::StaticVenStorage<> storage;
static oa3::NotificationStorage notes;

void setup() {
    Serial.begin(115200);
    Serial.printf("oa3 %s, OpenADR %s, sizeof(Event) %u, OA3_MAX_EVENTS %u\n", oa3::library_version(),
                  oa3::spec_version(), static_cast<unsigned>(sizeof(oa3::Event)),
                  static_cast<unsigned>(OA3_MAX_EVENTS));

    oa3::VenStorage mem = storage.storage();
    mem.notifications = &notes;
    static oa3::VenClient client(transport, clock_, mem, &mqtt);
    oa3::VenConfig config{};
    config.base_url = "https://price.grid-coordination.energy/openadr3/3.1.0";
    config.program_name = "EELEC-012041131";
    config.notifications = oa3::NotificationMode::kMqtt;
    config.exclusive_tls = true;
    config.mqtt_client_id = "oa3-esp32-ci";
    if (client.start(config)) {
        client.loop();
    }
    Serial.printf("VenClient: %s\n", oa3::to_string(client.status().status));
}

void loop() {}
