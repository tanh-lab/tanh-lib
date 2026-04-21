#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>

#include <tanh/modulation/ResolvedTarget.h>
#include <tanh/state/Parameter.h>
#include <tanh/utils/RealtimeSanitizer.h>

namespace thl::modulation {

// SmartHandle<T> wraps a ParameterHandle<T> and adds per-sample modulation
// buffer support. Holds a stable pointer to a ResolvedTarget in the matrix's
// map — no registration or rewiring needed.
//
// - Unmodulated: m_target is nullptr or both buffer pointers null → reads
//   State's AtomicCacheEntry directly (~1ns)
// - Modulated: applies base + modulation_buffer[offset] with type conversion
//
// The hot path is a single std::memory_order_acquire load of either the
// ResolvedTarget::m_voice or ResolvedTarget::m_mono atomic pointer, followed
// by direct reads through the returned const pointer. The pointed-to buffer
// object is immutable while published; the writer swaps the pointer and only
// destroys retired buffers after m_config.synchronize() has guaranteed no
// in-flight audio block still references them.
//
// Every access to the conditionally-allocated storage vectors
// (m_additive_*, m_replace_*, m_change_point_flags*, m_change_points) below
// is gated on VoiceBuffers::m_has_additive / m_has_replace (or the MonoBuffers
// equivalents). Those storage vectors are only allocated when the matching
// flag is set at construction time — reading through an unallocated span is
// UB. The flag-gate is also how this hot path stays safe across rebuilds:
// the writer may atomically publish a fresh buffer whose flag set differs
// from the one the matrix config was built against, so a caller holding an
// old config might land on a narrower buffer. Checking the flag on the
// buffer we actually loaded (not the one the config implies) keeps the read
// correct in that window.
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
    // - Poly target (m_voice atomic non-null): reads from voice buffers
    // - Mono target (m_mono atomic non-null): reads from mono buffers
    //
    // For targets with normalized buffers (non-linear ranges), the curve
    // conversion happens here — one pow() per read instead of 2N per block.
    T load(uint32_t modulation_offset = 0,
           uint32_t voice_index = 0) const TANH_NONBLOCKING_FUNCTION {
        if (!m_target) { return m_handle.load(); }

        if (auto* vb = m_target->m_voice.load(std::memory_order_acquire)) {
            float base_f = 0.0f;
            float mod = 0.0f;
            // Flag-gate before touching replace_*/additive_* storage: those
            // vectors are only allocated when the matching m_has_* flag was
            // set at VoiceBuffers construction. See header comment.
            if (vb->m_has_replace && vb->replace_active_voice(voice_index)[modulation_offset]) {
                base_f = vb->replace_voice(voice_index)[modulation_offset];
            } else {
                base_f = static_cast<float>(m_handle.load());
            }
            if (vb->m_has_additive) { mod = vb->additive_voice(voice_index)[modulation_offset]; }
            return apply_modulation(base_f, mod);
        } else if (auto* mb = m_target->m_mono.load(std::memory_order_acquire)) {
            float base_f = 0.0f;
            float mod = 0.0f;
            if (mb->m_has_replace && modulation_offset < mb->m_replace_active.size() &&
                mb->m_replace_active[modulation_offset]) {
                base_f = mb->m_replace_buffer[modulation_offset];
            } else {
                base_f = static_cast<float>(m_handle.load());
            }
            if (mb->m_has_additive && modulation_offset < mb->m_additive_buffer.size()) {
                mod = mb->m_additive_buffer[modulation_offset];
            }
            return apply_modulation(base_f, mod);
        }

        return m_handle.load();
    }

    // Access the mono-change-point list for this parameter's target. Returns
    // nullptr if the target has no mono buffer (unmodulated or poly-only).
    // The returned pointer is valid for the duration of the current RCU block
    // scope.
    //
    // TIMING: same caveat as change_points_voice() — only populated by
    // MonoBuffers::build_change_points() at end-of-block. Mid-block readers
    // should use change_point_flags() instead.
    const std::vector<uint32_t>* change_points() const TANH_NONBLOCKING_FUNCTION {
        if (!m_target) { return nullptr; }
        if (auto* mb = m_target->m_mono.load(std::memory_order_acquire)) {
            return &mb->m_change_points;
        }
        return nullptr;
    }

    // Access the mono change-point flag bitmask (one uint8_t per sample) for
    // this parameter's target. Returns nullptr if the target has no mono
    // buffer or carries no modulation flags.
    //
    // Live counterpart to change_points() — updated as the matrix applies
    // each upstream routing, so mid-block readers (e.g. a relay source)
    // observe change points already contributed by Tarjan-earlier sources.
    // Pointer is valid for the current RCU block scope.
    const uint8_t* change_point_flags() const TANH_NONBLOCKING_FUNCTION {
        if (!m_target) { return nullptr; }
        auto* mb = m_target->m_mono.load(std::memory_order_acquire);
        if (!mb || !(mb->m_has_additive || mb->m_has_replace)) { return nullptr; }
        return mb->m_change_point_flags.data();
    }

