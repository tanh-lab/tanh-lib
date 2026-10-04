// XYController: registration, layers (touch > motion), nearest-dot voices,
// Single/PerEffect switching, order independence, UI frames and trail.
// Threads and sanitizers: test_XYControllerThreads.cpp.

#include <gtest/gtest.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionRecorder.h>
#include <tanh/modulation/SmartHandle.h>
#include <tanh/modulation/XYController.h>
#include <tanh/modulation/XYPad.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#include "MotionRig.h"
#include "XYControllerRig.h"

using namespace thl::modulation;
using thl::dsp::transport::TransportInfo;
using xy_test::FakeBackend;
using xy_test::k_sr;
using xy_test::SimTransport;

namespace {

constexpr uint32_t k_bs = 256;

XYControllerConfig config(const std::string& id, uint32_t voices = 1) {
    XYControllerConfig c;
    c.m_id = id;
    c.m_num_voices = voices;
    return c;
}

bool has_source(const ModulationMatrix& m, const ModulationSource* src) {
    for (const auto& step : m.get_schedule()) {
        if (const auto* b = std::get_if<BulkStep>(&step); b != nullptr && b->m_source == src) {
            return true;
        }
    }
    return false;
}

/// Matrix + backend with slot parameters s<n>.a / s<n>.b / s<n>.wet.
struct Engine {
    explicit Engine(int slots = 8) {
        for (int n = 1; n <= slots; ++n) {
            backend.add(slot(n, "a"), 0.05f);
            backend.add(slot(n, "b"), 0.05f);
            backend.add(slot(n, "wet"), 0.3f);
        }
        matrix = std::make_unique<ModulationMatrix>(backend);
    }
    static std::string slot(int n, const char* p) { return "s" + std::to_string(n) + "." + p; }
    SmartHandle<float> handle(int n, const char* p) {
        return matrix->get_smart_handle<float>(slot(n, p));
    }
    void block(std::span<XYController* const> controllers, uint32_t n = k_bs) {
        const TransportInfo t = transport.next(n);
        for (auto* c : controllers) { c->set_transport(t); }
        matrix->process(n);
    }

    FakeBackend backend;
    std::unique_ptr<ModulationMatrix> matrix;
    SimTransport transport;
};

/// Put voice @p v of @p c at (x, y) with a tap (blocks drive @p cs).
void place(Engine& e,
           std::span<XYController* const> cs,
           XYController& c,
           uint32_t v,
           float x,
           float y,
           TouchId id = 900) {
    ASSERT_TRUE(c.touch_voice(v, id, x, y));
    e.block(cs);
    ASSERT_TRUE(c.release(id));
    e.block(cs);
}

}  // namespace

// ── Registration ──────────────────────────────────────────────────────────────

TEST(XYController, RegistersSourcesAndRemovesThemAndItsRoutingsInDtor) {
    Engine e(2);
    {
        XYController c(*e.matrix, config("pad"));
        EXPECT_EQ(c.source_id(XYPadAxis::X), "pad.x");
        EXPECT_EQ(c.source_id(XYPadAxis::Active), "pad.active");
        EXPECT_NE(c.route(XYPadAxis::X, "s1.a"), k_invalid_routing_id);
        EXPECT_NE(c.route(XYPadAxis::Active, "s1.wet"), k_invalid_routing_id);
        EXPECT_EQ(c.route(XYPadAxis::Y, "nope"), k_invalid_routing_id);
        EXPECT_EQ(c.routes().size(), 2u);
        EXPECT_TRUE(has_source(*e.matrix, &c.source(XYPadAxis::X)));
        EXPECT_EQ(e.matrix->to_json(false).size(), 2u);
    }
    EXPECT_TRUE(e.matrix->get_schedule().empty());
    EXPECT_EQ(e.matrix->to_json(false).size(), 0u);

    XYController again(*e.matrix, config("pad", 3));  // same id, now with voices
    EXPECT_EQ(again.source_id(XYPadAxis::Y, 2), "pad.2.y");
    EXPECT_EQ(e.matrix->get_schedule().size(), 9u);
}

