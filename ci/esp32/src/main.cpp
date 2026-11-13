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

static StubTransport transport;
static StubClock clock_;
static oa3::StaticVenStorage<> storage;

void setup() {
    Serial.begin(115200);
    Serial.printf("oa3 %s, OpenADR %s, sizeof(Event) %u, OA3_MAX_EVENTS %u\n", oa3::library_version(),
                  oa3::spec_version(), static_cast<unsigned>(sizeof(oa3::Event)),
                  static_cast<unsigned>(OA3_MAX_EVENTS));

    static oa3::VenClient client(transport, clock_, storage.storage());
    oa3::VenConfig config{};
    config.base_url = "https://price.grid-coordination.energy/openadr3/3.1.0";
    config.program_name = "EELEC-012041131";
    if (client.start(config)) {
        client.loop();
    }
    Serial.printf("VenClient: %s\n", oa3::to_string(client.status().status));
}

void loop() {}
