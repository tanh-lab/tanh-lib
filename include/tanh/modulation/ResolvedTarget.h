#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tanh/state/ParameterDefinitions.h"

namespace thl {
struct ParameterRecord;
}

namespace thl::modulation {

// Per-voice modulation buffers. Owns flat contiguous storage for additive
// and/or replace buffers across all voices. Only allocated when at least one
// polyphonic routing targets this parameter.
struct VoiceBuffers {
    uint32_t m_num_voices = 0;
    size_t m_block_size = 0;

    // ── Additive — only allocated if m_has_additive ─────────────────────
    bool m_has_additive = false;
    std::vector<float> m_additive_storage;
    std::vector<uint8_t> m_change_point_flags_storage;
    std::vector<std::vector<uint32_t>> m_change_points;

    // ── Replace — only allocated if m_has_replace ───────────────────────
    // uint8_t instead of bool: std::vector<bool> is a proxy type and does
    // not support pointer access into its storage.
    bool m_has_replace = false;
    std::vector<float> m_replace_storage;
    std::vector<uint8_t> m_replace_active_storage;

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

    void resize(uint32_t num_voices, size_t block_size, bool has_additive, bool has_replace) {
        m_num_voices = num_voices;
        m_block_size = block_size;
        m_has_additive = has_additive;
        m_has_replace = has_replace;

        const size_t total = static_cast<size_t>(num_voices) * block_size;

        if (m_has_additive) {
            m_additive_storage.assign(total, 0.0f);
        } else {
            m_additive_storage.clear();
        }

        // Change point flags needed for either additive or replace
        if (m_has_additive || m_has_replace) {
            m_change_point_flags_storage.assign(total, 0);
            m_change_points.resize(num_voices);
            for (auto& cp : m_change_points) {
                cp.clear();
                cp.reserve(block_size);
            }
        } else {
            m_change_point_flags_storage.clear();
            m_change_points.clear();
        }

        if (m_has_replace) {
            m_replace_storage.assign(total, 0.0f);
            m_replace_active_storage.assign(total, 0);
        } else {
            m_replace_storage.clear();
            m_replace_active_storage.clear();
        }
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

// Per-parameter modulation target. Owns the modulation buffer, change point
// flags (bitset), and the final sorted change point list built from the flags.
struct ResolvedTarget {
    // ── Hot fields read by SmartHandle::load() on every sample ───────────

    // Set during schedule rebuild for normalized depth processing.
    const Range* m_range = nullptr;

    // When true, buffers store normalized-space deltas and the curve conversion
    // (pow) happens once at SmartHandle::load() time instead of per-sample.
    bool m_uses_normalized_buffer = false;

    // Sparse flags — set at schedule-build time
    bool m_has_mono_additive = false;
    bool m_has_mono_replace = false;

    // Mono additive buffer — only allocated when m_has_mono_additive
    std::vector<float> m_additive_buffer;

    // Mono replace buffer — only allocated when m_has_mono_replace
    std::vector<float> m_replace_buffer;
    std::vector<uint8_t> m_replace_active;

    // Voice buffers — null if no poly routings target this parameter
    std::unique_ptr<VoiceBuffers> m_voice;

    // ── Cold fields (only used during process/build) ─────────────────────
    std::string m_id;

    // Mono change point flags — one per sample.
    std::vector<bool> m_change_point_flags;

    // Final sorted change point list, built from change_point_flags.
    std::vector<uint32_t> m_change_points;

    ParameterType m_type = ParameterType::Float;
    ParameterRecord* m_record = nullptr;

    [[nodiscard]] bool is_polyphonic() const { return m_voice != nullptr; }

    // RT-safe: read the base value as float from the parameter's atomic cache.
    [[nodiscard]] float read_base_as_float() const;

    void resize(size_t num_samples) {
        m_additive_buffer.assign(num_samples, 0.0f);
        m_change_point_flags.assign(num_samples, false);
        m_change_points.clear();
        m_change_points.reserve(num_samples);

        if (m_has_mono_replace) {
            m_replace_buffer.assign(num_samples, 0.0f);
            m_replace_active.assign(num_samples, 0);
        }

        if (m_voice) {
            m_voice->resize(m_voice->m_num_voices,
                            num_samples,
                            m_voice->m_has_additive,
                            m_voice->m_has_replace);
        }
    }

    void clear_per_block() {
        if (m_has_mono_additive) { std::ranges::fill(m_additive_buffer, 0.0f); }
        std::fill(  // NOLINT(modernize-use-ranges) — std::vector<bool> proxy
            m_change_point_flags.begin(),
            m_change_point_flags.end(),
            false);
        m_change_points.clear();

        if (m_has_mono_replace) {
            std::ranges::fill(m_replace_buffer, 0.0f);
            std::ranges::fill(m_replace_active, uint8_t{0});
        }

        if (m_voice) { m_voice->clear_per_block(); }
    }

    // Build sorted change_points from the flags bitset.
    void build_change_points() {
        m_change_points.clear();
        for (size_t i = 0; i < m_change_point_flags.size(); ++i) {
            if (m_change_point_flags[i]) { m_change_points.push_back(static_cast<uint32_t>(i)); }
        }
        if (m_voice) { m_voice->build_change_points(); }
    }
};

}  // namespace thl::modulation
