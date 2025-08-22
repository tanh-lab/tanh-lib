# pragma once

namespace thl {

namespace detail {
    // Split a path into the first component and the rest
    // Example: "audio.effects.reverb" -> {"audio", "effects.reverb"}
    inline std::pair<std::string_view, std::string_view> split_path(std::string_view path) {
        size_t pos = path.find('.');
        if (pos == std::string::npos) {
            return {path, ""};
        }
        return {path.substr(0, pos), path.substr(pos + 1)};
    }
    
    // Join parent and child paths
    // Example: join_path("audio", "reverb") -> "audio.reverb"
    // NOTE: result string must have sufficient capacity pre-allocated to avoid reallocations
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
        if (parent.empty()) return child.size();
        if (child.empty()) return parent.size();
        return parent.size() + 1 + child.size(); // +1 for dot
    }

} // namespace detail

} // namespace thl
