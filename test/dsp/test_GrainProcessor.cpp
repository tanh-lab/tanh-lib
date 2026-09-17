// granular::GrainProcessorImpl, the voice facade: parameter snapshot, note
// logic, mode switching, and the player / engine it dispatches to.

#include <gtest/gtest.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/audio/AudioDataStore.h>
#include <tanh/dsp/granular/GrainEngine.h>
#include <tanh/dsp/granular/GrainProcessor.h>
#include <tanh/dsp/slicing/SliceMap.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "GranularTestHelpers.h"

using namespace granular_test;

namespace {

// GrainProcessorImpl's parameter hooks are pure virtual: serve them from
// plain arrays.
class StubVoice final : public GrainProcessorImpl {
public:
    explicit StubVoice(thl::dsp::audio::AudioDataStore& store) : GrainProcessorImpl(store) {
        m_f[Volume] = 1.0f;
        m_f[Velocity] = 1.0f;
        m_f[SampleEnd] = 1.0f;
        m_f[EnvelopeSustain] = 1.0f;
        m_f[Size] = 0.5f;
        m_f[Density] = 0.5f;
        m_i[ChannelModeParam] = static_cast<int>(ChannelMode::TrueStereo);
        m_i[EngineModeParam] = static_cast<int>(EngineMode::GranularLoop);
    }
    bool m_playing{false};
    bool m_loop{true};
    bool m_slicer{false};
    bool m_has_map{false};
    thl::dsp::slicing::SliceMap m_map{};
    void set_mode(EngineMode mode) { m_i[EngineModeParam] = static_cast<int>(mode); }
    void set_sample_start(float v) { m_f[SampleStart] = v; }
    void set_density(float v) { m_f[Density] = v; }
    void set_size(float v) { m_f[Size] = v; }
    void set_window_shape(float v) { m_f[GrainWindowShape] = v; }
    void set_sample_end(float v) { m_f[SampleEnd] = v; }
    void set_release(float v) { m_f[EnvelopeRelease] = v; }
    bool read_slice_map(thl::dsp::slicing::SliceMap& out) override {
        if (!m_has_map) { return false; }
        out = m_map;
        return true;
    }

    std::vector<float> run(size_t frames) {
        std::vector<float> out;
        Block block(2, k_block);
        for (size_t done = 0; done < frames; done += k_block) {
            block.clear();
            thl::core::BufferView view(block.out(), 2, k_block);
            process(view);
            out.insert(out.end(), block.m_data[0].begin(), block.m_data[0].end());
        }
        return out;
    }

private:
    std::array<float, NumParameters> m_f{};
    std::array<int, NumParameters> m_i{};
    float get_parameter_float(Parameter p, uint32_t) override { return m_f[p]; }
    bool get_parameter_bool(Parameter p, uint32_t) override {
        if (p == LoopEnabled) { return m_loop; }
        if (p == SlicerEnabled) { return m_slicer; }
        return p == Playing && m_playing;
    }
    int get_parameter_int(Parameter p, uint32_t) override { return m_i[p]; }
};

// Six uneven slices: the fixture the design doc draws.
thl::dsp::slicing::SliceMap six_slices(size_t total_frames) {
    const std::vector<float> bounds{0.0f, 0.17f, 0.29f, 0.52f, 0.71f, 0.86f, 1.0f};
    return thl::dsp::slicing::SliceMap::from_normalized(bounds, total_frames);
}

}  // namespace

TEST(GrainProcessor, EnginesTolerateFewerOrMoreBlockChannelsThanPrepared) {
    PlayerRig rig({make_ramp(1, 10000)});
    Block mono(1, k_block);
    rig.render_block(mono.out(), 1, k_block);  // no channel 1
    for (size_t i = 0; i < k_block; ++i) {
        ASSERT_FLOAT_EQ(mono.m_data[0][i], static_cast<float>(i));
    }
    Block quad(4, k_block);
    rig.render_block(quad.out(), 4, k_block);
    for (size_t i = 0; i < k_block; ++i) {
        ASSERT_FLOAT_EQ(quad.m_data[0][i], static_cast<float>(k_block + i));
        ASSERT_FLOAT_EQ(quad.m_data[2][i], 0.0f);
        ASSERT_FLOAT_EQ(quad.m_data[3][i], 0.0f);
    }

    auto const views = views_of(rig.m_store);
    GrainEngine engine;
    engine.prepare(k_sample_rate, 2);
    GrainParams params;
    params.m_channel_mode = ChannelMode::TrueStereo;
    Block mono2(1, k_block);
    engine.render(views, 0, params, HeadMode::Scan, mono2.out(), 1, k_block, 0);
    Block quad2(4, k_block);
    engine.render(views, 0, params, HeadMode::Scan, quad2.out(), 4, k_block, k_block);
    for (size_t i = 0; i < k_block; ++i) {
        ASSERT_TRUE(std::isfinite(mono2.m_data[0][i]));
        ASSERT_FLOAT_EQ(quad2.m_data[3][i], 0.0f);
    }
}

