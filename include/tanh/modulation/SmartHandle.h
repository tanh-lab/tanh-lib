#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
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
    // voice_index selects which voice buffer to read from (0 = default/mono).
    //
    // A target is either mono-only or voice-only (never both):
    // - Poly target (m_voice != nullptr): reads from voice buffers
    // - Mono target: reads from mono buffers
    //
    // For targets with normalized buffers (non-linear ranges), the curve
    // conversion happens here — one pow() per read instead of 2N per block.
    T load(uint32_t modulation_offset = 0,
           uint32_t voice_index = 0) const TANH_NONBLOCKING_FUNCTION {
        if (!m_target) { return m_handle.load(); }

        float base_f = 0.0f;
        float mod = 0.0f;

        if (m_target->m_voice) {
            // ── Poly target: all modulation is in voice buffers ─────────
            if (m_target->m_voice->m_has_replace &&
                m_target->m_voice->replace_active_voice(voice_index)[modulation_offset]) {
                base_f = m_target->m_voice->replace_voice(voice_index)[modulation_offset];
            } else {
                base_f = static_cast<float>(m_handle.load());
            }
            if (m_target->m_voice->m_has_additive) {
                mod = m_target->m_voice->additive_voice(voice_index)[modulation_offset];
            }
        } else {
            // ── Mono target: all modulation is in mono buffers ──────────
            if (m_target->m_has_mono_replace &&
                modulation_offset < m_target->m_replace_active.size() &&
                m_target->m_replace_active[modulation_offset]) {
                base_f = m_target->m_replace_buffer[modulation_offset];
            } else {
                base_f = static_cast<float>(m_handle.load());
            }
            if (m_target->m_has_mono_additive &&
                modulation_offset < m_target->m_additive_buffer.size()) {
                mod = m_target->m_additive_buffer[modulation_offset];
            }
        }

        return apply_modulation(base_f, mod);
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
    float load_normalized(uint32_t modulation_offset = 0,
                          uint32_t voice_index = 0) const TANH_NONBLOCKING_FUNCTION {
        if (!m_target) {
            return m_handle.range().to_normalized(static_cast<float>(m_handle.load()));
        }

        float base_f = 0.0f;
        float mod = 0.0f;

        if (m_target->m_voice) {
            if (m_target->m_voice->m_has_replace &&
                m_target->m_voice->replace_active_voice(voice_index)[modulation_offset]) {
                base_f = m_target->m_voice->replace_voice(voice_index)[modulation_offset];
            } else {
                base_f = static_cast<float>(m_handle.load());
            }
            if (m_target->m_voice->m_has_additive) {
                mod = m_target->m_voice->additive_voice(voice_index)[modulation_offset];
            }
        } else {
            if (m_target->m_has_mono_replace &&
                modulation_offset < m_target->m_replace_active.size() &&
                m_target->m_replace_active[modulation_offset]) {
                base_f = m_target->m_replace_buffer[modulation_offset];
            } else {
                base_f = static_cast<float>(m_handle.load());
            }
            if (m_target->m_has_mono_additive &&
                modulation_offset < m_target->m_additive_buffer.size()) {
                mod = m_target->m_additive_buffer[modulation_offset];
            }
        }

        if (m_target->m_uses_normalized_buffer) {
            const float base_norm = m_target->m_range->to_normalized(base_f);
            float result_norm = base_norm + mod;
            if (m_target->m_range->m_periodic) {
                result_norm = std::fmod(result_norm, 1.0f);
                if (result_norm < 0.0f) { result_norm += 1.0f; }
            } else {
                result_norm = std::clamp(result_norm, 0.0f, 1.0f);
            }
            return result_norm;
        }
        return m_handle.range().to_normalized(base_f + mod);
    }

    size_t get_buffer_size() TANH_NONBLOCKING_FUNCTION {
        if (m_target) { return m_target->m_additive_buffer.size(); }
        return 0;
    }

    // Access the ResolvedTarget (for testing / advanced use).
    ResolvedTarget* target() const TANH_NONBLOCKING_FUNCTION { return m_target; }

private:
    // Common modulation application: base_f + mod with curve conversion and type cast.
    T apply_modulation(float base_f, float mod) const TANH_NONBLOCKING_FUNCTION {
        if (m_target->m_uses_normalized_buffer) {
            const float base_norm = m_target->m_range->to_normalized(base_f);
            float result_norm = base_norm + mod;
            if (m_target->m_range->m_periodic) {
                result_norm = std::fmod(result_norm, 1.0f);
                if (result_norm < 0.0f) { result_norm += 1.0f; }
            } else {
                result_norm = std::clamp(result_norm, 0.0f, 1.0f);
            }
            const float result = m_target->m_range->from_normalized(result_norm);
            return convert_float<T>(result);
        }
        // Plain-space delta (linear ranges or absolute depth mode)
        return convert_float<T>(base_f + mod);
    }

    template <typename U>
    static U convert_float(float value) TANH_NONBLOCKING_FUNCTION {
        if constexpr (std::is_same_v<U, float>) {
            return value;
        } else if constexpr (std::is_same_v<U, double>) {
            return static_cast<double>(value);
        } else if constexpr (std::is_same_v<U, int>) {
            return static_cast<int>(std::round(value));
        } else if constexpr (std::is_same_v<U, bool>) {
            return value >= 0.5f;
        }
    }

    thl::ParameterHandle<T> m_handle;
    ResolvedTarget* m_target = nullptr;
};

