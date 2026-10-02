// MotionRecorder with a ModulationMatrix, with concurrent UI / message / audio
// threads (run under -DTANH_WITH_TSAN=ON), and its audio path under RTSan
// (-DTANH_WITH_RTSAN=ON aborts on any allocation, lock or syscall in it).

#include <gtest/gtest.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/ModulationRouting.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionRecorder.h>
#include <tanh/modulation/ParameterBackend.h>
#include <tanh/modulation/SmartHandle.h>
#include <tanh/modulation/XYPad.h>
#include <tanh/state/ParameterDefinitions.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "MotionRig.h"

using namespace thl::modulation;
using motion_test::k_sr;
using thl::dsp::transport::TransportInfo;

namespace {

class FakeBackend : public ParameterBackend {
public:
    void add(const std::string& key, float default_value) {
        auto e = std::make_unique<Entry>();
        e->m_def =
            thl::ParameterDefinition::make_float(key, thl::Range::linear(0.0f, 1.0f), default_value)
                .modulatable(true);
        e->m_value.store(default_value);
        m_entries[key] = std::move(e);
    }
    std::optional<ParameterBinding> find(std::string_view key) const override {
        auto it = m_entries.find(key);
        if (it == m_entries.end()) { return std::nullopt; }
        return ParameterBinding{.m_def = it->second->m_def, .m_base = &it->second->m_value};
    }

private:
    struct Entry {
        thl::ParameterDefinition m_def;
        std::atomic<float> m_value{0.0f};
    };
    std::map<std::string, std::unique_ptr<Entry>, std::less<>> m_entries;
};

ModulationRouting replace_routing(std::string_view src, std::string_view dst, CombineMode mode) {
    ModulationRouting r(src, dst, 1.0f);
    r.m_combine_mode = mode;
    r.m_replace_priority = 10;
    return r;
}

TransportInfo playing_info(double beat, uint32_t n, double bpm = 120.0) {
    TransportInfo t;
    t.m_flags = TransportInfo::k_is_playing | TransportInfo::k_has_tempo |
                TransportInfo::k_has_beat_position;
    t.m_bpm = bpm;
    t.m_beat_position = beat;
    t.m_beats_per_sample = bpm / (60.0 * k_sr);
    t.m_num_samples = n;
    return t;
}

}  // namespace

// ── Matrix ────────────────────────────────────────────────────────────────────

// The recorder's sources copy its output; x/y as ReplaceHold, the gate as Replace.
TEST(MotionRecorderMatrix, SourcesCarryTheOutput) {
    constexpr uint32_t k_bs = 256;
    MotionRecorder rec;
    XYPad pad;
    FakeBackend backend;
    backend.add("slot_a", 0.2f);
    backend.add("gate", 0.3f);
    ModulationMatrix matrix(backend);
    matrix.add_source("motion.x", &rec.source(XYPadAxis::X));
    matrix.add_source("motion.active", &rec.source(XYPadAxis::Active));
    matrix.add_routing(replace_routing("motion.x", "slot_a", CombineMode::ReplaceHold));
    matrix.add_routing(replace_routing("motion.active", "gate", CombineMode::Replace));
    matrix.prepare(k_sr, k_bs);
    rec.prepare(k_sr, k_bs);
    pad.prepare(k_bs);
    auto a = matrix.get_smart_handle<float>("slot_a");
    auto gate = matrix.get_smart_handle<float>("gate");

    // No lane: the gate is closed, targets keep their base values.
    pad.process_block(k_bs);
    rec.process(playing_info(0.0, k_bs), pad.primary(), k_bs);
    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(a.load(k_bs - 1), 0.2f);
    EXPECT_FLOAT_EQ(gate.load(k_bs - 1), 0.3f);

    rec.load_lane(motion_test::beats_lane(4.0, 100));
    double beat = 0.0;
    for (int b = 0; b < 40; ++b) {
        pad.process_block(k_bs);
        rec.process(playing_info(beat, k_bs), pad.primary(), k_bs);
        matrix.process(k_bs);
        beat += k_bs * 120.0 / 60.0 / k_sr;
    }
    for (uint32_t i = 0; i < k_bs; i += 31) {
        EXPECT_NEAR(a.load(i), rec.out_x()[i], 1e-6) << i;
        EXPECT_FLOAT_EQ(gate.load(i), 1.0f);
    }
    // The matrix sees the recorder's change points (render ticks every 32 samples).
    EXPECT_GE(rec.source(XYPadAxis::X).get_change_points().size(), k_bs / 32);
}

