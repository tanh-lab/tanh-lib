#pragma once

#include <tanh/core/AbiNamespace.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace thl::inline THL_ABI_NAMESPACE::detail {
// Split a path into the first component and the rest
// Example: "audio.effects.reverb" -> {"audio", "effects.reverb"}
inline std::pair<std::string_view, std::string_view> split_path(std::string_view path) {
    const size_t pos = path.find('.');
    if (pos == std::string::npos) { return {path, ""}; }
    return {path.substr(0, pos), path.substr(pos + 1)};
}

// Join parent and child paths
// Example: join_path("audio", "reverb") -> "audio.reverb"
// NOTE: result string must have sufficient capacity pre-allocated to avoid
// reallocations
inline void join_path(std::string_view parent, std::string_view child, std::string& result) {
    result.clear();
    if (parent.empty()) {
        result.assign(child.data(), child.size());
        return;
    }
    if (child.empty()) {
        result.assign(parent.data(), parent.size());
        return;
    }
    // No reserve() call - caller must ensure sufficient capacity
    result.assign(parent.data(), parent.size());
    result += '.';
    result.append(child.data(), child.size());
}

constexpr size_t join_path_size(std::string_view parent, std::string_view child) {
    if (parent.empty()) { return child.size(); }
    if (child.empty()) { return parent.size(); }
    return parent.size() + 1 + child.size();  // +1 for dot
}

// True when `key` is the group `prefix` or lies below it: "engine1" holds "engine1" and
// "engine1.volume", not "engine10.volume". A prefix that ends with a dot selects every
// key that starts with it; an empty one selects all.
constexpr bool in_group(std::string_view key, std::string_view prefix) {
    if (prefix.empty()) { return true; }
    if (!key.starts_with(prefix)) { return false; }
    return prefix.back() == '.' || key.size() == prefix.size() || key[prefix.size()] == '.';
}

}  // namespace thl::inline THL_ABI_NAMESPACE::detail
