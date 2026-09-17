// granular::GrainEngine, its HeadPolicy implementations and the channel
// mixer, driven standalone (no voice, no parameter system).

#include <gtest/gtest.h>
#include <tanh/core/Buffer.h>
#include <tanh/dsp/audio/AudioDataStore.h>
#include <tanh/dsp/granular/ChannelMixer.h>
#include <tanh/dsp/granular/GrainEngine.h>
#include <tanh/dsp/granular/GrainVisualizer.h>
#include <tanh/dsp/granular/GranularTypes.h>
#include <tanh/dsp/granular/HeadPolicy.h>
#include <tanh/dsp/sampler/LoopRegion.h>
#include <tanh/dsp/sampler/SampleView.h>
#include <tanh/dsp/slicing/SliceMap.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <vector>

#include "GranularTestHelpers.h"

using namespace granular_test;
using thl::dsp::sampler::LoopRegion;
using thl::dsp::slicing::SliceMap;

namespace {

// Six uneven slices: the fixture the design doc draws.
SliceMap six_slices(size_t total_frames) {
    const std::vector<float> bounds{0.0f, 0.17f, 0.29f, 0.52f, 0.71f, 0.86f, 1.0f};
    return SliceMap::from_normalized(bounds, total_frames);
}

// Which slice a physical frame lies in.
size_t slice_at(const SliceMap& m, FramePos frame, size_t total) {
    for (size_t k = 0; k < m.m_count; ++k) {
        if (frame < static_cast<FramePos>(m.boundary_frame(k + 1, total))) { return k; }
    }
    return m.m_count - 1;
}

}  // namespace

TEST(LoopScanHead, ResumesScanWhenRegionShrinksBelowHead) {
    // Scan to 48000, then End drops to 20000 with temperature 0 (the
    // default). The next grain lands on Loop — and the scan must go on from
    // there, not return Loop for every grain until the next note-on.
    LoopScanHead head;
    std::mt19937 rng(1);
    HeadInputs params;
    constexpr size_t k_interval = 9600;

    auto const full = LoopRegion::full(96000);
    for (int i = 0; i < 5; ++i) { head.pick_start(full, 0.0f, k_interval, params, rng); }

    LoopRegion const small{.m_start = 0, .m_end = 20000, .m_loop_point = 0};
    FramePos const first = head.pick_start(small, 0.0f, k_interval, params, rng);
    FramePos const second = head.pick_start(small, 0.0f, k_interval, params, rng);
    FramePos const third = head.pick_start(small, 0.0f, k_interval, params, rng);
    EXPECT_EQ(first, FramePos{0});
    EXPECT_EQ(second, static_cast<FramePos>(k_interval));
    EXPECT_EQ(third, static_cast<FramePos>(2 * k_interval));
}

TEST(PositionSprayHead, TiltClipsTheWindow) {
    PositionSprayHead head;
    std::mt19937 rng(7);
    HeadInputs params;
    params.m_position = 0.5f;
    params.m_spray = 1.0f;  // the window past the edge clips, never wraps
    auto const region = LoopRegion::full(96000);
    auto const centre = static_cast<FramePos>(0.5f * static_cast<float>(96000 - 1));

    params.m_tilt = 1.0f;
    for (int i = 0; i < 500; ++i) {
        EXPECT_GE(head.pick_start(region, 0.0f, 0, params, rng), centre);
    }
    params.m_tilt = -1.0f;
    for (int i = 0; i < 500; ++i) {
        EXPECT_LE(head.pick_start(region, 0.0f, 0, params, rng), centre);
    }
    params.m_spray = 0.0f;
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(head.pick_start(region, 0.0f, 0, params, rng), centre);
    }
    // Position at the very end with a forward spray: everything clips to
    // the last frame instead of wrapping to the start.
    params.m_position = 1.0f;
    params.m_spray = 1.0f;
    params.m_tilt = 1.0f;
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(head.pick_start(region, 0.0f, 0, params, rng), FramePos{96000 - 1});
    }
}

