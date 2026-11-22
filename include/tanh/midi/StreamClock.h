#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace thl::inline THL_ABI_NAMESPACE::midi {

/// Tells where in the audio stream a moment of system time falls: the piece that lines
/// MIDI up with audio when the audio callback carries no timestamp of its own.
///
/// The only thing known about the audio stream's timing is when each block is asked for.
/// That is a poor clock taken at face value: callbacks come late, in bursts (two blocks
/// back to back, then a pause), or as several chunks of one device period. But a block is
/// never asked for *earlier* than the device needs it, so the earliest requests, seen over
/// a short stretch, lie on a straight line: frames against time at the sample rate. The
/// clock tracks that line (the minimum lateness over the last quarter to half second,
/// which also follows the slow drift between the audio clock and the system clock) and
/// gives every block the stretch of system time that lies a fixed delay D before it:
///
///     frames [n, n + N) of the stream  <-  system time [line(n) - D, line(n + N) - D)
///
/// D is the duration of the largest block plus `latency`. Something that happened inside a
/// block's window gets the sample offset of its place in it. The windows of consecutive
/// blocks join without gap or overlap whatever their sizes, so two events keep their
/// distance to the sample however irregular the callbacks are, and everything is late by
/// the same amount, D. Because D is at least the block's own length, a window never
/// reaches past the present: what it covers has already happened.
///
/// After a stall longer than k_resync_ns (the device skipped, the app was suspended) the
/// line is taken up afresh from the next request.
///
/// Audio thread only; no allocation, no locks.
class StreamClock {
public:
    /// The window of one block.
    struct Block {
        std::int64_t m_start_ns = 0;  ///< System time of the block's first frame.
        double m_ns_per_frame = 0.0;
        std::uint32_t m_frames = 0;

        /// The sample offset in this block of something that happened at `time_ns`. An
        /// event from before the block gets offset 0. Returns false when it lies after the
        /// block: it is not due yet and belongs to a later one.
        [[nodiscard]] bool offset_of(std::int64_t time_ns, std::uint32_t& offset) const noexcept
            TANH_NONBLOCKING_FUNCTION {
            const std::int64_t elapsed = time_ns - m_start_ns;
            if (elapsed <= 0) {
                offset = 0;
                return true;
            }
            const double frame = std::floor(static_cast<double>(elapsed) / m_ns_per_frame);
            if (frame >= static_cast<double>(m_frames)) { return false; }
            offset = static_cast<std::uint32_t>(frame);
            return true;
        }
    };

    /// A stall beyond this restarts the line.
    static constexpr std::int64_t k_resync_ns = 100'000'000;

    /// `max_block_frames` is the largest block the stream will ask for. `latency_ns` is
    /// added to that one block every event is late by: slack for a MIDI thread that hands
    /// a message over a little after its timestamp.
    void prepare(double sample_rate,
                 std::uint32_t max_block_frames,
                 std::int64_t latency_ns = 0) noexcept {
        m_ns_per_frame = 1.0e9 / sample_rate;
        m_delay_ns =
            std::llround(static_cast<double>(max_block_frames) * m_ns_per_frame) + latency_ns;
        // Half the history: the estimate is the minimum over the last one to two of these.
        m_bucket_length = static_cast<std::uint64_t>(std::max(1.0, sample_rate * 0.25));
        reset();
    }

    /// Forgets the stream; the next block starts a new one.
    void reset() noexcept TANH_NONBLOCKING_FUNCTION {
        m_started = false;
        m_frames_total = 0;
        m_bucket_frames = 0;
        m_previous_min = k_none;
        m_current_min = k_none;
    }

    /// Call once per block, first thing in the audio callback, with the system time (the
    /// clock of core::uptime_ns()) and the block's frame count.
    [[nodiscard]] Block begin_block(std::int64_t now_ns,
                                    std::uint32_t frames) noexcept TANH_NONBLOCKING_FUNCTION {
        if (!m_started) {
            m_started = true;
            m_origin_ns = now_ns;
        }

        // How late this request is against "frames at the sample rate since the start".
        const double ideal = static_cast<double>(m_frames_total) * m_ns_per_frame;
        const double lateness = static_cast<double>(now_ns - m_origin_ns) - ideal;

        double line = std::min(m_previous_min, m_current_min);
        if (lateness - line > static_cast<double>(k_resync_ns)) {
            m_previous_min = k_none;
            m_current_min = k_none;
        }
        m_current_min = std::min(m_current_min, lateness);
        line = std::min(m_previous_min, m_current_min);

        Block block;
        block.m_start_ns = m_origin_ns + std::llround(ideal + line) - m_delay_ns;
        block.m_ns_per_frame = m_ns_per_frame;
        block.m_frames = frames;

        m_frames_total += frames;
        m_bucket_frames += frames;
        if (m_bucket_frames >= m_bucket_length) {
            m_previous_min = m_current_min;
            m_current_min = k_none;
            m_bucket_frames = 0;
        }
        return block;
    }

    /// How late every event is rendered against the moment it happened: the largest block
    /// plus the configured latency.
    [[nodiscard]] std::int64_t latency_ns() const noexcept { return m_delay_ns; }

private:
    static constexpr double k_none = std::numeric_limits<double>::infinity();

    double m_ns_per_frame = 1.0e9 / 48000.0;
    double m_previous_min = k_none;  // minimum lateness of the bucket before
    double m_current_min = k_none;   // ... and of the one being filled
    std::int64_t m_delay_ns = 0;     // largest block + configured latency
    std::int64_t m_origin_ns = 0;    // system time of the stream's first block
    std::uint64_t m_frames_total = 0;
    std::uint64_t m_bucket_frames = 0;
    std::uint64_t m_bucket_length = 12000;
    bool m_started = false;
};

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