TEST(XYController, UnrouteOnlyOwnedRoutings) {
    Engine e(2);
    XYController a(*e.matrix, config("a"));
    XYController b(*e.matrix, config("b"));
    const uint32_t ra = a.route(XYPadAxis::X, "s1.a");
    const uint32_t rb = b.route(XYPadAxis::X, "s2.a");
    EXPECT_FALSE(a.unroute(rb));
    EXPECT_FALSE(a.set_route_enabled(rb, false));
    EXPECT_FALSE(a.set_route_depth(rb, 0.5f));
    EXPECT_TRUE(a.set_route_depth(ra, 0.5f));
    EXPECT_TRUE(a.unroute(ra));
    EXPECT_TRUE(a.routes().empty());
    EXPECT_EQ(b.routes().size(), 1u);
}

// ── Layers ────────────────────────────────────────────────────────────────────

TEST(XYController, TouchDrivesTargetsAndGateFallsBackOnRelease) {
    Engine e(1);
    XYController c(*e.matrix, config("pad"));
    XYController* cs[] = {&c};
    c.route(XYPadAxis::X, "s1.a");
    c.route(XYPadAxis::Y, "s1.b");
    c.route(XYPadAxis::Active, "s1.wet");
    auto a = e.handle(1, "a");
    auto b = e.handle(1, "b");
    auto wet = e.handle(1, "wet");
    e.matrix->prepare(k_sr, k_bs);

    e.block(cs);
    EXPECT_FLOAT_EQ(a.load(k_bs - 1), 0.05f);  // no value yet: base
    EXPECT_FLOAT_EQ(wet.load(k_bs - 1), 0.3f);

    c.touch(1, 0.7f, 0.2f);
    e.block(cs);
    EXPECT_FLOAT_EQ(a.load(k_bs - 1), 0.7f);
    EXPECT_FLOAT_EQ(b.load(k_bs - 1), 0.2f);
    EXPECT_FLOAT_EQ(wet.load(k_bs - 1), 1.0f);
    EXPECT_EQ(c.out_layer(0)[k_bs - 1], XYLayer::Touch);

    c.release(1);
    e.block(cs);
    EXPECT_FLOAT_EQ(a.load(k_bs - 1), 0.7f);    // x / y hold
    EXPECT_FLOAT_EQ(wet.load(k_bs - 1), 0.3f);  // gate closed → base
    EXPECT_EQ(c.out_layer(0)[k_bs - 1], XYLayer::None);
}

// The controller adds nothing to the recorder's composition: its outputs are
// bit-identical to a standalone pad → recorder chain fed the same input, so the
// recorder's single glide back after a release is the only one (no double glide).
TEST(XYController, TouchOverMotionHandoverIsTheRecordersSingleGlide) {
    Engine e(1);
    XYController c(*e.matrix, config("pad"));
    XYController* cs[] = {&c};
    c.route(XYPadAxis::X, "s1.a");
    e.matrix->prepare(k_sr, k_bs);

    XYPad ref_pad;
    MotionRecorder ref_rec;
    ref_pad.prepare(k_bs);
    ref_rec.prepare(k_sr, k_bs);
    SimTransport ref_t;

    const MotionLane lane = motion_test::beats_lane(4.0, 96);
    c.recorder().load_lane(lane);
    ref_rec.load_lane(lane);

    std::vector<float> x, ref_x;
    std::vector<XYLayer> layer;
    auto step = [&] {
        e.block(cs);
        ref_pad.process_block(k_bs);
        ref_rec.process(ref_t.next(k_bs), ref_pad.primary(), k_bs);
        for (uint32_t i = 0; i < k_bs; ++i) {
            x.push_back(c.out_x(0)[i]);
            layer.push_back(c.out_layer(0)[i]);
            ref_x.push_back(ref_rec.out_x()[i]);
        }
    };
    for (int b = 0; b < 40; ++b) { step(); }
    c.touch(1, 0.95f, 0.95f);
    ref_pad.touch(1, 0.95f, 0.95f);
    for (int b = 0; b < 40; ++b) { step(); }
    const size_t release_at = x.size();
    c.release(1);
    ref_pad.release(1);
    for (int b = 0; b < 40; ++b) { step(); }

    ASSERT_EQ(x.size(), ref_x.size());
    for (size_t i = 0; i < x.size(); ++i) { ASSERT_EQ(x[i], ref_x[i]) << i; }

    // Layers: Motion → Touch → Motion.
    EXPECT_EQ(layer[100], XYLayer::Motion);
    EXPECT_EQ(layer[release_at - 1], XYLayer::Touch);
    EXPECT_EQ(layer.back(), XYLayer::Motion);

    // The release → playback glide (25 ms raised cosine) has no step: its
    // steepest sample is at most π/2 × the average slope (plus the lane's own motion).
    const size_t glide = 1200;
    const float total = std::abs(x[release_at + glide + 64] - x[release_at - 1]);
    const double bound = (std::numbers::pi / 2.0 * total / glide) + 2e-3;
    EXPECT_LE(motion_test::max_sample_step(x, release_at, release_at + glide + 64), bound);

    // Matrix target equals the controller output.
    EXPECT_FLOAT_EQ(e.handle(1, "a").load(k_bs - 1), x.back());
}