TEST(PositionSprayHead, SliceSprayZeroStartsOnSliceStart) {
    PositionSprayHead head;
    std::mt19937 rng(3);
    HeadInputs params;
    auto const slices = six_slices(100000);
    params.m_slices = &slices;
    params.m_position = 0.4f;  // slice 3 (0-based 2) starts at 0.29
    params.m_spray = 0.0f;
    auto const region = LoopRegion::full(100000);
    // The very frame Sample / Loop mode's region would start on.
    auto const expected = static_cast<FramePos>(slices.boundary_frame(2, 100000));
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(head.pick_start(region, 0.0f, 0, params, rng), expected);
    }
    // Temperature at Spray 0 stays inside the chosen slice.
    for (int i = 0; i < 500; ++i) {
        FramePos const f = head.pick_start(region, 1.0f, 0, params, rng);
        EXPECT_EQ(slice_at(slices, f, 100000), 2u) << "f=" << f;
    }
}

TEST(PositionSprayHead, SliceTiltRightCoversChosenAndNext) {
    PositionSprayHead head;
    std::mt19937 rng(5);
    HeadInputs params;
    auto const slices = six_slices(100000);
    params.m_slices = &slices;
    params.m_position = 0.4f;      // slice index 2
    params.m_spray = 2.0f / 6.0f;  // two slices wide
    params.m_tilt = 1.0f;          // forward only: slices 2 and 3
    auto const region = LoopRegion::full(100000);
    std::array<int, 6> hist{};
    for (int i = 0; i < 2000; ++i) {
        hist[slice_at(slices, head.pick_start(region, 0.0f, 0, params, rng), 100000)]++;
    }
    EXPECT_EQ(hist[0] + hist[1] + hist[4] + hist[5], 0);
    // Equal share per slice, however long each is in time.
    EXPECT_GT(hist[2], 800);
    EXPECT_GT(hist[3], 800);
    // Spray 1/6, Tilt right: the chosen slice only, spread across it.
    params.m_spray = 1.0f / 6.0f;
    hist = {};
    FramePos lo = 1 << 30, hi = -1;
    for (int i = 0; i < 1000; ++i) {
        FramePos const f = head.pick_start(region, 0.0f, 0, params, rng);
        hist[slice_at(slices, f, 100000)]++;
        lo = std::min(lo, f);
        hi = std::max(hi, f);
    }
    EXPECT_EQ(hist[2], 1000);
    EXPECT_GT(hi - lo, static_cast<FramePos>(0.2f * 100000 * 0.9f));  // nearly the whole slice
}

TEST(PositionSprayHead, SliceTiltLeftExcludesChosenSlice) {
    PositionSprayHead head;
    std::mt19937 rng(9);
    HeadInputs params;
    auto const slices = six_slices(100000);
    params.m_slices = &slices;
    params.m_position = 0.4f;      // slice index 2
    params.m_spray = 2.0f / 6.0f;  // two slices
    params.m_tilt = -1.0f;         // the two before: 0 and 1
    auto const region = LoopRegion::full(100000);
    std::array<int, 6> hist{};
    for (int i = 0; i < 2000; ++i) {
        hist[slice_at(slices, head.pick_start(region, 0.0f, 0, params, rng), 100000)]++;
    }
    EXPECT_EQ(hist[2] + hist[3] + hist[4] + hist[5], 0);
    EXPECT_GT(hist[0], 800);
    EXPECT_GT(hist[1], 800);
    // Centred, Spray 1/6: one step each way -> slices 1 and 2.
    params.m_tilt = 0.0f;
    params.m_spray = 1.0f / 6.0f;
    hist = {};
    for (int i = 0; i < 2000; ++i) {
        hist[slice_at(slices, head.pick_start(region, 0.0f, 0, params, rng), 100000)]++;
    }
    EXPECT_EQ(hist[0] + hist[3] + hist[4] + hist[5], 0);
    EXPECT_GT(hist[1], 800);
    EXPECT_GT(hist[2], 800);
}

