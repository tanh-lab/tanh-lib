// slicing::SliceMap, slice-space steps and slicing::TransientSlicer.

#include <gtest/gtest.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/Numbers.h>
#include <tanh/dsp/sampler/LoopRegion.h>
#include <tanh/dsp/slicing/SliceMap.h>
#include <tanh/dsp/slicing/SliceSteps.h>
#include <tanh/dsp/slicing/TransientSlicer.h>

#include <cmath>
#include <cstddef>
#include <vector>

using namespace thl::dsp::slicing;

namespace {

// Six uneven slices: the fixture the design doc draws.
SliceMap six_slices(size_t total_frames) {
    const std::vector<float> bounds{0.0f, 0.17f, 0.29f, 0.52f, 0.71f, 0.86f, 1.0f};
    return SliceMap::from_normalized(bounds, total_frames);
}

}  // namespace

TEST(SliceMap, StepsFloorRoundAndClamp) {
    auto const m = six_slices(1000);
    EXPECT_TRUE(m.valid());
    // Position: floor(u·6), clamped into 0..5.
    EXPECT_EQ(slice_of_step(m, 0.0f), 0u);
    EXPECT_EQ(slice_of_step(m, 0.16f), 0u);
    EXPECT_EQ(slice_of_step(m, 0.17f), 1u);
    EXPECT_EQ(slice_of_step(m, 0.52f), 3u);  // 52 % -> slice 4 of 6 (0-based 3)
    EXPECT_EQ(slice_of_step(m, 1.0f), 5u);
    EXPECT_EQ(slice_of_step(m, 7.0f), 5u);
    EXPECT_EQ(slice_of_step(m, -1.0f), 0u);
    // Start / End: round(u·6), clamped into 0..6.
    EXPECT_EQ(boundary_of_step(m, 0.0f), 0u);
    EXPECT_EQ(boundary_of_step(m, 0.08f), 0u);
    EXPECT_EQ(boundary_of_step(m, 0.09f), 1u);
    EXPECT_EQ(boundary_of_step(m, 0.5f), 3u);
    EXPECT_EQ(boundary_of_step(m, 1.0f), 6u);
    EXPECT_EQ(boundary_of_step(m, 2.0f), 6u);
    // Frames.
    EXPECT_EQ(m.boundary_frame(0, 1000), 0u);
    EXPECT_EQ(m.boundary_frame(2, 1000), 290u);
    EXPECT_EQ(m.boundary_frame(6, 1000), 1000u);
    EXPECT_EQ(m.boundary_frame(9, 1000), 1000u);
    // Validity.
    SliceMap bad = m;
    bad.m_bounds[3] = bad.m_bounds[2];
    EXPECT_FALSE(bad.valid());
    bad = m;
    bad.m_count = 1;  // slice 0 would have to end at the total
    EXPECT_FALSE(bad.valid());
    bad = m;
    bad.m_bounds[6] = 990;  // must end at the total
    EXPECT_FALSE(bad.valid());
    EXPECT_FALSE(SliceMap{}.valid());
    EXPECT_TRUE(SliceMap::grid(4, 1000).valid());
    EXPECT_TRUE(SliceMap::grid(1, 1000).valid());
    EXPECT_EQ(SliceMap::grid(400, 100000).m_count, k_max_slices);
    EXPECT_EQ(SliceMap::grid(0, 1000).m_count, 1u);
    // A source of another length rescales.
    EXPECT_EQ(m.boundary_frame(2, 2000), 580u);
}

TEST(SliceMap, StepToNormIsMonotonicThroughUnevenBounds) {
    auto const m = six_slices(1000);
    EXPECT_FLOAT_EQ(step_to_norm(m, 0.0), 0.0f);
    EXPECT_FLOAT_EQ(step_to_norm(m, 6.0), 1.0f);
    EXPECT_FLOAT_EQ(step_to_norm(m, 3.0), 0.52f);
    EXPECT_NEAR(step_to_norm(m, 2.5), 0.29f + 0.5f * (0.52f - 0.29f), 1e-6f);
    EXPECT_FLOAT_EQ(step_to_norm(m, -1.0), 0.0f);
    EXPECT_FLOAT_EQ(step_to_norm(m, 9.0), 1.0f);
    float last = -1.0f;
    for (int i = 0; i <= 600; ++i) {
        float const v = step_to_norm(m, static_cast<double>(i) / 100.0);
        EXPECT_GE(v, last);
        last = v;
    }
}

