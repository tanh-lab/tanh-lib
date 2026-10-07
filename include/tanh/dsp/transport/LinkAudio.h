#pragma once

#include <tanh/core/Exports.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace thl::dsp::transport {

/**
 * @brief One buffer of audio received from a Link Audio channel, on the local beat grid.
 *
 * Interleaved float samples; the beats are local Link beat times (mapped from the sender's
 * session when the buffer arrived), so the buffer can be placed on the receiver's timeline.
 */
struct LinkAudioPacket {
    /// A Link Audio network packet carries at most 563 16-bit samples (1200-byte messages).
    static constexpr size_t k_max_samples = 576;

    std::array<float, k_max_samples> m_samples{};
    uint32_t m_num_frames = 0;
    uint32_t m_num_channels = 0;
    double m_begin_beat = 0.0;  ///< local beat of the first frame
    double m_end_beat = 0.0;    ///< local beat just after the last frame
};

/**
 * @brief Audio-thread seam for Link Audio: the subscribed channel in, our channel out.
 *
 * thl::link::LinkSession::audio_sharing() implements it over the Link SDKs (TANH_WITH_LINK);
 * tests script it. Both methods are called from the audio thread only and are real-time safe.
 */
class TANH_API LinkAudioBackend {
public:
    virtual ~LinkAudioBackend() = default;

    /// The next received packet, oldest first. False when none is waiting.
    virtual bool pop(LinkAudioPacket& out) TANH_NONBLOCKING_FUNCTION = 0;

    /**
     * @brief Sends one block of our output (planar) to the peers listening to our channel.
     *
     * @param begin_beat Local beat of the block's first frame: the beat the audio belongs to
     *                   (for processed received audio, the beat it was received at).
     * @return False when no peer listens or the SDK has no buffer free.
     */
    virtual bool send(const float* const* channels,
                      uint32_t num_channels,
                      uint32_t num_frames,
                      double begin_beat,
                      double quantum,
                      uint32_t sample_rate) TANH_NONBLOCKING_FUNCTION = 0;
};

/**
 * @brief Plays a received Link Audio channel on the local beat grid, a latency behind.
 *
 * Each block asks for the audio of a beat range, normally the block's beats minus a latency
 * that covers the network (after Ableton's LinkAudioHut renderer). The buffered packets are
 * read across their boundaries with cubic interpolation at the rate that maps the range onto
 * the block, which also follows tempo changes and a sender running at another sample rate.
 * Until the packets cover a range (start, audio arriving too late, a timeline jump) the block
 * is silent and the receiver starts over. A packet the network lost is skipped (the audio
 * around it plays on); one that arrives out of order is dropped.
 *
 * Audio thread only, after construction (which allocates the packet ring).
 */
class TANH_API LinkAudioReceiver {
public:
    /// Packets held (about three seconds of stereo at 48 kHz); the oldest go when it is full.
    static constexpr size_t k_capacity = 512;
    /// A packet starting this far before the last one's end is reordered and dropped.
    static constexpr double k_order_tolerance_beats = 1e-4;
    /// A packet starting this far before the last one's end means the timeline jumped back.
    static constexpr double k_jump_beats = 0.25;

    LinkAudioReceiver();

    /// Drops every buffered packet and starts over.
    void reset() TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief Takes the packets waiting in @p backend and renders the beat range
     *        [@p begin_beat, @p begin_beat + @p num_frames * @p beats_per_sample) into @p out.
     *
     * Mono packets go to every output channel, stereo packets to the first two (a third
     * output channel repeats the last packet channel).
     *
     * @return True when the range was covered; false and silence otherwise.
     */
    bool render(LinkAudioBackend& backend,
                float* const* out,
                uint32_t num_channels,
                uint32_t num_frames,
                double begin_beat,
                double beats_per_sample) TANH_NONBLOCKING_FUNCTION;

    /// Packets buffered after the last render().
    [[nodiscard]] size_t num_buffered() const { return m_count; }

    /// Beats of audio buffered beyond the last rendered range (after the last render()): how
    /// much margin the latency leaves. Negative or 0 while not playing.
    [[nodiscard]] double margin_beats() const { return m_margin_beats; }

    /// Packets taken from the backend since construction (unusable ones included).
    [[nodiscard]] uint64_t num_received() const { return m_received; }

    /// Times playback stopped because the audio for a range had not arrived (a dropout: raise
    /// the latency). Counts from construction.
    [[nodiscard]] uint64_t num_underruns() const { return m_underruns; }

private:
    [[nodiscard]] const LinkAudioPacket& packet(size_t i) const {
        return m_ring[(m_head + i) % k_capacity];
    }
    void pop_front() TANH_NONBLOCKING_FUNCTION;
    /// Sample @p channel of frame @p frame counted across the buffered packets (0 past the end).
    [[nodiscard]] float sample(int64_t frame, uint32_t channel) const TANH_NONBLOCKING_FUNCTION;
    void stop(float* const* out,
              uint32_t num_channels,
              uint32_t num_frames) TANH_NONBLOCKING_FUNCTION;

    std::vector<LinkAudioPacket> m_ring;
    size_t m_head = 0;
    size_t m_count = 0;
    bool m_playing = false;
    double m_read_frame = 0.0;  // in the first packet, frames (fractional)
    double m_margin_beats = 0.0;
    uint64_t m_underruns = 0;
    uint64_t m_received = 0;
};

}  // namespace thl::dsp::transport