TEST(PositionSprayHead, SliceWindowWrapsPastLastSlice) {
    PositionSprayHead head;
    std::mt19937 rng(11);
    HeadInputs params;
    auto const slices = six_slices(100000);
    params.m_slices = &slices;
    params.m_position = 0.9f;      // slice index 5 (the last)
    params.m_spray = 2.0f / 6.0f;  // the last slice and, wrapped, the first
    params.m_tilt = 1.0f;
    auto const region = LoopRegion::full(100000);
    std::array<int, 6> hist{};
    for (int i = 0; i < 2000; ++i) {
        hist[slice_at(slices, head.pick_start(region, 0.0f, 0, params, rng), 100000)]++;
    }
    EXPECT_EQ(hist[1] + hist[2] + hist[3] + hist[4], 0);
    EXPECT_GT(hist[5], 800);
    EXPECT_GT(hist[0], 800);
    // 100 % forward: every slice once.
    params.m_spray = 1.0f;
    hist = {};
    for (int i = 0; i < 6000; ++i) {
        hist[slice_at(slices, head.pick_start(region, 0.0f, 0, params, rng), 100000)]++;
    }
    for (int k = 0; k < 6; ++k) { EXPECT_GT(hist[static_cast<size_t>(k)], 700) << "k=" << k; }
}

TEST(GrainEngine, LoopScanIsOneXRegardlessOfDensity) {
    // With no jitter every grain must start exactly where the scan head is,
    // and the head advances one trigger interval per trigger: density and
    // advance cancel, so grain k starts at the frame it was triggered on.
    for (float density : {0.0f, 1.0f}) {
        thl::dsp::audio::AudioDataStore store;
        load(store, [] {
            std::vector<thl::core::BufferF> b;
            b.push_back(make_ramp(1, 96000));
            return b;
        }());
        auto const views = views_of(store);
        GrainVisualizer viz;
        RecordingListener listener;
        viz.add_listener(&listener);
        GrainEngine engine;
        engine.set_visualizer(&viz);
        engine.prepare(k_sample_rate, 2);
        engine.seed(3);
        engine.reset_schedule(HeadMode::Scan);

        GrainParams params;
        params.m_channel_mode = ChannelMode::TrueStereo;
        params.m_density = density;
        params.m_size = 0.0f;  // 2 ms grains, so the pool never fills

        Block block(2, k_block);
        size_t elapsed = 0;
        while (elapsed < 72000) {
            block.clear();
            engine.render(views, 0, params, HeadMode::Scan, block.out(), 2, k_block, elapsed);
            elapsed += k_block;
        }

        ASSERT_EQ(listener.m_dropped, 0u) << "listener capacity outgrown at density " << density;
        ASSERT_GE(listener.m_triggered.size(), 3u) << "density " << density;
        float const rate =
            k_min_grain_rate * std::pow(k_max_grain_rate / k_min_grain_rate, density);
        auto const interval = static_cast<size_t>(k_sample_rate / rate);
        for (size_t k = 0; k < listener.m_triggered.size(); ++k) {
            auto const start =
                static_cast<size_t>(std::lround(listener.m_triggered[k].m_pos * 96000.0f));
            EXPECT_EQ(start, k * interval) << "density " << density << " grain " << k;
        }
    }
}

TEST(GrainEngine, ReverseRegionGrainsReadBackwards) {
    // Rectangle window, one grain at a time, region reversed: the grain
    // enters at the mirrored frame and its samples descend.
    thl::dsp::audio::AudioDataStore store;
    load(store, [] {
        std::vector<thl::core::BufferF> b;
        b.push_back(make_ramp(1, 96000));
        return b;
    }());
    auto const views = views_of(store);
    GrainEngine engine;
    engine.prepare(k_sample_rate, 2);
    engine.reset_schedule(HeadMode::Scan);

    GrainParams params;
    params.m_channel_mode = ChannelMode::TrueStereo;
    params.m_density = 0.0f;       // one grain per 24000 frames
    params.m_size = 0.0f;          // 96-frame grains
    params.m_window_shape = 0.0f;  // rectangle: output == source
    params.m_head.m_start = 0.5f;  // 48000
    params.m_head.m_end = 0.25f;   // 24000 -> reversed, entry at 47999

    Block block(2, k_block);
    block.clear();
    engine.render(views, 0, params, HeadMode::Scan, block.out(), 2, k_block, 0);
    for (size_t i = 1; i < k_block; ++i) {
        ASSERT_NEAR(block.m_data[0][i], static_cast<float>(47999 - i), 1e-2f) << "i=" << i;
    }
}

