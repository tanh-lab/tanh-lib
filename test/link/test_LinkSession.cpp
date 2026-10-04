// LinkSession over the platform Link SDK (built with TANH_WITH_LINK).

#include <gtest/gtest.h>
#include <tanh/dsp/transport/LinkTransportClock.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/link/LinkSession.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

using thl::dsp::transport::LinkTransportClock;
using thl::dsp::transport::TransportInfo;
using thl::link::LinkSession;

namespace {

constexpr double k_sr = 48000.0;
constexpr uint32_t k_frames = 256;

double rt_block(LinkTransportClock& clk, int64_t host_us) TANH_NONBLOCKING_FUNCTION {
    clk.begin_block(k_frames, host_us);
    const double beat = clk.beat_at_sample(k_frames - 1);
    clk.end_block();
    return beat;
}

}  // namespace

TEST(LinkSession, DisabledSessionIsALocalTimeline) {
    LinkSession session(120.0);
    EXPECT_FALSE(session.is_enabled());
    EXPECT_EQ(session.num_peers(), 0u);
    EXPECT_NEAR(session.tempo(), 120.0, 1e-9);
#ifdef THL_PLATFORM_IOS
    EXPECT_NE(session.settings_view_controller(), nullptr);  // LinkKit settings view
#else
    EXPECT_EQ(session.settings_view_controller(), nullptr);
#endif

    LinkTransportClock clk(session.audio_backend());
    clk.prepare(k_sr);
    auto& backend = session.audio_backend();
    const int64_t t0 = backend.now_us();
    for (int b = 0; b < 50; ++b) {
        clk.begin_block(k_frames, t0 + std::llround(b * k_frames * 1e6 / k_sr));
        if (b > 0) { EXPECT_EQ(clk.discontinuities(), 0u) << "block " << b; }
        // Link time is integer µs, so the in-block slope carries ≤1 µs of rounding.
        EXPECT_NEAR(clk.block_info().m_beats_per_sample, 120.0 / (60.0 * k_sr), 1e-8);
        clk.end_block();
    }
}

TEST(LinkSession, EnableWithoutPeersDoesNotMoveTheBeat) {  // BEATTIME-1
    LinkSession session(120.0);
    LinkTransportClock clk(session.audio_backend());
    clk.prepare(k_sr);
    const int64_t t0 = session.audio_backend().now_us();
    for (int b = 0; b < 40; ++b) {
        if (b == 20) { session.set_enabled(true); }
        clk.begin_block(k_frames, t0 + std::llround(b * k_frames * 1e6 / k_sr));
        if (b > 0) { EXPECT_EQ(clk.discontinuities(), 0u) << "block " << b; }
        clk.end_block();
    }
    session.set_enabled(false);
}

TEST(LinkSession, LocalStartAndTempoRequest) {
    LinkSession session(100.0);
    LinkTransportClock clk(session.audio_backend());
    clk.prepare(k_sr);
    const int64_t t0 = session.audio_backend().now_us();
    clk.play();
    clk.set_bpm(150.0);
    clk.begin_block(k_frames, t0);
    EXPECT_TRUE(clk.is_playing());
    EXPECT_NEAR(clk.bpm(), 150.0, 1e-9);
    EXPECT_NEAR(clk.beat_at_sample(0), 0.0, 1e-6);  // quantized launch alone: beat 0 now
    clk.end_block();
    // The app-side state follows the audio commit on Link's thread: poll.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::abs(session.tempo() - 150.0) > 1e-9 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_NEAR(session.tempo(), 150.0, 1e-9);  // committed to the session
}

// Under -DTANH_WITH_RTSAN=ON this aborts if the real SDK's audio-thread calls
// (capture/commit/beatAtTime/...) allocate, lock or make syscalls.
TEST(LinkSession, LinkTransportClock_IsRealtimeSafe) {
    LinkSession session(120.0);
    LinkTransportClock clk(session.audio_backend());
    clk.prepare(k_sr);
    clk.set_output_latency_samples(128);
    const int64_t t0 = session.audio_backend().now_us();
    double acc = 0.0;
    for (int b = 0; b < 200; ++b) {
        if (b == 10) { clk.play(); }
        if (b == 50) { clk.set_bpm(128.0); }
        if (b == 100) { clk.stop(); }
        acc += rt_block(clk, t0 + std::llround(b * k_frames * 1e6 / k_sr));
    }
    EXPECT_NE(acc, 0.0);
}

// Two Link instances in one process discover each other over the loopback /
// local interfaces. Skipped when the network does not allow discovery.
TEST(LinkSession, TwoPeers_Loopback) {
#ifdef THL_PLATFORM_IOS
    GTEST_SKIP() << "LinkKit: enabling Link is a user action in the settings view";
#endif
    LinkSession a(120.0);
    LinkSession b(90.0);
    a.set_enabled(true);
    b.set_enabled(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((a.num_peers() == 0 || b.num_peers() == 0) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (a.num_peers() == 0 || b.num_peers() == 0) {
        GTEST_SKIP() << "no Link peer discovered within 5 s (network/multicast unavailable)";
    }

    LinkTransportClock ca(a.audio_backend());
    LinkTransportClock cb(b.audio_backend());
    ca.prepare(k_sr);
    cb.prepare(k_sr);
    ca.set_bpm(133.0);
    ca.begin_block(k_frames, a.audio_backend().now_us());
    ca.end_block();

    // b converges on a's tempo.
    const auto tempo_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::abs(b.tempo() - 133.0) > 1e-3 &&
           std::chrono::steady_clock::now() < tempo_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_NEAR(b.tempo(), 133.0, 1e-3);  // Link sends tempo as µs per beat

    // Phase agrees modulo the quantum at the same host time (within 1 ms).
    const int64_t t = a.audio_backend().now_us();
    ca.begin_block(k_frames, t);
    cb.begin_block(k_frames, t);
    const double q = 4.0;
    const double pa = ca.block_info().phase(q);
    const double pb = cb.block_info().phase(q);
    double diff = std::abs(pa - pb);
    diff = std::min(diff, q - diff);
    const double one_ms_beats = 133.0 / 60.0 / 1000.0;
    EXPECT_LT(diff, one_ms_beats) << "phase a " << pa << " phase b " << pb;
    ca.end_block();
    cb.end_block();

    a.set_enabled(false);
    b.set_enabled(false);
}