TEST(SliceSteps, RegionFromStepsSnapsToBoundaries) {
    auto const m = six_slices(1000);
    // Start near boundary 2 (0.29), End near boundary 5 (0.86): slices 2-4.
    auto r = region_from_steps(m, 0.3f, 0.8f, 1000);
    EXPECT_FALSE(r.m_reverse);
    EXPECT_EQ(r.m_start, 290u);
    EXPECT_EQ(r.m_end, 860u);
    EXPECT_EQ(r.m_loop_point, 290u);  // no Loop marker: repeats from Start
    // Full range.
    r = region_from_steps(m, 0.0f, 1.0f, 1000);
    EXPECT_EQ(r.m_start, 0u);
    EXPECT_EQ(r.m_end, 1000u);
}

TEST(SliceSteps, RegionFromStepsEndBeforeStartReverses) {
    auto const m = six_slices(1000);
    auto r = region_from_steps(m, 0.8f, 0.3f, 1000);
    EXPECT_TRUE(r.m_reverse);
    EXPECT_EQ(r.m_start, 290u);
    EXPECT_EQ(r.m_end, 860u);
    EXPECT_EQ(r.m_loop_point, 290u);
    // The virtual start reads the Start marker's side (boundary 5, minus 1).
    EXPECT_DOUBLE_EQ(r.physical(290.0), 859.0);
}

TEST(SliceSteps, RegionFromStepsEqualStartEndPlaysOneSlice) {
    auto const m = six_slices(1000);
    auto r = region_from_steps(m, 0.5f, 0.5f, 1000);  // both on boundary 3
    EXPECT_FALSE(r.m_reverse);
    EXPECT_EQ(r.m_start, 520u);
    EXPECT_EQ(r.m_end, 710u);
    // Both on the last boundary: the last slice.
    r = region_from_steps(m, 1.0f, 1.0f, 1000);
    EXPECT_EQ(r.m_start, 860u);
    EXPECT_EQ(r.m_end, 1000u);
    EXPECT_GT(r.size(), 0u);
}

TEST(SliceMap, NormalizedBoundsRoundTripAndValidate) {
    auto const m = six_slices(96000);
    auto const bounds = m.normalized_bounds();
    ASSERT_EQ(bounds.size(), 7u);
    EXPECT_FLOAT_EQ(bounds.front(), 0.0f);
    EXPECT_FLOAT_EQ(bounds.back(), 1.0f);
    auto const again = SliceMap::from_normalized(bounds, 96000);
    for (size_t k = 0; k <= m.m_count; ++k) { EXPECT_EQ(again.m_bounds[k], m.m_bounds[k]); }

    EXPECT_TRUE(valid_normalized_bounds(std::vector<float>{0.0f, 0.5f, 1.0f}, 1.0, 0.03, 2, 16));
    EXPECT_FALSE(valid_normalized_bounds(std::vector<float>{0.0f, 1.0f}, 1.0, 0.03, 2, 16));
    EXPECT_FALSE(
        valid_normalized_bounds(std::vector<float>{0.0f, 0.6f, 0.5f, 1.0f}, 1.0, 0.03, 2, 16));
    EXPECT_FALSE(
        valid_normalized_bounds(std::vector<float>{0.0f, 0.5f, 0.5f, 1.0f}, 1.0, 0.03, 2, 16));
    EXPECT_FALSE(valid_normalized_bounds(std::vector<float>{0.1f, 0.5f, 1.0f}, 1.0, 0.03, 2, 16));
    EXPECT_FALSE(valid_normalized_bounds(std::vector<float>{0.0f, 0.5f, 0.9f}, 1.0, 0.03, 2, 16));
    EXPECT_FALSE(valid_normalized_bounds(std::vector<float>{0.0f, 0.01f, 1.0f}, 1.0, 0.03, 2, 16));
    EXPECT_TRUE(valid_normalized_bounds(std::vector<float>{0.0f, 0.01f, 1.0f}, 0.0, 0.03, 2, 16));
    EXPECT_TRUE(valid_normalized_bounds(std::vector<float>{0.0f, 0.01f, 1.0f}, 10.0, 0.03, 2, 16));
    std::vector<float> seventeen(18);
    for (size_t i = 0; i < seventeen.size(); ++i) { seventeen[i] = static_cast<float>(i) / 17.0f; }
    seventeen.back() = 1.0f;
    EXPECT_FALSE(valid_normalized_bounds(seventeen, 10.0, 0.03, 2, 16));
    EXPECT_TRUE(valid_normalized_bounds(seventeen, 10.0, 0.03));
}

