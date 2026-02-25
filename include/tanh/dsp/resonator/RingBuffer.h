#pragma once

#include <vector>

namespace thl::dsp::resonator {

// Pre-allocated ring buffer for RT-safe FIFO operations.
// Call resize() during prepare/setup, then push/pop never allocate.
struct RingBuffer {
    std::vector<float> buf;
    int head = 0;
    int tail = 0;
    int count = 0;

    void resize(int capacity) {
        buf.assign(capacity + 1, 0.0f);
        head = tail = count = 0;
    }
    void push(float x) {
        buf[tail] = x;
        tail = (tail + 1) % static_cast<int>(buf.size());
        ++count;
    }
    float pop() {
        float x = buf[head];
        head = (head + 1) % static_cast<int>(buf.size());
        --count;
        return x;
    }
    bool empty() const { return count == 0; }
    int size() const { return count; }
    void clear() { head = tail = count = 0; }
};

} // namespace thl::dsp::resonator
