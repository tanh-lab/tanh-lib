#include <tanh/dsp/audio/AudioBufferView.h>
#include <tanh/dsp/metronome/MetronomePlayer.h>
#include <tanh/dsp/transport/InternalTransportClock.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace {

constexpr double k_sample_rate = 48000.0;
constexpr double k_bpm = 120.0;
// At 120 BPM: 1 beat = 24000 samples, 1 sixteenth = 6000, 1 bar (4/4) = 96000.

using thl::dsp::audio::AudioBufferView;
using thl::dsp::metronome::MetronomePlayerImpl;
using thl::dsp::transport::Division;
using thl::dsp::transport::InternalTransportClock;

// Test fixture: provides parameter values from a map and instruments
// trigger_accent/trigger_click so we can assert sample-accurate scheduling
// without relying on the audible voice rendering.
class TestMetronome : public MetronomePlayerImpl {
public:
    explicit TestMetronome(InternalTransportClock& clock) : MetronomePlayerImpl(clock) {}

    using MetronomePlayerImpl::Enabled;
    using MetronomePlayerImpl::Gain;
    using MetronomePlayerImpl::Parameter;
    using MetronomePlayerImpl::Rhythm;

    void set_param_bool(Parameter p, bool v) { m_bools[p] = v; }
    void set_param_float(Parameter p, float v) { m_floats[p] = v; }
    void set_param_int(Parameter p, int v) { m_ints[p] = v; }

    // Recorded events from the most recent process_modulated() call.
    struct Event {
        bool accent;          // false → click
        uint32_t sample_pos;  // absolute sample within the buffer
    };
    std::vector<Event> events;
    std::vector<uint32_t> modulation_offsets;  // captured per process() invocation

    void process(thl::dsp::audio::AudioBufferView buffer, uint32_t modulation_offset) override {
        modulation_offsets.push_back(modulation_offset);
        m_sample_counter = 0;  // reset for each sub-block
        MetronomePlayerImpl::process(buffer, modulation_offset);
    }

protected:
    // trigger_* fires inside the per-sample loop before tick_voices() is called
    // for that sample, so m_sample_counter == current sample index i.
    void trigger_accent() override { events.push_back({true, m_sample_counter}); }
    void trigger_click() override { events.push_back({false, m_sample_counter}); }
    float tick_voices() override {
        ++m_sample_counter;
        return 0.0f;
    }

private:
    uint32_t m_sample_counter = 0;

    float get_parameter_float(Parameter p, uint32_t /*off*/) override {
        auto it = m_floats.find(p);
        return it != m_floats.end() ? it->second : 1.0f;
    }
    bool get_parameter_bool(Parameter p, uint32_t /*off*/) override {
        auto it = m_bools.find(p);
        return it != m_bools.end() ? it->second : true;
    }
    int get_parameter_int(Parameter p, uint32_t /*off*/) override {
        auto it = m_ints.find(p);
        return it != m_ints.end() ? it->second : 0;
    }

    std::unordered_map<int, bool> m_bools;
    std::unordered_map<int, float> m_floats;
    std::unordered_map<int, int> m_ints;
};

struct StereoBuffer {
    std::vector<float> l, r;
    std::array<float*, 2> channels{};
    AudioBufferView view;

    explicit StereoBuffer(uint32_t frames, float fill = 0.0f) : l(frames, fill), r(frames, fill) {
        channels = {l.data(), r.data()};
        view = AudioBufferView(channels.data(), 2, frames);
    }
};

// Drive one process() call with the clock advanced by frame_count samples.
void run_block(InternalTransportClock& clk, TestMetronome& m, AudioBufferView buf) {
    clk.begin_block(static_cast<uint32_t>(buf.get_num_frames()));
    m.process(buf, 0);
    clk.end_block();
}

// ── 1. Sample-accurate scheduling ─────────────────────────────────────────────

