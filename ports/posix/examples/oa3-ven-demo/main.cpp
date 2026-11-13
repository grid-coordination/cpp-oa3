// Polls one program's events on an OpenADR 3 VTN, optionally follows its MQTT notifications,
// and prints the price in force.
#include <curl/curl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <oa3/client/ven_client.h>
#include <oa3/posix/curl_transport.h>
#include <oa3/posix/paho_mqtt_session.h>
#include <oa3/posix/posix_clock.h>
#include <oa3/posix/stderr_log.h>

namespace {

const char kUsage[] =
    "usage: oa3-ven-demo [options]\n"
    "  --base-url URL       VTN base URL (default\n"
    "                       https://price.grid-coordination.energy/openadr3/3.1.0)\n"
    "  --program NAME       programName (default EELEC-012041131)\n"
    "  --program-id ID      program id; with --program, the name must match\n"
    "  --target T           a target value, repeatable\n"
    "  --poll SECONDS       poll period (default 900)\n"
    "  --horizon ISO8601    window parsed from now (default PT24H)\n"
    "  --cacert PATH        trusted CA bundle (default: the system store)\n"
    "  --insecure           skip TLS peer and host verification (HTTPS and MQTTS)\n"
    "  --notifications off|mqtt\n"
    "                       follow the program's MQTT notifications (default off)\n"
    "  --mqtt-client-id ID  MQTT client id (default oa3-ven-demo-<pid>)\n"
    "  --mqtt-uri URI       broker URI, used instead of the VTN's /notifiers\n"
    "  --mqtt-allow-plain   accept mqtt:// or tcp:// when no TLS URI is offered\n"
    "  --mqtt-cacert PATH   trusted CA bundle for MQTTS (default: OpenSSL's default paths)\n"
    "  --exclusive-tls      close MQTTS around each HTTPS request (as on the classic ESP32)\n"
    "  --notify-poll SECONDS\n"
    "                       poll period while subscribed (default 3600)\n"
    "  --verbose            log at debug level\n"
    "  --once               exit 0 at the first price report (with mqtt: at the first\n"
    "                       connected report after a price), 1 at the first failure\n"
    "  --help\n";

volatile sig_atomic_t g_stop = 0;
int g_exit = -1;
bool g_once = false;
bool g_mqtt = false;
bool g_priced = false;

void on_signal(int) { g_stop = 1; }

[[noreturn]] void usage_error(const char* msg, const char* arg) {
    fprintf(stderr, "%s%s\n%s", msg, arg ? arg : "", kUsage);
    exit(2);
}

const char* flag_value(int argc, char** argv, int* i, const char* name) {
    const char* a = argv[*i];
    size_t n = strlen(name);
    if (strncmp(a, name, n) != 0) return nullptr;
    if (a[n] == '=') return a + n + 1;
    if (a[n] != '\0') return nullptr;
    if (*i + 1 >= argc) usage_error("missing value for ", name);
    return argv[++*i];
}

void format_time(const oa3::DateTime& t, char* buf, size_t size) {
    if (format_datetime(t, buf, size) == 0) snprintf(buf, size, "?");
}

void on_price(void*, const oa3::PriceInfo& p) {
    char next[oa3::kDateTimeStringSize] = "none";
    if (p.has_next_change) format_time(p.next_change, next, sizeof(next));
    if (p.active) {
        char start[oa3::kDateTimeStringSize];
        char end[oa3::kDateTimeStringSize] = "";
        format_time(p.span.start, start, sizeof(start));
        if (!p.span.end_indefinite) format_time(p.span.end, end, sizeof(end));
        printf("%s..%s  %s %s/%s  next %s\n", start, end, p.text, p.currency, p.units, next);
    } else {
        printf("no price in force (next %s)\n", next);
    }
    fflush(stdout);
    g_priced = true;
    if (g_once && !g_mqtt && g_exit < 0) g_exit = 0;
}

void on_status(void*, const oa3::VenStatusInfo& s) {
    printf("status: %s (http %s %d, parse %s, failures %u, retry %u ms)", to_string(s.status),
           to_string(s.http_result), s.http_status, to_string(s.parse_result),
           static_cast<unsigned>(s.consecutive_failures), static_cast<unsigned>(s.retry_in_ms));
    if (g_mqtt) {
        printf(" notify: %s (mqtt %s, failures %u, notifications %u)", to_string(s.notify),
               to_string(s.mqtt_result), static_cast<unsigned>(s.mqtt_failures),
               static_cast<unsigned>(s.notifications));
    }
    printf("\n");
    fflush(stdout);
    if (!g_once || g_exit >= 0) return;
    if (g_mqtt) {
        if (s.notify == oa3::NotifyState::kUnavailable || s.mqtt_failures > 0) {
            g_exit = 1;
            return;
        }
        if (s.notify == oa3::NotifyState::kConnected && g_priced) {
            g_exit = 0;
            return;
        }
    }
    switch (s.status) {
        case oa3::VenStatus::kProgramNotFound:
        case oa3::VenStatus::kTransportError:
        case oa3::VenStatus::kHttpError:
        case oa3::VenStatus::kParseError:
            g_exit = 1;
            break;
        default:
            break;
    }
}

oa3::StaticVenStorage<> storage;
oa3::NotificationStorage notes;
char body[256 * 1024];
char mqtt_payload[8192];

}  // namespace

