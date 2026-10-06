#include <gtest/gtest.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionRecorder.h>
#include <tanh/modulation/SmartHandle.h>
#include <tanh/modulation/XYController.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "MotionRecorderTestHelpers.h"
#include "XYControllerTestHelpers.h"

using namespace thl::modulation;
using thl::dsp::transport::TransportInfo;
using xy_test::FakeBackend;
using xy_test::k_sr;
using xy_test::SimTransport;

namespace {

constexpr uint32_t k_bs = 64;
constexpr uint32_t k_voices = 4;

struct Fixture {
    Fixture() {
        for (uint32_t n = 0; n < k_voices; ++n) {
            backend.add("s" + std::to_string(n) + ".a", 0.0f);
            backend.add("s" + std::to_string(n) + ".wet", 0.0f);
        }
        matrix = std::make_unique<ModulationMatrix>(backend);
        XYControllerConfig cfg;
        cfg.m_id = "single";
        cfg.m_num_voices = k_voices;
        cfg.m_recorder.m_max_points = 4096;
        controller = std::make_unique<XYController>(*matrix, cfg);
        for (uint32_t n = 0; n < k_voices; ++n) {
            routes.push_back(
                controller->route(XYPadAxis::X, "s" + std::to_string(n) + ".a", {}, n));
            routes.push_back(
                controller->route(XYPadAxis::Active, "s" + std::to_string(n) + ".wet", {}, n));
        }
        matrix->prepare(k_sr, k_bs);
    }
    ~Fixture() { controller.reset(); }

