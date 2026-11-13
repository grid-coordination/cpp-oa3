# Third-party licenses

Nothing is vendored. CMake fetches each library below at configure time, pinned by release and SHA-256; PlatformIO resolves ArduinoJson from `library.json`.

| Library | Version | Used by | Linked into | License |
|---------|---------|---------|-------------|---------|
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | 7.4.3 | The library (`CMakeLists.txt`, `library.json`) | Every program that uses the library (header-only) | MIT |
| [Unity](https://github.com/ThrowTheSwitch/Unity) | 2.6.1 | The unit tests (`test/CMakeLists.txt`) | The test executables only | MIT |
| [Paho MQTT C](https://github.com/eclipse-paho/paho.mqtt.c) | 1.3.16 | The POSIX port (`ports/posix/CMakeLists.txt`), unless an installed package is used | `oa3-ven-demo` and the port's tests (static, with OpenSSL) | EPL-2.0 or EDL-1.0 (BSD-3-Clause), at the distributor's choice |

## Obligations when distributing binaries

Ship ArduinoJson's license text and copyright notice with any binary built from the library. A binary that links Paho MQTT C, such as `oa3-ven-demo`, also ships Paho's copyright notice and the license chosen for it; under EDL-1.0 that is the license text and its disclaimer.
