#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <span>
#include <vector>

#include <tanh/modulation/ResolvedTarget.h>
#include <tanh/state/Parameter.h>
#include <tanh/utils/RealtimeSanitizer.h>

namespace thl::modulation {

// SmartHandle wraps a ParameterHandle<float> and adds per-sample modulation
// buffer support. Holds a stable pointer to a ResolvedTarget in the matrix's
// unordered_map — no registration or rewiring needed.
//
// - Unmodulated: m_target is nullptr or buffer empty → reads State's
//   AtomicCacheEntry directly (~1ns)
// - Modulated: reads base + modulation_buffer[offset]
class SmartHandle {
public:
    SmartHandle() = default;

    SmartHandle(thl::ParameterHandle<float> handle, ResolvedTarget* target)
        : m_handle(handle), m_target(target) {}

    // Read the parameter value at a given sample offset.
    // If a modulation target is attached, returns base + modulation.
    float load(uint32_t modulation_offset = 0) const TANH_NONBLOCKING_FUNCTION {
        float base = m_handle.load();
        if (m_target && modulation_offset < m_target->modulation_buffer.size()) {
            return base + m_target->modulation_buffer[modulation_offset];
        }
        return base;
    }

    // Access the change points for this parameter's modulation target.
    // Returns nullptr if unmodulated.
    const std::vector<uint32_t>* change_points() const TANH_NONBLOCKING_FUNCTION {
        return m_target ? &m_target->change_points : nullptr;
    }

    thl::ParameterHandle<float> raw_handle() const TANH_NONBLOCKING_FUNCTION { return m_handle; }
    bool is_valid() const TANH_NONBLOCKING_FUNCTION { return m_handle.is_valid(); }

    size_t get_buffer_size() TANH_NONBLOCKING_FUNCTION {
        if (m_target) return m_target->modulation_buffer.size();
        else return 0;
    }

private:
    thl::ParameterHandle<float> m_handle;
    ResolvedTarget* m_target = nullptr;
};

// Free utility: collect change points from a span of SmartHandles.
// Writes a sorted, deduplicated list of change points into target_buffer.
//
// RT-SAFETY CONTRACT (must hold to avoid allocation on the audio thread):
//   target_buffer.capacity() must be >= the block size used when the
//   ModulationMatrix was prepared. Change points are indices within a block,
//   so their count is bounded by block_size. The caller is responsible for
//   reserving the buffer once at prepare() time, e.g.:
//     m_change_points.reserve(samples_per_block);
//
// std::sort and the linear dedup search are both allocation-free. The only
// allocation risk is push_back exceeding capacity — the assert below catches
// that at debug time.
inline void collect_change_points(
    std::span<const SmartHandle> handles, std::vector<uint32_t>& target_buffer) TANH_NONBLOCKING_FUNCTION {
    target_buffer.clear();
    for (auto& h : handles) {
        if (auto* cp = h.change_points()) {
            for (uint32_t v : *cp) {
                bool cb_in_target_buffer = false;
                for (uint32_t t : target_buffer) {
                    if (v == t) {
                        cb_in_target_buffer = true;
                        break;
                    }
                }
                if (!cb_in_target_buffer) {
                    assert(target_buffer.size() < target_buffer.capacity() &&
                           "collect_change_points: target_buffer capacity exceeded — "
                           "reserve at least samples_per_block before calling on the audio thread");
                    target_buffer.push_back(v);
                }
            }
        }
    }
    std::sort(target_buffer.begin(), target_buffer.end());
}

// Overload: collect change points from explicit span lists.
inline std::vector<uint32_t> collect_change_points(
    std::initializer_list<std::span<const uint32_t>> target_change_point_lists) {
    uint32_t max_sample = 0;
    for (auto& list : target_change_point_lists) {
        for (uint32_t cp : list) {
            if (cp > max_sample) max_sample = cp;
        }
    }
    if (max_sample == 0) {
        bool any_non_empty = false;
        for (auto& list : target_change_point_lists) {
            if (!list.empty()) { any_non_empty = true; break; }
        }
        if (!any_non_empty) return {};
    }

    std::vector<bool> flags(max_sample + 1, false);
    for (auto& list : target_change_point_lists) {
        for (uint32_t cp : list) {
            flags[cp] = true;
        }
    }

    std::vector<uint32_t> result;
    for (uint32_t i = 0; i <= max_sample; ++i) {
        if (flags[i]) result.push_back(i);
    }
    return result;
}

}  // namespace thl::modulation
