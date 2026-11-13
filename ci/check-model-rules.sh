#!/bin/sh
# Fails if model/ or client/ uses heap allocation, STL containers or strings, lambdas, or a platform
# header (CONTRIBUTING.md). Line comments are stripped first; the model has no block comments.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)

banned='(^|[^_[:alnum:]])(new|delete)[[:space:]]+[_[:alpha:]]'
banned="$banned"'|(^|[^_[:alnum:]])(malloc|calloc|realloc|free|strdup|alloca)[[:space:]]*\('
banned="$banned"'|std::(string|wstring|vector|list|deque|map|set|unordered_|function|unique_ptr|shared_ptr|make_)'
banned="$banned"'|#[[:space:]]*include[[:space:]]*[<"](Arduino\.h|WString\.h|esp_|freertos/|FreeRTOS|sys/|unistd\.h|pthread\.h|string>|vector>|map>|functional>|memory>)'
banned="$banned"'|(^|[^_[:alnum:]])String[[:space:]]*[_[:alpha:](]'
banned="$banned"'|\[[^]]*\][[:space:]]*\([^)]*\)[[:space:]]*(mutable[[:space:]]*)?(->[^{]*)?\{'
banned="$banned"'|/\*'

rc=0
for f in $(find "$root/model/include" "$root/model/src" "$root/client/include" "$root/client/src" -type f \( -name '*.h' -o -name '*.cpp' \) | sort); do
    hits=$(sed 's://.*$::' "$f" | grep -nE "$banned" || true)
    if [ -n "$hits" ]; then
        printf '%s\n' "$hits" | sed "s|^|${f#"$root"/}:|"
        rc=1
    fi
done
exit "$rc"
