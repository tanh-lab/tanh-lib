#pragma once

#include <cstddef>

namespace thl::dsp::filter {

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

    void process(float* in_out, size_t size) {
        float x = m_x;
        float y = m_y;
        const float pole = m_pole;
        while (size--) {
            const float old_x = x;
            x = *in_out;
            *in_out++ = y = y * pole + x - old_x;
        }
        m_x = x;
        m_y = y;
    }

private:
    float m_pole = 0.0f;
    float m_x = 0.0f;
    float m_y = 0.0f;
};

}  // namespace thl::dsp::filter
