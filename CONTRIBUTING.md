# Contributing to cpp-oa3

`cpp-oa3` is a portable C++ core for [OpenADR 3.1.0](https://github.com/grid-coordination/openadr3-specification) VENs and, later, small VTNs. Platform ports supply HTTP, MQTT, mDNS and the clock; [esp32-firmware](https://github.com/electrification-bus/esp32-firmware) is the ESP32 port.

## Issues and pull requests

Use [Issues](https://github.com/grid-coordination/cpp-oa3/issues) for bug reports (port, platform, compiler, VTN, and ideally a failing test or log) and concrete feature requests. For small fixes, open a pull request directly. For substantive changes (new public API, changes to the port interfaces, new ports, changes to on-the-wire behavior), open an Issue first so we can agree on scope.

## Rules for the code

- **The specification is the source of truth.** When the code and the [OpenADR 3.1.0 specification](https://github.com/grid-coordination/openadr3-specification) disagree, fix the code. Tolerating a known VTN quirk is fine; name the VTN and the quirk in a comment. Update `.oa3-spec.json` when you sync to a newer specification commit.
- **House style for the core.** It runs on microcontrollers, so:
  - No dynamic allocation in steady state. Use fixed-size buffers and static or caller-owned storage.
  - No STL, no Arduino `String`, no lambdas. Use `snprintf` over string concatenation, and the C function-pointer plus `void*` context idiom for callbacks. Members are underscore-prefixed.
  - No Arduino, ESP-IDF, FreeRTOS or OS header. Anything platform-specific goes through the port interfaces. CI builds the core on a host with only its own include directories and ArduinoJson.
  - C++17, GCC or Clang, warnings as errors in CI.
- **Run the tests.** `cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure`. To add a suite, create `test/test_<name>/test_<name>.cpp` with its own `main()`; `test/CMakeLists.txt` picks it up. CI also builds the library for the ESP32 (`pio run -d ci/esp32`).
- **Respect upstream licenses: reference, don't copy.** This library is MIT. Do not paste source from copyleft projects into it.
- **Keep comments to a minimum.** Reserve comments for the non-obvious why (a spec corner, a platform quirk).
- **Don't hard-wrap Markdown prose.** Write each paragraph as a single line.

## LLM coding assistants

Keep personal assistant configuration out of the repo: `AGENTS.md`, `CLAUDE.md`, `CLAUDE.local.md`, `.claude/`, `.cursor/` and the like are gitignored.

## Maintenance posture

`cpp-oa3` is an active alpha project. Updates and maintenance, including responses to issues, happen as time and resources permit.