// Touch semantics: after a release playback resumes at the current loop phase,
// not at the loop start.
TEST(XYController, ReleaseResumesPlaybackAtTheCurrentPhase) {
    Engine e(1);
    XYController c(*e.matrix, config("pad"));
    XYController* cs[] = {&c};
    e.matrix->prepare(k_sr, k_bs);
    const MotionLane lane = motion_test::beats_lane(4.0, 96);
    c.recorder().load_lane(lane);
    for (int b = 0; b < 10; ++b) { e.block(cs); }
    c.touch(1, 0.1f, 0.1f);
    for (int b = 0; b < 200; ++b) { e.block(cs); }  // ~1 bar held
    c.release(1);
    for (int b = 0; b < 20; ++b) { e.block(cs); }  // glide done
    // The last sample of the block sits one sample before the transport's beat.
    const double beat = e.transport.m_beat - (120.0 / 60.0 / k_sr);
    EXPECT_NEAR(c.recorder().playback_phase(), std::fmod(e.transport.m_beat, 4.0), 1e-6);
    EXPECT_NEAR(c.out_x(0)[k_bs - 1], lane.sample(std::fmod(beat, 4.0)).m_x, 0.01f);
    EXPECT_NEAR(c.out_y(0)[k_bs - 1], lane.sample(std::fmod(beat, 4.0)).m_y, 0.01f);
    EXPECT_EQ(c.out_layer(0)[k_bs - 1], XYLayer::Motion);
}

// A host seek reaches the controller as k_jumped from the clock: the playing
// lane glides to the new phase. With reset() (host reset) in the same block the
// recorder re-locks at once: no glide, the first sample is already the lane at
// the new beat.
TEST(XYController, ResetRelocksWithoutGlide) {
    for (const bool with_reset : {false, true}) {
        SCOPED_TRACE(with_reset ? "reset()" : "seek only");
        Engine e(1);
        XYController c(*e.matrix, config("pad"));
        XYController* cs[] = {&c};
        e.matrix->prepare(k_sr, k_bs);
        const MotionLane lane = motion_test::beats_lane(4.0, 96);
        c.recorder().load_lane(lane);
        for (int b = 0; b < 40; ++b) { e.block(cs); }
        const float before = c.out_x(0)[k_bs - 1];

        e.transport.m_beat += 1.3;  // seek
        TransportInfo t = e.transport.next(k_bs);
        t.m_flags |= TransportInfo::k_jumped;
        t.m_jump_delta_beats = 1.3;
        if (with_reset) { c.reset(); }
        c.set_transport(t);
        e.matrix->process(k_bs);

        const float want = lane.sample(std::fmod(t.m_beat_position, 4.0)).m_x;
        if (with_reset) {
            EXPECT_EQ(c.recorder().jump_glide_count(), 0u);
            EXPECT_NEAR(c.out_x(0)[0], want, 0.01f);
        } else {
            EXPECT_EQ(c.recorder().jump_glide_count(), 1u);
            EXPECT_NEAR(c.out_x(0)[0], before, 0.01f);  // glides from the last output
        }
        for (int b = 0; b < 10; ++b) { e.block(cs); }  // both end on the lane
        const double beat = e.transport.m_beat - (120.0 / 60.0 / k_sr);
        EXPECT_NEAR(c.out_x(0)[k_bs - 1], lane.sample(std::fmod(beat, 4.0)).m_x, 0.01f);
    }
}