// Routing the recorder's own output back into the matrix never reaches its
// input (it reads the pad's pre-matrix stream): arming does not start a take.
TEST(MotionRecorderMatrix, NoFeedback) {
    constexpr uint32_t k_bs = 256;
    MotionRecorder rec;
    XYPad pad;
    FakeBackend backend;
    backend.add("slot_a", 0.0f);
    backend.add("gate", 0.0f);
    ModulationMatrix matrix(backend);
    pad.add_to(matrix, "pad");
    matrix.add_source("motion.x", &rec.source(XYPadAxis::X));
    matrix.add_source("motion.active", &rec.source(XYPadAxis::Active));
    matrix.add_routing(replace_routing("motion.x", "slot_a", CombineMode::ReplaceHold));
    matrix.add_routing(replace_routing("motion.active", "gate", CombineMode::Replace));
    matrix.prepare(k_sr, k_bs);
    rec.prepare(k_sr, k_bs);
    rec.load_lane(motion_test::beats_lane(4.0, 100));
    rec.arm(LoopLength::Free);
    auto gate = matrix.get_smart_handle<float>("gate");
    double beat = 0.0;
    for (int b = 0; b < 400; ++b) {
        pad.process_block(k_bs);
        rec.process(playing_info(beat, k_bs), pad.primary(), k_bs);
        matrix.process(k_bs);
        beat += k_bs * 120.0 / 60.0 / k_sr;
    }
    EXPECT_FLOAT_EQ(gate.load(0), 1.0f);         // playback drives the target …
    EXPECT_EQ(rec.state(), MotionState::Armed);  // … but never starts a take
    EXPECT_FALSE(rec.service());
    pad.remove_from(matrix);
}

// ── Reverse ───────────────────────────────────────────────────────────────────

TEST(MotionRecorder, ReversePlayback) {
    constexpr uint32_t k_bs = 256;
    MotionRecorder rec;
    rec.prepare(k_sr, k_bs);
    MotionLane lane = motion_test::beats_lane(4.0, 100);
    rec.load_lane(lane);
    double beat = 0.0;
    auto run = [&](int blocks, std::vector<float>& xs, std::vector<double>& beats) {
        for (int b = 0; b < blocks; ++b) {
            rec.process(playing_info(beat, k_bs), XYPadStream{}, k_bs);
            for (uint32_t i = 0; i < k_bs; ++i) {
                xs.push_back(rec.out_x()[i]);
                beats.push_back(beat + (i * 120.0 / 60.0 / k_sr));
            }
            beat += k_bs * 120.0 / 60.0 / k_sr;
        }
    };
    std::vector<float> xs;
    std::vector<double> beats;
    run(200, xs, beats);
    ASSERT_TRUE(rec.set_reverse(true));
    const size_t from = xs.size();
    run(400, xs, beats);
    EXPECT_TRUE(rec.ui_snapshot().m_reverse);
    // Mirrored phase: beat b plays lane phase (4 - b mod 4), after a 25 ms glide.
    for (size_t s = from + 1300; s < xs.size(); s += 37) {
        const double phase = 4.0 - std::fmod(beats[s], 4.0);
        ASSERT_NEAR(xs[s], lane.sample(phase).m_x, 2e-4) << s;
    }
    EXPECT_LE(motion_test::max_sample_step(xs, from, from + 1300), 0.01);
}

// ── Concurrency (TSan) ────────────────────────────────────────────────────────

