# Porting oa3_client

A port gives `VenClient` an HTTP(S) transport, a clock, a log sink, its storage and, for MQTT notifications, an MQTT session, and calls `loop()` from one task. Everything else (URLs, paging, parsing, backoff, price evaluation) is in the core. `ports/posix` is the reference port; this document adds what an ESP32 port on arduino-esp32 3.x needs.

| Piece | Interface | Header |
|---|---|---|
| HTTP(S) | `oa3::HttpTransport` | `oa3/client/http_transport.h` |
| MQTT(S), optional | `oa3::MqttSession` | `oa3/client/mqtt_session.h` |
| Wall and monotonic time | `oa3::Clock` | `oa3/client/clock.h` |
| Log | `oa3::LogSink` (`LogFn` + `void* ctx` + `max_level`) | `oa3/client/log.h` |
| Event arrays and JSON arena | `oa3::VenStorage` | `oa3/client/ven_client.h` |
| Task | calls `VenClient::loop()` | |

## HttpTransport

`get(request, on_response, ctx)` performs one GET and returns once the connection is closed. The contract:

- One request at a time, and no connection outlives `get()`. On the classic ESP32 a TLS session costs 42-45 KB of internal RAM and a second one does not fit, so the port opens a connection per request and closes it before returning. The client makes at most one request per `loop()` and pages `/programs` and `/events` over successive calls, so it never asks for a second connection while one is open.
- `request.url` is absolute and already percent-encoded, at most `OA3_URL_MAX` (511) characters. Send every entry of `request.headers` (today `Accept: application/json`; a bearer token will arrive the same way). `request.timeout_ms` bounds the whole request, name resolution included (default 15,000 from `VenConfig::http_timeout_ms`); 0 leaves it to the transport. Library timeouts usually budget each phase separately, so take one deadline at the start of `get()` and give each phase (DNS, connect, handshake, headers, body) what is left of it.
- Once a status line arrives, call `on_response(ctx, status, body)` exactly once, for any status, before `get()` returns. `body` is an `oa3::ByteReader` over the response body with any chunked encoding removed; `read()` returns -1 at its end. The handler may stop reading early; the transport then discards the rest by closing the connection. `body` is valid only during the call.
- Return `kOk` when the handler ran and the body was delivered in full or abandoned by the handler. Otherwise return the failure: `kInvalidUrl`, `kDnsFailed`, `kConnectFailed`, `kTlsFailed`, `kTimeout`, `kTooLarge` or `kIoError` (anything else, including a body cut short). After a failure the client discards whatever the handler parsed, so a transport may call the handler and still fail, for example when the deadline passes mid-body.
- Do not follow redirects: deliver a 3xx to `on_response` like any other status. The client fails a non-2xx response, and following one could leave HTTPS for `http://` without `allow_plaintext`.
- `post()` has a default that returns `kUnsupported`. Leave it alone: the client sends no POST until VTN authentication (token and `/vens`) is added in a later release.

The transport needs no body buffer: the parser reads the stream directly, one top-level JSON object at a time, into the arena.

### arduino-esp32 3.x: HTTPClient and NetworkClientSecure

Keep one `NetworkClientSecure` and one `HTTPClient` as members of the transport, and per `get()`:

1. `secure.setCACert(pem)` once at init, with the PEM of the VTN's root (for the price server, the Amazon RSA 2048 chain's root; esp32-firmware stores it at `/certs/priceGrid-coordinationEnergy.ca`). `setInsecure()` only behind an explicit opt-in. `secure.setHandshakeTimeout(seconds)` takes seconds.
2. `http.begin(secure, url)`; `http.useHTTP10(true)`, which also turns off keep-alive in this version (`_reuse = !useHTTP10`) and keeps the server from sending a chunked body that `getStream()` would deliver raw; `http.setConnectTimeout(ms)`; `http.setTimeout(ms)` (a `uint16_t`, so at most 65,535 ms, applied per read); `http.addHeader(name, value)` for each request header. Leave redirects at the default, `HTTPC_DISABLE_FOLLOW_REDIRECTS`.
3. Resolve the host yourself before `GET()` (see "Name resolution" below) and return `kDnsFailed` if that fails. Then `int code = http.GET()`. A negative code is a failure: `HTTPC_ERROR_CONNECTION_REFUSED` (-1) covers TCP and TLS failures alike (`connect()` returning false). `secure.lastError(buf, len)` tells them apart only partly: in arduino-esp32 3.3.11 it is -1 for a TCP connect error, a select timeout or a handshake timeout, and a real mbedTLS code (below -1) for a TLS failure, so return `kTlsFailed` when it is below -1 and log its text, else `kConnectFailed`. It keeps its old value across a DNS failure, which is another reason to resolve first. Map `HTTPC_ERROR_READ_TIMEOUT` (-11) to `kTimeout` and any other negative code to `kIoError`.
4. For a positive code, wrap `http.getStream()` in a `ByteReader` and call `on_response(ctx, code, reader)`. The price server sends no Content-Length under HTTP/1.0, so the body ends when the server closes the connection; when `http.getSize()` is not -1, stop after that many bytes.
5. `http.end()`, which closes the connection and frees the TLS session, on every path. Return `kTimeout` if the reader hit the deadline, `kIoError` if the connection closed with Content-Length bytes still owed, else `kOk`.

A reader for step 4, with the deadline taken from `request.timeout_ms`:

```cpp
class StreamReader final : public oa3::ByteReader {
public:
    StreamReader(NetworkClient& s, int size, uint32_t deadline_ms)
        : _s(s), _left(size), _deadline(deadline_ms) {}
    int read() override {
        char c;
        return readBytes(&c, 1) == 1 ? static_cast<unsigned char>(c) : -1;
    }
    size_t readBytes(char* buf, size_t len) override {
        size_t n = 0;
        while (n < len && _left != 0) {
            const int avail = _s.available();
            if (avail > 0) {
                size_t want = len - n;
                if (_left > 0 && want > static_cast<size_t>(_left)) want = _left;
                const int got = _s.read(reinterpret_cast<uint8_t*>(buf + n), want);
                if (got > 0) {
                    n += got;
                    if (_left > 0) _left -= got;
                }
            } else if (!_s.connected()) {
                cut_short = _left > 0;
                break;
            } else {
                delay(1);
            }
            if (static_cast<int32_t>(millis() - _deadline) >= 0) {
                timed_out = true;
                break;
            }
        }
        return n;
    }
    bool timed_out = false;
    bool cut_short = false;  // closed with Content-Length bytes still owed

private:
    NetworkClient& _s;
    int _left;  // -1: until the connection closes
    uint32_t _deadline;
};
```

## MqttSession

An MQTT 3.1.1 client the port passes to the `VenClient` constructor; it is needed only with `VenConfig::notifications = NotificationMode::kMqtt`. The client drives it from the task that runs `loop()`, so it needs no thread of its own, and holds one session at a time. Every call blocks at most its timeout. The contract:

- `connect(options, &session_present)` opens a session, ending any open one first. `options` is borrowed for the call: host, port and `tls` from the broker URI, `client_id` from `VenConfig::mqtt_client_id`, no username or password (the client supports ANONYMOUS brokers only), `verify` and `ca_pem` from `mqtt_verify` and `mqtt_ca_pem` (`nullptr`: the port's default roots), `keepalive_s`, `clean_session`, and `timeout_ms` for DNS, TCP, TLS and CONNACK together. On `kOk`, `*session_present` is the CONNACK flag.
- Resolve the broker host under the board's DNS lock (see "Name resolution" below) before connecting, and return `kDnsFailed` if that fails. The connect runs on the client's task, inside `loop()`.
- Map other failures onto `MqttResult`: `kConnectFailed`, `kTlsFailed`, `kTlsVerifyFailed` (the certificate or host name did not verify), `kRefused` for CONNACK 1-3, `kNotAuthorized` for 4-5, `kTimeout`, `kNetworkDown` when the port knows it has no network, else `kIoError`.
- `disconnect()` may be called at any time, connected or not. It sends DISCONNECT when connected, closes the connection and frees everything a previous `connect()` allocated, the TLS session included, before it returns, also after a failed `connect()` or a lost session. The broker keeps a session opened with `clean_session` false.
- `subscribe(filter, qos, timeout_ms)` and `unsubscribe(filter, timeout_ms)` return when the SUBACK or UNSUBACK arrives; a SUBACK of 0x80 is `kSubscribeFailed`. The client subscribes at QoS 1.
- `poll(max_wait_ms, on_message, ctx)` delivers every message already received, waiting up to `max_wait_ms` for the first (the client passes 0), and sends PINGREQ when keepalive requires it. Acknowledge each QoS 1 message after `on_message` returns. Return `kConnectionLost` when the session ended, before or during the call, after delivering what arrived before the loss.
- `MqttMessage` is valid only during `on_message`. Set `retained` from the PUBLISH when the library reports it. When a payload exceeds the port's buffer, deliver it with `payload` `nullptr`, `too_large` true and `len` the size on the wire when known; the client then fetches `/events`, unless the message is retained. A price-server notification is about 3.5 KB; 8 KB is a comfortable buffer.
- `connected()` is the port's view of the session; the client treats false as a loss.

The client calls `poll(0)` at the start of each `loop()` while subscribed, and `ms_until_due()` is then at most `VenConfig::mqtt_poll_ms` (100 by default), so a port that sleeps for `ms_until_due()` picks messages up promptly and keeps the session alive.

A broker sends its retained messages on every new subscription, which can be after the client's `/events` snapshot. The client applies a retained CREATE or UPDATE only as an update of a stored event with a newer `modificationDateTime`, and drops a CREATE or UPDATE of an event the snapshot lacks when its `modificationDateTime` is before the snapshot, which covers a port that cannot report `retained`.

### arduino-esp32 3.x: arduino-mqtt

Use 256dpi's arduino-mqtt (`MQTTClient`, 2.5.2 in esp32-firmware), which runs in the caller's task through `loop()`. ESP-IDF's esp-mqtt runs a task of its own, which the contract does not allow. Keep one `NetworkClientSecure net` and one `MQTTClient mqtt(8192, 512)` as members of the port: the constructor allocates the read and write buffers once, and the read buffer must hold the largest notification plus its topic and header.

| Contract | arduino-mqtt |
|---|---|
| `connect` | `net.setCACert(ca_pem)` (or the port's default root), `net.setHandshakeTimeout(seconds)`, `mqtt.begin(host, port, net)`, `mqtt.setKeepAlive(keepalive_s)`, `mqtt.setCleanSession(clean_session)`, `mqtt.setTimeout(timeout_ms)`, then `mqtt.connect(client_id)`. On false, `mqtt.returnCode()` 1-3 is `kRefused` and 4-5 `kNotAuthorized`; `mqtt.lastError()` `LWMQTT_NETWORK_FAILED_CONNECT` is `kTlsFailed` when `net.lastError(buf, len)` is below -1, else `kConnectFailed`; `LWMQTT_NETWORK_TIMEOUT` is `kTimeout`. On true, `*session_present = mqtt.sessionPresent()`. |
| `disconnect` | `mqtt.disconnect()`, then `net.stop()` always: `MQTTClient::disconnect()` returns early when it is no longer connected, and only `stop()` frees the TLS session. |
| `subscribe` | `mqtt.subscribe(filter, 1)`; on false, `LWMQTT_FAILED_SUBSCRIPTION` is `kSubscribeFailed`. A failed subscribe closes the connection. |
| `unsubscribe` | `mqtt.unsubscribe(filter)` |
| `poll` | Set `mqtt.ref = this` and `mqtt.onMessageAdvanced(cb)` once; `cb(client, topic, bytes, length)` forwards to the `on_message` and `ctx` stored for the current `poll()`. Call `mqtt.loop()` once; it calls `cb` before it sends the PUBACK. Return `kConnectionLost` when `loop()` returns false or `mqtt.connected()` is false. |
| `connected` | `mqtt.connected()` |

Two limits of this library:

- Its callback has no retained flag, so the port reports `retained` false and relies on the client's `modificationDateTime` check above.
- It cannot deliver an oversized message as `too_large`. By default a PUBLISH larger than the read buffer fails with `LWMQTT_BUFFER_TOO_SHORT` and `loop()` closes the connection; with `dropOverflow(true)` the packet is drained without its topic and without a PUBACK, and only `droppedMessages()` counts it. Either way the broker redelivers the unacknowledged QoS 1 message to the kept session on every resume, so size the read buffer so that no notification can exceed it.

### One TLS session at a time

On the classic ESP32 a TLS session costs 42-45 KB of internal RAM and a second one does not fit, so an MQTTS session and an HTTPS request cannot be open together. Set `VenConfig::exclusive_tls`: the client then

- fetches `/events` before connecting, and connects with `clean_session` false, so the broker queues QoS 1 notifications while the session is closed;
- calls `disconnect()` before every HTTPS request while a session may hold TLS, the hourly fallback poll included, and after a failed connect or subscribe, and resumes at the first `loop()` with no request due, so all pages of one fetch run before it reconnects;
- subscribes again on a resume that finds no session, without another fetch, and polls `/events` every `poll_period_s` instead of `notify_poll_period_s` until a resume finds the session kept. A first connect has no session, so the first fallback poll after it comes at `poll_period_s`.

The rule for the port: `disconnect()` must free the TLS session whatever state the connection is in, because the client's next call may be an HTTPS `get()`. `exclusive_tls` has no effect with a plain `mqtt://` broker. Leave it off on a target that fits two TLS sessions: measured with arduino-esp32 3.3.11, an ESP32-S3 (Waveshare ESP32-S3-POE-ETH) kept 76.9 KB of internal RAM and an ESP32-P4 (Olimex ESP32-P4-DevKit) 140.9 KB at their lowest with an MQTTS session open through every HTTPS poll, a second session costing 44-48 KB on both.

## Plaintext

OpenADR 3.1.0 requires HTTPS "regardless of its intended operating environment" (Definition.md, HTTPS/TLS) and MQTTS for its broker (Definition.md, MQTT), so TLS is the default and always available. `VenConfig::allow_plaintext` (default false) is a documented deviation for a VTN on a LAN or a test bench that offers no TLS:

- false: `start()` refuses a base URL that is not `https://` and an `mqtt_uri` that is not `mqtts://` or `ssl://`. With either setting, a broker URI is used only with the scheme `mqtts`, `ssl`, `mqtt` or `tcp`. When `/notifiers` offers only `mqtt://` or `tcp://` URIs, notifications are `UNAVAILABLE` (logged as "only plaintext URIs, and allow_plaintext is off") and polling continues.
- true: `start()` also accepts an `http://` base URL and a plain `mqtt_uri`, and a plain `/notifiers` URI is used only when no TLS URI is offered. The client logs a warning that the connection is unencrypted and not conformant at `start()` for `http://`, and the first time it selects a plain broker after `start()`.

Schemes are case-insensitive. With an `http://` base URL and a plain broker no connection needs TLS, so leave `exclusive_tls` off.

On arduino-esp32 3.x, the port picks the client by scheme: a `NetworkClient` instead of `NetworkClientSecure` for an `http://` URL in the transport, and arduino-mqtt over a plain `NetworkClient` when `MqttConnectOptions::tls` is false.

## Clock

`now(DateTime* out)` returns the UTC instant, or false while the wall clock is unset; the client reports `WAITING_FOR_CLOCK` and makes no request until it is true, because every fetch window starts at `now`. On the ESP32, return false until SNTP has synchronized (`esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED` once, or `time()` past a floor such as 2020-01-01, as `PosixClock` does), then fill `out` with `oa3::from_unix(tv.tv_sec, tv.tv_usec * 1000, out)` from `gettimeofday()`. Times are UTC throughout; the library has no zone handling.

`monotonic_ms()` is `millis()`: unaffected by SNTP steps, and the client compares differences, so the wrap at 2^32 ms is harmless. When the wall clock moves more than 5 s away from the monotonic one between two `loop()` calls (an SNTP step), the client fetches again so its window covers the new now.

## Log

`VenConfig::log` is a `LogSink`: a function, its `ctx` and a `max_level`; lines above `max_level` are never formatted. Each call gets one NUL-terminated line of at most `OA3_LOG_LINE_MAX` (159) characters with no newline, valid only during the call, on the task that runs `loop()`. Forward it to `Serial` or `ESP_LOGx`; keep it short, because it runs inside the client. A value-initialized sink logs nothing.

## Storage

`VenStorage` is the client's working memory: two `Event` arrays (`events` and `scratch`, `capacity` elements each; a fetch fills `scratch` and swaps on success, so a failure keeps the last good events) and the `JsonArena` the parser uses for one top-level object at a time. An `Event` is about 5.4 KB with the default capacities, so `OA3_MAX_EVENTS=2` costs about 21.6 KB in arrays plus the arena (16 KB by default on a 32-bit target). `StaticVenStorage<>` puts all of it in `.bss`; on the `ci/esp32` build with `OA3_MAX_EVENTS=2` it is 37,984 bytes.

MQTT notifications also need a `NotificationStorage` in `VenStorage::notifications`: the `Notification` a message is parsed into (an `Event` and a `Program`), the `/notifiers` and topics responses and the topic lists, 10,088 bytes on the `ci/esp32` build. A client with notifications off needs none of it.

On a board with PSRAM, allocate it there once at init and never free it:

```cpp
#include <esp_heap_caps.h>
#include <new>

const size_t n = OA3_MAX_EVENTS;
const size_t arena_size = OA3_JSON_ARENA_SIZE;
void* mem = heap_caps_aligned_alloc(alignof(oa3::Event), 2 * n * sizeof(oa3::Event), MALLOC_CAP_SPIRAM);
oa3::Event* events = static_cast<oa3::Event*>(mem);
for (size_t i = 0; i < 2 * n; ++i) new (&events[i]) oa3::Event();
void* arena_buf = heap_caps_malloc(arena_size, MALLOC_CAP_SPIRAM);
static oa3::JsonArena arena(arena_buf, arena_size);
void* notes_mem = heap_caps_aligned_alloc(alignof(oa3::NotificationStorage), sizeof(oa3::NotificationStorage), MALLOC_CAP_SPIRAM);
auto* notes = new (notes_mem) oa3::NotificationStorage();
const oa3::VenStorage storage{events, events + n, n, &arena, notes};
```

Without notifications, leave the last member out. Without PSRAM, use `StaticVenStorage<>` (and a static `NotificationStorage`) and leave the internal heap to the TLS session. After a fetch, `arena.high_water()` shows how much of the arena one event needed; shrink `OA3_JSON_ARENA_SIZE` toward it, with margin, if RAM is short.

## Sizing for 24 h

For one price-server program (one event per day of 24 hourly intervals, 23 or 25 on DST days, about 10 KB per `/events` response):

| Setting | Value |
|---|---|
| `VenConfig::horizon` | `PT24H` (the default) |
| `-DOA3_MAX_EVENTS` | 2: a 24 h window overlaps at most two daily events |
| `OA3_MAX_INTERVALS` | 25 (the default) |
| `OA3_JSON_ARENA_SIZE` | 16 KB (the 32-bit default) |
| `VenConfig::poll_period_s` | 900 (the default); the price changes at interval boundaries without a request |

A device that needs a longer view raises both: a 72 h window needs `OA3_MAX_EVENTS` 4. When more events overlap the window than fit, the earliest are kept and the status is `TRUNCATED`.

## Task

Run the client on a FreeRTOS task of its own with at least 6 KB of stack; 8 KB is the recommendation. In the M2 spike the loop task bottomed at 2.6 KB free of 8 KB during GET and parse. The parse runs inside the transport's `get()`, below `HTTPClient` and the TLS read: on xtensa the events-response path takes about 2.0 KB and ArduinoJson adds about 112 bytes per nesting level (about 1.1 KB at the default limit of 10). Check `uxTaskGetStackHighWaterMark()` on the device after the first fetch.

`VenClient` is not thread-safe, and every callback runs inside `loop()` on this task. Another task asks for an early fetch by sending a message the OA3 task turns into `request_poll()`; the price goes out by copying `PriceInfo` into a queue from `on_price_change`. A `loop()` that makes a request blocks for up to `http_timeout_ms`; otherwise it returns at once. Sleep for `ms_until_due()`, capped so that queued messages are seen.

On the classic ESP32, nothing else may hold a TLS session while the client fetches. The client's own MQTTS session follows this rule with `VenConfig::exclusive_tls`; anything else that holds one must close it before the request, or be scheduled between requests.

### Name resolution

`HTTPClient::GET()` resolves the host from inside `connect()`, on the client's task. In arduino-esp32 3.3.11 the first `Network.hostByName()` after an address change clears the DNS cache without the lwIP core lock, and asserts ("Required to lock TCPIP core functionality!") if another lookup is pending, for example SNTP's or another task's MQTT or OTA connect. Serialize every name lookup on the board behind one lock, wait for pending SNTP lookups, and resolve the VTN host under that lock before `GET()`. esp32-firmware does this with `network_dns_prime(host)`. The same applies to the broker host: `MQTTClient::connect()` resolves it inside `NetworkClientSecure::connect()` on the client's task, so resolve it under the lock before `mqtt.connect()`.

## Minimal wiring

```cpp
#include <oa3/client/ven_client.h>

static ArduinoHttpTransport http;  // the HttpTransport above
static SntpClock clock_;           // the Clock above
static oa3::StaticVenStorage<> storage;
static oa3::VenClient client(http, clock_, storage.storage());

static void on_price(void*, const oa3::PriceInfo& p) {
    if (p.active) Serial.printf("price %s %s/%s\n", p.text, p.currency, p.units);
    else Serial.println("no price in force");
}

static void on_log(void*, oa3::LogLevel level, const char* line) {
    Serial.printf("oa3 %s %s\n", oa3::to_string(level), line);
}

static void oa3_task(void*) {
    oa3::VenConfig c{};
    c.base_url = "https://price.grid-coordination.energy/openadr3/3.1.0";
    c.program_name = "EELEC-012041131";
    c.on_price_change = on_price;
    c.log = oa3::LogSink{on_log, nullptr, oa3::LogLevel::kInfo};
    if (!client.start(c)) vTaskDelete(nullptr);
    for (;;) {
        client.loop();
        uint32_t wait = client.ms_until_due();
        if (wait > 1000) wait = 1000;
        vTaskDelay(pdMS_TO_TICKS(wait) + 1);
    }
}

void setup() {
    // Wi-Fi or Ethernet up, configTime(0, 0, "pool.ntp.org"), CA loaded into the transport.
    xTaskCreatePinnedToCore(oa3_task, "oa3", 8192, nullptr, 1, nullptr, 1);
}
```

`start()` copies the configuration's strings and returns false for an invalid one: no base URL or program, a base URL that is not `https://` (or `http://` with `allow_plaintext`), a URL that cannot fit `OA3_URL_MAX` with the longest id and targets, a `program_id` that is not a spec objectID, a poll or backoff period above 2,000,000 s, or `stale_after_polls * poll_period_s` at or above 2^31 ms (with the default `stale_after_polls` of 3, a poll period above 715,827 s). With MQTT notifications it also requires an `MqttSession`, `VenStorage::notifications` and a `mqtt_client_id` of 1 to `OA3_MQTT_CLIENT_ID_MAX` (63) characters, `poll_period_s <= notify_poll_period_s <= 2,000,000`, `stale_after_polls * notify_poll_period_s` below 2^31 ms, and an `mqtt_uri`, when given, that parses and is `mqtts://` or `ssl://` unless `allow_plaintext`. A port that takes these from user input should refuse names and ids its own buffers would truncate, since `start()` cannot tell a truncated value from a real one.