TEST(GrainProcessor, PrepareSeedsTheActiveModeFromTheParameter) {
    // A preset that boots in Sample mode must not run a block of grains
    // first: the very first block is the head (ramp), not grain-windowed.
    thl::dsp::audio::AudioDataStore store;
    load(store, [] {
        std::vector<thl::core::BufferF> b;
        b.push_back(make_ramp(1, 96000));
        return b;
    }());
    StubVoice voice(store);
    voice.set_mode(EngineMode::Sample);
    voice.prepare(k_sample_rate, k_block, 2);
    voice.m_playing = true;
    auto const out = voice.run(k_block);
    // The ADSR floors attack at 0.1 ms (~5 samples); after that out == n.
    for (size_t n = 8; n < k_block; ++n) { ASSERT_NEAR(out[n], static_cast<float>(n), 1e-3f); }
}

TEST(GrainProcessor, ModeSwitchFadesThroughZeroAndRevertsMidFade) {
    // DC source in Sample mode: the output IS the mode gain. Requesting
    // another mode ramps it linearly to 0 over 15 ms (720 frames); revoking
    // the request mid-fade ramps straight back up with no switch.
    thl::dsp::audio::AudioDataStore store;
    load(store, [] {
        std::vector<thl::core::BufferF> b;
        thl::core::BufferF dc(1, 96000, k_sample_rate);
        std::fill_n(dc.get_write_pointer(0), 96000, 1.0f);
        b.push_back(std::move(dc));
        return b;
    }());
    StubVoice voice(store);
    voice.set_mode(EngineMode::Sample);
    voice.prepare(k_sample_rate, k_block, 2);
    voice.m_playing = true;
    voice.run(1024);  // sounding, gain 1

    constexpr float k_step = 1.0f / 720.0f;
    voice.set_mode(EngineMode::GranularLoop);
    auto down = voice.run(256);
    for (size_t k = 0; k < 256; ++k) {
        ASSERT_NEAR(down[k], 1.0f - static_cast<float>(k + 1) * k_step, 1e-4f) << "k=" << k;
    }
    voice.set_mode(EngineMode::Sample);  // revert
    auto up = voice.run(512);
    float const from = 1.0f - 256.0f * k_step;
    for (size_t k = 0; k < 256; ++k) {
        ASSERT_NEAR(up[k], std::min(1.0f, from + static_cast<float>(k + 1) * k_step), 1e-4f)
            << "k=" << k;
    }
    for (size_t k = 300; k < 512; ++k) { ASSERT_NEAR(up[k], 1.0f, 1e-5f); }

    // A full fade: silence is reached, then the switch happens and gain
    // ramps back with the grain engine underneath.
    voice.set_mode(EngineMode::GranularLoop);
    auto full = voice.run(1024);
    for (size_t k = 0; k < 720; ++k) {
        ASSERT_NEAR(full[k], 1.0f - static_cast<float>(k + 1) * k_step, 1e-4f) << "k=" << k;
    }
    for (size_t k = 720; k < 768; ++k) { ASSERT_NEAR(full[k], 0.0f, 1e-5f) << "k=" << k; }
}

TEST(GrainProcessor, PositionModeIgnoresLoopOff) {
    // Position mode has no travelling head: Loop off changes nothing and
    // grains keep coming for as long as the gate is held.
    thl::dsp::audio::AudioDataStore store;
    load(store, [] {
        std::vector<thl::core::BufferF> b;
        b.push_back(make_ramp(1, 96000));
        return b;
    }());
    StubVoice voice(store);
    voice.set_mode(EngineMode::GranularPosition);
    voice.m_loop = false;
    voice.prepare(k_sample_rate, k_block, 2);
    voice.m_playing = true;
    auto const out = voice.run(48000);
    EXPECT_TRUE(voice.is_active());
    float last_energy = 0.0f;
    for (size_t n = 48000 - 4800; n < 48000; ++n) { last_energy += std::abs(out[n]); }
    EXPECT_GT(last_energy, 0.0f);
}

