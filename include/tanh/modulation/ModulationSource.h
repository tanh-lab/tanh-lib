#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace thl::modulation {

class ModulationSource {
public:
    virtual ~ModulationSource() = default;

    virtual void prepare(double sample_rate, size_t samples_per_block) = 0;

    // Bulk mode: process the entire block, writing output to m_output_buffer
    // and recording change points in m_change_points.
    virtual void process(size_t num_samples) = 0;

    // Cyclic mode: process a single sample at sample_index, writing the result
    // to *out. Sources should record their own change points via
    // record_change_point() when their output changes significantly.
    virtual void process_single(float* out, uint32_t sample_index) = 0;

    // Parameter keys this source exposes for modulation-on-modulation.
    // Used by the matrix to build the dependency graph for Tarjan SCC.
    virtual std::vector<std::string> parameter_keys() const { return {}; }

    float last_output() const { return m_last_output; }

    const std::vector<uint32_t>& get_change_points() const { return m_change_points; }

    void clear_change_points() { m_change_points.clear(); }

    const std::vector<float>& get_output_buffer() const { return m_output_buffer; }

    float get_output_at(uint32_t index) const { return m_output_buffer[index]; }

protected:
    void resize_buffers(size_t num_samples) {
        m_output_buffer.resize(num_samples, 0.0f);
        m_change_points.clear();
        m_change_points.reserve(num_samples);
    }

    void record_change_point(uint32_t sample_index) { m_change_points.push_back(sample_index); }

    std::vector<float> m_output_buffer;
    std::vector<uint32_t> m_change_points;
    float m_last_output = 0.0f;
};

}  // namespace thl::modulation
