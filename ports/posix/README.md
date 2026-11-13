# POSIX port

`oa3_client` on Linux and macOS: an `HttpTransport` over libcurl, an `MqttSession` over Paho MQTT C with OpenSSL, the clock and log bindings, the `oa3-ven-demo` program, and their tests. CMake only; PlatformIO never builds this directory, because the root `library.json` limits a PlatformIO build to `model/src/` and `client/src/`.

| Path | Contents |
|---|---|
| `include/oa3/posix/curl_transport.h`, `src/curl_transport.cpp` | `CurlHttpTransport`: one libcurl easy handle and one connection per `get()`, the body collected in a caller-owned buffer |
| `include/oa3/posix/paho_mqtt_session.h`, `src/paho_mqtt_session.cpp` | `PahoMqttSession`: the synchronous Paho client with no callbacks, so no thread; each payload copied into a caller-owned buffer |
| `include/oa3/posix/posix_clock.h`, `src/posix_clock.cpp` | `PosixClock`: `CLOCK_REALTIME` (valid from 2020-01-01) and `CLOCK_MONOTONIC` |
| `include/oa3/posix/stderr_log.h`, `src/stderr_log.cpp` | `stderr_log`: a `LogFn` that prefixes each line with the UTC time in milliseconds |
| `examples/oa3-ven-demo/main.cpp` | `oa3-ven-demo` |
| `test/` | `test_posix_curl_transport` (a loopback HTTP/1.1 server), `test_posix_clock`, `test_posix_paho_loopback` (a mosquitto broker on a loopback port), `test_posix_paho_mqtt_session` (live) |

## Build

Needs libcurl and OpenSSL with their development headers (Debian/Ubuntu: `apt-get install libcurl4-openssl-dev libssl-dev`; macOS ships libcurl, and OpenSSL comes from Homebrew `openssl@3`, found through `brew --prefix` unless `OPENSSL_ROOT_DIR` is set). Paho MQTT C v1.3.16 is fetched and built static with SSL; `-DOA3_POSIX_PAHO=system` uses an installed `eclipse-paho-mqtt-c` package with `paho-mqtt3cs` instead, and the default `auto` uses one when present. `test_posix_paho_loopback` is registered only when `mosquitto` is installed (`apt-get install mosquitto`, `brew install mosquitto`).

```bash
cmake -S ports/posix -B build-posix
cmake --build build-posix -j
ctest --test-dir build-posix --output-on-failure
```

To build offline, pass `-DFETCHCONTENT_SOURCE_DIR_ARDUINOJSON=<checkout>`. `-DOA3_POSIX_WERROR=ON` makes warnings in the port, the demo and the tests errors, as CI does. `-DOA3_POSIX_TESTS=OFF` skips the tests; `-DOA3_POSIX_LIVE_TESTS=ON` registers the tests labeled `live`, against the price server and its broker: `oa3_posix_live` (`oa3-ven-demo --once`), `oa3_posix_live_mqtt` (with `--notifications mqtt`), `oa3_posix_live_mqtt_exclusive` (also `--exclusive-tls`), and `test_posix_paho_mqtt_session` (MQTTS on 8883 and MQTT on 1883). `ctest -LE live` skips them.

## Run

```bash
build-posix/oa3-ven-demo --program EELEC-012041131 --poll 60 --verbose
build-posix/oa3-ven-demo --program EELEC-012041131 --notifications mqtt --verbose
```

| Option | Default |
|---|---|
| `--base-url URL` | `https://price.grid-coordination.energy/openadr3/3.1.0` |
| `--program NAME` | `EELEC-012041131` |
| `--program-id ID` | none; with `--program`, the name must match |
| `--target T` | none; repeatable, up to `OA3_MAX_TARGETS` |
| `--poll SECONDS` | 900 |
| `--horizon ISO8601` | `PT24H` |
| `--cacert PATH` | the system CA store |
| `--insecure` | off; skips TLS peer and host verification, for HTTPS and MQTTS |
| `--allow-plaintext` | off; admits an `http://` base URL, and `mqtt://` and `tcp://` when no TLS URI is offered ([`doc/porting.md`](../../doc/porting.md#plaintext)) |
| `--notifications off\|mqtt` | `off`; `mqtt` follows the program's notifications from the VTN's broker |
| `--mqtt-client-id ID` | `oa3-ven-demo-<pid>` |
| `--mqtt-uri URI` | none; used instead of the URIs `/notifiers` advertises |
| `--mqtt-cacert PATH` | OpenSSL's default CA paths |
| `--exclusive-tls` | off; closes the MQTTS session around each HTTPS request, as on the classic ESP32 |
| `--notify-poll SECONDS` | 3600; the poll period while subscribed |
| `--verbose` | off; logs at debug level, including each request URL |
| `--once` | off; exits 0 at the first price report (with `mqtt`, at the first connected report after a price), 1 at the first failure |

Price changes and status changes print on stdout, the library's log on stderr. With `mqtt`, each status line also shows the notification state and the count of notifications applied. Ctrl-C or SIGTERM stops it with exit 0. The demo's response buffer is 256 KB and its MQTT payload buffer 8 KB.