TEST(GrainEngine, LoopOneShotStopsTriggeringAtEnd) {
    thl::dsp::audio::AudioDataStore store;
    load(store, [] {
        std::vector<thl::core::BufferF> b;
        b.push_back(make_ramp(1, 96000));
        return b;
    }());
    auto const views = views_of(store);
    GrainVisualizer viz;
    RecordingListener listener;
    viz.add_listener(&listener);
    GrainEngine engine;
    engine.set_visualizer(&viz);
    engine.prepare(k_sample_rate, 2);
    engine.seed(3);
    engine.reset_schedule(HeadMode::Scan);

    GrainParams params;
    params.m_channel_mode = ChannelMode::TrueStereo;
    params.m_density = 1.0f;  // 480-frame interval
    params.m_size = 0.0f;
    params.m_head.m_end = 0.1f;  // 9600 frames: 20 triggers, then done
    params.m_head.m_loop = false;

    Block block(2, k_block);
    size_t elapsed = 0;
    while (elapsed < 48000) {
        block.clear();
        engine.render(views, 0, params, HeadMode::Scan, block.out(), 2, k_block, elapsed);
        elapsed += k_block;
    }
    EXPECT_TRUE(engine.finished(HeadMode::Scan));
    EXPECT_FALSE(engine.finished(HeadMode::Spray));
    EXPECT_EQ(listener.m_triggered.size(), 20u);
    for (auto const& t : listener.m_triggered) { EXPECT_LT(t.m_pos, 0.1f); }
    // Looping again: the scan restarts and keeps going.
    engine.reset_schedule(HeadMode::Scan);
    EXPECT_FALSE(engine.finished(HeadMode::Scan));
    params.m_head.m_loop = true;
    listener.m_triggered.clear();
    elapsed = 0;
    while (elapsed < 48000) {
        block.clear();
        engine.render(views, 0, params, HeadMode::Scan, block.out(), 2, k_block, elapsed);
        elapsed += k_block;
    }
    EXPECT_EQ(listener.m_triggered.size(), 100u);
}

