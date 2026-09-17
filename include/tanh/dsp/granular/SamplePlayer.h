#pragma once

#include <tanh/core/Exports.h>
#include <tanh/dsp/granular/ChannelMixer.h>
#include <tanh/dsp/granular/GrainVisualizer.h>
#include <tanh/dsp/granular/GranularTypes.h>
#include <tanh/dsp/granular/SampleReader.h>
#include <tanh/dsp/granular/SampleRegion.h>
#include <tanh/dsp/granular/VoiceParams.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <cstddef>
#include <vector>

namespace thl::dsp::granular {

// Sample mode: one continuous interpolating head at Velocity (varispeed),
// no grains.
//
// Loop wrap, as a hardware sampler's loop crossfade: over the last stretch
// before End the head is blended into the audio just before Loop, so at End
// it is already reading what follows Loop and the jump is silent. When the
// sample has less room before Loop than past End (Loop near the sample's
// start), the fade runs after the wrap instead: the head starts at Loop
// under a copy that carries on past End and fades out. Either fade is at
// most half the loop body. Equal-power, or equal-gain when End and Loop
// both sit on zero crossings (the reads are then in phase and equal-power
// would bulge). While a fade runs the region is held; marker moves land at
// the wrap.
//
// Every other discontinuity — pitch-bank switch, retrigger, End moved
// behind the head — parks a copy of the live head in a small pool of
// outgoing heads to ride out a crossfade while the live head fades back in.
// A second discontinuity inside a fade parks the live head again, with its
// current fade-in gain, instead of dropping the first tail or skipping the
// fade, so a fast pitch sweep never hard-cuts. Nothing here allocates after
// prepare().
class TANH_API SamplePlayer {
public:
    SamplePlayer(const SampleReader& reader, GrainVisualizer& viz);
    // Holds references into its owner: never copied.
    SamplePlayer(const SamplePlayer&) = delete;
    SamplePlayer& operator=(const SamplePlayer&) = delete;

    void prepare(double sample_rate, size_t num_channels);

    // Stop and forget the head, telling the visualisation if it was sounding.
    void reset();

    // Note-on: a head that is still sounding (release tail, legato) crossfades
    // back to the region start on its next render; a silent one restarts cold.
    void note_on();

    bool is_started() const { return m_started; }

    // One-shot (VoiceParams::m_loop == false): the head reached End, its tail
    // is fading out and the live head renders silence. note_on / reset clear
    // it. The voice releases its envelope on this.
    bool finished() const { return m_finished; }

    // Render one block. Returns false on a silent early-out (no bank, empty
    // bank, empty region), which also resets the head so a stale one is
    // never painted.
    bool render(const AudioBlock& block, const VoiceParams& params) TANH_NONBLOCKING_FUNCTION;

    // Per block, after the voice has applied its envelope.
    void report_visualization() const;

private:
    struct OutgoingHead {
        double m_head{0.0};
        // Parked inside a loop fade: the tail keeps the blend it had, a
        // second read m_copy_offset frames behind at m_copy_gain.
        double m_copy_offset{0.0};
        float m_head_gain{1.0f};
        float m_copy_gain{0.0f};
        size_t m_sample_index{0};
        size_t m_source_channels{0};  // 0 = bank gone, reads silence
        size_t m_remaining{0};        // 0 = slot free
        float m_gain{1.0f};           // live head's fade-in gain when parked
    };
    static constexpr size_t k_max_outgoing_heads = 3;

    // What this block plays from.
    struct Source {
        size_t m_bank{0};
        size_t m_frames{0};
        size_t m_channels{0};
        SampleRegion m_region{};
        float m_velocity{1.0f};  // varispeed, bounded
        bool m_joined{false};    // End and Loop were snapped onto zero crossings
    };

    // This block's loop wrap: the effective Loop, the fade lengths before
    // End and after the wrap (source frames, at most one non-zero) and
    // whether the join is in phase.
    struct LoopPlan {
        double m_point{0.0};
        double m_pre{0.0};
        double m_post{0.0};
        bool m_in_phase{false};
    };

    // Start / End / Loop in frames, before and after snap + minimum span.
    struct Markers {
        size_t m_start{0};
        size_t m_end{0};
        size_t m_loop{0};
        bool operator==(const Markers&) const = default;
    };

    // The loop fade of the current pass: blend weights for this frame.
    struct LoopFade {
        // The copy reads this far behind the head: the loop body before End,
        // minus the body after the wrap (it reads past End).
        double m_copy_offset{0.0};
        float m_head_gain{1.0f};
        float m_copy_gain{0.0f};
    };