TEST(XYController, LatchKeepsTheGateOpenUntilTurnedOff) {
    Engine e(1);
    XYController c(*e.matrix, config("pad"));
    XYController* cs[] = {&c};
    c.route(XYPadAxis::Active, "s1.wet");
    auto wet = e.handle(1, "wet");
    e.matrix->prepare(k_sr, k_bs);
    c.set_latch(true);
    c.touch(1, 0.4f, 0.4f);
    e.block(cs);
    c.release(1);
    e.block(cs);
    e.block(cs);
    EXPECT_FLOAT_EQ(wet.load(k_bs - 1), 1.0f);
    EXPECT_EQ(c.out_layer(0)[k_bs - 1], XYLayer::None);

    c.set_latch(false);
    e.block(cs);
    EXPECT_FLOAT_EQ(wet.load(0), 0.3f);
    ASSERT_FALSE(c.change_points(0).empty());
    EXPECT_EQ(c.change_points(0).front(), 0u);

    c.set_latch(true);
    c.touch(2, 0.4f, 0.4f);
    e.block(cs);
    c.release(2);
    e.block(cs);
    c.reset();  // host reset closes a latched gate
    e.block(cs);
    EXPECT_FLOAT_EQ(wet.load(k_bs - 1), 0.3f);
}

// ── Voices: nearest dot ───────────────────────────────────────────────────────

TEST(XYController, TouchGrabsTheNearestEnabledDotAndMovesOnlyThatVoice) {
    Engine e(8);
    XYController c(*e.matrix, config("single", 8));
    XYController* cs[] = {&c};
    e.matrix->prepare(k_sr, k_bs);
    e.block(cs);
    for (uint32_t v = 0; v < 8; ++v) {
        place(e, cs, c, v, 0.1f + (0.1f * static_cast<float>(v)), 0.5f);
    }

    // Near dot 5 (x = 0.6).
    ASSERT_TRUE(c.touch(1, 0.62f, 0.55f));
    EXPECT_EQ(c.voice_of(1), 5u);
    // A second finger at the same place takes the nearest dot nobody holds.
    ASSERT_TRUE(c.touch(2, 0.62f, 0.55f));
    EXPECT_EQ(c.voice_of(2), 6u);  // x = 0.7 is nearer than 0.5
    // Dragging finger 1 across dot 2 keeps voice 5.
    ASSERT_TRUE(c.touch(1, 0.3f, 0.9f));
    EXPECT_EQ(c.voice_of(1), 5u);
    e.block(cs);
    EXPECT_FLOAT_EQ(c.out_x(5)[k_bs - 1], 0.3f);
    EXPECT_FLOAT_EQ(c.out_y(5)[k_bs - 1], 0.9f);
    EXPECT_FLOAT_EQ(c.out_x(2)[k_bs - 1], 0.3f);  // dot 2 did not move
    EXPECT_FLOAT_EQ(c.out_y(2)[k_bs - 1], 0.5f);
    EXPECT_EQ(c.out_active(2)[k_bs - 1], 0);
    EXPECT_EQ(c.out_active(5)[k_bs - 1], 1);
    c.release(1);
    c.release(2);
    EXPECT_EQ(c.voice_of(1), XYController::k_no_voice);
    e.block(cs);

    // Disabled voices (inactive slots) are never grabbed.
    c.set_voice_enabled(3, false);
    ASSERT_TRUE(c.touch(3, 0.4f, 0.5f));  // exactly on dot 3
    EXPECT_NE(c.voice_of(3), 3u);
    c.release(3);
    for (uint32_t v = 0; v < 8; ++v) { c.set_voice_enabled(v, false); }
    EXPECT_FALSE(c.touch(4, 0.5f, 0.5f));
    EXPECT_FALSE(c.touch(5, NAN, 0.5f));
}

// ── Mode switch ───────────────────────────────────────────────────────────────

