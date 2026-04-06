#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

#include <tanh/modulation/ResolvedTarget.h>
#include <tanh/state/Parameter.h>
#include <tanh/utils/RealtimeSanitizer.h>

namespace thl::modulation {

// SmartHandle<T> wraps a ParameterHandle<T> and adds per-sample modulation
// buffer support. Holds a stable pointer to a ResolvedTarget in the matrix's
// map — no registration or rewiring needed.
//
// - Unmodulated: m_target is nullptr or buffer empty → reads State's
//   AtomicCacheEntry directly (~1ns)
// - Modulated: applies base + modulation_buffer[offset] with type conversion
//
// T must be one of: float, double, int, bool.
template <typename T>
class SmartHandle {
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, double> || std::is_same_v<T, int> ||
                      std::is_same_v<T, bool>,
                  "SmartHandle only supports numeric types (float, double, int, bool)");

public:
    SmartHandle() = default;

    SmartHandle(thl::ParameterHandle<T> handle, ResolvedTarget* target)
        : m_handle(handle), m_target(target) {}

    // Read the parameter value at a given sample offset.
    // If a modulation target is attached, returns base + modulation.
    //
    // For targets with normalized buffers (non-linear ranges), the curve
    // conversion happens here — one pow() per read instead of 2N per block.
    T load(uint32_t modulation_offset = 0) const TANH_NONBLOCKING_FUNCTION {
        const T base = m_handle.load();
        if (m_target && modulation_offset < m_target->m_modulation_buffer.size()) {
            const float mod = m_target->m_modulation_buffer[modulation_offset];
            if (m_target->m_uses_normalized_buffer) {
                // Buffer stores normalized-space delta — apply curve conversion
                const auto base_f = static_cast<float>(base);
                const float base_norm = m_target->m_range->to_normalized(base_f);
                float result_norm = base_norm + mod;
                if (m_target->m_range->m_periodic) {
                    result_norm = std::fmod(result_norm, 1.0f);
                    if (result_norm < 0.0f) { result_norm += 1.0f; }
                } else {
                    result_norm = std::clamp(result_norm, 0.0f, 1.0f);
                }
                const float result = m_target->m_range->from_normalized(result_norm);
                if constexpr (std::is_same_v<T, float>) {
                    return result;
                } else if constexpr (std::is_same_v<T, double>) {
                    return static_cast<double>(result);
                } else if constexpr (std::is_same_v<T, int>) {
                    return static_cast<int>(std::round(result));
                } else if constexpr (std::is_same_v<T, bool>) {
                    return result >= 0.5f;
                }
            }
            // Plain-space delta (linear ranges or absolute depth mode)
            if constexpr (std::is_same_v<T, float>) {
                return base + mod;
            } else if constexpr (std::is_same_v<T, double>) {
                return base + static_cast<double>(mod);
            } else if constexpr (std::is_same_v<T, int>) {
                return static_cast<int>(std::round(static_cast<float>(base) + mod));
            } else if constexpr (std::is_same_v<T, bool>) {
                return (base ? 1.0f : 0.0f) + mod >= 0.5f;
            }
        }
        return base;
    }

    // Access the change points for this parameter's modulation target.
    // Returns nullptr if unmodulated.
    const std::vector<uint32_t>* change_points() const TANH_NONBLOCKING_FUNCTION {
        return m_target ? &m_target->m_change_points : nullptr;
    }

    thl::ParameterHandle<T> raw_handle() const TANH_NONBLOCKING_FUNCTION { return m_handle; }
    bool is_valid() const TANH_NONBLOCKING_FUNCTION { return m_handle.is_valid(); }

    // ── Metadata accessors (RT-safe — immutable after construction) ──────
    [[nodiscard]] const thl::ParameterDefinition& def() const TANH_NONBLOCKING_FUNCTION {
        return m_handle.def();
    }
    [[nodiscard]] const thl::Range& range() const TANH_NONBLOCKING_FUNCTION {
        return m_handle.range();
    }
    [[nodiscard]] std::string_view key() const TANH_NONBLOCKING_FUNCTION { return m_handle.key(); }
    [[nodiscard]] uint32_t id() const TANH_NONBLOCKING_FUNCTION { return m_handle.id(); }
    [[nodiscard]] uint32_t flags() const TANH_NONBLOCKING_FUNCTION { return m_handle.flags(); }

