#pragma once

#include <tanh/core/Exports.h>
#include <tanh/dsp/granular/ChannelMixer.h>
#include <tanh/dsp/granular/GrainVisualizer.h>
#include <tanh/dsp/granular/GranularTypes.h>
#include <tanh/dsp/granular/HeadPolicy.h>
#include <tanh/dsp/granular/SampleReader.h>
#include <tanh/dsp/granular/SampleRegion.h>
#include <tanh/dsp/granular/VoiceParams.h>

#include <cstddef>
#include <random>
#include <vector>

namespace thl::dsp::granular {

// The grain scheduler and renderer shared by the granular modes: a
// pre-allocated pool of Hann-windowed grains triggered at the Density rate,
// each with jittered size / pitch / pan. Where a grain starts is decided by
// the HeadPolicy for the mode the voice asks for; the render maths never
// branches on the mode. Nothing here allocates after construction.
class TANH_API GrainEngine {
public:
    GrainEngine(const SampleReader& reader, GrainVisualizer& viz);

    void prepare(double sample_rate, size_t num_channels);

    // Restart the trigger clock and the mode's head (note-on, mode switch).
    void reset_schedule(EngineMode mode);
    // Silence every grain, telling the visualisation.
    void deactivate_all();

    // Render one block. `playback_elapsed_samples` (since note-on) drives the
    // temperature ramp.
    void render(const AudioBlock& block,
                const VoiceParams& params,
                EngineMode mode,
                size_t playback_elapsed_samples);

private:
    // The pitch bank this block reads from.
    struct Bank {
        size_t m_index{0};
        size_t m_frames{0};
        size_t m_channels{1};
    };

    HeadPolicy& head_for(EngineMode mode);

    // render() in order: update_trigger_rate, select_bank, then per frame
    // trigger_due_grain + mix_grains_frame, then report_visualization.
    void update_trigger_rate(float density);
    Bank select_bank(int raw_sample_index);
    void trigger_due_grain(const Bank& bank,
                           const VoiceParams& params,
                           HeadPolicy& head,
                           size_t playback_elapsed_samples);
    // Window every active grain at its current position, read the source,
    // pan and accumulate into `frame`; grains that ended are retired.
    void mix_grains_frame(const Bank& bank, const VoiceParams& params, channel_mixer::Frame& frame);
    void report_visualization(size_t num_frames, const Bank& bank);

    // trigger_grain() in order: find_free_grain, jitter size and velocity,
    // ask the head where, fit_to_region, start_grain.
    void trigger_grain(const Bank& bank,
                       const VoiceParams& params,
                       HeadPolicy& head,
                       size_t playback_elapsed_samples);
    Grain* find_free_grain();
    // Truncate a grain that would overshoot the region end. Returns the
    // frames it covers in the source (0 = nothing fits); `grain_size` is
    // shrunk to match.
    static size_t fit_to_region(long start,
                                const SampleRegion& region,
                                float velocity,
                                size_t& grain_size);
    void start_grain(Grain& grain, long start, size_t grain_size, float velocity, size_t bank);
    size_t calculate_grain_size(float grain_size_param, float temperature);
    float calculate_velocity(float velocity, float temperature);
    float apply_temperature_ramp(float temperature, size_t playback_elapsed_samples) const;

    const SampleReader& m_reader;
    GrainVisualizer& m_viz;
    LoopScanHead m_loop_head;
    PositionSprayHead m_position_head;

    double m_sample_rate{48000.0};
    size_t m_channels{2};

    std::vector<Grain> m_grains;
    size_t m_next_grain_time{0};
    size_t m_min_grain_interval{100};
    size_t m_current_bank{0};

    std::mt19937 m_random_generator;
    std::uniform_real_distribution<float> m_uni_dist;
};

}  // namespace thl::dsp::granular