TEST(GrainProcessor, LoopOneShotReleasesOnlyAfterTheLastGrainEnds) {
    // GranularLoop, Loop off, region [0, 4800), 480-frame trigger interval,
    // rectangle window. The last grain starts at 4320 and is fitted to End,
    // so it sounds until output frame 4800; the release must not start
    // before that, or the last grain is cut.
    thl::dsp::audio::AudioDataStore store;
    load(store, [] {
        std::vector<thl::core::BufferF> b;
        b.push_back(make_ramp(1, 96000));
        return b;
    }());
    StubVoice voice(store);
    voice.set_mode(EngineMode::GranularLoop);
    voice.set_sample_end(0.05f);   // 4800 frames
    voice.set_release(0.001f);     // ~48 frames
    voice.set_density(1.0f);       // 480-frame interval
    voice.set_size(1.0f);          // 400 ms grains, truncated to the region
    voice.set_window_shape(0.0f);  // rectangle: output == source
    voice.m_loop = false;
    voice.prepare(k_sample_rate, k_block, 2);
    voice.m_playing = true;
    auto const out = voice.run(9600);
    // Still sounding well after the last trigger (frame 4320) — a release
    // started there would have gone silent by ~4400. Every grain alive reads
    // the same source frame (10 overlap at this density), so the output is at
    // least one grain's worth; the window's edge ends them just before End.
    ASSERT_GE(out[4600], 4600.0f * 0.95f);
    ASSERT_GE(out[4700], 4700.0f * 0.95f);
    // Silent after the last grain ended and the release ran out.
    for (size_t n = 5200; n < 9600; ++n) { ASSERT_NEAR(out[n], 0.0f, 1e-3f) << "n=" << n; }
    EXPECT_FALSE(voice.is_active());
}

TEST(GrainProcessor, OneShotReleasesAndDoesNotRetriggerWhileGateHeld) {
    // Sample mode, Loop off, region [0, 4800). The head reaches End after
    // 4800 frames: the envelope releases and, with the gate still held, the
    // voice must stay silent instead of re-triggering. Releasing and
    // pressing again replays.
    thl::dsp::audio::AudioDataStore store;
    load(store, [] {
        std::vector<thl::core::BufferF> b;
        thl::core::BufferF dc(1, 96000, k_sample_rate);
        std::fill_n(dc.get_write_pointer(0), 96000, 1.0f);
        b.push_back(std::move(dc));
        return b;
    }());
    StubVoice voice(store);
    voice.set_mode(EngineMode::Sample);
    voice.set_sample_end(0.05f);  // 4800 frames
    voice.set_release(0.001f);    // release ~48 frames
    voice.m_loop = false;
    voice.prepare(k_sample_rate, k_block, 2);
    voice.m_playing = true;
    auto out = voice.run(4800 + 4800);
    ASSERT_NEAR(out[2400], 1.0f, 1e-3f);
    // Well after End (tail + release are < 600 frames): silence, and it stays.
    for (size_t n = 4800 + 1200; n < 9600; ++n) { ASSERT_NEAR(out[n], 0.0f, 1e-4f) << "n=" << n; }
    EXPECT_FALSE(voice.is_active());
    // Still held: nothing.
    out = voice.run(2400);
    for (float v : out) { ASSERT_NEAR(v, 0.0f, 1e-4f); }
    // Gate falls and rises: the head plays again from Start.
    voice.m_playing = false;
    voice.run(k_block);
    voice.m_playing = true;
    out = voice.run(2400);
    EXPECT_TRUE(voice.is_active());
    ASSERT_NEAR(out[1200], 1.0f, 1e-3f);
}

TEST(GrainProcessor, WithoutSliceMapIgnoresSlicerFlag) {
    // SlicerEnabled set but no map from the host: Sample mode plays the plain
    // Start..End region (the ramp from 0), exactly as with the flag off.
    thl::dsp::audio::AudioDataStore store;
    load(store, [] {
        std::vector<thl::core::BufferF> b;
        b.push_back(make_ramp(1, 96000));
        return b;
    }());
    StubVoice voice(store);
    voice.set_mode(EngineMode::Sample);
    voice.m_slicer = true;
    voice.m_has_map = false;
    voice.prepare(k_sample_rate, k_block, 2);
    voice.m_playing = true;
    auto out = voice.run(k_block);
    for (size_t n = 8; n < k_block; ++n) { ASSERT_NEAR(out[n], static_cast<float>(n), 1e-3f); }

    // With a map, Start 0.5 / End 1 snap to boundary 3 of the six: the head
    // enters at frame 0.52 * 96000.
    StubVoice sliced(store);
    sliced.set_mode(EngineMode::Sample);
    sliced.m_slicer = true;
    sliced.m_has_map = true;
    sliced.m_map = six_slices(96000);
    sliced.set_sample_start(0.5f);
    sliced.prepare(k_sample_rate, k_block, 2);
    sliced.m_playing = true;
    out = sliced.run(k_block);
    auto const entry = static_cast<float>(sliced.m_map.boundary_frame(3, 96000));
    for (size_t n = 8; n < k_block; ++n) {
        ASSERT_NEAR(out[n], entry + static_cast<float>(n), 1e-2f) << "n=" << n;
    }
}