TEST(MetronomePlayer, ClickFiresAtCorrectSampleOffsetForBeat) {
    InternalTransportClock clk;
    clk.prepare(k_sample_rate);
    clk.set_bpm(k_bpm);
    clk.set_time_signature(4, 4);
    clk.play();

    TestMetronome m(clk);
    m.prepare(k_sample_rate, 24000, 2);
    m.set_param_int(TestMetronome::Rhythm, static_cast<int>(Division::Beat));

    // Block 1: spans beat 0 and reaches up to (but not including) beat 1.
    // At sample 0: bar boundary → accent. No click within (Beat == bar in 4/4 only on bar starts).
    StereoBuffer b0(24000);
    run_block(clk, m, b0.view);

    ASSERT_EQ(m.events.size(), 1u);
    EXPECT_TRUE(m.events[0].accent);
    EXPECT_EQ(m.events[0].sample_pos, 0u);

    m.events.clear();

    // Block 2: spans beats 1 and 2 — both clicks (no accent, since bar boundary
    // already passed at start of block 1).
    StereoBuffer b1(48000);  // 2 beats long
    run_block(clk, m, b1.view);

    ASSERT_EQ(m.events.size(), 2u);
    EXPECT_FALSE(m.events[0].accent);
    EXPECT_EQ(m.events[0].sample_pos, 0u);  // beat 1 at offset 0
    EXPECT_FALSE(m.events[1].accent);
    EXPECT_EQ(m.events[1].sample_pos, 24000u);  // beat 2 at offset 24000
}

TEST(MetronomePlayer, SixteenthDivisionFiresFourTimesPerBeat) {
    InternalTransportClock clk;
    clk.prepare(k_sample_rate);
    clk.set_bpm(k_bpm);
    clk.set_time_signature(4, 4);
    clk.play();

    TestMetronome m(clk);
    m.prepare(k_sample_rate, 24000, 2);
    m.set_param_int(TestMetronome::Rhythm, static_cast<int>(Division::Sixteenth));

    // 1 beat = 24000 samples = 4 sixteenths × 6000 samples each.
    // Block of exactly one beat starting at beat 0:
    //   sample 0    → bar boundary fires accent (not click)
    //   sample 6000  → click
    //   sample 12000 → click
    //   sample 18000 → click
    StereoBuffer buf(24000);
    run_block(clk, m, buf.view);

    ASSERT_EQ(m.events.size(), 4u);
    EXPECT_TRUE(m.events[0].accent);
    EXPECT_EQ(m.events[0].sample_pos, 0u);
    EXPECT_FALSE(m.events[1].accent);
    EXPECT_EQ(m.events[1].sample_pos, 6000u);
    EXPECT_FALSE(m.events[2].accent);
    EXPECT_EQ(m.events[2].sample_pos, 12000u);
    EXPECT_FALSE(m.events[3].accent);
    EXPECT_EQ(m.events[3].sample_pos, 18000u);
}

// ── 2. Mixing semantics ───────────────────────────────────────────────────────

TEST(MetronomePlayer, MixesIntoBufferRatherThanOverwriting) {
    InternalTransportClock clk;
    clk.prepare(k_sample_rate);
    clk.set_bpm(k_bpm);
    clk.play();

    TestMetronome m(clk);
    m.prepare(k_sample_rate, 24000, 2);
    m.set_param_bool(TestMetronome::Enabled, false);
    // tick_voices returns 0, no active voices, no triggers → metronome adds 0
    // every sample. With +=, upstream signal survives. With =, it gets wiped.

    constexpr float k_upstream = 0.5f;
    StereoBuffer buf(512, k_upstream);
    run_block(clk, m, buf.view);

    for (size_t i = 0; i < 512; ++i) {
        EXPECT_FLOAT_EQ(buf.l[i], k_upstream) << "sample " << i;
        EXPECT_FLOAT_EQ(buf.r[i], k_upstream) << "sample " << i;
    }
}

// ── 3. Rhythm is block-quantized ──────────────────────────────────────────────

TEST(MetronomePlayer, RhythmChangeAppliesOnNextBlockNotMidBlock) {
    InternalTransportClock clk;
    clk.prepare(k_sample_rate);
    clk.set_bpm(k_bpm);
    clk.set_time_signature(4, 4);
    clk.play();

    TestMetronome m(clk);
    m.prepare(k_sample_rate, 24000, 2);
    m.set_param_int(TestMetronome::Rhythm, static_cast<int>(Division::Beat));

    // Block 1: one beat at Beat rhythm — only bar accent at sample 0.
    StereoBuffer b0(24000);
    run_block(clk, m, b0.view);
    EXPECT_EQ(m.events.size(), 1u);

    m.events.clear();
    m.set_param_int(TestMetronome::Rhythm, static_cast<int>(Division::Sixteenth));

    // Block 2: one beat at Sixteenth rhythm — 4 sixteenth clicks (no accent
    // since bar boundary already passed).
    StereoBuffer b1(24000);
    run_block(clk, m, b1.view);
    EXPECT_EQ(m.events.size(), 4u);
}