namespace {

constexpr double k_sample_rate = 48000.0;
constexpr size_t k_num_frames = 4 * 48000;  // 4 s

struct Burst {
    size_t frame;
    float amplitude;
};

/// Stereo buffer of decaying 1 kHz sine bursts (10 ms decay) at the given
/// frames, optionally over a quiet continuous 100 Hz background tone.
thl::core::BufferF make_bursts(const std::vector<Burst>& bursts, float background = 0.0f) {
    thl::core::BufferF buffer(2, k_num_frames, k_sample_rate);
    float* left = buffer.get_write_pointer(0);
    float* right = buffer.get_write_pointer(1);
    for (size_t f = 0; f < k_num_frames; ++f) {
        const double t = static_cast<double>(f) / k_sample_rate;
        const float bg =
            background * static_cast<float>(std::sin(2.0 * std::numbers::pi * 100.0 * t));
        left[f] = bg;
        right[f] = bg;
    }
    for (const Burst& burst : bursts) {
        const size_t length = static_cast<size_t>(0.060 * k_sample_rate);
        for (size_t i = 0; i < length && burst.frame + i < k_num_frames; ++i) {
            const double t = static_cast<double>(i) / k_sample_rate;
            const float v =
                burst.amplitude * static_cast<float>(std::exp(-t / 0.010) *
                                                     std::sin(2.0 * std::numbers::pi * 1000.0 * t));
            left[burst.frame + i] += v;
            right[burst.frame + i] += 0.5f * v;
        }
    }
    return buffer;
}

SliceMap pick(const thl::core::BufferF& buffer, size_t count, bool refine = true) {
    TransientSlicer const slicer;
    return slicer.pick(slicer.analyse(buffer, k_sample_rate), count, refine ? &buffer : nullptr);
}

/// Every slice at least the slicer's minimum long (with a hair of slack).
bool slices_long_enough(const SliceMap& map) {
    auto const min_frames = static_cast<uint64_t>(TransientSlicer::Settings{}.m_min_slice_seconds *
                                                  k_sample_rate * 0.999);
    for (size_t k = 0; k < map.m_count; ++k) {
        if (map.m_bounds[k + 1] - map.m_bounds[k] < min_frames) { return false; }
    }
    return true;
}

std::vector<float> mono_of(const thl::core::BufferF& buffer) {
    std::vector<float> mono(buffer.get_num_samples());
    for (size_t ch = 0; ch < buffer.get_num_channels(); ++ch) {
        const float* src = buffer.get_read_pointer(ch);
        for (size_t f = 0; f < mono.size(); ++f) { mono[f] += src[f]; }
    }
    for (float& v : mono) { v /= static_cast<float>(buffer.get_num_channels()); }
    return mono;
}

constexpr long k_tolerance_frames = static_cast<long>(0.005 * k_sample_rate);  // 5 ms

/// True when some interior boundary lies within 5 ms before `frame`
/// (or up to one frame after it, for the rounding of a boundary at frame-1).
bool has_boundary_before(const SliceMap& bounds, size_t frame) {
    for (size_t i = 1; i < bounds.m_count; ++i) {
        const auto b = static_cast<long>(bounds.m_bounds[i]);
        const long target = static_cast<long>(frame);
        if (b <= target + 1 && b >= target - k_tolerance_frames) { return true; }
    }
    return false;
}

size_t boundaries_in(const SliceMap& bounds, long lo, long hi) {
    size_t n = 0;
    for (size_t i = 1; i < bounds.m_count; ++i) {
        const auto b = static_cast<long>(bounds.m_bounds[i]);
        if (b >= lo && b <= hi) { ++n; }
    }
    return n;
}

}  // namespace