// Free utility: collect change points from a span of SmartHandles.
// Writes a sorted, deduplicated list of change points into target_buffer.
// voice_index selects per-voice change points for polyphonic targets (0 = mono/default).
//
// RT-SAFETY CONTRACT (must hold to avoid allocation on the audio thread):
//   target_buffer.capacity() must be >= the block size used when the
//   ModulationMatrix was prepared. The caller is responsible for reserving
//   the buffer once at prepare() time, e.g.:
//     m_change_points.reserve(samples_per_block);
//
// Uses target_buffer's pre-reserved memory as a bitset for O(1) dedup and
// inherently sorted output. No stack allocation, no block size limit.
// O(H*C + block_size/32) instead of the naive O(H*C*M).
//
// Layout within target_buffer (capacity N, W = ceil(N/32) bitset words):
//   [ output indices ...          | bitset words at N-W .. N-1 ]
// The bitset is placed at the high end. During extraction the write pointer
// advances at most 32× faster than the read pointer, but the W-word bitset
// starts N-W positions ahead of position 0, giving exactly enough headroom
// (32·W ≤ N for any N ≥ 32, and W = ceil(N/32)).
template <typename T>
void collect_change_points(std::span<const SmartHandle<T>> handles,
                           std::vector<uint32_t>& target_buffer,
                           uint32_t voice_index = 0) TANH_NONBLOCKING_FUNCTION {
    // First pass: find max index to determine bitset size
    uint32_t max_index = 0;
    bool has_any = false;

    for (const auto& h : handles) {
        if (const auto* cp = h.change_points()) {
            for (const uint32_t v : *cp) {
                if (v > max_index) { max_index = v; }
                has_any = true;
            }
        }
        if (h.target() && h.target()->m_voice &&
            (h.target()->m_voice->m_has_additive || h.target()->m_voice->m_has_replace) &&
            voice_index < h.target()->m_voice->m_num_voices) {
            for (const uint32_t v : h.target()->m_voice->m_change_points[voice_index]) {
                if (v > max_index) { max_index = v; }
                has_any = true;
            }
        }
    }

    if (!has_any) {
        target_buffer.resize(0);
        return;
    }

    const size_t count = static_cast<size_t>(max_index) + 1;
    const size_t words_needed = (count + 31) / 32;
    assert(count <= target_buffer.capacity() &&
           "collect_change_points: target_buffer capacity too small — "
           "reserve at least samples_per_block before calling");

    // Resize to cover the full range. Bitset goes at [count - words_needed, count).
    target_buffer.resize(count);
    const size_t bitset_base = count - words_needed;
    std::memset(&target_buffer[bitset_base], 0, words_needed * sizeof(uint32_t));

    // Second pass: set bits in the high-end bitset region
    for (const auto& h : handles) {
        if (const auto* cp = h.change_points()) {
            for (const uint32_t v : *cp) {
                target_buffer[bitset_base + v / 32] |= uint32_t{1} << (v % 32);
            }
        }
        if (h.target() && h.target()->m_voice &&
            (h.target()->m_voice->m_has_additive || h.target()->m_voice->m_has_replace) &&
            voice_index < h.target()->m_voice->m_num_voices) {
            for (const uint32_t v : h.target()->m_voice->m_change_points[voice_index]) {
                target_buffer[bitset_base + v / 32] |= uint32_t{1} << (v % 32);
            }
        }
    }

    // Extraction: read from bitset at the high end, write indices to the low end.
    // The write pointer never overtakes the read pointer because the bitset
    // starts count-W positions ahead, and each word produces at most 32 entries.
    size_t write = 0;
    for (size_t w = 0; w < words_needed; ++w) {
        const auto word = target_buffer[bitset_base + w];
        auto bits = word;
        while (bits != 0) {
            const int bit = std::countr_zero(bits);
            target_buffer[write++] = static_cast<uint32_t>(w * 32 + bit);
            bits &= bits - 1;  // clear lowest set bit
        }
    }
    target_buffer.resize(write);  // shrink only — trivial type, no allocation
}

// Overload: collect change points from explicit span lists into target_buffer.
// Same RT-safety contract as the SmartHandle overload: target_buffer must have
// sufficient capacity pre-reserved by the caller.
inline void collect_change_points(
    std::initializer_list<std::span<const uint32_t>> target_change_point_lists,
    std::vector<uint32_t>& target_buffer) {
    // First pass: find max index
    uint32_t max_index = 0;
    bool has_any = false;
    for (const auto& list : target_change_point_lists) {
        for (const uint32_t cp : list) {
            if (cp > max_index) { max_index = cp; }
            has_any = true;
        }
    }

    if (!has_any) {
        target_buffer.resize(0);
        return;
    }

    const size_t count = static_cast<size_t>(max_index) + 1;
    const size_t words_needed = (count + 31) / 32;
    assert(count <= target_buffer.capacity() &&
           "collect_change_points: target_buffer capacity too small — "
           "reserve at least samples_per_block before calling");

    // Bitset at the high end of target_buffer
    target_buffer.resize(count);
    const size_t bitset_base = count - words_needed;
    std::memset(&target_buffer[bitset_base], 0, words_needed * sizeof(uint32_t));

    // Set bits
    for (const auto& list : target_change_point_lists) {
        for (const uint32_t cp : list) {
            target_buffer[bitset_base + cp / 32] |= uint32_t{1} << (cp % 32);
        }
    }

    // Extract sorted indices
    size_t write = 0;
    for (size_t w = 0; w < words_needed; ++w) {
        const auto word = target_buffer[bitset_base + w];
        auto bits = word;
        while (bits != 0) {
            const int bit = std::countr_zero(bits);
            target_buffer[write++] = static_cast<uint32_t>(w * 32 + bit);
            bits &= bits - 1;
        }
    }
    target_buffer.resize(write);
}

}  // namespace thl::modulation