int main(int argc, char** argv) {
    const char* base_url = "https://price.grid-coordination.energy/openadr3/3.1.0";
    const char* program = "EELEC-012041131";
    const char* program_id = nullptr;
    const char* targets[OA3_MAX_TARGETS];
    size_t target_count = 0;
    unsigned long poll_s = 900;
    oa3::Duration horizon;
    parse_duration("PT24H", &horizon);
    const char* cacert = nullptr;
    bool insecure = false;
    bool verbose = false;
    oa3::NotificationMode notifications = oa3::NotificationMode::kOff;
    const char* mqtt_client_id = nullptr;
    const char* mqtt_uri = nullptr;
    bool mqtt_allow_plain = false;
    const char* mqtt_cacert = nullptr;
    bool exclusive_tls = false;
    unsigned long notify_poll_s = 0;

    for (int i = 1; i < argc; ++i) {
        const char* v;
        if ((v = flag_value(argc, argv, &i, "--base-url"))) {
            base_url = v;
        } else if ((v = flag_value(argc, argv, &i, "--program-id"))) {
            program_id = v;
        } else if ((v = flag_value(argc, argv, &i, "--program"))) {
            program = v;
        } else if ((v = flag_value(argc, argv, &i, "--target"))) {
            if (target_count == OA3_MAX_TARGETS) usage_error("too many targets at ", v);
            targets[target_count++] = v;
        } else if ((v = flag_value(argc, argv, &i, "--poll"))) {
            char* end;
            poll_s = strtoul(v, &end, 10);
            if (*end || poll_s == 0) usage_error("bad --poll ", v);
        } else if ((v = flag_value(argc, argv, &i, "--horizon"))) {
            if (!parse_duration(v, &horizon)) usage_error("bad --horizon ", v);
        } else if ((v = flag_value(argc, argv, &i, "--notifications"))) {
            if (strcmp(v, "off") == 0) {
                notifications = oa3::NotificationMode::kOff;
            } else if (strcmp(v, "mqtt") == 0) {
                notifications = oa3::NotificationMode::kMqtt;
            } else {
                usage_error("bad --notifications ", v);
            }
        } else if ((v = flag_value(argc, argv, &i, "--mqtt-client-id"))) {
            mqtt_client_id = v;
        } else if ((v = flag_value(argc, argv, &i, "--mqtt-uri"))) {
            mqtt_uri = v;
        } else if (strcmp(argv[i], "--mqtt-allow-plain") == 0) {
            mqtt_allow_plain = true;
        } else if ((v = flag_value(argc, argv, &i, "--mqtt-cacert"))) {
            mqtt_cacert = v;
        } else if (strcmp(argv[i], "--exclusive-tls") == 0) {
            exclusive_tls = true;
        } else if ((v = flag_value(argc, argv, &i, "--notify-poll"))) {
            char* end;
            notify_poll_s = strtoul(v, &end, 10);
            if (*end || notify_poll_s == 0) usage_error("bad --notify-poll ", v);
        } else if ((v = flag_value(argc, argv, &i, "--cacert"))) {
            cacert = v;
        } else if (strcmp(argv[i], "--insecure") == 0) {
            insecure = true;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--once") == 0) {
            g_once = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            fputs(kUsage, stdout);
            return 0;
        } else {
            usage_error("unknown argument ", argv[i]);
        }
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        fprintf(stderr, "curl_global_init failed\n");
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    oa3::posix::CurlHttpTransport http(body, sizeof(body));
    http.set_ca_file(cacert);
    http.set_insecure(insecure);
    oa3::posix::PosixClock clock;
    oa3::posix::PahoMqttSession session(mqtt_payload, sizeof(mqtt_payload));
    session.set_ca_file(mqtt_cacert);
    oa3::VenStorage mem = storage.storage();
    mem.notifications = &notes;
    oa3::VenClient client(http, clock, mem, &session);
    g_mqtt = notifications == oa3::NotificationMode::kMqtt;
    char client_id[32];
    if (!mqtt_client_id) {
        snprintf(client_id, sizeof(client_id), "oa3-ven-demo-%ld", static_cast<long>(getpid()));
        mqtt_client_id = client_id;
    }

    oa3::VenConfig config{};
    config.base_url = base_url;
    config.program_name = program;
    config.program_id = program_id;
    config.targets = target_count ? targets : nullptr;
    config.target_count = target_count;
    config.poll_period_s = static_cast<uint32_t>(poll_s);
    config.horizon = horizon;
    config.notifications = notifications;
    config.mqtt_client_id = mqtt_client_id;
    config.mqtt_uri = mqtt_uri;
    config.mqtt_allow_plain = mqtt_allow_plain;
    config.mqtt_verify = !insecure;
    config.exclusive_tls = exclusive_tls;
    if (notify_poll_s) config.notify_poll_period_s = static_cast<uint32_t>(notify_poll_s);
    config.on_price_change = on_price;
    config.on_status = on_status;
    config.log.fn = oa3::posix::stderr_log;
    config.log.max_level = verbose ? oa3::LogLevel::kDebug : oa3::LogLevel::kInfo;
    if (!client.start(config)) {
        fprintf(stderr, "invalid configuration\n");
        curl_global_cleanup();
        return 2;
    }

    while (!g_stop && g_exit < 0) {
        client.loop();
        if (g_exit >= 0) break;
        uint32_t wait = client.ms_until_due();
        if (wait > 1000) wait = 1000;
        struct timespec ts = {static_cast<time_t>(wait / 1000),
                              static_cast<long>(wait % 1000) * 1000000L};
        nanosleep(&ts, nullptr);
    }
    client.stop();
    curl_global_cleanup();
    return g_exit > 0 ? g_exit : 0;
}
