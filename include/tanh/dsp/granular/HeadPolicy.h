#pragma once

#include <tanh/dsp/granular/SampleRegion.h>
#include <tanh/dsp/granular/VoiceParams.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <random>

namespace thl::dsp::granular {

// pick_start's "trigger nothing" answer (a one-shot scan that is over).
inline constexpr FramePos k_no_grain = -1;

// One draw in [0, 1).
inline float unit_random(std::mt19937& rng) {
    return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
}

// Where the grain scheduler's next grain comes from. The GrainEngine has no
// idea which policy it runs; the voice hands it one per engine mode. Both
// decisions a mode makes live here: the region (the slicer seam — a slice
// source overrides region()) and the start of the next grain. Called once
// per grain trigger, never per sample, so virtual dispatch is free.
class HeadPolicy {
public:
    virtual ~HeadPolicy() = default;

    virtual SampleRegion region(size_t total_frames, const VoiceParams& params) const = 0;

    // Next grain's start, absolute in source frames, or negative when no
    // grain should be triggered. `temperature` is the (ramped) position
    // temperature; `interval` the frames between triggers.
    virtual FramePos pick_start(const SampleRegion& region,
                                float temperature,
                                size_t interval,
                                const VoiceParams& params,
                                std::mt19937& rng) = 0;

    virtual void reset() = 0;

    // One-shot: the head reached End with Loop off. No grain is triggered
    // once this is set; the voice releases. reset() clears it.
    virtual bool finished() const { return false; }

protected:
    // Temperature jitter around `start` (relative to the region), wrapped
    // into the region however many times it takes, then made absolute.
    static FramePos jitter_and_wrap(FramePos start,
                                    float temperature,
                                    const SampleRegion& region,
                                    std::mt19937& rng) {
        auto const max_position = static_cast<FramePos>(region.size());
        float rand_value = unit_random(rng);      // [0, 1)
        rand_value = (rand_value - 0.5f) * 2.0f;  // [-1, 1)
        rand_value *= temperature;
        start += static_cast<FramePos>(rand_value * static_cast<float>(max_position));
        while (start >= max_position) { start -= max_position; }
        while (start < 0) { start += max_position; }
        return start + static_cast<FramePos>(region.m_start);
    }
};

// GranularLoop: a scan head runs Start -> End at 1x and restarts at Loop.
// Density and the head advance cancel (the head moves one trigger interval
// per trigger), so the scan is always 1x regardless of density.
class LoopScanHead final : public HeadPolicy {
public:
    SampleRegion region(size_t total_frames, const VoiceParams& p) const override {
        if (p.m_slicer) {
            return SampleRegion::from_slices(p.m_sample_start,
                                             p.m_sample_end,
                                             p.m_slices,
                                             total_frames);
        }
        return SampleRegion::from_normalized(p.m_sample_start,
                                             p.m_sample_end,
                                             p.m_sample_loop_point,
                                             total_frames);
    }

    FramePos pick_start(const SampleRegion& region,
                        float temperature,
                        size_t interval,
                        const VoiceParams& params,
                        std::mt19937& rng) override {
        auto const max_position = static_cast<FramePos>(region.size());
        if (max_position <= 0) { return static_cast<FramePos>(region.m_start); }

        auto const loop = static_cast<FramePos>(region.m_loop_point - region.m_start);
        FramePos start = m_sequential_position;
        // The region shrank under the head (End dragged or modulated below
        // it): restart at Loop and keep scanning from there. Returning Loop
        // without advancing would pin every following grain to it. With
        // Loop off the scan is over instead.
        if (start >= max_position) {
            if (!params.m_loop) {
                m_finished = true;
                return k_no_grain;
            }
            start = loop;
        }
        FramePos const picked = jitter_and_wrap(start, temperature, region, rng);

        // Scanner traverses the full region (start -> end), then restarts at
        // loop_point — or, one-shot, stops triggering.
        m_sequential_position = start + static_cast<FramePos>(interval);
        if (m_sequential_position >= max_position) {
            if (params.m_loop) {
                m_sequential_position = loop;
            } else {
                m_finished = true;
            }
        }
        return picked;
    }

    void reset() override {
        m_sequential_position = 0;
        m_finished = false;
    }

