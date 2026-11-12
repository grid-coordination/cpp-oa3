#pragma once

#include <stddef.h>

#include <oa3/model/entities.h>

namespace oa3 {

// An object without targets is open to every VEN; one with targets matches when it shares
// at least one target with the VEN (Definition.md:654-680; openleadr-rs event.rs:314-319).
// Targets compare exactly, case included.
bool targets_match(const TargetList& object_targets, const char* const* ven_targets,
                   size_t ven_target_count);
bool targets_match(const TargetList& object_targets, const TargetList& ven_targets);

bool targets_contain(const TargetList& targets, const char* target);

}  // namespace oa3
