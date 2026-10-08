#include <gtest/gtest.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/XYController.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "XYControllerTestHelpers.h"

using namespace thl::modulation;
using xy_test::FakeBackend;
using xy_test::k_sr;
using xy_test::SimTransport;

namespace {

constexpr uint32_t k_bs = 480;  // 10 ms
constexpr int64_t k_ms = 1'000'000;
constexpr int64_t k_clock_start = 5'000'000'000;

int64_t block_time(int block) {
    return k_clock_start + (block * 10 * k_ms);
}

struct Rig {
    explicit Rig(std::optional<double> delay_ms, uint32_t prepared = k_bs) {
        XYControllerConfig cfg;
        cfg.m_id = "pad";
        cfg.m_input_delay_ms = delay_ms;
        matrix = std::make_unique<ModulationMatrix>(backend);
        controller = std::make_unique<XYController>(*matrix, cfg);
        controller->prepare(k_sr, prepared);
    }
    void block(int index, uint32_t n = k_bs, bool timed = true) {
        if (timed) { controller->set_block_time(block_time(index)); }
        controller->process_block(transport.next(n));
    }

    FakeBackend backend;
    std::unique_ptr<ModulationMatrix> matrix;
    std::unique_ptr<XYController> controller;
    SimTransport transport;
};

/// One recorded touch: down, moves every 4 ms along a curve, up.
struct GestureEvent {
    int64_t m_time = 0;
    float m_x = 0.0f;
    float m_y = 0.0f;
    bool m_up = false;
};

std::vector<GestureEvent> gesture() {
    std::vector<GestureEvent> events;
    const int64_t start = block_time(0) + (3 * k_ms);
    for (int i = 0; i <= 30; ++i) {
        const float p = static_cast<float>(i) / 30.0f;
        events.push_back({.m_time = start + (i * 4 * k_ms),
                          .m_x = 0.5f + (0.4f * std::sin(6.0f * p)),
                          .m_y = p});
    }
    events.push_back({.m_time = start + (123 * k_ms), .m_up = true});
    return events;
}

/// Feed the gesture, each event arriving @p lateness_ns(i) after it happened,
/// and return out_x, out_y and out_active of every block.
template <typename Lateness>
std::vector<float> play(const std::vector<GestureEvent>& events, Lateness lateness_ns) {
    Rig rig(20.0);
    XYController& c = *rig.controller;
    std::vector<float> out;
    size_t next = 0;
    for (int b = 0; b < 20; ++b) {
        while (next < events.size() && events[next].m_time + lateness_ns(next) <= block_time(b)) {
            const GestureEvent& e = events[next++];
            if (e.m_up) {
                c.release(1, e.m_time);
            } else {
                c.touch(1, e.m_x, e.m_y, e.m_time);
            }
        }
        rig.block(b);
        for (uint32_t i = 0; i < k_bs; ++i) {
            out.push_back(c.out_x(0)[i]);
            out.push_back(c.out_y(0)[i]);
            out.push_back(c.out_active(0)[i]);
        }
    }
    return out;
}

}  // namespace

TEST(XYControllerInput, TimedTouchPlaysAtItsSample) {
    Rig rig(20.0);
    XYController& c = *rig.controller;
    rig.block(0);
    c.touch(1, 0.3f, 0.7f, block_time(1) - (17 * k_ms));  // 3 ms into block 1
    rig.block(1);
    EXPECT_EQ(c.out_active(0)[143], 0);
    EXPECT_EQ(c.out_active(0)[144], 1);
    EXPECT_FLOAT_EQ(c.out_x(0)[144], 0.3f);
    EXPECT_FLOAT_EQ(c.out_y(0)[144], 0.7f);
}

TEST(XYControllerInput, DefaultDelayIsOnePreparedBlockPlusFiveMs) {
    Rig rig(std::nullopt);
    XYController& c = *rig.controller;
    c.touch(1, 0.3f, 0.7f, block_time(0) - (12 * k_ms));  // delay 15 ms: 3 ms in
    rig.block(0);
    EXPECT_EQ(c.out_active(0)[143], 0);
    EXPECT_EQ(c.out_active(0)[144], 1);
}

