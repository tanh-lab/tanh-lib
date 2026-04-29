#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tanh/state/ModulationScope.h"
#include "tanh/state/ParameterDefinitions.h"

namespace thl {
struct ParameterRecord;
}

namespace thl::modulation {

// Per-voice modulation buffers. Owns flat contiguous storage for additive
// and/or replace buffers across all voices, plus per-voice change-point state.
// Only allocated when at least one polyphonic routing targets this parameter.
//
// Structure (sizes, flags) is set once at construction and never changes — the
// writer never resizes an existing buffer; it allocates a fresh VoiceBuffers
// and atomic-swaps the pointer on its owning ResolvedTarget. Contents are
// mutated per-block by the audio thread (apply_routing_*, clear_per_block,
// build_change_points); that's single-threaded from the audio thread's
// perspective and never collides with the writer.
struct VoiceBuffers {
    uint32_t m_num_voices = 0;
    size_t m_block_size = 0;

    // ── Additive — only allocated if m_has_additive ─────────────────────
    bool m_has_additive = false;
    std::vector<float> m_additive_storage;

    // Change-point flags are needed whenever either additive or replace is
    // present, so they live at the VoiceBuffers level rather than per-path.
    std::vector<uint8_t> m_change_point_flags_storage;
    std::vector<std::vector<uint32_t>> m_change_points;

    // ── Replace — only allocated if m_has_replace ───────────────────────
    // uint8_t instead of bool: std::vector<bool> is a proxy type and does
    // not support pointer access into its storage.
    bool m_has_replace = false;
    std::vector<float> m_replace_storage;
    std::vector<uint8_t> m_replace_active_storage;

    VoiceBuffers() = default;

    VoiceBuffers(uint32_t num_voices, size_t block_size, bool has_additive, bool has_replace)
        : m_num_voices(num_voices)
        , m_block_size(block_size)
        , m_has_additive(has_additive)
        , m_has_replace(has_replace) {
        const size_t total = static_cast<size_t>(num_voices) * block_size;

        if (m_has_additive) { m_additive_storage.assign(total, 0.0f); }

        if (m_has_additive || m_has_replace) {
            m_change_point_flags_storage.assign(total, 0);
            m_change_points.resize(num_voices);
            for (auto& cp : m_change_points) {
                cp.clear();
                cp.reserve(block_size);
            }
        }

        if (m_has_replace) {
            m_replace_storage.assign(total, 0.0f);
            m_replace_active_storage.assign(total, 0);
        }
    }

    float* additive_voice(uint32_t v) {
        assert(m_has_additive && v < m_num_voices);
        return m_additive_storage.data() + static_cast<size_t>(v) * m_block_size;
    }

    const float* additive_voice(uint32_t v) const {
        assert(m_has_additive && v < m_num_voices);
        return m_additive_storage.data() + static_cast<size_t>(v) * m_block_size;
    }

    float* replace_voice(uint32_t v) {
        assert(m_has_replace && v < m_num_voices);
        return m_replace_storage.data() + static_cast<size_t>(v) * m_block_size;
    }

    const float* replace_voice(uint32_t v) const {
        assert(m_has_replace && v < m_num_voices);
        return m_replace_storage.data() + static_cast<size_t>(v) * m_block_size;
    }

    uint8_t* replace_active_voice(uint32_t v) {
        assert(m_has_replace && v < m_num_voices);
        return m_replace_active_storage.data() + static_cast<size_t>(v) * m_block_size;
    }

    const uint8_t* replace_active_voice(uint32_t v) const {
        assert(m_has_replace && v < m_num_voices);
        return m_replace_active_storage.data() + static_cast<size_t>(v) * m_block_size;
    }

    void clear_per_block() {
        if (m_has_additive) { std::ranges::fill(m_additive_storage, 0.0f); }
        if (m_has_additive || m_has_replace) {
            std::ranges::fill(m_change_point_flags_storage, uint8_t{0});
            for (auto& cp : m_change_points) { cp.clear(); }
        }
        if (m_has_replace) {
            std::ranges::fill(m_replace_storage, 0.0f);
            std::ranges::fill(m_replace_active_storage, uint8_t{0});
        }
    }

    void build_change_points() {
        if (!m_has_additive && !m_has_replace) { return; }
        for (uint32_t v = 0; v < m_num_voices; ++v) {
            auto& cp = m_change_points[v];
            cp.clear();
            const size_t base = static_cast<size_t>(v) * m_block_size;
            for (size_t i = 0; i < m_block_size; ++i) {
                if (m_change_point_flags_storage[base + i]) {
                    cp.push_back(static_cast<uint32_t>(i));
                }
            }
        }
    }
};

// Mono (single-voice) modulation buffers. Owns the additive/replace buffers
// plus the mono change-point flags and sorted list. Only allocated when a
// target has at least one mono routing.
//
// Same lifetime discipline as VoiceBuffers: structure is fixed at construction
// time, writer never mutates contents, audio thread owns all content writes
// (clear_per_block, apply_routing_global_to_global, build_change_points).
struct MonoBuffers {
    size_t m_block_size = 0;
    bool m_has_additive = false;
    bool m_has_replace = false;

    // Additive — only allocated if m_has_additive
    std::vector<float> m_additive_buffer;

    // Replace — only allocated if m_has_replace
    std::vector<float> m_replace_buffer;
    std::vector<uint8_t> m_replace_active;

