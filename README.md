# cpp-oa3

A portable C++ library for [OpenADR 3.1.0](https://github.com/grid-coordination/openadr3-specification), written for microcontrollers first. The first target is a VEN on the ESP32 that receives prices from an OpenADR 3 VTN; the same core builds on POSIX hosts.

The library is the core only. A port supplies HTTP, MQTT, mDNS and the clock; the ESP32 port lives in [esp32-firmware](https://github.com/electrification-bus/esp32-firmware).

## Status

Pre-release. The repository has the build, test and CI skeleton; the entity model and the VEN client follow.

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

## Related projects

- [python-oa3](https://github.com/grid-coordination/python-oa3) and [python-oa3-client](https://github.com/grid-coordination/python-oa3-client): the Python library and client
- [clj-oa3](https://github.com/grid-coordination/clj-oa3) and [clj-oa3-client](https://github.com/grid-coordination/clj-oa3-client): the Clojure library and client
- [cpp-sdk](https://github.com/electrification-bus/cpp-sdk): the eBus C++ core, whose conventions this library follows

## License

MIT. See [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
