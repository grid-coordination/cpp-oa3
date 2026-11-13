#pragma once

#include <stdint.h>

#include <oa3/client/limits.h>

namespace oa3 {

enum class LogLevel : uint8_t { kError, kWarn, kInfo, kDebug };

const char* to_string(LogLevel level);

// One line, NUL-terminated, without a newline, at most OA3_LOG_LINE_MAX characters. Valid
// only during the call.
typedef void (*LogFn)(void* ctx, LogLevel level, const char* line);

// A value-initialized sink logs nothing.
struct LogSink {
    LogFn fn;
    void* ctx;
    LogLevel max_level;  // lines above it are not formatted
};

void log_printf(const LogSink& sink, LogLevel level, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

}  // namespace oa3
