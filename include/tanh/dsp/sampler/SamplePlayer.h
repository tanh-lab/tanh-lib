#pragma once

#include <tanh/core/Exports.h>
#include <tanh/dsp/sampler/LoopCrossfade.h>
#include <tanh/dsp/sampler/LoopMarkers.h>
#include <tanh/dsp/sampler/LoopRegion.h>
#include <tanh/dsp/sampler/SampleView.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <cstddef>
#include <span>

namespace thl::dsp::sampler {

/// Tuning of a SamplePlayer, fixed at prepare().
struct PlayerSettings {
    /// Crossfade at a loop wrap, a source switch, a retrigger (seconds). A
    /// loop wrap shortens it to at most half the loop body.
    float m_crossfade_seconds{0.010f};
    /// Shortest Start -> End span and loop body (seconds).
    float m_min_loop_seconds{0.002f};
    /// How far snap moves a marker to reach a zero crossing (seconds).
    float m_snap_radius_seconds{0.005f};
    /// Playback-rate bounds; set_speed() is clamped into them.
    float m_min_speed{0.01f};
    float m_max_speed{8.0f};
};

/**
 * @class SamplePlayer
 * @brief A varispeed sample player / looper: one continuous interpolating
 * play head over a region of a sample, click-free at every discontinuity.
 *
 * @par Region
 * Start / End / Loop come from set_markers() (normalised, with a minimum
 * span and optional zero-crossing snap, see LoopMarkers) or from
 * set_region() (an exact region, e.g. a slice). End before Start plays
 * backwards; moving either marker never moves the audible head.
 *
 * @par Loop
 * With loop on, the head wraps from End to Loop through a loop crossfade
 * (see LoopFadePlan): over the last stretch before End the head blends into
 * the audio before Loop, or, without room there, fades out a copy running
 * past End after the wrap. Equal-power, or equal-gain when the join is in
 * phase (snapped). While a loop fade runs the region is held; marker moves
 * land at the wrap. With loop off the head plays Start -> End once and then
 * reports finished().
 *
 * @par Sources
 * render() takes a span of alternative sources (e.g. pitch-shifted copies,
 * round-robins) and the index to play. Sources must be equal-length and
 * time-aligned: switching keeps the head position and crossfades. A source
 * that becomes empty reads as silence.
 *
 * @par Discontinuities
 * A source switch, a retrigger, End moved behind the head or a one-shot
 * reaching End park a copy of the head in a small pool of outgoing heads
 * that fade out while the live head fades back in. A second discontinuity
 * inside a fade parks the head again with its current gain.
 *
 * @par Real-Time Safety
 * prepare() allocates. Everything else is real-time safe and allocation
 * free; the setters are meant to be called once per block before render()
 * on the audio thread.
 *
 * @code
 * sampler::SamplePlayer player;
 * player.prepare(48000.0);
 * player.note_on();
 * const auto source = sampler::SampleView::of(buffer);
 * player.set_markers(0.25f, 0.75f, 0.5f);
 * player.render({&source, 1}, 0, outputs, 2, 256);
 * @endcode
 */
class TANH_API SamplePlayer {
public:
    SamplePlayer() = default;
    SamplePlayer(const SamplePlayer&) = delete;
    SamplePlayer& operator=(const SamplePlayer&) = delete;

    /// Build the fade table and derive frame lengths. Allocates; not RT-safe.
    void prepare(double sample_rate, const PlayerSettings& settings = {});

    /// Stop and forget the head and every tail.
    void reset();

    /// Note-on: a head that is still sounding crossfades back to the region
    /// start on the next render; a silent one restarts cold.
    void note_on();

    /// Normalised Start / End / Loop, resolved per block with the minimum
    /// span and (if enabled) snap. Replaces a region set by set_region().
    void set_markers(float start, float end, float loop) {
        m_start = start;
        m_end = end;
        m_loop_marker = loop;
        m_use_region = false;
    }

    /// An exact region in frames (no minimum span, no snap, never in phase).
    /// Replaces the markers until set_markers() is called again.
    void set_region(const LoopRegion& region) {
        m_explicit_region = region;
        m_use_region = true;
    }

    /// Playback rate (1 = original speed and pitch), clamped to the settings.
    void set_speed(float speed) { m_speed = speed; }
    /// Loop (true) or play once (false).
    void set_loop(bool loop) { m_loop = loop; }
    /// Snap markers to upward zero crossings (see LoopMarkers).
    void set_snap(bool snap) { m_snap = snap; }

    /**
     * @brief The audio behind the sources changed in place or was replaced.
     *
     * Snapped markers are cached against a source's address and length; a
     * new sample of the same length can land at the same address. Call this
     * whenever the host loads new audio (it is cheap: the next render
     * re-resolves the markers).
     */
    void sources_changed() { m_markers.invalidate(); }