// ── 4. Stopped clock fires no new triggers ────────────────────────────────────

TEST(MetronomePlayer, StoppedClockFiresNoTriggers) {
    InternalTransportClock clk;
    clk.prepare(k_sample_rate);
    clk.set_bpm(k_bpm);
    clk.set_time_signature(4, 4);
    // Note: clk.play() NOT called.

    TestMetronome m(clk);
    m.prepare(k_sample_rate, 24000, 2);
    m.set_param_int(TestMetronome::Rhythm, static_cast<int>(Division::Sixteenth));

    StereoBuffer buf(96000);  // would normally produce many clicks
    run_block(clk, m, buf.view);

    EXPECT_TRUE(m.events.empty());
}

// ── 5. Disabled metronome fires no new triggers ───────────────────────────────

TEST(MetronomePlayer, DisabledMetronomeFiresNoTriggers) {
    InternalTransportClock clk;
    clk.prepare(k_sample_rate);
    clk.set_bpm(k_bpm);
    clk.set_time_signature(4, 4);
    clk.play();

    TestMetronome m(clk);
    m.prepare(k_sample_rate, 24000, 2);
    m.set_param_bool(TestMetronome::Enabled, false);
    m.set_param_int(TestMetronome::Rhythm, static_cast<int>(Division::Sixteenth));

    StereoBuffer buf(96000);
    run_block(clk, m, buf.view);

    EXPECT_TRUE(m.events.empty());
}

// ── 6. process_modulated splits at change points ──────────────────────────────

TEST(MetronomePlayer, ProcessModulatedSplitsAtChangePoints) {
    InternalTransportClock clk;
    clk.prepare(k_sample_rate);
    clk.set_bpm(k_bpm);
    clk.play();

    TestMetronome m(clk);
    m.prepare(k_sample_rate, 512, 2);
    m.set_param_bool(TestMetronome::Enabled, false);

    StereoBuffer buf(512);
    clk.begin_block(512);

    const std::array<uint32_t, 2> change_points = {100, 300};
    m.process_modulated(buf.view, std::span<const uint32_t>(change_points));

    clk.end_block();

    // Expected sub-blocks: [0,100), [100,300), [300,512) → modulation_offset
    // values 0, 100, 300.
    ASSERT_EQ(m.modulation_offsets.size(), 3u);
    EXPECT_EQ(m.modulation_offsets[0], 0u);
    EXPECT_EQ(m.modulation_offsets[1], 100u);
    EXPECT_EQ(m.modulation_offsets[2], 300u);
}

// ── 7. Bar boundary fires accent, sub-bar boundaries fire click ───────────────

TEST(MetronomePlayer, BarBoundaryFiresAccentSubBarFiresClick) {
    InternalTransportClock clk;
    clk.prepare(k_sample_rate);
    clk.set_bpm(k_bpm);
    clk.set_time_signature(4, 4);
    clk.play();

    TestMetronome m(clk);
    m.prepare(k_sample_rate, 96000, 2);
    m.set_param_int(TestMetronome::Rhythm, static_cast<int>(Division::Beat));

    // One full bar at 4/4 = 4 beats = 96000 samples.
    // Expect 1 accent (bar start) + 3 clicks (interior beats).
    StereoBuffer buf(96000);
    run_block(clk, m, buf.view);

    int accents = 0, clicks = 0;
    for (const auto& e : m.events) {
        if (e.accent) {
            ++accents;
        } else {
            ++clicks;
        }
    }
    EXPECT_EQ(accents, 1);
    EXPECT_EQ(clicks, 3);

    // First event must be the accent at sample 0.
    ASSERT_FALSE(m.events.empty());
    EXPECT_TRUE(m.events.front().accent);
    EXPECT_EQ(m.events.front().sample_pos, 0u);
}

}  // namespace