    // render() in order: resolve_source, begin_or_switch, refresh_outgoing_banks,
    // then per frame read_live_frame + mix_outgoing_tails + advance_head.
    bool resolve_source(const VoiceParams& params, Source& out);
    // Unsliced markers: minimum span, then Loop Snap. Cached on the raw input.
    // `joined`: End and Loop both landed on zero crossings.
    SampleRegion resolve_markers(const VoiceParams& params,
                                 size_t bank,
                                 size_t total_frames,
                                 bool& joined);
    // Nearest upward zero crossing to `target` in [lo, hi] within the snap
    // radius, else `target` clamped to [lo, hi].
    size_t snap_to_crossing(size_t bank, size_t target, size_t lo, size_t hi) const;
    bool upward_crossing_at(size_t bank, size_t frame) const;
    // First frame, pitch-bank switch or retrigger: place the head, park a tail.
    void begin_or_switch(const Source& src);
    // Keep every head on its physical frame when the region moved (a reversed
    // region re-mirrors on each Start / End change).
    void rebase_to_region(const SampleRegion& region);
    void refresh_outgoing_banks();
    double effective_loop_point(const Source& src) const;
    LoopPlan plan_loop(const Source& src) const;
    // Opens the fade before End when the head enters it, closes the fade
    // after the wrap when the head leaves it, and sets m_loop_fade's blend
    // for this frame; closes either (parking the blend) if Loop was switched
    // off.
    void update_loop_fade(const Source& src, const VoiceParams& params, const LoopPlan& plan);
    void open_fade(bool after_wrap, double to, double copy_offset, bool in_phase);
    void read_live_frame(const Source& src, const VoiceParams& params, channel_mixer::Frame& frame);
    void read_blend(const Source& src,
                    const VoiceParams& params,
                    size_t bank,
                    size_t source_channels,
                    double head,
                    double copy_offset,
                    float head_gain,
                    float copy_gain,
                    channel_mixer::Frame& frame) const;
    void mix_outgoing_tails(const Source& src,
                            const VoiceParams& params,
                            channel_mixer::Frame& frame);
    void advance_head(const Source& src, const VoiceParams& params, const LoopPlan& plan);
    // Quarter-sine gain at fade position t in [0, 1] (0 -> 0, 1 -> 1).
    float fade_curve_at(double t) const;

    // Equal-power crossfade law, `remaining` frames of m_fade_length left.
    // Both gains read m_fade_curve, built once in prepare(): the fade is a
    // quarter sine, and cos(t * pi/2) == sin((1 - t) * pi/2), so one table
    // indexed by the integer counter serves both directions exactly — no
    // interpolation, and no sin/cos on the audio thread (a block fading the
    // live head plus four tails wanted five transcendentals per frame).
    float fade_in_gain(size_t remaining) const;
    float fade_out_gain(size_t remaining) const;
    // Parks the live head (with the loop fade's blend, if one is open).
    void start_crossfade(size_t old_sample_index, size_t old_source_channels);

    const SampleReader& m_reader;
    GrainVisualizer& m_viz;

    double m_sample_rate{48000.0};
    size_t m_channels{2};
    size_t m_fade_length{1};
    size_t m_min_span{1};     // k_player_min_loop_duration in frames
    size_t m_snap_radius{0};  // k_player_snap_radius in frames

    // sin(k / m_fade_length * pi/2) for k in [0, m_fade_length]. Sized in
    // prepare(), so every index the fade counters produce is in range.
    std::vector<float> m_fade_curve{0.0f, 1.0f};

    std::array<OutgoingHead, k_max_outgoing_heads> m_outgoing{};
    double m_play_head{0.0};
    size_t m_fade_remaining{0};  // live head's fade-in counter
    size_t m_sample_index{0};
    size_t m_total_frames{0};  // for viz normalisation after the block
    SampleRegion m_region{};   // last block's region, for the viz mirror
    // Loop fade of this pass, from m_fade_from to m_fade_to (virtual): before
    // End, or after the wrap (m_fade_after_wrap).
    bool m_fade_open{false};
    bool m_fade_after_wrap{false};
    bool m_fade_in_phase{false};
    double m_fade_from{0.0};
    double m_fade_to{0.0};
    LoopFade m_loop_fade{};  // blend of the frame being rendered
    // resolve_markers cache: recomputed only when an input changes.
    struct MarkerCache {
        Markers m_raw{};
        Markers m_resolved{};
        const float* m_data{nullptr};  // the bank's buffer, so a reload misses
        size_t m_total{0};
        bool m_snap{false};
        bool m_joined{false};
        bool m_valid{false};
    } m_marker_cache;
    bool m_started{false};
    bool m_restart{false};
    bool m_finished{false};
};

}  // namespace thl::dsp::granular