    // Change points — allocated whenever m_has_additive || m_has_replace.
    // uint8_t (not bool) so callers can write through a pointer.
    std::vector<uint8_t> m_change_point_flags;
    std::vector<uint32_t> m_change_points;

    MonoBuffers() = default;

    MonoBuffers(size_t block_size, bool has_additive, bool has_replace)
        : m_block_size(block_size), m_has_additive(has_additive), m_has_replace(has_replace) {
        if (m_has_additive) { m_additive_buffer.assign(block_size, 0.0f); }
        if (m_has_replace) {
            m_replace_buffer.assign(block_size, 0.0f);
            m_replace_active.assign(block_size, 0);
        }
        if (m_has_additive || m_has_replace) {
            m_change_point_flags.assign(block_size, 0);
            m_change_points.clear();
            m_change_points.reserve(block_size);
        }
    }

    void clear_per_block() {
        if (m_has_additive) { std::ranges::fill(m_additive_buffer, 0.0f); }
        if (m_has_additive || m_has_replace) {
            std::ranges::fill(m_change_point_flags, uint8_t{0});
            m_change_points.clear();
        }
        if (m_has_replace) {
            std::ranges::fill(m_replace_buffer, 0.0f);
            std::ranges::fill(m_replace_active, uint8_t{0});
        }
    }

    void build_change_points() {
        if (!m_has_additive && !m_has_replace) { return; }
        m_change_points.clear();
        for (size_t i = 0; i < m_block_size; ++i) {
            if (m_change_point_flags[i]) { m_change_points.push_back(static_cast<uint32_t>(i)); }
        }
    }
};

// Per-parameter modulation target. Exposes swappable VoiceBuffers and
// MonoBuffers via atomic pointers; SmartHandle and the matrix's audio-thread
// paths read these without any RCU call or refcount. The writer allocates
// fresh buffer instances under m_writer_mutex, atomic-stores the new pointers
// *after* m_config.synchronize() has drained all in-flight audio blocks, and
// reclaims retired buffers at the end of the rebuild.
struct ResolvedTarget {
    // ── Hot fields read by SmartHandle::load() on every sample ───────────

    // Set during schedule rebuild for normalized depth processing.
    const Range* m_range = nullptr;

    // When true, buffers store normalized-space deltas and the curve conversion
    // (pow) happens once at SmartHandle::load() time instead of per-sample.
    bool m_uses_normalized_buffer = false;

    // Atomic buffer pointers — swapped by writer, read by audio thread.
    // nullptr means this target has no active routings of that polyphony.
    // Pointed-to object is owned by m_voice_owner / m_mono_owner (writer-only)
    // and stays alive until retired past a synchronize() barrier.
    std::atomic<VoiceBuffers*> m_voice{nullptr};
    std::atomic<MonoBuffers*> m_mono{nullptr};

    // ── Cold fields (writer-only; not read on the audio hot path) ────────
    std::string m_id;
    ParameterType m_type = ParameterType::Float;
    ParameterRecord* m_record = nullptr;

    // Scope declared on the parameter's ParameterDefinition, copied and
    // validated by ensure_target_with_lock. Gates routing validity: only
    // (Global, Global), (Global, scope), and (scope, scope) pairs are
    // accepted; cross-scope and scope→Global routings are rejected.
    // Unlike m_voice / m_mono, this is authoritative metadata — it does
    // not change based on which routings currently exist.
    ModulationScope m_scope = k_global_scope;

    // Owner of the currently-published VoiceBuffers / MonoBuffers. Touched only
    // under m_writer_mutex. Paired with the atomic pointers above.
    std::unique_ptr<VoiceBuffers> m_voice_owner;
    std::unique_ptr<MonoBuffers> m_mono_owner;

    // Retirement lists for deferred reclamation. After a rebuild stores new
    // owners, the previous owners are pushed here and drained only after
    // m_config.synchronize() has guaranteed no in-flight audio block still
    // references them.
    std::vector<std::unique_ptr<VoiceBuffers>> m_voice_retired;
    std::vector<std::unique_ptr<MonoBuffers>> m_mono_retired;

    ResolvedTarget() = default;

    // Non-copyable, non-movable — contains atomics and owned buffers with
    // stable addresses. Stored in std::map nodes, so no move/copy is needed
    // after insertion.
    ResolvedTarget(const ResolvedTarget&) = delete;
    ResolvedTarget& operator=(const ResolvedTarget&) = delete;
    ResolvedTarget(ResolvedTarget&&) = delete;
    ResolvedTarget& operator=(ResolvedTarget&&) = delete;

    [[nodiscard]] bool is_polyphonic() const {
        return m_voice.load(std::memory_order_acquire) != nullptr;
    }

    // RT-safe: read the base value as float from the parameter's atomic cache.
    [[nodiscard]] float read_base_as_float() const;

    // Called once per block from the audio thread at block start.
    void clear_per_block() {
        if (auto* v = m_voice.load(std::memory_order_acquire)) { v->clear_per_block(); }
        if (auto* m = m_mono.load(std::memory_order_acquire)) { m->clear_per_block(); }
    }

    // Called once per block from the audio thread after the schedule has
    // finished filling change-point flags.
    void build_change_points() {
        if (auto* v = m_voice.load(std::memory_order_acquire)) { v->build_change_points(); }
        if (auto* m = m_mono.load(std::memory_order_acquire)) { m->build_change_points(); }
    }
};

}  // namespace thl::modulation
