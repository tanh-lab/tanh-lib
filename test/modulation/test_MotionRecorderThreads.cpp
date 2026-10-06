#include <gtest/gtest.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionRecorder.h>
#include <tanh/modulation/XYController.h>
#include <tanh/modulation/detail/XYPad.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

#include "MotionRecorderTestHelpers.h"

using namespace thl::modulation;
using motion_test::k_sr;
using thl::dsp::transport::TransportInfo;

using motion_test::playing_info;

// UI thread: touches and recorder commands at about 1 kHz. Message thread:
// service(), load_lane(), clear() and lane() in a loop. Audio thread: 200k
// blocks of random size. No data race (TSan), output finite and in [0, 1],
// published take ids increase.
TEST(MotionRecorderConcurrency, Stress) {
    constexpr uint32_t k_max_block = 256;
    constexpr int k_blocks = 200000;
    MotionRecorder rec;
    detail::XYPad pad{1, MonoPriority::Last};
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
            rec.process(t, pad.output(), n);
            for (uint32_t i = 0; i < n; ++i) {
                const float x = rec.out_x()[i];
                const float y = rec.out_y()[i];
                if (!(x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f)) {
                    bad_samples.fetch_add(1, std::memory_order_relaxed);
                }
            }
            const uint32_t id = rec.snapshot().m_take_id;
            if (id != last_id) {
                last_id = id;
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
                (void)rec.arm(static_cast<LoopLength>(w % 6));
            } else if (w < 78) {
                (void)rec.record(LoopLength::Free);
            } else if (w < 81) {
                (void)rec.disarm();
            } else if (w < 83) {
                (void)rec.stop();
            } else if (w < 86) {
                (void)rec.play();
            } else if (w < 88) {
                (void)rec.set_reverse(w % 2 == 0);
            } else if (w < 89) {
                rec.set_stopped_transport(w % 2 == 0 ? StoppedTransport::Hold
                                                     : StoppedTransport::FreeRun);
            }
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

// service() concurrent with the audio thread's take handoff: the recorder that
// is serviced from another thread plays bit-identical to one serviced between
// blocks, because the switch from a take buffer to its published copy is
// silent. Under TSan this also checks the handoff's memory ordering.
TEST(MotionRecorderConcurrency, ConcurrentServiceKeepsTheHandoffSilent) {
    constexpr uint32_t k_bs = 16;
    constexpr int k_takes = 300;
    MotionRecorderConfig config;
    config.m_rate = 2000.0;
    MotionRecorder raced;
    MotionRecorder reference;
    raced.prepare(k_sr, k_bs, config);
    reference.prepare(k_sr, k_bs, config);

    std::atomic<int> publish_requests{0};
    std::atomic<bool> done{false};
    std::thread message([&] {
        int served = 0;
        while (!done.load(std::memory_order_acquire)) {
            if (publish_requests.load(std::memory_order_acquire) > served) {
                raced.service();
                ++served;
            }
        }
    });

    std::vector<float> x(k_bs);
    std::vector<float> y(k_bs, 0.5f);
    std::vector<uint8_t> active(k_bs);
    uint32_t mismatches = 0;
    double beat = 0.0;
    auto block = [&](bool touched, float position) {
        std::fill(x.begin(), x.end(), position);
        std::fill(active.begin(), active.end(), touched ? 1 : 0);
        const std::array<uint32_t, 1> change_points{0};
        const MotionInput input{.m_x = x.data(),
                                .m_y = y.data(),
                                .m_active = active.data(),
                                .m_change_points = change_points,
                                .m_num_samples = k_bs};
        const TransportInfo t = playing_info(beat, k_bs);
        raced.process(t, input, k_bs);
        reference.process(t, input, k_bs);
        beat = t.beat_end();
        for (uint32_t i = 0; i < k_bs; ++i) {
            if (raced.out_x()[i] != reference.out_x()[i] ||
                raced.out_gate()[i] != reference.out_gate()[i]) {
                ++mismatches;
            }
        }
    };

    for (int take = 0; take < k_takes; ++take) {
        ASSERT_TRUE(raced.record(LoopLength::Free));
        ASSERT_TRUE(reference.record(LoopLength::Free));
        for (int b = 0; b < 12; ++b) { block(true, 0.1f + (0.05f * static_cast<float>(b))); }
        block(false, 0.0f);  // the release ends the take
        const uint32_t version = raced.lane_version();
        publish_requests.fetch_add(1, std::memory_order_release);
        for (int b = 0; raced.lane_version() == version && b < 100000; ++b) { block(false, 0.0f); }
        reference.service();
        for (int b = 0; b < 4; ++b) { block(false, 0.0f); }
    }
    done.store(true, std::memory_order_release);
    message.join();
    EXPECT_EQ(mismatches, 0u);
    EXPECT_EQ(raced.lane_version(), reference.lane_version());
}

namespace {

float run_block(detail::XYPad& pad, MotionRecorder& rec, const TransportInfo& t, uint32_t n)
    TANH_NONBLOCKING_FUNCTION {
    pad.process_block(n);
    rec.process(t, pad.output(), n);
    float acc = 0.0f;
    for (uint32_t i = 0; i < n; ++i) { acc += rec.out_x()[i] + rec.out_y()[i] + rec.out_gate()[i]; }
    acc += static_cast<float>(rec.change_points().size());
    acc += static_cast<float>(rec.playback_phase());
    return acc;
}

}  // namespace

// Arm, record, auto-finish, publish, touch override, lane load, a transport
// jump, reverse and clear on one recorder. The RTSan preset checks the audio path.
TEST(MotionRecorder, FullScenario) {
    constexpr uint32_t k_bs = 256;
    MotionRecorder rec;
    detail::XYPad pad{1, MonoPriority::Last};
    rec.prepare(k_sr, k_bs);
    pad.prepare(k_bs);
    double beat = 0.0;
    float acc = 0.0f;
    auto run = [&](int blocks, double jump = 0.0) {
        for (int b = 0; b < blocks; ++b) {
            if (b == 0) { beat += jump; }
            acc += run_block(pad, rec, playing_info(beat, k_bs), k_bs);
            beat += k_bs * 120.0 / 60.0 / k_sr;
        }
    };
    run(4);
    ASSERT_TRUE(rec.arm(LoopLength::Bars1));  // a 1-bar take auto-finishes after 4 beats
    for (int b = 0; b < 420; ++b) {
        pad.touch(1, 0.5f + (0.4f * std::sin(b * 0.05f)), 0.5f);
        run(1);
    }
    pad.release(1);
    run(10);
    EXPECT_EQ(rec.snapshot().m_state, MotionState::Playing);
    EXPECT_TRUE(rec.service());  // publish (message thread, not RT)
    run(10);                     // switch to the RCU lane, free the buffer
    pad.touch(1, 0.9f, 0.9f);    // override …
    run(20);
    pad.release(1);  // … and glide back
    run(20);
    rec.load_lane(motion_test::beats_lane(4.0, 100));
    run(20);
    run(20, 2.5);                            // transport jump
    ASSERT_TRUE(rec.arm(LoopLength::Free));  // a free take in Beats
    for (int b = 0; b < 50; ++b) {
        pad.touch(1, 0.2f, 0.1f + (0.01f * static_cast<float>(b)));
        run(1);
    }
    pad.release(1);
    run(10);
    ASSERT_TRUE(rec.set_reverse(true));
    run(20);
    rec.clear();
    run(10);
    EXPECT_TRUE(std::isfinite(acc));
}
