#pragma once

#include <array>
#include <atomic>
#include <vector>

namespace thl::dsp::audio {

/**
 * RT-safe double-buffered audio data storage.
 *
 * Provides lock-free access for the RT thread (read from active buffer)
 * and allows a loader thread to write to the inactive buffer, then atomically swap.
 *
 */
class AudioDataStore {
public:
    AudioDataStore() = default;

    // --- RT thread interface (read-only) ---

    /**
     * Get the currently active audio data buffer.
     * RT-safe: uses acquire semantics.
     */
    const std::vector<std::vector<float>>& get_active() const {
        auto active_index = static_cast<size_t>(m_active_index.load(std::memory_order_acquire));
        return m_buffers[active_index];
    }

    /**
     * Check if audio data has been loaded.
     */
    bool is_loaded() const {
        auto active_index = static_cast<size_t>(m_active_index.load(std::memory_order_acquire));
        return !m_buffers[active_index].empty();
    }

    /**
     * Get the root note of the currently loaded sample.
     */
    int get_root_note() const {
        return m_root_note.load(std::memory_order_acquire);
    }

    // --- Loader thread interface ---

    /**
     * Get the inactive buffer for writing new data.
     * Only call from the loader thread.
     */
    std::vector<std::vector<float>>& get_inactive() {
        auto active_index = static_cast<size_t>(m_active_index.load(std::memory_order_acquire));
        auto index_inactive = 1 - active_index;
        return m_buffers[index_inactive];
    }

    /**
     * Commit the inactive buffer, making it active.
     * Call after writing data to get_inactive().
     * Only call from the loader thread.
     *
     * @param root_note The MIDI root note of the loaded sample
     */
    void commit(int root_note) {
        m_root_note.store(root_note, std::memory_order_release);
        int inactive = 1 - m_active_index.load(std::memory_order_acquire);
        m_active_index.store(inactive, std::memory_order_release);
    }

private:
    std::array<std::vector<std::vector<float>>, 2> m_buffers;
    std::atomic<int> m_active_index{0};
    std::atomic<int> m_root_note{60};
};

} // namespace thl::dsp::audio
