# Third-party licenses

Nothing is vendored. CMake fetches each library below at configure time, pinned by release and SHA-256; PlatformIO resolves ArduinoJson from `library.json`.

| Library | Version | Used by | Linked into | License |
|---------|---------|---------|-------------|---------|
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | 7.4.3 | The library (`CMakeLists.txt`, `library.json`) | Every program that uses the library (header-only) | MIT |
| [Unity](https://github.com/ThrowTheSwitch/Unity) | 2.6.1 | The unit tests (`test/CMakeLists.txt`) | The test executables only | MIT |

## Obligations when distributing binaries

Ship ArduinoJson's license text and copyright notice with any binary built from the library.
