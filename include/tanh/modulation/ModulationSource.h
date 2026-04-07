#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <tanh/core/Exports.h>

namespace thl::modulation {

class TANH_API ModulationSource {
public:
    explicit ModulationSource(bool has_mono = true,
                              uint32_t num_voices = 0,
                              bool fully_active = true)
        : m_has_mono_output(has_mono)
        , m_has_voice_output(num_voices > 0)
        , m_num_voices(num_voices)
        , m_fully_active(fully_active) {}

    virtual ~ModulationSource() = default;

    virtual void prepare(double sample_rate, size_t samples_per_block) = 0;

    // Process num_samples starting at offset, writing output to
    // m_output_buffer[offset..offset+num_samples]. Sources should record
    // change points via record_change_point() when output changes.
    virtual void process(size_t /*num_samples*/, size_t /*offset*/ = 0) {}

    // Process num_samples for a single voice starting at offset, writing
    // output to voice_output(voice_index)[offset..offset+num_samples].
    virtual void process_voice(uint32_t /*voice_index*/,
                               size_t /*num_samples*/,
                               size_t /*offset*/ = 0) {}

    // Parameter keys this source exposes for modulation-on-modulation.
    // Used by the matrix to build the dependency graph for Tarjan SCC.
    virtual std::vector<std::string> parameter_keys() const { return {}; }

    // ── Capability flags ────────────────────────────────────────────────
    [[nodiscard]] bool has_mono_output() const { return m_has_mono_output; }
    [[nodiscard]] bool has_voice_output() const { return m_has_voice_output; }
    [[nodiscard]] uint32_t num_voices() const { return m_num_voices; }
    [[nodiscard]] bool is_fully_active() const { return m_fully_active; }

    // ── Mono accessors ──────────────────────────────────────────────────
    float last_output() const { return m_last_output; }
    const std::vector<uint32_t>& get_change_points() const { return m_change_points; }
    void clear_change_points() { m_change_points.clear(); }
    const std::vector<float>& get_output_buffer() const { return m_output_buffer; }
    float get_output_at(uint32_t index) const { return m_output_buffer[index]; }

    // ── Voice accessors ─────────────────────────────────────────────────
    float* voice_output(uint32_t v) {
        assert(v < m_num_voices);
        return m_voice_output_storage.data() + static_cast<size_t>(v) * m_block_size;
    }

    const float* voice_output(uint32_t v) const {
        assert(v < m_num_voices);
        return m_voice_output_storage.data() + static_cast<size_t>(v) * m_block_size;
    }

    const std::vector<uint32_t>& get_voice_change_points(uint32_t v) const {
        assert(v < m_num_voices);
        return m_voice_change_points[v];
    }

    void clear_voice_change_points() {
        for (auto& cp : m_voice_change_points) { cp.clear(); }
    }

    // ── Output active mask accessors ────────────────────────────────────
    const std::vector<uint8_t>& get_output_active() const { return m_output_active; }

    uint8_t get_output_active_at(uint32_t index) const { return m_output_active[index]; }

    const uint8_t* voice_output_active(uint32_t v) const {
        assert(v < m_num_voices);
        return m_voice_output_active_storage.data() + static_cast<size_t>(v) * m_block_size;
    }

    void clear_output_active() { std::memset(m_output_active.data(), 0, m_output_active.size()); }

    void clear_voice_output_active() {
        std::memset(m_voice_output_active_storage.data(), 0, m_voice_output_active_storage.size());
    }

protected:
    // Allocate all buffers based on constructor flags. Call from prepare().
    void resize_buffers(size_t block_size) {
        m_block_size = block_size;

        if (m_has_mono_output) {
            m_output_buffer.resize(block_size, 0.0f);
            m_change_points.clear();
            m_change_points.reserve(block_size);
        }

        if (m_has_voice_output) {
            const size_t total = static_cast<size_t>(m_num_voices) * block_size;
            m_voice_output_storage.assign(total, 0.0f);
            m_voice_change_points.resize(m_num_voices);
            for (auto& cp : m_voice_change_points) {
                cp.clear();
                cp.reserve(block_size);
            }
        }

        if (!m_fully_active && m_has_mono_output) { m_output_active.assign(block_size, 0); }

        if (!m_fully_active && m_has_voice_output) {
            const size_t total = static_cast<size_t>(m_num_voices) * block_size;
            m_voice_output_active_storage.assign(total, 0);
        }
    }

    void record_change_point(uint32_t sample_index) { m_change_points.push_back(sample_index); }

    void record_voice_change_point(uint32_t voice_index, uint32_t sample_index) {
        assert(voice_index < m_num_voices);
        m_voice_change_points[voice_index].push_back(sample_index);
    }

    void set_output_active(uint32_t sample_index) { m_output_active[sample_index] = 1; }

    void set_voice_output_active(uint32_t voice_index, uint32_t sample_index) {
        assert(voice_index < m_num_voices);
        m_voice_output_active_storage[static_cast<size_t>(voice_index) * m_block_size +
                                      sample_index] = 1;
    }

    // Mono buffers
    std::vector<float> m_output_buffer;
    std::vector<uint32_t> m_change_points;
    float m_last_output = 0.0f;

    // Voice buffers — flat contiguous: voice v at v * m_block_size
    std::vector<float> m_voice_output_storage;
    std::vector<std::vector<uint32_t>> m_voice_change_points;

private:
    // Capability flags — set at construction, immutable
    const bool m_has_mono_output;
    const bool m_has_voice_output;
    const uint32_t m_num_voices;
    const bool m_fully_active;

    size_t m_block_size = 0;

    // Output active masks — only allocated when !m_fully_active
    std::vector<uint8_t> m_output_active;
    std::vector<uint8_t> m_voice_output_active_storage;
};

}  // namespace thl::modulation
