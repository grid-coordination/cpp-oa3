# POSIX port

`oa3_client` on Linux and macOS: an `HttpTransport` over libcurl, the clock and log bindings, the `oa3-ven-demo` program, and their tests. CMake only; PlatformIO never builds this directory, because the root `library.json` limits a PlatformIO build to `model/src/` and `client/src/`.

| Path | Contents |
|---|---|
| `include/oa3/posix/curl_transport.h`, `src/curl_transport.cpp` | `CurlHttpTransport`: one libcurl easy handle and one connection per `get()`, the body collected in a caller-owned buffer |
| `include/oa3/posix/posix_clock.h`, `src/posix_clock.cpp` | `PosixClock`: `CLOCK_REALTIME` (valid from 2020-01-01) and `CLOCK_MONOTONIC` |
| `include/oa3/posix/stderr_log.h`, `src/stderr_log.cpp` | `stderr_log`: a `LogFn` that prefixes each line with the UTC time in milliseconds |
| `examples/oa3-ven-demo/main.cpp` | `oa3-ven-demo` |
| `test/` | `test_posix_curl_transport` (a loopback HTTP/1.1 server), `test_posix_clock` |

## Build

Needs libcurl with its development headers (Debian/Ubuntu: `apt-get install libcurl4-openssl-dev`; macOS ships one).

```bash
cmake -S ports/posix -B build-posix
cmake --build build-posix -j
ctest --test-dir build-posix --output-on-failure
```

To build offline, pass `-DFETCHCONTENT_SOURCE_DIR_ARDUINOJSON=<checkout>`. `-DOA3_POSIX_WERROR=ON` makes warnings in the port, the demo and the tests errors, as CI does. `-DOA3_POSIX_TESTS=OFF` skips the tests; `-DOA3_POSIX_LIVE_TESTS=ON` registers `oa3_posix_live`, which runs `oa3-ven-demo --once` against the price server.

## Run

```bash
build-posix/oa3-ven-demo --program EELEC-012041131 --poll 60 --verbose
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
| `--insecure` | off; skips TLS peer and host verification |
| `--verbose` | off; logs at debug level, including each request URL |
| `--once` | off; exits 0 at the first price report, 1 at the first failure |

Price changes and status changes print on stdout, the library's log on stderr. Ctrl-C or SIGTERM stops it with exit 0. The demo's response buffer is 256 KB.
