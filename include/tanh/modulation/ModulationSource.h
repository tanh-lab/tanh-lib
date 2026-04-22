#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <tanh/core/Exports.h>
#include <tanh/state/ModulationScope.h>

namespace thl::modulation {

class TANH_API ModulationSource {
public:
    // A source declares its polyphony scope at construction. ModulationMatrix
    // hands a per-scope voice_count into prepare() — sources never carry their
    // own copy of a voice count. A source whose scope is
    // k_global_scope emits one mono output buffer; any other scope
    // emits voice_count per-voice buffers.
    //
    // fully_active=true (default): every sample is considered active, so
    // per-sample active masks are not allocated and downstream code short-
    // circuits the "is sample i active?" test. Event-driven sources (gates,
    // touches) override with false.
    explicit ModulationSource(ModulationScope scope, bool fully_active = true)
        : m_scope(scope), m_fully_active(fully_active) {}

    virtual ~ModulationSource() = default;

    // Allocate buffers sized to samples_per_block. Sources in the global scope
    // allocate a single mono output buffer and voice_count is ignored (but
    // should be 1 by convention). Sources in any other scope allocate
    // voice_count × samples_per_block voice buffers.
    //
    // voice_count must match the matrix's registered voice count for this
    // scope. ModulationMatrix::prepare() forwards the correct value.
    virtual void prepare(double sample_rate, size_t samples_per_block, uint32_t voice_count) = 0;

    // Per-block reset hook. Called exactly once per source per block by
    // ModulationMatrix::process_with_scope(), *before* pre_process_block().
    // The default wipes only the change-point lists — these are per-block
    // transition metadata and are never meaningful cross-block.
    //
    // What is intentionally NOT touched here:
    //   - m_output_buffer / m_voice_output_storage (the value buffers)
    //   - m_output_active / m_voice_output_active_storage (the active masks)
    //
    // Those are authored state: the source decides whether to hold them
    // across blocks (XYTouchSource-style: last value & gate persist until
    // the next event) or overwrite them every block (LFO-style). The matrix
    // takes no position — if a subclass wants its masks zeroed at block
    // start it can call clear_output_active() / clear_voice_output_active()
    // here (or override this method entirely).
    virtual void clear_per_block() {
        clear_change_points();
        if (!is_global()) { clear_voice_change_points(); }
    }

    // Optional input-snapshot hook. Called exactly once per source per block
    // by ModulationMatrix::process_with_scope(), *after* clear_per_block()
    // and before any ScheduleStep runs. Event-driven sources override this
    // to drain their input queue once and write the full block's worth of
    // value, mask, and change-point state — all three then survive to the
    // propagation pass (because clear_per_block already ran). Default is a
    // no-op; procedural sources (LFOs, envelopes) just do their work in
    // process() / process_voice() and leave this alone.
    virtual void pre_process_block() {}

    // Process num_samples starting at offset, writing output to
    // m_output_buffer[offset..offset+num_samples]. Sources should record
    // change points via record_change_point() when output changes.
    // Called only for global-scope sources.
    virtual void process(size_t /*num_samples*/, size_t /*offset*/ = 0) {}

    // Process num_samples for a single voice starting at offset, writing
    // output to voice_output(voice_index)[offset..offset+num_samples].
    // Called only for non-global-scope sources.
    virtual void process_voice(uint32_t /*voice_index*/,
                               size_t /*num_samples*/,
                               size_t /*offset*/ = 0) {}

    // Parameter keys this source exposes for modulation-on-modulation.
    // Used by the matrix to build the dependency graph for Tarjan SCC.
    virtual std::vector<std::string> parameter_keys() const { return {}; }

    // ── Capability / identity ───────────────────────────────────────────
    [[nodiscard]] ModulationScope scope() const { return m_scope; }
    [[nodiscard]] bool is_global() const { return m_scope == k_global_scope; }
    [[nodiscard]] uint32_t num_voices() const { return m_num_voices; }
    [[nodiscard]] bool is_fully_active() const { return m_fully_active; }
    [[nodiscard]] size_t block_size() const { return m_block_size; }

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
    std::vector<uint8_t>& get_output_active() { return m_output_active; }

    uint8_t get_output_active_at(uint32_t index) const { return m_output_active[index]; }

    const uint8_t* voice_output_active(uint32_t v) const {
        assert(v < m_num_voices);
        return m_voice_output_active_storage.data() + static_cast<size_t>(v) * m_block_size;
    }

    uint8_t* voice_output_active(uint32_t v) {
        assert(v < m_num_voices);
        return m_voice_output_active_storage.data() + static_cast<size_t>(v) * m_block_size;
    }

    void clear_output_active() { std::memset(m_output_active.data(), 0, m_output_active.size()); }

    void clear_voice_output_active() {
        std::memset(m_voice_output_active_storage.data(), 0, m_voice_output_active_storage.size());
    }

protected:
    // Allocate buffers based on scope and the matrix-provided voice_count.
    // Call from prepare(). For global scope, voice_count is ignored and a
    // single mono buffer is allocated. For any non-global scope, voice_count
    // voice buffers are allocated.
    void resize_buffers(size_t block_size, uint32_t voice_count) {
        m_block_size = block_size;

        if (is_global()) {
            m_num_voices = 1;
            m_output_buffer.resize(block_size, 0.0f);
            m_change_points.clear();
            m_change_points.reserve(block_size);

            if (!m_fully_active) { m_output_active.assign(block_size, 0); }
        } else {
            m_num_voices = voice_count;
            const size_t total = static_cast<size_t>(m_num_voices) * block_size;
            m_voice_output_storage.assign(total, 0.0f);
            m_voice_change_points.resize(m_num_voices);
            for (auto& cp : m_voice_change_points) {
                cp.clear();
                cp.reserve(block_size);
            }

            if (!m_fully_active) { m_voice_output_active_storage.assign(total, 0); }
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
    // Scope — set at construction, immutable
    const ModulationScope m_scope;
    const bool m_fully_active;

    // Voice count — assigned by resize_buffers() using the matrix-provided
    // value. Remains immutable across a prepare() cycle.
    uint32_t m_num_voices = 0;
    size_t m_block_size = 0;

    // Output active masks — only allocated when !m_fully_active
    std::vector<uint8_t> m_output_active;
    std::vector<uint8_t> m_voice_output_active_storage;
};

}  // namespace thl::modulation