namespace {

/// The ElasticFX wiring: 8 PerEffect controllers (1 voice) and one Single
/// controller (8 voices), slot n's a / b ← PerEffect n and ← Single voice n.
struct PadSystem {
    explicit PadSystem(Engine& e) : m_router(*e.matrix) {
        m_single = std::make_unique<XYController>(*e.matrix, config("single", 8));
        for (size_t n = 0; n < 8; ++n) {
            m_per_effect[n] =
                std::make_unique<XYController>(*e.matrix, config("pad" + std::to_string(n + 1)));
        }
        for (size_t n = 0; n < 8; ++n) {
            for (const auto* p : {"a", "b"}) {
                const auto axis = p[0] == 'a' ? XYPadAxis::X : XYPadAxis::Y;
                EXPECT_TRUE(m_router.add_target(Engine::slot(static_cast<int>(n) + 1, p),
                                                axis,
                                                *m_per_effect[n],
                                                0,
                                                *m_single,
                                                static_cast<uint32_t>(n)));
            }
        }
        all.push_back(m_single.get());
        for (auto& c : m_per_effect) { all.push_back(c.get()); }
    }
    std::unique_ptr<XYController> m_single;
    std::array<std::unique_ptr<XYController>, 8> m_per_effect;
    XYModeRouter m_router;
    std::vector<XYController*> all;
};

}  // namespace

TEST(XYController, ModeSwitchSelectsTheDriverPerSlot) {
    Engine e(8);
    PadSystem ps(e);
    e.matrix->prepare(k_sr, k_bs);
    e.block(ps.all);
    for (uint32_t n = 0; n < 8; ++n) {
        place(e, ps.all, *ps.m_per_effect[n], 0, 0.1f * static_cast<float>(n + 1), 0.2f, 10 + n);
        place(e, ps.all, *ps.m_single, n, 0.9f - (0.1f * static_cast<float>(n)), 0.8f, 20 + n);
    }
    e.block(ps.all);
    for (int n = 1; n <= 8; ++n) {
        EXPECT_FLOAT_EQ(e.handle(n, "a").load(0), 0.1f * static_cast<float>(n)) << n;
        EXPECT_FLOAT_EQ(e.handle(n, "b").load(0), 0.2f) << n;
    }
    ps.m_router.set_mode(XYPadMode::Single);
    e.block(ps.all);
    for (int n = 1; n <= 8; ++n) {
        EXPECT_FLOAT_EQ(e.handle(n, "a").load(0), 0.9f - (0.1f * static_cast<float>(n - 1))) << n;
        EXPECT_FLOAT_EQ(e.handle(n, "b").load(0), 0.8f) << n;
    }
    ps.m_router.set_mode(XYPadMode::PerEffect);
    e.block(ps.all);
    EXPECT_FLOAT_EQ(e.handle(3, "a").load(0), 0.3f);
}

// 1000 mode toggles from a second thread while the audio thread runs: no
// rebuild (same published config) and, with both drivers of a slot on the same
// spot, no sample of any slot ever leaves that spot (no base-value gap, no step).
TEST(XYController, ModeSwitchWithoutRebuildAndWithoutStep) {
    Engine e(8);
    PadSystem ps(e);
    e.matrix->prepare(k_sr, k_bs);
    e.block(ps.all);
    for (uint32_t n = 0; n < 8; ++n) {
        const float x = 0.1f * static_cast<float>(n + 1);
        place(e, ps.all, *ps.m_per_effect[n], 0, x, 0.6f, 10 + n);
        place(e, ps.all, *ps.m_single, n, x, 0.6f, 20 + n);
    }
    std::vector<SmartHandle<float>> a, b;
    for (int n = 1; n <= 8; ++n) {
        a.push_back(e.handle(n, "a"));
        b.push_back(e.handle(n, "b"));
    }
    const ProcessingConfig* before = nullptr;
    {
        const auto scope = e.matrix->read_scope();
        before = &scope.data();
    }

    std::atomic<bool> stop{false};
    std::atomic<int> blocks{0};
    std::atomic<int> bad{0};
    std::thread audio([&] {
        while (!stop.load(std::memory_order_acquire)) {
            e.block(ps.all);
            for (size_t n = 0; n < 8; ++n) {
                const float x = 0.1f * static_cast<float>(n + 1);
                for (uint32_t i = 0; i < k_bs; ++i) {
                    if (a[n].load(i) != x || b[n].load(i) != 0.6f) { bad.fetch_add(1); }
                }
            }
            blocks.fetch_add(1, std::memory_order_release);
        }
    });
    for (int k = 0; k < 1000; ++k) {
        ps.m_router.set_mode((k % 2) == 0 ? XYPadMode::Single : XYPadMode::PerEffect);
        const int target = blocks.load(std::memory_order_acquire) + 1;
        while (blocks.load(std::memory_order_acquire) < target) { std::this_thread::yield(); }
    }
    stop.store(true, std::memory_order_release);
    audio.join();
    EXPECT_EQ(bad.load(), 0);
    const auto scope = e.matrix->read_scope();
    EXPECT_EQ(&scope.data(), before);
}