// UI thread: touches and recorder commands at about 1 kHz. Message thread:
// service(), load_lane(), clear() and lane() in a loop. Audio thread: 200k
// blocks of random size. No data race (TSan), output finite and in [0, 1],
// published take ids increase.
TEST(MotionRecorderConcurrency, Stress) {
    constexpr uint32_t k_max_block = 256;
    constexpr int k_blocks = 200000;
    MotionRecorder rec;
    XYPad pad;
    rec.prepare(k_sr, k_max_block);
    pad.prepare(k_max_block);

    std::atomic<bool> done{false};
    std::atomic<uint64_t> bad_samples{0};
    std::atomic<uint64_t> takes_seen{0};

    std::thread audio([&] {
        std::mt19937 rng(7);
        std::uniform_int_distribution<uint32_t> size(1, k_max_block);
        std::uniform_int_distribution<int> event(0, 999);
        double beat = 0.0;
        double bpm = 120.0;
        uint32_t last_id = 0;
        for (int b = 0; b < k_blocks; ++b) {
            const uint32_t n = size(rng);
            const int e = event(rng);
            if (e < 3) { beat += 3.25; }                       // seek
            if (e == 3) { bpm = bpm > 100.0 ? 90.0 : 140.0; }  // tempo change
            TransportInfo t = playing_info(beat, n, bpm);
            if (e == 5) { t.m_flags &= ~TransportInfo::k_is_playing; }
            pad.process_block(n);
            rec.process(t, pad.primary(), n);
            for (uint32_t i = 0; i < n; ++i) {
                const float x = rec.out_x()[i];
                const float y = rec.out_y()[i];
                if (!(x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f)) {
                    bad_samples.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (rec.playing_take_id() != last_id) {
                last_id = rec.playing_take_id();
                takes_seen.fetch_add(1, std::memory_order_relaxed);
            }
            beat += n * bpm / 60.0 / k_sr;
        }
        done.store(true, std::memory_order_release);
    });

    std::thread ui([&] {
        std::mt19937 rng(11);
        std::uniform_real_distribution<float> pos(0.0f, 1.0f);
        std::uniform_int_distribution<int> what(0, 99);
        bool down = false;
        while (!done.load(std::memory_order_acquire)) {
            const int w = what(rng);
            if (w < 60) {
                pad.touch(1, pos(rng), pos(rng));
                down = true;
            } else if (w < 70 && down) {
                pad.release(1);
                down = false;
            } else if (w < 76) {
                rec.arm(static_cast<LoopLength>(w % 6));
            } else if (w < 78) {
                rec.record(LoopLength::Free);
            } else if (w < 81) {
                rec.disarm();
            } else if (w < 83) {
                rec.stop();
            } else if (w < 86) {
                rec.play();
            } else if (w < 88) {
                rec.set_reverse(w % 2 == 0);
            } else if (w < 89) {
                rec.set_stopped_transport(w % 2 == 0 ? StoppedTransport::Hold
                                                     : StoppedTransport::FreeRun);
            }
            (void)rec.ui_snapshot();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        pad.release_all();
    });

    uint32_t last_published = 0;
    bool monotone = true;
    int published = 0;
    int loads = 0;
    std::mt19937 rng(3);
    std::uniform_int_distribution<int> what(0, 99);
    while (!done.load(std::memory_order_acquire)) {
        if (rec.service()) { ++published; }
        const int w = what(rng);
        if (w < 2) {
            rec.load_lane(motion_test::beats_lane(4.0, 50));
            ++loads;
        } else if (w < 3) {
            rec.clear();
        }
        uint32_t id = 0;
        size_t points = 0;
        rec.read_lane([&](const MotionLane& l) {
            id = l.m_take_id;
            points = l.num_points();
        });
        if (id < last_published) { monotone = false; }
        last_published = id;
        const MotionLane copy = rec.lane();
        if (copy.m_take_id < id) { monotone = false; }
        (void)points;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    audio.join();
    ui.join();
    rec.service();

    std::printf("[stress] takes published %d, lanes loaded %d, playing-id changes %llu\n",
                published,
                loads,
                static_cast<unsigned long long>(takes_seen.load()));
    EXPECT_EQ(bad_samples.load(), 0u);
    EXPECT_TRUE(monotone);
    EXPECT_GT(published, 0);
    EXPECT_GT(published + loads, 0);
    EXPECT_GT(takes_seen.load(), 0u);
}

// ── RTSan ─────────────────────────────────────────────────────────────────────

namespace {

float rt_block(XYPad& pad, MotionRecorder& rec, const TransportInfo& t, uint32_t n)
    TANH_NONBLOCKING_FUNCTION {
    pad.process_block(n);
    rec.process(t, pad.primary(), n);
    float acc = 0.0f;
    for (uint32_t i = 0; i < n; ++i) { acc += rec.out_x()[i] + rec.out_y()[i] + rec.out_gate()[i]; }
    acc += static_cast<float>(rec.change_points().size());
    acc += static_cast<float>(rec.playback_phase());
    return acc;
}

}  // namespace

// Arm, record, auto-finish, publish, touch override, lane load and a transport
// jump: under -DTANH_WITH_RTSAN=ON nothing in process() allocates or locks.
TEST(MotionRecorderRtsan, FullScenario) {
    constexpr uint32_t k_bs = 256;
    MotionRecorder rec;
    XYPad pad;
    rec.prepare(k_sr, k_bs);
    pad.prepare(k_bs);
    double beat = 0.0;
    float acc = 0.0f;
    auto run = [&](int blocks, double jump = 0.0) {
        for (int b = 0; b < blocks; ++b) {
            if (b == 0) { beat += jump; }
            acc += rt_block(pad, rec, playing_info(beat, k_bs), k_bs);
            beat += k_bs * 120.0 / 60.0 / k_sr;
        }
    };
    run(4);
    rec.arm(LoopLength::Bars1);  // a 1-bar take auto-finishes after 4 beats
    for (int b = 0; b < 420; ++b) {
        pad.touch(1, 0.5f + (0.4f * std::sin(b * 0.05f)), 0.5f);
        run(1);
    }
    pad.release(1);
    run(10);
    EXPECT_EQ(rec.state(), MotionState::Playing);
    EXPECT_TRUE(rec.service());  // publish (message thread, not RT)
    run(10);                     // switch to the RCU lane, free the buffer
    pad.touch(1, 0.9f, 0.9f);    // override …
    run(20);
    pad.release(1);  // … and glide back
    run(20);
    rec.load_lane(motion_test::beats_lane(4.0, 100));
    run(20);
    run(20, 2.5);               // transport jump
    rec.arm(LoopLength::Free);  // a free take in Beats
    for (int b = 0; b < 50; ++b) {
        pad.touch(1, 0.2f, 0.1f + (0.01f * static_cast<float>(b)));
        run(1);
    }
    pad.release(1);
    run(10);
    rec.set_reverse(true);
    run(20);
    rec.clear();
    run(10);
    EXPECT_TRUE(std::isfinite(acc));
}