    // Access the per-voice replace-active mask (one uint8_t per sample) for
    // polyphonic targets. Returns nullptr if the target has no voice buffer
    // or no Replace routing — in either case the parameter is reading base
    // at every sample. Lets downstream sources propagate "input is modulated
    // right now" state into their own voice_output_active.
    const uint8_t* replace_active_voice(uint32_t voice_index) const TANH_NONBLOCKING_FUNCTION {
        if (!m_target) { return nullptr; }
        auto* vb = m_target->m_voice.load(std::memory_order_acquire);
        // !m_has_replace → m_replace_active_storage was never allocated;
        // returning nullptr is the contract, not an error.
        if (!vb || !vb->m_has_replace) { return nullptr; }
        return vb->replace_active_voice(voice_index);
    }

    // Access the per-voice change-point list for this parameter's target.
    // Returns nullptr if the target has no voice buffer, carries no
    // modulation flags, or voice_index is out of range. Pointer is valid
    // for the current RCU block scope. Symmetric to change_points() but
    // for polyphonic targets.
    //
    // TIMING: the list is only populated by ResolvedTarget::build_change_points(),
    // which the matrix runs once per block *after* all sources have executed.
    // Readers that run mid-block (e.g. a relay source inside
    // process_source_bulk_with_scope) will observe stale previous-block data.
    // For live in-block reads use change_point_flags_voice() instead.
    const std::vector<uint32_t>* change_points_voice(uint32_t voice_index) const
        TANH_NONBLOCKING_FUNCTION {
        if (!m_target) { return nullptr; }
        auto* vb = m_target->m_voice.load(std::memory_order_acquire);
        if (!vb || !(vb->m_has_additive || vb->m_has_replace)) { return nullptr; }
        if (voice_index >= vb->m_num_voices) { return nullptr; }
        return &vb->m_change_points[voice_index];
    }