// ── 8 controllers, 8 slots ────────────────────────────────────────────────────

TEST(XYController, EightControllersDriveEightSlotsWithoutCrosstalk) {
    Engine e(8);
    std::vector<std::unique_ptr<XYController>> pads;
    std::vector<XYController*> ptrs;
    for (int n = 1; n <= 8; ++n) {
        pads.push_back(
            std::make_unique<XYController>(*e.matrix, config("pad" + std::to_string(n))));
        ptrs.push_back(pads.back().get());
        pads.back()->route(XYPadAxis::X, Engine::slot(n, "a"));
        pads.back()->route(XYPadAxis::Y, Engine::slot(n, "b"));
        pads.back()->route(XYPadAxis::Active, Engine::slot(n, "wet"));
    }
    e.matrix->prepare(k_sr, k_bs);
    // Odd pads touched, even pads play a lane.
    for (size_t n = 0; n < 8; ++n) {
        if (n % 2 == 0) {
            pads[n]->touch(1,
                           0.05f * static_cast<float>(n + 1),
                           1.0f - (0.1f * static_cast<float>(n)));
        } else {
            pads[n]->recorder().load_lane(
                motion_test::beats_lane(2.0 + static_cast<double>(n), 96));
        }
    }
    for (int b = 0; b < 30; ++b) { e.block(ptrs); }
    for (size_t n = 0; n < 8; ++n) {
        const int slot = static_cast<int>(n) + 1;
        auto a = e.handle(slot, "a");
        auto bb = e.handle(slot, "b");
        auto wet = e.handle(slot, "wet");
        for (uint32_t i = 0; i < k_bs; i += 17) {
            EXPECT_FLOAT_EQ(a.load(i), pads[n]->out_x(0)[i]) << n;
            EXPECT_FLOAT_EQ(bb.load(i), pads[n]->out_y(0)[i]) << n;
            EXPECT_FLOAT_EQ(wet.load(i), 1.0f) << n;
        }
        if (n % 2 == 0) {
            EXPECT_FLOAT_EQ(a.load(0), 0.05f * static_cast<float>(n + 1));
            EXPECT_EQ(pads[n]->out_layer(0)[0], XYLayer::Touch);
        } else {
            EXPECT_EQ(pads[n]->out_layer(0)[0], XYLayer::Motion);
        }
    }
}

// ── Order independence ────────────────────────────────────────────────────────

// Whichever output the matrix reaches first drives the controller, exactly
// once per block.
TEST(XYController, DrivesOncePerBlockInAnyOutputOrder) {
    Engine e(1);
    XYController c(*e.matrix, config("pad"));
    c.prepare(k_sr, k_bs);
    std::array<ModulationSource*, 3> outs{&c.source(XYPadAxis::X),
                                          &c.source(XYPadAxis::Y),
                                          &c.source(XYPadAxis::Active)};
    std::array<size_t, 3> order{0, 1, 2};
    int perm = 0;
    SimTransport t;
    do {
        const float x = 0.1f * static_cast<float>(perm + 1);
        c.touch(1, x, 1.0f - x);
        const uint64_t driven = c.blocks_driven();
        c.set_transport(t.next(k_bs));
        for (auto* o : outs) { o->clear_per_block(); }
        for (const size_t i : order) { outs[i]->pre_process_block(); }
        for (const size_t i : order) { outs[i]->process(k_bs, 0); }
        EXPECT_EQ(c.blocks_driven(), driven + 1);
        EXPECT_FLOAT_EQ(outs[0]->get_output_at(k_bs - 1), x);
        EXPECT_FLOAT_EQ(outs[1]->get_output_at(k_bs - 1), 1.0f - x);
        EXPECT_FLOAT_EQ(outs[2]->get_output_at(k_bs - 1), 1.0f);
        ++perm;
    } while (std::next_permutation(order.begin(), order.end()));
}

