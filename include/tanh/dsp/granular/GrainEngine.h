#pragma once

#include <tanh/core/Exports.h>
#include <tanh/dsp/granular/ChannelMixer.h>
#include <tanh/dsp/granular/GrainVisualizer.h>
#include <tanh/dsp/granular/GranularTypes.h>
#include <tanh/dsp/granular/HeadPolicy.h>
#include <tanh/dsp/sampler/LoopRegion.h>
#include <tanh/dsp/sampler/SampleView.h>
#include <tanh/dsp/utils/MorphWindow.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>

namespace thl::dsp::granular {

/// Where grains start (see HeadPolicy).
enum class HeadMode {
    Spray,  ///< Around a fixed Position, within a Spray / Tilt window.
    Scan,   ///< A scan head travelling Start -> End at 1x, re-entering at Loop.
};

/// One block of grain controls. Normalised controls are [0, 1].
struct GrainParams {
    float m_size{0.5f};     ///< Grain length between k_min / k_max_grain_size.
    float m_density{0.5f};  ///< Trigger rate between k_min / k_max_grain_rate.
    float m_pitch{1.0f};    ///< Read rate per grain (1 = original pitch).
    float m_temperature_size{0.0f};
    float m_temperature_position{0.0f};
    float m_temperature_pitch{0.0f};
    float m_window_shape{4.0f};  ///< utils::MorphWindow shape (4 = Hann).
    float m_window_tilt{0.0f};
    ChannelMode m_channel_mode{ChannelMode::MonoToStereo};
    float m_spread{0.0f};  ///< Per-grain pan spread.
    HeadInputs m_head{};
};

/**
 * @class GrainEngine
 * @brief A granular synthesiser over a sample: a pre-allocated pool of
 * windowed grains triggered at the Density rate, each with jittered size,
 * pitch and pan.
 *
 * Where each grain starts is decided by the HeadPolicy of the HeadMode the
 * caller asks for; the render maths never branches on it.
 *
 * @par Sources
 * render() takes a span of alternative, equal-length sources (e.g. a
 * pitch::PitchBank) and the index to trigger new grains from. A grain keeps
 * reading the source it started on; switching retriggers at once.
 *
 * @par Visualisation
 * With a GrainVisualizer set, grain starts, updates (rate-limited) and ends
 * are reported.
 *
 * @par Real-Time Safety
 * prepare() and set_visualizer() are setup calls. render() and the
 * schedule calls are real-time safe; nothing allocates after construction.
 */
class TANH_API GrainEngine {
public:
    GrainEngine();
    GrainEngine(const GrainEngine&) = delete;
    GrainEngine& operator=(const GrainEngine&) = delete;

    void prepare(double sample_rate, size_t num_channels);

    /// Report to `visualizer` (nullptr: report nothing). Must outlive the
    /// engine's use of it.
    void set_visualizer(GrainVisualizer* visualizer) { m_viz = visualizer; }

    /// Reseed the jitter generator (reproducible grain streams).
    void seed(uint32_t value) { m_random_generator.seed(value); }

    /// Restart the trigger clock and the mode's head (note-on, mode switch).
    void reset_schedule(HeadMode mode);
    /// Silence every grain.
    void deactivate_all();

    /// One pass (loop off) is over: the head reached End, nothing triggers
    /// any more and every grain has ended. reset_schedule() clears it.
    bool finished(HeadMode mode) const { return head_for(mode).finished() && !any_grain_active(); }
    bool any_grain_active() const;

    /**
     * @brief Render one block, overwriting all `num_channels` of `out`
     * (channels past the prepared count are written silent).
     * @param elapsed_samples Samples since note-on: drives the temperature
     *        ramp (position temperature eases in over the first second).
     */
    void render(std::span<const sampler::SampleView> sources,
                size_t source,
                const GrainParams& params,
                HeadMode mode,
                float* const* out,
                size_t num_channels,
                size_t num_frames,
                size_t elapsed_samples) TANH_NONBLOCKING_FUNCTION;

private:
    // The source this block triggers from.
    struct Source {
        size_t m_index{0};
        size_t m_frames{0};
        size_t m_channels{1};
        bool m_valid{false};
    };

    HeadPolicy& head_for(HeadMode mode);
    const HeadPolicy& head_for(HeadMode mode) const;

    // render() in order: update_trigger_rate, select_source (+ retrigger on a
    // switch), region from the head, then per frame trigger_due_grain +
    // mix, then report_visualization.
    void update_trigger_rate(float density);
    static Source select_source(std::span<const sampler::SampleView> sources, size_t index);
    void trigger_due_grain(const Source& src,
                           const LoopRegion& region,
                           const GrainParams& params,
                           HeadPolicy& head,
                           size_t elapsed_samples);
    // Per block, per grain: resolve its source and its pan to gains, so the
    // frame loop does no validation and no switch.
    void prime_grain(size_t index,
                     std::span<const sampler::SampleView> sources,
                     const GrainParams& params);
    template <ChannelMode M>
    void render_frames(float* const* out,
                       size_t num_channels,
                       size_t num_frames,
                       const GrainParams& params,
                       const Source& src,
                       const LoopRegion& region,
                       HeadPolicy& head,
                       size_t elapsed_samples);
    void report_visualization(size_t num_frames, const Source& src);

    // trigger_grain() in order: find_free_grain, jitter size and pitch, ask
    // the head where, fit_to_region, start_grain.
    void trigger_grain(const Source& src,
                       const LoopRegion& region,
                       const GrainParams& params,
                       HeadPolicy& head,
                       size_t elapsed_samples);
    Grain* find_free_grain();
    // Truncate a grain that would overshoot the region end. Returns the
    // frames it covers in the source (0 = nothing fits); `grain_size` is
    // shrunk to match.
    static size_t fit_to_region(FramePos start,
                                const LoopRegion& region,
                                float velocity,
                                size_t& grain_size);
    void start_grain(Grain& grain,
                     FramePos start,
                     size_t grain_size,
                     float velocity,
                     size_t source,
                     const GrainParams& params);
    size_t calculate_grain_size(float grain_size_param, float temperature);
    float calculate_velocity(float velocity, float temperature);
    float apply_temperature_ramp(float temperature, size_t elapsed_samples) const;

    GrainVisualizer* m_viz{nullptr};
    const utils::MorphWindow& m_window{utils::MorphWindow::shared()};
    LoopScanHead m_loop_head;
    PositionSprayHead m_position_head;

    double m_sample_rate{48000.0};
    size_t m_channels{2};

    std::array<Grain, k_max_grains> m_grains{};
    std::array<sampler::SampleView, k_max_grains> m_grain_sources{};  // valid within render()
    std::span<const sampler::SampleView> m_block_sources;             // valid within render()
    size_t m_next_grain_time{0};
    size_t m_min_grain_interval{100};
    size_t m_current_source{0};

    std::mt19937 m_random_generator;
    std::uniform_real_distribution<float> m_uni_dist;
};

}  // namespace thl::dsp::granular
