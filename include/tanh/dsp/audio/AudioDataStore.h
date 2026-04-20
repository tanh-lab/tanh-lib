#pragma once

#include <tanh/dsp/audio/AudioBuffer.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace thl::dsp::audio {

/**
 * RT-safe single-buffered audio data storage with CAS state machine.
 *
 * Uses a three-state atomic (Idle, Reading, Loading) to coordinate
 * between the RT thread and a loader thread with minimal memory overhead.
 *
 * RT thread:  CAS(Idle→READING) to read, store(Idle) when done.
 *             If CAS fails (loader active), output silence for that block.
 * Loader:     Spin CAS(Idle→Loading), move data in, store(Idle).
 */
class AudioDataStore {
public:
    AudioDataStore() = default;

    bool begin_read() {
        // NOLINTNEXTLINE(misc-const-correctness) — mutated by compare_exchange_strong on failure.
        int expected = Idle;
        return m_state.compare_exchange_strong(expected,
                                               Reading,
                                               std::memory_order_acq_rel,
                                               std::memory_order_relaxed);
    }

    void end_read() { m_state.store(Idle, std::memory_order_release); }

    const std::vector<AudioBuffer>& get_buffer() const { return m_buffer; }

    bool is_loaded() const { return m_loaded.load(std::memory_order_acquire); }

    int get_root_note() const { return m_root_note.load(std::memory_order_acquire); }

    uint32_t get_load_generation() const {
        return m_load_generation.load(std::memory_order_acquire);
    }

    std::vector<AudioBuffer>& begin_load() {
        // Spin until we can acquire Loading state
        while (true) {
            // NOLINTNEXTLINE(misc-const-correctness) — mutated by compare_exchange_weak on failure.
            int expected = Idle;
            if (m_state.compare_exchange_weak(expected,
                                              Loading,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
                break;
            }
            std::this_thread::yield();
        }

        m_buffer.clear();
        m_buffer.shrink_to_fit();
        return m_buffer;
    }

    void commit_load(int root_note) {
        m_root_note.store(root_note, std::memory_order_relaxed);
        m_loaded.store(true, std::memory_order_relaxed);
        m_load_generation.fetch_add(1, std::memory_order_relaxed);

        m_state.store(Idle, std::memory_order_release);
    }

private:
    enum State { Idle = 0, Reading = 1, Loading = 2 };

    std::vector<AudioBuffer> m_buffer;
    std::atomic<int> m_state{Idle};
    std::atomic<int> m_root_note{60};
    std::atomic<bool> m_loaded{false};
    std::atomic<uint32_t> m_load_generation{0};
};

}  // namespace thl::dsp::audio