    // Access the per-voice change-point flag bitmask (one uint8_t per sample)
    // for this parameter's target. Returns nullptr if the target has no voice
    // buffer, carries no modulation flags, or voice_index is out of range.
    //
    // Unlike change_points_voice(), the flag storage is updated live as the
    // matrix applies each upstream routing — so a mid-block reader (e.g. a
    // relay source) sees the change points already contributed by sources
    // that Tarjan ordered before it. Pointer is valid for the current RCU
    // block scope.
    const uint8_t* change_point_flags_voice(uint32_t voice_index) const TANH_NONBLOCKING_FUNCTION {
        if (!m_target) { return nullptr; }
        auto* vb = m_target->m_voice.load(std::memory_order_acquire);
        if (!vb || !(vb->m_has_additive || vb->m_has_replace)) { return nullptr; }
        if (voice_index >= vb->m_num_voices) { return nullptr; }
        return vb->m_change_point_flags_storage.data() +
               static_cast<size_t>(voice_index) * vb->m_block_size;
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
        bool has_mod_state = false;

        if (auto* vb = m_target->m_voice.load(std::memory_order_acquire)) {
            has_mod_state = true;
            if (vb->m_has_replace && vb->replace_active_voice(voice_index)[modulation_offset]) {
                base_f = vb->replace_voice(voice_index)[modulation_offset];
            } else {
                base_f = static_cast<float>(m_handle.load());
            }
            if (vb->m_has_additive) { mod = vb->additive_voice(voice_index)[modulation_offset]; }
        } else if (auto* mb = m_target->m_mono.load(std::memory_order_acquire)) {
            has_mod_state = true;
            if (mb->m_has_replace && modulation_offset < mb->m_replace_active.size() &&
                mb->m_replace_active[modulation_offset]) {
                base_f = mb->m_replace_buffer[modulation_offset];
            } else {
                base_f = static_cast<float>(m_handle.load());
            }
            if (mb->m_has_additive && modulation_offset < mb->m_additive_buffer.size()) {
                mod = mb->m_additive_buffer[modulation_offset];
            }
        }

        if (!has_mod_state) {
            return m_handle.range().to_normalized(static_cast<float>(m_handle.load()));
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

    // Returns the block size of whichever buffer set is currently published
    // for this target, or 0 if unmodulated.
    size_t get_buffer_size() const TANH_NONBLOCKING_FUNCTION {
        if (!m_target) { return 0; }
        if (auto* vb = m_target->m_voice.load(std::memory_order_acquire)) {
            return vb->m_block_size;
        }
        if (auto* mb = m_target->m_mono.load(std::memory_order_acquire)) {
            return mb->m_block_size;
        }
        return 0;
    }

    // Access the ResolvedTarget (for testing / advanced use).
    ResolvedTarget* target() const TANH_NONBLOCKING_FUNCTION { return m_target; }

private:
    // Common modulation application: base_f + mod with curve conversion and type cast.
    T apply_modulation(float base_f, float mod) const TANH_NONBLOCKING_FUNCTION {
        float result;
        if (m_target->m_uses_normalized_buffer) {
            const float base_norm = m_target->m_range->to_normalized(base_f);
            float result_norm = base_norm + mod;
            if (m_target->m_range->m_periodic) {
                result_norm = std::fmod(result_norm, 1.0f);
                if (result_norm < 0.0f) { result_norm += 1.0f; }
            } else {
                result_norm = std::clamp(result_norm, 0.0f, 1.0f);
            }
            result = m_target->m_range->from_normalized(result_norm);
        } else {
            // Plain-space delta (linear ranges or absolute depth mode)
            result = base_f + mod;
        }
        // Snap stepped int targets (step > 1) to the parameter's step boundary
        // before the int cast. For step == 1 this is equivalent to the
        // round-to-nearest already done by convert_float<int>; for larger
        // steps it keeps modulated values on the quantized grid.
        if constexpr (std::is_same_v<T, int>) { result = m_target->m_range->snap(result); }
        return convert_float<T>(result);
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

namespace detail {

// Dispatch helper: extract a ResolvedTarget* from anything "handle-like".
// Supports:
//   - SmartHandle<T>                     — direct .target() call
//   - std::variant<Ts...>                — std::visit each alternative;
//                                          alternatives without .target()
//                                          (e.g. plain ParameterHandle<T>
//                                          for non-modulatable params)
//                                          contribute nullptr and are skipped
//                                          by callers.
//   - ResolvedTarget*                    — identity (pass-through)
// Users extend by providing another overload.
template <typename H>
ResolvedTarget* handle_target(const H& h) TANH_NONBLOCKING_FUNCTION {
    if constexpr (std::is_pointer_v<H>) {
        return h;
    } else if constexpr (requires { h.target(); }) {
        return h.target();
    } else {
        return std::visit(
            [](const auto& inner) -> ResolvedTarget* {
                if constexpr (requires { inner.target(); }) {
                    return inner.target();
                } else {
                    return nullptr;
                }
            },
            h);
    }
}

}  // namespace detail

// Free utility: collect change points from a span of handles.
// Writes a sorted, deduplicated list of change points into target_buffer.
// voice_index selects per-voice change points for polyphonic targets (0 = mono/default).
//
// Accepts any element type for which detail::handle_target(h) resolves to a
// ResolvedTarget* — SmartHandle<T>, std::variant<SmartHandle<Ts>...>, or a
// raw ResolvedTarget*. nullptr targets (e.g. unattached handles) are skipped.
// This lets callers keep a single heterogeneous handle array (per-parameter
// std::variant over differently-typed SmartHandles) without maintaining a
// parallel ResolvedTarget* array just for this call.
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
template <typename Handle>
void collect_change_points(std::span<const Handle> handles,
                           std::vector<uint32_t>& target_buffer,
                           uint32_t voice_index = 0) TANH_NONBLOCKING_FUNCTION {
    // First pass: find max index to determine bitset size.
    // NOLINTNEXTLINE(misc-const-correctness) — mutated in the loop below.
    uint32_t max_index = 0;
    // NOLINTNEXTLINE(misc-const-correctness) — mutated in the loop below.
    bool has_any = false;

    for (const auto& h : handles) {
        auto* t = detail::handle_target(h);
        if (!t) { continue; }
        if (auto* mb = t->m_mono.load(std::memory_order_acquire)) {
            for (const uint32_t v : mb->m_change_points) {
                if (v > max_index) { max_index = v; }
                has_any = true;
            }
        }
        if (auto* vb = t->m_voice.load(std::memory_order_acquire)) {
            // m_change_points is sized to m_num_voices only when the buffer
            // actually carries modulation (either flag set); an all-flags-
            // clear VoiceBuffers leaves it empty and indexing would be OOB.
            if ((vb->m_has_additive || vb->m_has_replace) && voice_index < vb->m_num_voices) {
                for (const uint32_t v : vb->m_change_points[voice_index]) {
                    if (v > max_index) { max_index = v; }
                    has_any = true;
                }
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

    // Second pass: set bits in the high-end bitset region.
    for (const auto& h : handles) {
        auto* t = detail::handle_target(h);
        if (!t) { continue; }
        if (auto* mb = t->m_mono.load(std::memory_order_acquire)) {
            for (const uint32_t v : mb->m_change_points) {
                target_buffer[bitset_base + v / 32] |= uint32_t{1} << (v % 32);
            }
        }
        if (auto* vb = t->m_voice.load(std::memory_order_acquire)) {
            if ((vb->m_has_additive || vb->m_has_replace) && voice_index < vb->m_num_voices) {
                for (const uint32_t v : vb->m_change_points[voice_index]) {
                    target_buffer[bitset_base + v / 32] |= uint32_t{1} << (v % 32);
                }
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
    // NOLINTNEXTLINE(misc-const-correctness) — mutated in the loop below.
    uint32_t max_index = 0;
    // NOLINTNEXTLINE(misc-const-correctness) — mutated in the loop below.
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
