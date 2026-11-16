#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferView.h>

#include <cstddef>

namespace thl::inline THL_ABI_NAMESPACE::dsp::filter {

class DCBlocker {
public:
    void prepare(float pole) {
        m_x = 0.0f;
        m_y = 0.0f;
        m_pole = pole;
    }

    void reset() {
        m_x = 0.0f;
        m_y = 0.0f;
    }

    void process(thl::core::BufferView in_out) {
        float* ptr = in_out.get_write_pointer(0);  // NOLINT(misc-const-correctness)
        size_t size = in_out.get_num_samples();
        float x = m_x;
        float y = m_y;
        const float pole = m_pole;
        while (size--) {
            const float old_x = x;
            x = *ptr;
            *ptr++ = y = y * pole + x - old_x;
        }
        m_x = x;
        m_y = y;
    }

private:
    float m_pole = 0.0f;
    float m_x = 0.0f;
    float m_y = 0.0f;
};

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::filter
