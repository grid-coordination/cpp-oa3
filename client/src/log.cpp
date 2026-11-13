#include <oa3/client/log.h>

#include <stdarg.h>
#include <stdio.h>

namespace oa3 {

const char* to_string(LogLevel level) {
    switch (level) {
        case LogLevel::kError:
            return "ERROR";
        case LogLevel::kWarn:
            return "WARN";
        case LogLevel::kInfo:
            return "INFO";
        case LogLevel::kDebug:
            return "DEBUG";
    }
    return "UNKNOWN";
}

void log_printf(const LogSink& sink, LogLevel level, const char* fmt, ...) {
    if (sink.fn == nullptr || level > sink.max_level) {
        return;
    }
    char line[OA3_LOG_LINE_MAX + 1];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    sink.fn(sink.ctx, level, line);
}

}  // namespace oa3