TEST(XYControllerInput, OversizedBlockChunksKeepTheTiming) {
    Rig rig(20.0);
    XYController& c = *rig.controller;
    // Sample 700 of a 960-sample block = sample 220 of its second chunk.
    c.touch(1, 0.3f, 0.7f, block_time(0) - (20 * k_ms) + (700 * k_ms / 48));
    rig.block(0, 2 * k_bs);
    EXPECT_EQ(c.num_samples(), k_bs);
    EXPECT_EQ(c.out_active(0)[219], 0);
    EXPECT_EQ(c.out_active(0)[220], 1);
}

TEST(XYControllerInput, WithoutBlockTimeTouchesAreSpreadAsUntimed) {
    Rig timed(20.0);
    Rig untimed(20.0);
    timed.controller->touch(1, 0.3f, 0.7f, block_time(0));
    timed.controller->touch(1, 0.6f, 0.2f, block_time(0) + k_ms);
    untimed.controller->touch(1, 0.3f, 0.7f);
    untimed.controller->touch(1, 0.6f, 0.2f);
    timed.block(0, k_bs, false);
    untimed.block(0, k_bs, false);
    for (uint32_t i = 0; i < k_bs; ++i) {
        ASSERT_EQ(timed.controller->out_x(0)[i], untimed.controller->out_x(0)[i]) << i;
        ASSERT_EQ(timed.controller->out_active(0)[i], untimed.controller->out_active(0)[i]) << i;
    }
}

// Arrival jitter within the delay's margin (here 10 ms over one block) changes
// nothing: the output follows the timestamps, not the arrival times.
TEST(XYControllerInput, JitteredArrivalMatchesIdealTiming) {
    const auto events = gesture();
    const auto ideal = play(events, [](size_t) { return int64_t{0}; });
    std::mt19937 rng(11);
    std::uniform_int_distribution<int64_t> jitter(0, 5 * k_ms);
    std::vector<int64_t> lateness(events.size());
    for (auto& l : lateness) { l = jitter(rng); }
    const auto jittered = play(events, [&](size_t i) { return lateness[i]; });

    ASSERT_EQ(ideal.size(), jittered.size());
    for (size_t i = 0; i < ideal.size(); ++i) { ASSERT_EQ(ideal[i], jittered[i]) << i; }

    // Ramps, not a 4 ms staircase: x takes far more values than there are moves.
    std::set<float> xs;
    for (size_t i = 0; i < ideal.size(); i += 3) { xs.insert(ideal[i]); }
    EXPECT_GT(xs.size(), 10 * events.size());
}

TEST(XYControllerInput, ConcurrentTimedTouchesAndAudio) {
    Rig rig(std::nullopt, 64);
    XYController& c = *rig.controller;
    std::atomic<bool> done{false};
    std::thread audio([&] {
        while (!done.load(std::memory_order_acquire)) {
            c.set_block_time(XYController::clock_now_ns());
            c.process_block(rig.transport.next(64));
        }
        for (int i = 0; i < 8; ++i) {
            c.set_block_time(XYController::clock_now_ns() + (100 * k_ms));
            c.process_block(rig.transport.next(64));
        }
    });

    std::mt19937 rng(3);
    std::uniform_real_distribution<float> pos(0.0f, 1.0f);
    for (int i = 0; i < 20000; ++i) {
        const int64_t now = XYController::clock_now_ns();
        if (i % 50 == 49) {
            c.release(1, now);
        } else {
            c.touch(1, pos(rng), pos(rng), now);
        }
    }
    c.release(1, XYController::clock_now_ns());
    while (!c.flush()) { std::this_thread::yield(); }
    done.store(true, std::memory_order_release);
    audio.join();

    EXPECT_EQ(c.out_active(0)[63], 0);
    EXPECT_GE(c.out_x(0)[63], 0.0f);
    EXPECT_LE(c.out_x(0)[63], 1.0f);
}
