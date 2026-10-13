#pragma once

#include <tanh/core/Exports.h>
#include <tanh/core/threading/LockFreeQueue.h>
#include <tanh/core/threading/TripleBuffer.h>
#include <tanh/modulation/XYFrame.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace thl::modulation::detail {

/**
 * @brief The UI side of an XYController: per-block frames and the live trail.
 *
 * The audio thread fills the voices of the frame being written (voice()), adds
 * trail points of recording voices (add_trail()) and calls publish() once per
 * block; frames go out at most at the frame rate, and at once when anything
 * discrete changed. One UI thread reads them (read(), drain()).
 */
class TANH_API XYFramePublisher {
public:
    /// Message thread, audio stopped: the frame rate and the trail spacing.
    void prepare(double sample_rate, double frame_rate_hz, uint32_t trail_interval);

    /// Audio thread: voice @p index of the frame being written.
    [[nodiscard]] XYVoiceFrame& voice(uint32_t index) TANH_NONBLOCKING_FUNCTION {
        return m_frames.write_buffer().m_voices[index];
    }

    /// One block of a recording voice.
    struct TrailBlock {
        const float* m_x = nullptr;
        const float* m_y = nullptr;
        const uint8_t* m_gate = nullptr;
        const uint8_t* m_live = nullptr;
        uint32_t m_num_samples = 0;
        float m_progress = 0.0f;
        // The take's loop position at the block's start and end (0..1, may wrap).
        float m_position_from = 0.0f;
        float m_position_to = 0.0f;
    };

    /**
     * @brief Audio thread: trail points of a recording voice, every trail interval.
     *
     * @param countdown the voice's offset to its next point, carried across blocks
     *                  (0 when the voice starts recording).
     */
    void add_trail(uint8_t voice,
                   const TrailBlock& block,
                   uint32_t& countdown) TANH_NONBLOCKING_FUNCTION;

    /// Audio thread: completes the block's frame and publishes it when due.
    void publish(uint64_t sample_time, uint32_t num_voices, bool latch, uint32_t num_samples)
        TANH_NONBLOCKING_FUNCTION;

    /// UI thread: the newest frame, false (out untouched) if nothing is new.
    bool read(XYFrame& out) { return m_frames.read(out); }
    /// UI thread: trail points, oldest first.
    size_t drain(std::span<XYPathPoint> out);

private:
    TripleBuffer<XYFrame> m_frames;
    thl::core::LockFreeQueue<XYPathPoint, k_xy_trail_capacity> m_trail;
    uint32_t m_trail_interval = 32;
    uint64_t m_sequence = 0;
    uint32_t m_trail_dropped = 0;
    uint32_t m_frame_interval = 200;
    uint32_t m_since_publish = 0;
    uint64_t m_last_signature = 0;  // discrete state of the last published frame
};

}  // namespace thl::modulation::detail