TEST(ChannelMixer, ModeMatrixIsExactAndHeadMatchesCentredGrain) {
    // Stereo source: channel 1 = channel 0 + 1000. At position 10 the
    // samples are 10 and 1010, mono average 510.
    auto const stereo_buffer = make_ramp(2, 100, 1000.0f);
    auto const mono_buffer = make_ramp(1, 100);
    auto const quad_buffer = make_ramp(4, 100, 1000.0f);
    auto const stereo = thl::dsp::sampler::SampleView::of(stereo_buffer);
    auto const mono = thl::dsp::sampler::SampleView::of(mono_buffer);
    auto const quad = thl::dsp::sampler::SampleView::of(quad_buffer);
    using channel_mixer::Frame;
    auto grain =
        [&](const thl::dsp::sampler::SampleView& src, ChannelMode mode, float pan, size_t out_ch) {
            Frame f{};
            channel_mixer::accumulate_grain(src, 10.0f, mode, pan, 1.0f, out_ch, f);
            return f;
        };
    // A head frame at position 10: the source's channels (a mono source
    // duplicated, as SamplePlayer renders it), then the channel mode.
    auto head = [&](const thl::dsp::sampler::SampleView& src,
                    ChannelMode mode,
                    float width,
                    size_t out_ch) {
        std::array<float, k_max_channel_support> in{};
        std::array<float, k_max_channel_support> outs{};
        std::array<const float*, k_max_channel_support> in_ptrs{};
        std::array<float*, k_max_channel_support> out_ptrs{};
        for (size_t ch = 0; ch < k_max_channel_support; ++ch) {
            size_t const read = src.m_num_channels == 1 ? 0 : ch;
            in[ch] = thl::dsp::sampler::read_clamped(src, read, 10.0);
            in_ptrs[ch] = &in[ch];
            out_ptrs[ch] = &outs[ch];
        }
        channel_mixer::mix_head(in_ptrs.data(),
                                src.m_num_channels,
                                mode,
                                width,
                                out_ptrs.data(),
                                out_ch,
                                1);
        Frame f{};
        std::copy(outs.begin(), outs.end(), f.begin());
        return f;
    };

    // MonoToStereo: centred grain and head agree (half per side).
    auto g = grain(stereo, ChannelMode::MonoToStereo, 0.5f, 2);
    auto h = head(stereo, ChannelMode::MonoToStereo, 0.0f, 2);
    EXPECT_FLOAT_EQ(g[0], 255.0f);
    EXPECT_FLOAT_EQ(g[1], 255.0f);
    EXPECT_FLOAT_EQ(h[0], g[0]);
    EXPECT_FLOAT_EQ(h[1], g[1]);
    // Hard-left grain keeps the mono sum.
    g = grain(stereo, ChannelMode::MonoToStereo, 0.0f, 2);
    EXPECT_FLOAT_EQ(g[0] + g[1], 510.0f);

    // TrueStereo: centred grain = source (2x compensation); head width 1 =
    // source, width 0 = mid in both.
    g = grain(stereo, ChannelMode::TrueStereo, 0.5f, 2);
    EXPECT_FLOAT_EQ(g[0], 10.0f);
    EXPECT_FLOAT_EQ(g[1], 1010.0f);
    h = head(stereo, ChannelMode::TrueStereo, 1.0f, 2);
    EXPECT_FLOAT_EQ(h[0], 10.0f);
    EXPECT_FLOAT_EQ(h[1], 1010.0f);
    h = head(stereo, ChannelMode::TrueStereo, 0.0f, 2);
    EXPECT_FLOAT_EQ(h[0], 510.0f);
    EXPECT_FLOAT_EQ(h[1], 510.0f);
    // Mono source is duplicated.
    g = grain(mono, ChannelMode::TrueStereo, 0.5f, 2);
    EXPECT_FLOAT_EQ(g[0], 10.0f);
    EXPECT_FLOAT_EQ(g[1], 10.0f);
    h = head(mono, ChannelMode::TrueStereo, 1.0f, 2);
    EXPECT_FLOAT_EQ(h[0], 10.0f);
    EXPECT_FLOAT_EQ(h[1], 10.0f);

    // TrueMultichannel: even channels take left energy, odd right; width
    // per pair; output truncated to the channels the voice has.
    g = grain(quad, ChannelMode::TrueMultichannel, 0.25f, 4);
    EXPECT_FLOAT_EQ(g[0], 10.0f * 0.75f * 2.0f);
    EXPECT_FLOAT_EQ(g[1], 1010.0f * 0.25f * 2.0f);
    EXPECT_FLOAT_EQ(g[2], 2010.0f * 0.75f * 2.0f);
    EXPECT_FLOAT_EQ(g[3], 3010.0f * 0.25f * 2.0f);
    h = head(quad, ChannelMode::TrueMultichannel, 1.0f, 4);
    EXPECT_FLOAT_EQ(h[2], 2010.0f);
    EXPECT_FLOAT_EQ(h[3], 3010.0f);
    h = head(quad, ChannelMode::TrueMultichannel, 0.0f, 4);
    EXPECT_FLOAT_EQ(h[2], 2510.0f);
    EXPECT_FLOAT_EQ(h[3], 2510.0f);
    g = grain(quad, ChannelMode::TrueMultichannel, 0.5f, 2);
    EXPECT_FLOAT_EQ(g[2], 0.0f);
    EXPECT_FLOAT_EQ(g[3], 0.0f);
}

TEST(GrainEngine, RendersAPlainBufferWithoutAnyVoice) {
    // Standalone use: one buffer, no store, no visualiser.
    auto const ramp = make_ramp(1, 96000);
    auto const source = thl::dsp::sampler::SampleView::of(ramp);
    GrainEngine engine;
    engine.prepare(k_sample_rate, 2);
    engine.seed(7);
    engine.reset_schedule(HeadMode::Spray);
    GrainParams params;
    params.m_channel_mode = ChannelMode::TrueStereo;
    params.m_head.m_position = 0.5f;
    Block block(2, k_block);
    float energy = 0.0f;
    for (size_t b = 0; b < 200; ++b) {
        engine
            .render({&source, 1}, 0, params, HeadMode::Spray, block.out(), 2, k_block, b * k_block);
        for (float v : block.m_data[0]) { energy += std::abs(v); }
    }
    EXPECT_GT(energy, 0.0f);
    EXPECT_TRUE(engine.any_grain_active());
    engine.deactivate_all();
    EXPECT_FALSE(engine.any_grain_active());
}