    FakeBackend backend;
    std::unique_ptr<ModulationMatrix> matrix;
    std::unique_ptr<XYController> controller;
    std::vector<uint32_t> routes;
    SimTransport transport;
};

/// Internal consistency of one frame (all fields from the same block).
bool frame_ok(const XYFrame& f, std::string& why) {
    if (f.m_num_voices != k_voices) {
        why = "num_voices";
        return false;
    }
    for (uint32_t v = 0; v < XYFrame::k_max_voices; ++v) {
        const XYVoiceFrame& x = f.m_voices[v];
        if (v >= k_voices) {
            if (x.m_has_value || x.m_active != 0 || x.m_layer != XYLayer::None) {
                why = "unused voice";
                return false;
            }
            continue;
        }
        const bool unit = x.m_x >= 0.0f && x.m_x <= 1.0f && x.m_y >= 0.0f && x.m_y <= 1.0f &&
                          x.m_motion.m_phase >= 0.0f && x.m_motion.m_phase <= 1.0f;
        if (!unit) {
            why = "range";
            return false;
        }
        if (x.m_touched != (x.m_layer == XYLayer::Touch)) {
            why = "touched vs layer";
            return false;
        }
        if (x.m_layer != XYLayer::None && (x.m_active == 0 || !x.m_has_value)) {
            why = "layer without gate";
            return false;
        }
        if (x.m_layer == XYLayer::None && !f.m_latch && x.m_active != 0) {
            why = "gate without layer";
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(XYControllerThreads, SnapshotConsistentUnderConcurrentUiMessageAndAudio) {
    Fixture fx;
    XYController& c = *fx.controller;
    constexpr int k_blocks = 6000;  // 8 s of audio at 64 samples
    std::atomic<bool> audio_done{false};

    std::thread audio([&] {
        for (int b = 0; b < k_blocks; ++b) {
            c.set_transport(fx.transport.next(k_bs));
            fx.matrix->process(k_bs);
        }
        audio_done.store(true, std::memory_order_release);
    });

    std::thread message([&] {
        int k = 0;
        while (!audio_done.load(std::memory_order_acquire)) {
            c.service();
            if (k % 50 == 0) {
                c.recorder(static_cast<uint32_t>(k / 50) % k_voices)
                    .load_lane(motion_test::beats_lane(2.0, 48));
            }
            if (k % 7 == 0) {
                c.set_route_enabled(fx.routes[static_cast<size_t>(k / 7) % fx.routes.size()],
                                    (k / 7) % 2 == 0);
            }
            ++k;
            std::this_thread::yield();
        }
    });

    // UI thread (this one): touches, recorder commands, frames, trail, path.
    uint64_t reads = 0;
    uint64_t last_seq = 0;
    uint64_t last_time = 0;
    int bad = 0;
    std::string why;
    std::array<uint32_t, k_voices> seen_version{};
    size_t path_points = 0;
    std::vector<XYPathPoint> trail(256);
    size_t trail_points = 0;
    XYFrame f;
    for (uint64_t i = 0; !audio_done.load(std::memory_order_acquire) || i < 1000; ++i) {
        const float ph = static_cast<float>(i) * 0.001f;
        const TouchId id = 1 + ((i / 2000) % 3);
        if (i % 2000 == 1999) {
            c.release(id);
        } else {
            c.touch(id, 0.5f + (0.45f * std::sin(ph)), 0.5f + (0.45f * std::cos(ph)));
        }
        if (i % 5000 == 0) {
            (void)c.recorder(static_cast<uint32_t>(i / 5000) % k_voices).arm(LoopLength::Free);
        }
        if (i % 3000 == 0) { c.set_latch(((i / 3000) % 2) != 0); }
        c.flush();

        if (c.read_frame(f)) {
            ++reads;
            if (!frame_ok(f, why)) { ++bad; }
            if (f.m_sequence <= last_seq || f.m_sample_time < last_time) { ++bad; }
            last_seq = f.m_sequence;
            last_time = f.m_sample_time;
            for (uint32_t v = 0; v < k_voices; ++v) {
                if (f.m_voices[v].m_path_version != seen_version[v]) {
                    seen_version[v] = f.m_voices[v].m_path_version;
                    c.read_path(v,
                                [&](const MotionLane& lane) { path_points += lane.num_points(); });
                }
            }
        }
        trail_points += c.drain_trail(trail);
        if (i > 2'000'000) { break; }  // safety net
    }
    c.release_all();
    audio.join();
    message.join();

    EXPECT_EQ(bad, 0) << why;
    EXPECT_GT(reads, 100u);
    EXPECT_GT(path_points, 0u);
    EXPECT_EQ(last_time % k_bs, 0u);
    (void)trail_points;
}

namespace {

float run_block(ModulationMatrix& matrix,
                std::span<XYController* const> controllers,
                const TransportInfo& t,
                SmartHandle<float>& probe) {
    for (auto* c : controllers) { c->set_transport(t); }
    matrix.process(t.m_num_samples);
    float acc = probe.load(0);
    for (auto* c : controllers) {
        acc += c->out_x(0)[0] + static_cast<float>(c->change_points(0).size());
    }
    return acc;
}

}  // namespace

// Touches (nearest dot), recording, publication, playback, glide, latch, frames,
// trail, routing toggles and reset() on several controllers. The RTSan preset
// checks the audio path.
TEST(XYController, FullScenario) {
    FakeBackend backend;
    for (int n = 1; n <= 4; ++n) {
        backend.add("s" + std::to_string(n) + ".a", 0.0f);
        backend.add("s" + std::to_string(n) + ".b", 0.0f);
    }
    ModulationMatrix matrix(backend);
    XYControllerConfig cfg;
    cfg.m_id = "multi";
    cfg.m_num_voices = 4;
    cfg.m_recorder.m_max_points = 8192;
    XYController multi(matrix, cfg);
    std::vector<std::unique_ptr<XYController>> singles;
    std::vector<uint32_t> multi_routes;
    std::vector<uint32_t> single_routes;
    for (int n = 1; n <= 4; ++n) {
        const std::string target = "s" + std::to_string(n) + ".a";
        cfg.m_id = "pad" + std::to_string(n);
        cfg.m_num_voices = 1;
        singles.push_back(std::make_unique<XYController>(matrix, cfg));
        XYRouteOptions options;
        options.m_enabled = false;
        multi_routes.push_back(
            multi.route(XYPadAxis::X, target, options, static_cast<uint32_t>(n - 1)));
        single_routes.push_back(singles.back()->route(XYPadAxis::X, target));
    }
    auto use_multi = [&](bool on) {
        for (const uint32_t id : multi_routes) { multi.set_route_enabled(id, on); }
        for (size_t n = 0; n < single_routes.size(); ++n) {
            singles[n]->set_route_enabled(single_routes[n], !on);
        }
    };
    std::vector<XYController*> all{&multi};
    for (auto& c : singles) { all.push_back(c.get()); }
    auto probe = matrix.get_smart_handle<float>("s1.a");
    matrix.prepare(k_sr, 256);
    SimTransport transport;
    float acc = 0.0f;
    std::vector<XYPathPoint> trail(512);
    XYFrame frame;
    auto run = [&](int blocks) {
        for (int b = 0; b < blocks; ++b) {
            acc += run_block(matrix, all, transport.next(256), probe);
            multi.read_frame(frame);
            multi.drain_trail(trail);
        }
    };

    run(4);
    ASSERT_TRUE(multi.recorder(1).arm(LoopLength::Bars1));
    for (int b = 0; b < 400; ++b) {
        multi.touch(7, 0.3f + (0.2f * std::sin(static_cast<float>(b) * 0.05f)), 0.4f);
        singles[0]->touch(1, 0.6f, 0.2f + (0.001f * static_cast<float>(b)));
        run(1);
    }
    multi.release(7);
    singles[0]->release(1);
    run(10);
    multi.service();
    use_multi(true);
    run(20);
    multi.set_latch(true);
    multi.touch(8, 0.9f, 0.9f);
    run(10);
    multi.release(8);
    run(10);
    multi.reset();
    run(5);
    use_multi(false);
    singles[2]->recorder().load_lane(motion_test::beats_lane(4.0, 96));
    run(40);
    multi.set_ui_attached(false);
    run(5);
    EXPECT_TRUE(std::isfinite(acc));
    EXPECT_EQ(multi.stale_transport_blocks(), 0u);
}