// Two matrices, the same two controllers registered in opposite orders and
// under ids that sort in opposite orders: identical targets, sample by sample.
TEST(XYController, MatrixSourceOrderDoesNotChangeTheOutput) {
    auto run = [](bool swapped) {
        Engine e(2);
        // Role one gets id "aa" in one run and "zz" in the other, and the
        // controllers are constructed in opposite orders.
        std::unique_ptr<XYController> one, two;
        if (swapped) {
            two = std::make_unique<XYController>(*e.matrix, config("aa"));
            one = std::make_unique<XYController>(*e.matrix, config("zz"));
        } else {
            one = std::make_unique<XYController>(*e.matrix, config("aa"));
            two = std::make_unique<XYController>(*e.matrix, config("zz"));
        }
        one->route(XYPadAxis::X, "s1.a");
        one->route(XYPadAxis::Y, "s1.b");
        two->route(XYPadAxis::X, "s2.a");
        two->route(XYPadAxis::Active, "s2.wet");
        e.matrix->prepare(k_sr, k_bs);
        two->recorder().load_lane(motion_test::beats_lane(4.0, 96));
        XYController* cs[] = {one.get(), two.get()};
        std::vector<float> out;
        for (int b = 0; b < 60; ++b) {
            if (b == 5) { one->touch(1, 0.3f, 0.7f); }
            if (b == 20) { two->touch(1, 0.9f, 0.1f); }
            if (b == 30) { one->release(1); }
            if (b == 40) { two->release(1); }
            e.block(cs);
            for (const char* p : {"s1.a", "s1.b", "s2.a", "s2.wet"}) {
                auto h = e.matrix->get_smart_handle<float>(p);
                for (uint32_t i = 0; i < k_bs; ++i) { out.push_back(h.load(i)); }
            }
        }
        return out;
    };
    const auto a = run(false);
    const auto b = run(true);
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) { ASSERT_EQ(a[i], b[i]) << i; }
}

TEST(XYController, ProcessBlockBeforeTheMatrixIsNotDrivenTwice) {
    Engine e(1);
    XYController c(*e.matrix, config("pad"));
    c.route(XYPadAxis::X, "s1.a");
    e.matrix->prepare(k_sr, k_bs);
    SimTransport t;
    c.touch(1, 0.25f, 0.5f);
    for (int b = 0; b < 4; ++b) {
        c.process_block(t.next(100));  // a short host block, pre-matrix
        e.matrix->process(100);
    }
    EXPECT_EQ(c.blocks_driven(), 4u);
    EXPECT_EQ(c.num_samples(), 100u);
    EXPECT_EQ(c.stale_transport_blocks(), 0u);
    EXPECT_FLOAT_EQ(e.handle(1, "a").load(99), 0.25f);
}

// ── UI snapshot ───────────────────────────────────────────────────────────────

TEST(XYController, FramesAreRateLimitedAndPublishedAtOnceOnStateChange) {
    Engine e(1);
    XYController c(*e.matrix, config("pad"));
    XYController* cs[] = {&c};
    e.matrix->prepare(k_sr, 32);
    XYFrame f;
    EXPECT_FALSE(c.read_frame(f));

    e.block(cs, 32);
    ASSERT_TRUE(c.read_frame(f));  // first block
    EXPECT_EQ(f.m_num_voices, 1u);
    EXPECT_EQ(f.m_sample_time, 32u);
    EXPECT_FALSE(c.read_frame(f));

    // 48000 / 240 = 200 samples per frame: idle 32-sample blocks publish every 7th.
    int frames = 0;
    for (int b = 0; b < 70; ++b) {
        e.block(cs, 32);
        frames += c.read_frame(f) ? 1 : 0;
    }
    EXPECT_EQ(frames, 10);

    c.touch(1, 0.5f, 0.5f);  // touch-down: published in that very block
    e.block(cs, 32);
    ASSERT_TRUE(c.read_frame(f));
    EXPECT_TRUE(f.m_voices[0].m_touched);
    EXPECT_EQ(f.m_voices[0].m_layer, XYLayer::Touch);
    EXPECT_EQ(f.m_voices[0].m_active, 1);

    c.set_ui_attached(false);
    c.release(1);
    for (int b = 0; b < 20; ++b) { e.block(cs, 32); }
    EXPECT_FALSE(c.read_frame(f));
    c.set_ui_attached(true);
    e.block(cs, 32);
    ASSERT_TRUE(c.read_frame(f));
    EXPECT_FALSE(f.m_voices[0].m_touched);
    EXPECT_EQ(f.m_sample_time, 32u * 93u);
}

