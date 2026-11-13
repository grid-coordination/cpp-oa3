# cpp-oa3

A portable C++ library for [OpenADR 3.1.0](https://github.com/grid-coordination/openadr3-specification), written for microcontrollers first. The first target is a VEN on the ESP32 that receives prices from an OpenADR 3 VTN; the same core builds on POSIX hosts.

The library is the core only. A port supplies HTTP, MQTT, mDNS and the clock; the ESP32 port lives in [esp32-firmware](https://github.com/electrification-bus/esp32-firmware).

## Status

Pre-release. `oa3_model` (time, entities, streaming parse, intervals and prices) and `oa3_client` (`VenClient`, a VEN that polls and can follow MQTT notifications) are in place, with a POSIX port. The ESP32 port is in progress in esp32-firmware; [`doc/porting.md`](doc/porting.md) describes what a port supplies.

## Building on a host

Needs CMake 3.18 or later and a C++17 compiler. CMake fetches ArduinoJson and Unity, pinned by release and SHA-256.

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Using with PlatformIO

`library.json` describes the library to PlatformIO for any framework. `ci/esp32` is a minimal project that builds it for the Olimex ESP32-POE-ISO on the platform esp32-firmware uses:

```bash
pio run -d ci/esp32
```

## POSIX demo

`ports/posix` builds the library with a libcurl transport, a Paho MQTT C session and `oa3-ven-demo`, which follows one program on a VTN and prints the price in force. It needs libcurl and OpenSSL (on macOS, Homebrew `openssl@3`); CMake fetches Paho MQTT C ([`ports/posix/README.md`](ports/posix/README.md)).

```bash
cmake -S ports/posix -B build-posix
cmake --build build-posix -j
build-posix/oa3-ven-demo --program EELEC-012041131 --poll 60 --verbose
build-posix/oa3-ven-demo --program EELEC-012041131 --notifications mqtt --verbose
```

The default VTN is the price server, `https://price.grid-coordination.energy/openadr3/3.1.0`; with `--notifications mqtt` the demo also subscribes to the program's notifications on the broker the VTN advertises. Each price change prints a line such as `2026-11-13T02:00:00Z..2026-11-13T03:00:00Z  0.108155 USD/KWH  next 2026-11-13T03:00:00Z`; status changes print on stdout and the library's log on stderr. Ctrl-C stops it.

## Related projects

- [python-oa3](https://github.com/grid-coordination/python-oa3) and [python-oa3-client](https://github.com/grid-coordination/python-oa3-client): the Python library and client
- [clj-oa3](https://github.com/grid-coordination/clj-oa3) and [clj-oa3-client](https://github.com/grid-coordination/clj-oa3-client): the Clojure library and client
- [cpp-sdk](https://github.com/electrification-bus/cpp-sdk): the eBus C++ core, whose conventions this library follows

## License

MIT. See [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
