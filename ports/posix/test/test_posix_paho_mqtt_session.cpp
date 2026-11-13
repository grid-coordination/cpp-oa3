// PahoMqttSession against the price server's broker, over TLS on 8883 and plain TCP on 1883.
// argv[1], when given, is the program id whose event topics are subscribed.
#include <stdio.h>
#include <unistd.h>

#include <oa3/posix/paho_mqtt_session.h>

namespace {

int g_failures = 0;

void check_result(oa3::MqttResult got, oa3::MqttResult want, const char* what) {
    printf("%s: %s\n", what, oa3::to_string(got));
    if (got != want) {
        fprintf(stderr, "FAIL %s: %s, want %s\n", what, oa3::to_string(got), oa3::to_string(want));
        ++g_failures;
    }
}

void on_message(void* ctx, const oa3::MqttMessage& m) {
    ++*static_cast<int*>(ctx);
    printf("message %s: %zu bytes%s%s\n", m.topic, m.len, m.retained ? ", retained" : "",
           m.too_large ? ", too large" : "");
}

char payload[8192];

void run(bool tls, const char* topic) {
    oa3::posix::PahoMqttSession s(payload, sizeof(payload));
    char client_id[48];
    snprintf(client_id, sizeof(client_id), "oa3-posix-test-%ld", static_cast<long>(getpid()));
    oa3::MqttConnectOptions o;
    o.host = "mqtt.grid-coordination.energy";
    o.port = tls ? 8883 : 1883;
    o.tls = tls;
    o.client_id = client_id;
    bool present = true;
    printf("%s://%s:%u\n", tls ? "mqtts" : "mqtt", o.host, static_cast<unsigned>(o.port));
    check_result(s.connect(o, &present), oa3::MqttResult::kOk, "connect");
    if (!s.connected()) return;
    check_result(s.subscribe(topic, 1, 15000), oa3::MqttResult::kOk, "subscribe");
    int count = 0;
    oa3::MqttResult r = oa3::MqttResult::kOk;
    for (int i = 0; i < 30 && r == oa3::MqttResult::kOk; ++i) r = s.poll(100, on_message, &count);
    check_result(r, oa3::MqttResult::kOk, "poll");
    printf("%d messages\n", count);
    s.disconnect();
    if (s.connected()) {
        fprintf(stderr, "FAIL still connected after disconnect\n");
        ++g_failures;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const char* program_id = argc > 1 ? argv[1] : "2d3e1839-f8cb-4a1b-aa6c-c1168297d5cc";
    char topic[192];
    snprintf(topic, sizeof(topic), "OpenADR/3.1.0/programs/%s/events/+", program_id);
    run(true, topic);
    run(false, topic);
    printf("%s\n", g_failures ? "FAILED" : "OK");
    return g_failures ? 1 : 0;
}