TEST(XYController, PathVersionBumpsOnlyOnChange) {
    Engine e(1);
    XYController c(*e.matrix, config("pad"));
    XYController* cs[] = {&c};
    e.matrix->prepare(k_sr, k_bs);
    XYFrame f;
    e.block(cs);
    ASSERT_TRUE(c.read_frame(f));
    const uint32_t v0 = f.m_voices[0].m_path_version;

    c.recorder().load_lane(motion_test::beats_lane(4.0, 96));
    e.block(cs);
    ASSERT_TRUE(c.read_frame(f));
    EXPECT_EQ(f.m_voices[0].m_path_version, v0 + 1);
    for (int b = 0; b < 100; ++b) {
        e.block(cs);
        if (c.read_frame(f)) { ASSERT_EQ(f.m_voices[0].m_path_version, v0 + 1); }
        EXPECT_FALSE(c.service());  // nothing to publish while it plays
    }
    EXPECT_EQ(f.m_voices[0].m_motion_state, MotionState::Playing);

    // A free take: arm, gesture, release → one more version after service().
    c.recorder().arm(LoopLength::Free);
    e.block(cs);
    for (int b = 0; b < 40; ++b) {
        c.touch(1, 0.2f + (0.01f * static_cast<float>(b)), 0.5f);
        e.block(cs);
    }
    c.release(1);
    e.block(cs);
    EXPECT_TRUE(c.service());
    e.block(cs);
    e.block(cs);
    c.read_frame(f);
    EXPECT_EQ(f.m_voices[0].m_path_version, v0 + 2);
    uint32_t points = 0;
    c.read_path(0,
                [&](const MotionLane& lane) { points = static_cast<uint32_t>(lane.num_points()); });
    EXPECT_GT(points, 0u);
}

TEST(XYController, TrailDropsOldestNotBlocks) {
    Engine e(1);
    XYControllerConfig cfg = config("pad");
    XYController c(*e.matrix, cfg);
    cfg.m_id = "ref";
    XYController ref(*e.matrix, cfg);  // same input, UI detached
    ref.set_ui_attached(false);
    XYController* cs[] = {&c, &ref};
    e.matrix->prepare(k_sr, k_bs);
    e.block(cs);

    c.recorder().record(LoopLength::Free);
    ref.recorder().record(LoopLength::Free);
    const int blocks = static_cast<int>(10.0 * k_sr / k_bs);  // 10 s, no drain
    for (int b = 0; b < blocks; ++b) {
        const float x = 0.5f + (0.4f * std::sin(static_cast<float>(b) * 0.01f));
        c.touch(1, x, 0.5f);
        ref.touch(1, x, 0.5f);
        e.block(cs);
        for (uint32_t i = 0; i < k_bs; ++i) {
            ASSERT_EQ(c.out_x(0)[i], ref.out_x(0)[i]);  // the audio path is unaffected
        }
    }
    XYFrame f;
    ASSERT_TRUE(c.read_frame(f));
    EXPECT_GT(f.m_trail_dropped, 0u);
    std::vector<XYPathPoint> pts(k_xy_trail_capacity + 16);
    const size_t n = c.drain_trail(pts);
    EXPECT_EQ(n, k_xy_trail_capacity);
    // The newest point is from the last block (trail interval 32).
    EXPECT_NEAR(pts[n - 1].m_x, c.out_x(0)[k_bs - 32], 1e-6f);
    EXPECT_GE(pts[n - 1].m_progress, pts[0].m_progress);
    std::vector<XYPathPoint> none(4);
    EXPECT_EQ(ref.drain_trail(none), 0u);
}