    bool finished() const override { return m_finished; }

private:
    FramePos m_sequential_position{0};
    bool m_finished{false};
};

// GranularPosition: the head is parked on Position (absolute in the sample,
// no travelling head, Start / Loop / End dormant). Each grain is drawn
// uniformly from the Spray window around it — a deliberate width, biased
// before (-) or after (+) the point by Tilt: the symmetric [-1, 1] window
// slides to [tilt - 1, tilt + 1] and is clipped. A window past either edge
// of the sample is clipped to the edge (the waveform band shows exactly
// this), never wrapped. Position temperature is applied on top exactly as
// in Loop mode, so it can leave the window.
//
// Slicing: the same formula in slice steps. Position picks a slice k, Spray
// is a width S in slices (100 % = every slice), the window [k + lo·S,
// k + hi·S) is anchored at the slice's start and Tilt leans it: Tilt right
// gives the chosen slice and the next ones, Tilt left the slices before it,
// centred both. Past the last slice it wraps to the first. Spray 0 puts
// every grain on the slice's first frame. Temperature jitters in steps and
// wraps inside the window (inside the chosen slice at Spray 0), so it never
// reaches an unchosen slice. The step is then mapped back to time through
// the boundaries, so short and long slices are picked equally often.
class PositionSprayHead final : public HeadPolicy {
public:
    SampleRegion region(size_t total_frames, const VoiceParams& /*params*/) const override {
        return SampleRegion::full(total_frames);
    }

    FramePos pick_start(const SampleRegion& region,
                        float temperature,
                        size_t /*interval*/,
                        const VoiceParams& p,
                        std::mt19937& rng) override {
        auto const max_position = static_cast<FramePos>(region.size());
        if (max_position <= 0) { return static_cast<FramePos>(region.m_start); }
        if (p.m_slicer) { return pick_slice_start(region, temperature, p, rng); }

        float const window_lo = std::max(-1.0f, p.m_tilt - 1.0f);
        float const window_hi = std::min(1.0f, p.m_tilt + 1.0f);
        float const u = unit_random(rng);  // [0, 1)
        float const spray_offset = (window_lo + u * (window_hi - window_lo)) * p.m_spray;
        FramePos const start =
            std::clamp(static_cast<FramePos>(p.m_position * static_cast<float>(max_position - 1)) +
                           static_cast<FramePos>(spray_offset * static_cast<float>(max_position)),
                       FramePos{0},
                       max_position - 1);
        return jitter_and_wrap(start, temperature, region, rng);
    }

    void reset() override {}

private:
    static double wrap_into(double v, double lo, double hi) {
        double const width = hi - lo;
        if (width <= 0.0) { return lo; }
        v = std::fmod(v - lo, width);
        if (v < 0.0) { v += width; }
        return lo + v;
    }

    FramePos pick_slice_start(const SampleRegion& region,
                              float temperature,
                              const VoiceParams& p,
                              std::mt19937& rng) {
        auto const n = static_cast<double>(p.m_slices.m_count);
        auto const k = static_cast<double>(p.m_slices.slice_of_step(p.m_position));
        double const spray = static_cast<double>(p.m_spray) * n;  // width in slices
        double const tilt = static_cast<double>(p.m_tilt);
        double window_lo = k + std::max(-1.0, tilt - 1.0) * spray;
        double window_hi = k + std::min(1.0, tilt + 1.0) * spray;
        double v = k;
        if (spray > 0.0 && window_hi > window_lo) {
            v = window_lo + static_cast<double>(unit_random(rng)) * (window_hi - window_lo);
        } else {
            window_lo = k;
            window_hi = k + 1.0;
        }
        // Temperature: ± temperature·N steps, wrapped inside the window.
        double const jitter = (static_cast<double>(unit_random(rng)) - 0.5) * 2.0 *
                              static_cast<double>(temperature) * n;
        v = wrap_into(v + jitter, window_lo, window_hi);
        v = wrap_into(v, 0.0, n);  // past the last slice -> the first
        // Same rounding as SliceMap::boundary_frame, so Spray 0 lands on the
        // very frame Sample / Loop mode's region starts on.
        auto const frame =
            static_cast<FramePos>(std::llround(static_cast<double>(p.m_slices.step_to_norm(v)) *
                                               static_cast<double>(max_frames(region))));
        return std::clamp(frame, FramePos{0}, max_frames(region) - 1) +
               static_cast<FramePos>(region.m_start);
    }

    static FramePos max_frames(const SampleRegion& region) {
        return std::max(FramePos{1}, static_cast<FramePos>(region.size()));
    }
};

}  // namespace thl::dsp::granular
