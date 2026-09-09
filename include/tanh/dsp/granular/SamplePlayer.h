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

namespace thl::dsp::granular {

// Sample mode: one continuous interpolating head at Velocity (varispeed),
// no grains. Every discontinuity — loop wrap, pitch-bank switch, retrigger
// — parks a copy of the live head in a small pool of outgoing heads to
// ride out an equal-power crossfade while the live head fades back in. A
// second discontinuity inside a fade parks the live head again, with its
// current fade-in gain, instead of dropping the first tail or skipping the
// fade, so a fast pitch sweep or a short loop at high velocity never
// hard-cuts. Nothing here allocates after prepare().
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

    // Render one block. Returns false on a silent early-out (no bank, empty
    // bank, empty region), which also resets the head so a stale one is
    // never painted.
    bool render(const AudioBlock& block, const VoiceParams& params) TANH_NONBLOCKING_FUNCTION;

    // Per block, after the voice has applied its envelope.
    void report_visualization() const;

private:
    struct OutgoingHead {
        double m_head{0.0};
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
    };

    // render() in order: resolve_source, begin_or_switch, refresh_outgoing_banks,
    // then per frame read_live_frame + mix_outgoing_tails + advance_head.
    bool resolve_source(const VoiceParams& params, Source& out) const;
    // First frame, pitch-bank switch or retrigger: place the head, park a tail.
    void begin_or_switch(const Source& src);
    void refresh_outgoing_banks();
    double effective_loop_point(const Source& src) const;
    void read_live_frame(const Source& src, const VoiceParams& params, channel_mixer::Frame& frame);
    void mix_outgoing_tails(const Source& src,
                            const VoiceParams& params,
                            channel_mixer::Frame& frame);
    void advance_head(const Source& src, double loop_point);

    // Equal-power crossfade law, `remaining` frames of m_fade_length left.
    float fade_in_gain(size_t remaining) const;
    float fade_out_gain(size_t remaining) const;
    void start_crossfade(size_t old_sample_index, size_t old_source_channels);

    const SampleReader& m_reader;
    GrainVisualizer& m_viz;

    double m_sample_rate{48000.0};
    size_t m_channels{2};
    size_t m_fade_length{1};

    std::array<OutgoingHead, k_max_outgoing_heads> m_outgoing{};
    double m_play_head{0.0};
    size_t m_fade_remaining{0};  // live head's fade-in counter
    size_t m_sample_index{0};
    size_t m_total_frames{0};  // for viz normalisation after the block
    SampleRegion m_region{};   // last block's region, for the viz mirror
    bool m_started{false};
    bool m_restart{false};
};

}  // namespace thl::dsp::granular
