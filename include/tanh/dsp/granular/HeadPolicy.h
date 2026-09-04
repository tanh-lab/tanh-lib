#pragma once

#include <tanh/dsp/granular/SampleRegion.h>
#include <tanh/dsp/granular/VoiceParams.h>

#include <algorithm>
#include <cstddef>
#include <random>

namespace thl::dsp::granular {

// Where the grain scheduler's next grain comes from. The GrainEngine has no
// idea which policy it runs; the voice hands it one per engine mode. Both
// decisions a mode makes live here: the region (the slicer seam — a slice
// source overrides region()) and the start of the next grain. Called once
// per grain trigger, never per sample, so virtual dispatch is free.
class HeadPolicy {
public:
    virtual ~HeadPolicy() = default;

    virtual SampleRegion region(size_t total_frames, const VoiceParams& params) const = 0;

    // Next grain's start, absolute in source frames. `temperature` is the
    // (ramped) position temperature; `interval` the frames between triggers.
    virtual long pick_start(const SampleRegion& region,
                            float temperature,
                            size_t interval,
                            const VoiceParams& params,
                            std::mt19937& rng) = 0;

    virtual void reset() = 0;

protected:
    // Temperature jitter around `start` (relative to the region), wrapped
    // into the region however many times it takes, then made absolute.
    static long jitter_and_wrap(long start,
                                float temperature,
                                const SampleRegion& region,
                                std::mt19937& rng) {
        long const max_position = static_cast<long>(region.size());
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        float rand_value = dist(rng);             // [0, 1)
        rand_value = (rand_value - 0.5f) * 2.0f;  // [-1, 1)
        rand_value *= temperature;
        start += static_cast<long>(rand_value * static_cast<float>(max_position));
        while (start >= max_position) { start -= max_position; }
        while (start < 0) { start += max_position; }
        return start + static_cast<long>(region.m_start);
    }
};

// GranularLoop: a scan head runs Start -> End at 1x and restarts at Loop.
// Density and the head advance cancel (the head moves one trigger interval
// per trigger), so the scan is always 1x regardless of density.
class LoopScanHead final : public HeadPolicy {
public:
    SampleRegion region(size_t total_frames, const VoiceParams& p) const override {
        return SampleRegion::from_normalized(p.m_sample_start,
                                             p.m_sample_end,
                                             p.m_sample_loop_point,
                                             total_frames);
    }

    long pick_start(const SampleRegion& region,
                    float temperature,
                    size_t interval,
                    const VoiceParams& /*params*/,
                    std::mt19937& rng) override {
        long const max_position = static_cast<long>(region.size());
        if (max_position <= 0) { return static_cast<long>(region.m_start); }

        long const loop = static_cast<long>(region.m_loop_point - region.m_start);
        long start = m_sequential_position;
        // The region shrank under the head (End dragged or modulated below
        // it): restart at Loop and keep scanning from there. Returning Loop
        // without advancing would pin every following grain to it.
        if (start >= max_position) { start = loop; }
        long const picked = jitter_and_wrap(start, temperature, region, rng);

        // Scanner traverses the full region (start -> end), then restarts at
        // loop_point.
        m_sequential_position = start + static_cast<long>(interval);
        if (m_sequential_position >= max_position) { m_sequential_position = loop; }
        return picked;
    }

    void reset() override { m_sequential_position = 0; }

private:
    long m_sequential_position{0};
};

// GranularPosition: the head is parked on Position (absolute in the sample,
// no travelling head, Start / Loop / End dormant). Each grain is drawn
// uniformly from the Spray window around it — a deliberate width, biased
// before (-) or after (+) the point by Tilt: the symmetric [-1, 1] window
// slides to [tilt - 1, tilt + 1] and is clipped. Position temperature is
// applied on top exactly as in Loop mode, so it can leave the window.
class PositionSprayHead final : public HeadPolicy {
public:
    SampleRegion region(size_t total_frames, const VoiceParams& /*params*/) const override {
        return SampleRegion::full(total_frames);
    }

    long pick_start(const SampleRegion& region,
                    float temperature,
                    size_t /*interval*/,
                    const VoiceParams& p,
                    std::mt19937& rng) override {
        long const max_position = static_cast<long>(region.size());
        if (max_position <= 0) { return static_cast<long>(region.m_start); }

        float const window_lo = std::max(-1.0f, p.m_tilt - 1.0f);
        float const window_hi = std::min(1.0f, p.m_tilt + 1.0f);
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        float const u = dist(rng);  // [0, 1)
        float const spray_offset = (window_lo + u * (window_hi - window_lo)) * p.m_spray;
        long const start = static_cast<long>(p.m_position * static_cast<float>(max_position - 1)) +
                           static_cast<long>(spray_offset * static_cast<float>(max_position));
        return jitter_and_wrap(start, temperature, region, rng);
    }

    void reset() override {}
};

}  // namespace thl::dsp::granular
