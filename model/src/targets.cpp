#include <oa3/model/targets.h>

#include <string.h>

namespace oa3 {

namespace {

size_t clamped_count(const TargetList& t) {
    return t.count < OA3_MAX_TARGETS ? t.count : OA3_MAX_TARGETS;
}

}  // namespace

bool targets_contain(const TargetList& targets, const char* target) {
    if (target == nullptr) return false;
    const size_t n = clamped_count(targets);
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(targets.items[i], target) == 0) return true;
    }
    return false;
}

bool targets_match(const TargetList& object_targets, const char* const* ven_targets,
                   size_t ven_target_count) {
    if (object_targets.count == 0 && object_targets.non_string_count == 0) return true;
    if (ven_targets == nullptr) return false;
    for (size_t j = 0; j < ven_target_count; ++j) {
        if (targets_contain(object_targets, ven_targets[j])) return true;
    }
    return false;
}

bool targets_match(const TargetList& object_targets, const TargetList& ven_targets) {
    if (object_targets.count == 0 && object_targets.non_string_count == 0) return true;
    const size_t n = clamped_count(ven_targets);
    for (size_t j = 0; j < n; ++j) {
        if (targets_contain(object_targets, ven_targets.items[j])) return true;
    }
    return false;
}

}  // namespace oa3