    // Read the parameter value normalized to [0, 1] at a given sample offset.
    // For normalized buffers this avoids the from_norm→to_norm roundtrip.
    float load_normalized(uint32_t modulation_offset = 0) const TANH_NONBLOCKING_FUNCTION {
        if (m_target && m_target->m_uses_normalized_buffer &&
            modulation_offset < m_target->m_modulation_buffer.size()) {
            const auto base_f = static_cast<float>(m_handle.load());
            const float base_norm = m_target->m_range->to_normalized(base_f);
            float result_norm = base_norm + m_target->m_modulation_buffer[modulation_offset];
            if (m_target->m_range->m_periodic) {
                result_norm = std::fmod(result_norm, 1.0f);
                if (result_norm < 0.0f) { result_norm += 1.0f; }
            } else {
                result_norm = std::clamp(result_norm, 0.0f, 1.0f);
            }
            return result_norm;
        }
        return m_handle.range().to_normalized(static_cast<float>(load(modulation_offset)));
    }

    size_t get_buffer_size() TANH_NONBLOCKING_FUNCTION {
        if (m_target) { return m_target->m_modulation_buffer.size(); }
        return 0;
    }

private:
    thl::ParameterHandle<T> m_handle;
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
// Uses a stack-allocated bitset (1 KiB) for O(1) dedup and inherently sorted
// output. Supports block sizes up to 8192 samples. O(H*C + block_size/64)
// instead of the naive O(H*C*M).
template <typename T>
void collect_change_points(std::span<const SmartHandle<T>> handles,
                           std::vector<uint32_t>& target_buffer) TANH_NONBLOCKING_FUNCTION {
    // 128 × 64 bits = 8192 sample indices. 1 KiB on the stack.
    static constexpr size_t k_bitset_words = 128;
    static constexpr size_t k_max_samples = k_bitset_words * 64;

    target_buffer.clear();

    std::array<uint64_t, k_bitset_words> flags{};
    uint32_t max_index = 0;

    for (auto& h : handles) {
        if (const auto* cp = h.change_points()) {
            for (const uint32_t v : *cp) {
                assert(v < k_max_samples &&
                       "collect_change_points: change point index exceeds 8192 — "
                       "block size too large for stack bitset");
                flags[v / 64] |= uint64_t{1} << (v % 64);
                if (v > max_index) { max_index = v; }
            }
        }
    }

    // Linear scan extracts set bits in sorted order — no final sort needed.
    const size_t words_to_scan = max_index / 64 + 1;
    for (size_t w = 0; w < words_to_scan; ++w) {
        auto word = flags[w];
        while (word != 0) {
            const int bit = std::countr_zero(word);
            assert(target_buffer.size() < target_buffer.capacity() &&
                   "collect_change_points: target_buffer capacity exceeded — "
                   "reserve at least samples_per_block before calling on the audio thread");
            target_buffer.push_back(static_cast<uint32_t>(w * 64 + bit));
            word &= word - 1;  // clear lowest set bit
        }
    }
}

// Overload: collect change points from explicit span lists.
inline std::vector<uint32_t> collect_change_points(
    std::initializer_list<std::span<const uint32_t>> target_change_point_lists) {
    uint32_t max_sample = 0;
    for (auto& list : target_change_point_lists) {
        for (const uint32_t cp : list) {
            if (cp > max_sample) { max_sample = cp; }
        }
    }
    if (max_sample == 0) {
        bool any_non_empty = false;
        for (auto& list : target_change_point_lists) {
            if (!list.empty()) {
                any_non_empty = true;
                break;
            }
        }
        if (!any_non_empty) { return {}; }
    }

    std::vector<bool> flags(max_sample + 1, false);
    for (auto& list : target_change_point_lists) {
        for (const uint32_t cp : list) { flags[cp] = true; }
    }

    std::vector<uint32_t> result;
    for (uint32_t i = 0; i <= max_sample; ++i) {
        if (flags[i]) { result.push_back(i); }
    }
    return result;
}

}  // namespace thl::modulation