TEST(TransientSlicer, BurstsAtKnownFramesGiveBoundariesJustBeforeThem) {
    const std::vector<Burst> bursts = {{0, 1.0f},
                                       {24000, 1.0f},
                                       {57600, 0.8f},
                                       {96000, 1.0f},
                                       {148800, 0.9f}};
    const auto buffer = make_bursts(bursts);
    TransientSlicer const slicer;
    const TransientAnalysis analysis = slicer.analyse(buffer, k_sample_rate);
    ASSERT_FALSE(analysis.empty());

    const auto map = slicer.pick(analysis, bursts.size(), &buffer);
    ASSERT_EQ(map.m_count, bursts.size());
    EXPECT_TRUE(map.valid());
    EXPECT_TRUE(slices_long_enough(map));
    for (size_t i = 1; i < bursts.size(); ++i) {
        EXPECT_TRUE(has_boundary_before(map, bursts[i].frame)) << "burst at " << bursts[i].frame;
    }
}

TEST(TransientSlicer, WithoutTheBufferBoundariesStillLandJustBeforeTheBursts) {
    const std::vector<Burst> bursts = {{0, 1.0f},
                                       {24000, 1.0f},
                                       {57600, 0.8f},
                                       {96000, 1.0f},
                                       {148800, 0.9f}};
    const auto map = pick(make_bursts(bursts), bursts.size(), false);
    ASSERT_EQ(map.m_count, bursts.size());
    EXPECT_TRUE(map.valid());
    // Hop resolution: the boundary is the start of the peak hop, < 256 frames
    // (5.3 ms) before the burst.
    for (size_t i = 1; i < bursts.size(); ++i) {
        bool found = false;
        for (size_t k = 1; k < map.m_count; ++k) {
            const auto b = static_cast<long>(map.m_bounds[k]);
            if (b <= static_cast<long>(bursts[i].frame) &&
                b > static_cast<long>(bursts[i].frame) - 260) {
                found = true;
            }
        }
        EXPECT_TRUE(found) << "burst at " << bursts[i].frame;
    }
}

TEST(TransientSlicer, CountKeepsTheStrongestPeaks) {
    // 8 bursts, quiet and loud alternating (the one at frame 0 can never be a
    // boundary); count 5 -> the 4 interior boundaries are the 4 loud ones.
    const std::vector<Burst> bursts = {{0, 0.03f},
                                       {20000, 1.0f},
                                       {44000, 0.03f},
                                       {66000, 1.0f},
                                       {90000, 0.03f},
                                       {114000, 1.0f},
                                       {138000, 0.03f},
                                       {160000, 1.0f}};
    const auto map = pick(make_bursts(bursts), 5);
    ASSERT_EQ(map.m_count, 5u);
    EXPECT_TRUE(map.valid());
    for (const size_t loud : {20000u, 66000u, 114000u, 160000u}) {
        EXPECT_TRUE(has_boundary_before(map, loud)) << "loud burst at " << loud;
    }
    for (const size_t quiet : {44000u, 90000u, 138000u}) {
        EXPECT_FALSE(has_boundary_before(map, quiet)) << "quiet burst at " << quiet;
    }
}

