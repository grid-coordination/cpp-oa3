#pragma once

#include <oa3/client/log.h>

namespace oa3 {
namespace posix {

// A LogFn writing "<UTC time> oa3 <LEVEL> <line>" to stderr. ctx is unused.
void stderr_log(void* ctx, LogLevel level, const char* line);

}  // namespace posix
}  // namespace oa3
