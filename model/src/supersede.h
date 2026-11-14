#pragma once

#include <stddef.h>

#include <oa3/model/intervals.h>

namespace oa3 {
namespace detail {

// superseded() with the extra cover from any source, such as an event not yet copied out of
// its JSON: extra_extend acts as extend_cover() on extra_source.
typedef bool (*ExtendFn)(const void* source, const char* payload_type, const DateTime& limit,
                         DateTime* t);

bool superseded_core(const Event& target, size_t target_order, const Event* events, size_t count,
                     const Precedence* extra, ExtendFn extra_extend, const void* extra_source,
                     const TimeInterval& window, const char* payload_type);

}  // namespace detail
}  // namespace oa3