TEST(TransientSlicer, TwoBurstsTenMillisecondsApartYieldOneBoundary) {
    const size_t first = 96000;
    const size_t second = first + static_cast<size_t>(0.010 * k_sample_rate);
    const auto map = pick(make_bursts({{0, 1.0f}, {first, 1.0f}, {second, 1.0f}}), 3);
    ASSERT_EQ(map.m_count, 3u);
    EXPECT_TRUE(map.valid());
    EXPECT_EQ(boundaries_in(map,
                            static_cast<long>(first) - k_tolerance_frames,
                            static_cast<long>(second) + k_tolerance_frames),
              1u);
    EXPECT_TRUE(has_boundary_before(map, first));
}

TEST(TransientSlicer, TooFewPeaksFillTheLargestGaps) {
    const auto map = pick(make_bursts({{0, 1.0f}, {62400, 1.0f}, {124800, 1.0f}}), 6);
    ASSERT_EQ(map.m_count, 6u);
    EXPECT_TRUE(map.valid());
    EXPECT_TRUE(has_boundary_before(map, 62400));
    EXPECT_TRUE(has_boundary_before(map, 124800));
    // Every filled slice keeps a sensible length: 1.3 s gaps split evenly.
    for (size_t k = 0; k < map.m_count; ++k) {
        EXPECT_GT(static_cast<double>(map.m_bounds[k + 1] - map.m_bounds[k]) / k_sample_rate, 0.3);
    }
}

TEST(TransientSlicer, SilenceFallsBackToTheGrid) {
    const thl::core::BufferF silence(2, k_num_frames, k_sample_rate);
    TransientSlicer const slicer;
    const TransientAnalysis analysis = slicer.analyse(silence, k_sample_rate);
    auto const same = [](const SliceMap& a, const SliceMap& b) {
        if (a.m_count != b.m_count || a.m_total_frames != b.m_total_frames) { return false; }
        for (size_t k = 0; k <= a.m_count; ++k) {
            if (a.m_bounds[k] != b.m_bounds[k]) { return false; }
        }
        return true;
    };
    for (const size_t count : {2u, 3u, 5u, 7u, 16u}) {
        EXPECT_TRUE(
            same(slicer.pick(analysis, count, &silence), SliceMap::grid(count, k_num_frames)));
        EXPECT_TRUE(same(slicer.pick(analysis, count), SliceMap::grid(count, k_num_frames)));
    }
    EXPECT_FALSE(slicer.pick(TransientAnalysis{}, 4).valid());  // no length: no map
}

TEST(TransientSlicer, BoundariesSnapToZeroCrossingsOfTheMonoSignal) {
    // Continuous background tone so the zero-crossing constraint is non-trivial.
    const std::vector<Burst> bursts = {{0, 1.0f},
                                       {24000, 1.0f},
                                       {57600, 0.8f},
                                       {96000, 1.0f},
                                       {148800, 0.9f}};
    const auto buffer = make_bursts(bursts, 0.2f);
    const auto mono = mono_of(buffer);
    const auto map = pick(buffer, bursts.size());
    ASSERT_EQ(map.m_count, bursts.size());
    EXPECT_TRUE(map.valid());
    for (size_t i = 1; i < map.m_count; ++i) {
        const auto n = static_cast<size_t>(map.m_bounds[i]);
        ASSERT_GT(n, 0u);
        ASSERT_LT(n, mono.size());
        const bool zero_or_sign_change = mono[n] == 0.0f || mono[n - 1] * mono[n] <= 0.0f;
        EXPECT_TRUE(zero_or_sign_change) << "boundary " << i << " at frame " << n;
    }
    for (size_t i = 1; i < bursts.size(); ++i) {
        EXPECT_TRUE(has_boundary_before(map, bursts[i].frame)) << "burst at " << bursts[i].frame;
    }
}

TEST(TransientSlicer, PickClampsTheCount) {
    const auto buffer = make_bursts({{0, 1.0f}, {96000, 1.0f}});
    TransientSlicer const slicer;
    const TransientAnalysis analysis = slicer.analyse(buffer, k_sample_rate);
    EXPECT_EQ(slicer.pick(analysis, 0, &buffer).m_count, 1u);
    EXPECT_EQ(slicer.pick(analysis, 400, &buffer).m_count, k_max_slices);
}
