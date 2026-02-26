#pragma once

#include <vector>

namespace thl::dsp::resonator {

// Pre-allocated ring buffer for RT-safe FIFO operations.
// Call resize() during prepare/setup, then push/pop never allocate.
struct RingBuffer {
    void resize(int capacity) {
        m_buf.assign(capacity + 1, 0.0f);
        m_head = m_tail = m_count = 0;
    }
    void push(float x) {
        m_buf[m_tail] = x;
        m_tail = (m_tail + 1) % static_cast<int>(m_buf.size());
        ++m_count;
    }
    float pop() {
        float x = m_buf[m_head];
        m_head = (m_head + 1) % static_cast<int>(m_buf.size());
        --m_count;
        return x;
    }
    bool empty() const { return m_count == 0; }
    int size() const { return m_count; }
    void clear() { m_head = m_tail = m_count = 0; }

private:
    std::vector<float> m_buf;
    int m_head = 0;
    int m_tail = 0;
    int m_count = 0;
};

} // namespace thl::dsp::resonator