    /**
     * @brief Render one block.
     *
     * Writes source channel c into `out[c]` for c < num_channels; a mono
     * source feeds every channel, channels past a multichannel source's own
     * are silent. The output is overwritten, not mixed into.
     *
     * @return false on a silent early-out (no source, empty source, empty
     *         region), which also resets the player.
     */
    bool render(std::span<const SampleView> sources,
                size_t source,
                float* const* out,
                size_t num_channels,
                size_t num_frames) TANH_NONBLOCKING_FUNCTION;

    /// The head is playing (between the first render after note_on() and
    /// reset() / an early-out).
    [[nodiscard]] bool started() const { return m_started; }
    /// The render that just ran started the head.
    [[nodiscard]] bool just_started() const { return m_just_started; }
    /// One-shot reached End: its tail is fading, the head renders silence.
    /// note_on() / reset() clear it.
    [[nodiscard]] bool finished() const { return m_finished; }

    /// Region of the last render.
    [[nodiscard]] const LoopRegion& region() const { return m_region; }
    /// Frames of the source the last render played.
    [[nodiscard]] size_t source_frames() const { return m_total_frames; }
    /// Clamped playback rate of the last render.
    [[nodiscard]] float speed() const { return m_block_speed; }
    /// Head as a fraction of the source, kept inside the region (a finished
    /// one-shot parks past End). 0 when nothing played.
    [[nodiscard]] float normalized_position() const;

private:
    struct OutgoingHead {
        double m_head{0.0};
        // Parked inside a loop fade: the tail keeps the blend it had, a
        // second read m_copy_offset frames behind at m_copy_gain.
        double m_copy_offset{0.0};
        float m_head_gain{1.0f};
        float m_copy_gain{0.0f};
        size_t m_source{0};
        bool m_source_valid{false};  // false = source gone, reads silence
        size_t m_remaining{0};       // 0 = slot free
        float m_gain{1.0f};          // live head's fade-in gain when parked
    };
    static constexpr size_t k_max_outgoing_heads = 3;

    using Frame = std::array<float, k_max_channels>;

    // What this block plays from.
    struct Block {
        std::span<const SampleView> m_sources;
        size_t m_source{0};
        size_t m_frames{0};
        size_t m_channels{0};  // channels rendered
        LoopRegion m_region{};
        float m_speed{1.0f};
        bool m_joined{false};
    };

    // This block's loop wrap plus whether the join is in phase.
    struct LoopPlan {
        LoopFadePlan m_fade{};
        bool m_in_phase{false};
    };

    // The loop fade of the current pass: blend weights for this frame.
    struct LoopFade {
        // The copy reads this far behind the head: the loop body before End,
        // minus the body after the wrap (it reads past End).
        double m_copy_offset{0.0};
        float m_head_gain{1.0f};
        float m_copy_gain{0.0f};
    };

    bool resolve_block(std::span<const SampleView> sources,
                       size_t source,
                       size_t num_channels,
                       Block& out);
    void begin_or_switch(const Block& blk);
    void rebase_to_region(const LoopRegion& region);
    void refresh_outgoing_sources(const Block& blk);
    LoopPlan plan_loop(const Block& blk) const;
    void update_loop_fade(const Block& blk, const LoopPlan& plan);
    void open_fade(bool after_wrap, double to, double copy_offset, bool in_phase);
    void read_live_frame(const Block& blk, Frame& frame);
    void read_blend(const Block& blk,
                    const SampleView& view,
                    double head,
                    double copy_offset,
                    float head_gain,
                    float copy_gain,
                    Frame& frame) const;
    void mix_outgoing_tails(const Block& blk, Frame& frame);
    void advance_head(const Block& blk, const LoopPlan& plan);
    void start_crossfade(size_t old_source, bool old_source_valid);

    // Controls
    float m_start{0.0f};
    float m_end{1.0f};
    float m_loop_marker{0.0f};
    LoopRegion m_explicit_region{};
    bool m_use_region{false};
    float m_speed{1.0f};
    bool m_loop{true};
    bool m_snap{false};

    // Settings
    double m_sample_rate{48000.0};
    float m_min_speed{0.01f};
    float m_max_speed{8.0f};
    size_t m_min_span{1};
    FadeCurve m_curve;
    LoopMarkers m_markers;

    // State
    std::array<OutgoingHead, k_max_outgoing_heads> m_outgoing{};
    double m_play_head{0.0};
    size_t m_fade_remaining{0};  // live head's fade-in counter
    size_t m_source{0};
    size_t m_total_frames{0};
    float m_block_speed{1.0f};
    LoopRegion m_region{};
    // Loop fade of this pass, from m_fade_from to m_fade_to (virtual): before
    // End, or after the wrap (m_fade_after_wrap).
    bool m_fade_open{false};
    bool m_fade_after_wrap{false};
    bool m_fade_in_phase{false};
    double m_fade_from{0.0};
    double m_fade_to{0.0};
    LoopFade m_loop_fade{};
    bool m_started{false};
    bool m_just_started{false};
    bool m_restart{false};
    bool m_finished{false};
};

}  // namespace thl::dsp::sampler
