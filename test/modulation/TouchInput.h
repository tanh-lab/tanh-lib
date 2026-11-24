#pragma once

#include <tanh/core/threading/LockFreeQueue.h>
#include <tanh/modulation/MotionTypes.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace motion_test {

/// One queued event of a TouchInput.
struct TouchEvent {
    enum class Kind : uint8_t { Down, Move, Up };
    Kind m_kind = Kind::Move;
    float m_x = 0.0f;
    float m_y = 0.0f;
};

/// One finger on a two-channel (x, y) MotionRecorder input, for the recorder's tests: touch(),
/// release() and release_all() on one thread, prepare() before, process_block() and output() on the
/// audio thread. The events drained in a block are spread evenly over it (event i of n at i * block
/// / n) and held, each with a change point, as an XY pad places untimed touches.
class TouchInput {
public:
    using Event = TouchEvent;
    using Kind = TouchEvent::Kind;

    void prepare(size_t samples_per_block) {
        if (samples_per_block <= m_x.size()) { return; }
        m_x.assign(samples_per_block, m_x_now);
        m_y.assign(samples_per_block, m_y_now);
        m_active.assign(samples_per_block, m_active_now);
        m_change_points.assign(samples_per_block, 0);
    }

    /// Put the finger down at (@p x, @p y), or move it there; @p id is ignored (one finger).
    bool touch(uint64_t /*id*/, float x, float y) {
        if (!std::isfinite(x) || !std::isfinite(y)) { return false; }
        const Event event{.m_kind = m_held ? Kind::Move : Kind::Down,
                          .m_x = std::clamp(x, 0.0f, 1.0f),
                          .m_y = std::clamp(y, 0.0f, 1.0f)};
        if (!m_queue.try_push(event)) { return false; }
        m_held = true;
        m_last_x = event.m_x;
        m_last_y = event.m_y;
        return true;
    }

    /// Lift the finger: the gate closes, the position holds.
    bool release(uint64_t /*id*/ = 0) {
        if (!m_held) { return false; }
        m_held = false;
        return m_queue.try_push(Event{.m_kind = Kind::Up, .m_x = m_last_x, .m_y = m_last_y});
    }

    void release_all() { release(); }

    void process_block(uint32_t num_samples) TANH_NONBLOCKING_FUNCTION {
        const auto n = static_cast<uint32_t>(std::min<size_t>(num_samples, m_x.size()));
        m_num_samples = n;
        m_num_change_points = 0;
        if (n == 0) { return; }  // keep the events for a real block

        size_t count = 0;
        Event event;
        while (count < m_drained.size() && m_queue.try_pop(event)) { m_drained[count++] = event; }

        uint32_t position = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto at = static_cast<uint32_t>((i * static_cast<size_t>(n)) / count);
            render(position, at);
            m_x_now = m_drained[i].m_x;
            m_y_now = m_drained[i].m_y;
            m_active_now = m_drained[i].m_kind == Kind::Up ? 0 : 1;
            if (m_num_change_points == 0 || m_change_points[m_num_change_points - 1] != at) {
                m_change_points[m_num_change_points++] = at;
            }
            position = at;
        }
        render(position, n);
    }

    [[nodiscard]] thl::modulation::MotionInput output() TANH_NONBLOCKING_FUNCTION {
        if (m_x.empty()) { return {}; }
        m_channels = {m_x.data(), m_y.data()};
        return thl::modulation::MotionInput{
            .m_channels = m_channels,
            .m_active = m_active.data(),
            .m_change_points =
                std::span<const uint32_t>(m_change_points.data(), m_num_change_points),
            .m_num_samples = m_num_samples,
        };
    }

private:
    static constexpr size_t k_capacity = 256;

    void render(uint32_t from, uint32_t to) {
        if (from >= to) { return; }
        std::fill(m_x.begin() + from, m_x.begin() + to, m_x_now);
        std::fill(m_y.begin() + from, m_y.begin() + to, m_y_now);
        std::fill(m_active.begin() + from, m_active.begin() + to, m_active_now);
    }

    thl::core::LockFreeQueue<Event, k_capacity> m_queue;
    std::array<Event, k_capacity> m_drained{};

    // The touching thread's side.
    bool m_held = false;
    float m_last_x = 0.0f;
    float m_last_y = 0.0f;

    // The audio thread's side.
    float m_x_now = 0.0f;
    float m_y_now = 0.0f;
    uint8_t m_active_now = 0;
    std::vector<float> m_x;
    std::vector<float> m_y;
    std::vector<uint8_t> m_active;
    std::vector<uint32_t> m_change_points;
    std::array<const float*, 2> m_channels{};
    size_t m_num_change_points = 0;
    uint32_t m_num_samples = 0;
};

}  // namespace motion_test
