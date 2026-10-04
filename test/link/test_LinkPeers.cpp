// Ableton Link TEST-PLAN cases with real Link peers in one process (desktop,
// TANH_WITH_LINK). "Ours" is the code path an app uses: thl::link::LinkSession +
// LinkTransportClock, driven by simulated audio blocks. "Peer" is a LinkHut-like
// app on the raw Link C++ SDK and serves as the reference.
//
// Simulated audio: fixed sample rate and block size, block n has the callback host
// time start + n * frames / sr (exact, no jitter) and is rendered once the Link
// clock has passed that time, as a device callback would. Waits poll with generous
// timeouts instead of sleeping fixed times. Every test skips when the two peers
// do not discover each other (no multicast) or a foreign Link peer shows up.
//
// CTest label "link-peers" (exclude with `ctest -LE link-peers`); all Link tests
// share the RESOURCE_LOCK "link-network" so ctest -j never runs two at once.

#include <gtest/gtest.h>
#include <tanh/dsp/transport/LinkBackend.h>
#include <tanh/dsp/transport/LinkTransportClock.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/link/LinkSession.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <ableton/Link.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using thl::dsp::transport::LinkTransportClock;
using thl::dsp::transport::TransportInfo;
using thl::link::LinkSession;
using namespace std::chrono_literals;

namespace {

constexpr double k_sr = 48000.0;
constexpr double k_quantum = 4.0;
constexpr auto k_discovery_timeout = 10s;
constexpr auto k_sync_timeout = 5s;
// Link ignores network updates on the audio thread for 1 s after a local
// audio-thread commit (kLocalModGracePeriod). Peer changes are made after it.
constexpr auto k_grace_period = 1200ms;
// When two sessions meet, the one enabled more than 0.5 s earlier wins (Link's
// SESSION_EPS on the ghost time). 0.8 s lost that race in ~40 % of local runs
// (enable and discovery start asynchronously); 2 s never did.
constexpr auto k_session_age = 2000ms;
// LinkHut applies tempo changes at its output time (host time + output latency).
constexpr int64_t k_peer_output_latency_us = 10000;

// Phase agreement between our clock and the peer at the same host time. Both read
// the same host clock; the residual is the error of Link's ghost-time estimate
// between the two instances (ping-pong measurement over loopback) plus ≤ 1 µs of
// block-boundary rounding: measured 4-20 µs (printed as "[ measured ]"). 0.25 ms
// (12 samples at 48 kHz) leaves room for a loaded machine and is still far below
// the TEST-PLAN's 3 ms.
constexpr double k_phase_tolerance_ms = 0.25;

const char* const k_no_discovery =
    "no Link peer discovered within 10 s (network/multicast unavailable)";
const char* const k_foreign_peer = "a foreign Link peer is on the network";

int64_t micros(std::chrono::microseconds t) {
    return t.count();
}

/// Beat difference modulo the quantum, wrapped to [-q/2, q/2], in ms at @p bpm.
double phase_diff_ms(double ours, double peer, double bpm) {
    double d = std::fmod(ours - peer, k_quantum);
    if (d > k_quantum / 2) { d -= k_quantum; }
    if (d < -k_quantum / 2) { d += k_quantum; }
    return d * 60000.0 / bpm;
}

// ── The peer: a LinkHut-like app on the raw Link SDK (app-thread API) ─────────
class Peer {
public:
    explicit Peer(double bpm) : m_link(bpm) {}
    ~Peer() { m_link.enable(false); }
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;
    Peer(Peer&&) = delete;
    Peer& operator=(Peer&&) = delete;

    void enable(bool start_stop_sync) {
        m_link.enableStartStopSync(start_stop_sync);
        m_link.enable(true);
    }
    [[nodiscard]] size_t num_peers() const { return m_link.numPeers(); }
    [[nodiscard]] int64_t now_us() const { return micros(m_link.clock().micros()); }

    [[nodiscard]] double tempo() const { return m_link.captureAppSessionState().tempo(); }
    [[nodiscard]] double beat_at(int64_t t) const {
        return m_link.captureAppSessionState().beatAtTime(std::chrono::microseconds(t), k_quantum);
    }
    [[nodiscard]] int64_t time_at_beat(double beat) const {
        return micros(m_link.captureAppSessionState().timeAtBeat(beat, k_quantum));
    }
    /// Host time of the first bar (quantum boundary) at or after @p t.
    [[nodiscard]] int64_t bar_at_or_after(int64_t t) const {
        const double b = beat_at(t);
        return time_at_beat(std::ceil(b / k_quantum) * k_quantum);
    }
    [[nodiscard]] bool is_playing() const { return m_link.captureAppSessionState().isPlaying(); }
    [[nodiscard]] int64_t time_for_is_playing() const {
        return micros(m_link.captureAppSessionState().timeForIsPlaying());
    }

    /// LinkHut: setTempo(bpm, output time).
    void set_tempo(double bpm) {
        auto s = m_link.captureAppSessionState();
        s.setTempo(bpm, std::chrono::microseconds(now_us() + k_peer_output_latency_us));
        m_link.commitAppSessionState(s);
    }
    void set_playing(bool playing, int64_t t) {
        auto s = m_link.captureAppSessionState();
        s.setIsPlaying(playing, std::chrono::microseconds(t));
        m_link.commitAppSessionState(s);
    }

private:
    ableton::Link m_link;
};

// ── Ours: LinkSession + LinkTransportClock over simulated audio blocks ───────
struct Block {
    int64_t m_host_us = 0;  ///< callback host time (no latency)
    int64_t m_t0_us = 0;    ///< output time the clock used
    TransportInfo m_info;
    thl::dsp::transport::LinkState m_state;  ///< captured session state of the block
};

// The audio-callback part; RTSan checks the real SDK's audio-thread calls here.
void render_block(LinkTransportClock& clock,
                  thl::dsp::transport::LinkBackend& backend,
                  uint32_t frames,
                  int64_t host_us,
                  Block& out) TANH_NONBLOCKING_FUNCTION {
    clock.begin_block(frames, host_us);
    out.m_host_us = host_us;
    out.m_t0_us = clock.output_time_us();
    out.m_info = clock.block_info();
    out.m_state = backend.state();
    clock.end_block();
}

class Ours {
public:
    explicit Ours(double bpm, uint32_t frames = 256)
        : m_frames(frames), m_session(bpm), m_clock(m_session.audio_backend()) {
        m_clock.prepare(k_sr);
        m_start_us = backend().now_us();
    }

    LinkSession& session() { return m_session; }
    LinkTransportClock& clock() { return m_clock; }
    thl::dsp::transport::LinkBackend& backend() { return m_session.audio_backend(); }
    [[nodiscard]] uint32_t frames() const { return m_frames; }
    /// Every rendered block, in order.
    [[nodiscard]] const std::vector<Block>& blocks() const { return m_blocks; }
    [[nodiscard]] const Block& last() const { return m_blocks.back(); }

    /// Render every block whose callback host time has passed.
    void pump() {
        for (;;) {
            const int64_t host =
                m_start_us + std::llround(static_cast<double>(m_next) *
                                          static_cast<double>(m_frames) * 1e6 / k_sr);
            if (host > backend().now_us()) { return; }
            Block b;
            render_block(m_clock, backend(), m_frames, host, b);
            m_blocks.push_back(b);
            ++m_next;
        }
    }

    /// Keep rendering until @p done() holds; false on timeout.
    template <typename Pred>
    bool run_until(Pred done, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            pump();
            if (done()) { return true; }
            if (std::chrono::steady_clock::now() > deadline) { return false; }
            std::this_thread::sleep_for(1ms);
        }
    }

    void run_for(std::chrono::milliseconds duration) {
        const auto end = std::chrono::steady_clock::now() + duration;
        run_until([&] { return std::chrono::steady_clock::now() >= end; }, duration + 1s);
    }

    /// Render one more block (after a queued request) and return it.
    const Block& next_block() {
        const size_t n = m_blocks.size();
        run_until([&] { return m_blocks.size() > n; }, 1s);
        return m_blocks[n];
    }

private:
    uint32_t m_frames;
    LinkSession m_session;
    LinkTransportClock m_clock;
    int64_t m_start_us = 0;
    uint64_t m_next = 0;
    std::vector<Block> m_blocks;
};

enum class Connect { Ok, NoDiscovery, ForeignPeer };

/// Wait (rendering) until ours and the peer see exactly each other.
Connect connect(Ours& ours, const Peer& peer) {
    const bool found =
        ours.run_until([&] { return ours.session().num_peers() > 0 && peer.num_peers() > 0; },
                       k_discovery_timeout);
    if (!found) { return Connect::NoDiscovery; }
    ours.run_for(200ms);  // let the session settle (measurement, join)
    if (ours.session().num_peers() != 1 || peer.num_peers() != 1) { return Connect::ForeignPeer; }
    return Connect::Ok;
}

#define LINK_CONNECT_OR_SKIP(ours, peer)                                    \
    do {                                                                    \
        const Connect c_ = connect(ours, peer);                             \
        if (c_ == Connect::NoDiscovery) { GTEST_SKIP() << k_no_discovery; } \
        if (c_ == Connect::ForeignPeer) { GTEST_SKIP() << k_foreign_peer; } \
    } while (false)

/// Max |our phase - peer phase| in ms over blocks [from, end).
double max_phase_error_ms(const Ours& ours, const Peer& peer, size_t from) {
    double worst = 0.0;
    const auto& blocks = ours.blocks();
    for (size_t i = from; i < blocks.size(); ++i) {
        const auto& b = blocks[i];
        const double e =
            phase_diff_ms(b.m_info.m_beat_position, peer.beat_at(b.m_t0_us), b.m_info.m_bpm);
        worst = std::max(worst, std::abs(e));
    }
    return worst;
}

/// Run for @p duration and return the worst phase error of those blocks.
double measure_phase_error_ms(Ours& ours, const Peer& peer, std::chrono::milliseconds duration) {
    const size_t from = ours.blocks().size();
    // The peer is captured after the blocks were rendered: same timeline as long as
    // nobody changes it meanwhile.
    ours.run_for(duration);
    return max_phase_error_ms(ours, peer, from);
}

/// Every block in [from, end): no k_jumped / k_timeline_reset, and every bpm
/// change carries k_tempo_changed.
void expect_continuous(const Ours& ours, size_t from) {
    const auto& blocks = ours.blocks();
    for (size_t i = std::max<size_t>(from, 1); i < blocks.size(); ++i) {
        const auto& info = blocks[i].m_info;
        ASSERT_FALSE(info.has(TransportInfo::k_jumped))
            << "block " << i << " delta " << info.m_jump_delta_beats;
        ASSERT_FALSE(info.has(TransportInfo::k_timeline_reset)) << "block " << i;
        if (std::abs(info.m_bpm - blocks[i - 1].m_info.m_bpm) > 1e-6) {
            ASSERT_TRUE(info.has(TransportInfo::k_tempo_changed)) << "block " << i;
        }
        ASSERT_NEAR(info.m_beat_position, blocks[i - 1].m_info.beat_end(), 1e-9) << "block " << i;
    }
}

size_t count_flag(const Ours& ours, size_t from, uint32_t flag) {
    size_t n = 0;
    for (size_t i = from; i < ours.blocks().size(); ++i) {
        if (ours.blocks()[i].m_info.has(flag)) { ++n; }
    }
    return n;
}

void report(const char* what, double value, const char* unit) {
    std::cout << "[ measured ] " << what << ": " << value << ' ' << unit << '\n';
    ::testing::Test::RecordProperty(what, std::to_string(value));
}

}  // namespace

// TEMPO-1 (app → LinkHut): our tempo request while playing reaches the peer; on
// our side it is a tempo change, not a jump, and the phases stay together.
TEST(LinkPeers, Tempo1_OurTempoChangeReachesPeer) {
    Ours ours(120.0);
    Peer peer(120.0);
    ours.session().set_enabled(true);
    peer.enable(false);
    LINK_CONNECT_OR_SKIP(ours, peer);

    ours.clock().play();
    ours.run_for(300ms);
    const size_t from = ours.blocks().size();

    ours.clock().set_bpm(140.0);
    const Block& b = ours.next_block();
    EXPECT_NEAR(b.m_info.m_bpm, 140.0, 1e-9);
    EXPECT_EQ(b.m_info.discontinuities(), TransportInfo::k_tempo_changed);

    ASSERT_TRUE(
        ours.run_until([&] { return std::abs(peer.tempo() - 140.0) < 1e-3; }, k_sync_timeout))
        << "peer tempo " << peer.tempo();
    const double err = measure_phase_error_ms(ours, peer, 500ms);
    report("tempo1_out_phase_error_ms", err, "ms");
    EXPECT_LT(err, k_phase_tolerance_ms);
    expect_continuous(ours, from);
}

// TEMPO-1 (LinkHut → app): the peer changes the tempo twice while we play. Each
// change reaches our clock as k_tempo_changed without k_jumped (the change
// applies at the peer's output time, which may lie inside a block we already
// rendered; the ContinuityTracker absorbs that) and the phases stay together.
TEST(LinkPeers, Tempo1_PeerTempoChangeReachesUsWithoutJump) {
    Ours ours(120.0);
    Peer peer(120.0);
    ours.session().set_enabled(true);
    peer.enable(false);
    LINK_CONNECT_OR_SKIP(ours, peer);

    ours.clock().play();
    ours.run_for(k_grace_period);
    const size_t from = ours.blocks().size();

    for (const double bpm : {150.0, 90.0}) {
        peer.set_tempo(bpm);
        ASSERT_TRUE(ours.run_until([&] { return std::abs(ours.last().m_info.m_bpm - bpm) < 1e-3; },
                                   k_sync_timeout))
            << "our tempo " << ours.last().m_info.m_bpm << " want " << bpm;
        const double err = measure_phase_error_ms(ours, peer, 300ms);
        report("tempo1_in_phase_error_ms", err, "ms");
        EXPECT_LT(err, k_phase_tolerance_ms) << bpm << " bpm";
    }
    expect_continuous(ours, from);
    EXPECT_GE(count_flag(ours, from, TransportInfo::k_tempo_changed), 2u);
    EXPECT_TRUE(ours.last().m_info.is_playing());
}

// After joining a running session our beat phase (quantum 4) matches the peer's
// at the same host time within k_phase_tolerance_ms.
TEST(LinkPeers, PhaseAlignedAfterJoin) {
    Peer peer(120.0);
    peer.enable(false);
    Ours ours(120.0);
    ours.session().set_enabled(true);
    LINK_CONNECT_OR_SKIP(ours, peer);
    ours.run_for(300ms);

    const double err = measure_phase_error_ms(ours, peer, 1000ms);
    report("join_phase_error_ms", err, "ms");
    EXPECT_LT(err, k_phase_tolerance_ms);
}

// TEMPO-2: joining an existing session (peer at 130 bpm, playing) adopts its
// tempo and never pushes ours (100 bpm).
TEST(LinkPeers, Tempo2_JoiningKeepsTheSessionTempo) {
    Peer peer(130.0);
    peer.enable(false);
    peer.set_playing(true, peer.now_us());
    std::this_thread::sleep_for(k_session_age);

    Ours ours(100.0);
    ours.run_for(100ms);
    EXPECT_NEAR(ours.last().m_info.m_bpm, 100.0, 1e-9);
    ours.session().set_enabled(true);
    LINK_CONNECT_OR_SKIP(ours, peer);

    ASSERT_TRUE(ours.run_until([&] { return std::abs(ours.last().m_info.m_bpm - 130.0) < 1e-3; },
                               k_sync_timeout))
        << "our tempo " << ours.last().m_info.m_bpm;
    ours.run_for(500ms);
    EXPECT_NEAR(peer.tempo(), 130.0, 1e-3);
    EXPECT_NEAR(ours.session().tempo(), 130.0, 1e-3);
    EXPECT_NEAR(ours.last().m_info.m_bpm, 130.0, 1e-3);
}

// TEMPO-5 (with BEATTIME-1): enabling and disabling Link without a session to
// join keeps our tempo and beat.
TEST(LinkPeers, Tempo5_EnableDisableKeepsOurTempo) {
    Ours ours(120.0);
    ours.clock().play();
    ours.clock().set_bpm(97.0);
    ours.run_for(200ms);
    const size_t from = ours.blocks().size();

    ours.session().set_enabled(true);
    ours.run_for(500ms);
    if (ours.session().num_peers() != 0) { GTEST_SKIP() << k_foreign_peer; }
    EXPECT_NEAR(ours.last().m_info.m_bpm, 97.0, 1e-9);
    EXPECT_NEAR(ours.session().tempo(), 97.0, 1e-9);

    ours.clock().set_bpm(111.0);
    ours.run_for(200ms);
    ours.session().set_enabled(false);
    ours.run_for(300ms);
    EXPECT_NEAR(ours.last().m_info.m_bpm, 111.0, 1e-9);
    EXPECT_NEAR(ours.session().tempo(), 111.0, 1e-9);
    EXPECT_TRUE(ours.last().m_info.is_playing());
    expect_continuous(ours, from);
    EXPECT_EQ(count_flag(ours, from, TransportInfo::k_tempo_changed), 1u);
}

// BEATTIME-2: we play in our own session; a peer (other tempo) joins it. Our beat
// does not move: no k_jumped / k_timeline_reset, and every block start stays on
// the line we were on before the join. The peer adopts our tempo and phase.
TEST(LinkPeers, Beattime2_PeerJoiningDoesNotMoveOurBeat) {
    Ours ours(120.0);
    ours.session().set_enabled(true);
    ours.clock().play();
    ours.run_for(k_session_age);  // our session is the older one; grace period over
    if (ours.session().num_peers() != 0) { GTEST_SKIP() << k_foreign_peer; }

    const size_t from = ours.blocks().size();
    const Block ref = ours.last();
    Peer peer(90.0);
    peer.enable(false);
    LINK_CONNECT_OR_SKIP(ours, peer);
    ours.run_for(1000ms);

    expect_continuous(ours, from);
    double worst_us = 0.0;
    for (size_t i = from; i < ours.blocks().size(); ++i) {
        const auto& b = ours.blocks()[i];
        const double line = ref.m_info.m_beat_position +
                            (static_cast<double>(b.m_t0_us - ref.m_t0_us) * 120.0 / 60e6);
        worst_us = std::max(worst_us, std::abs(b.m_info.m_beat_position - line) * 60e6 / 120.0);
    }
    report("beattime2_beat_deviation_us", worst_us, "us");
    EXPECT_LT(worst_us, 2.0);  // block-boundary rounding only
    EXPECT_NEAR(ours.last().m_info.m_bpm, 120.0, 1e-9);
    EXPECT_NEAR(peer.tempo(), 120.0, 1e-3);
    EXPECT_LT(max_phase_error_ms(ours, peer, ours.blocks().size() - 50), k_phase_tolerance_ms);
}

// STARTSTOPSTATE-1: with start/stop sync, the peer starts at its next bar and
// stops a few hundred ms later. k_started lands in the block that contains the
// start time, at a bar of our timeline; k_stopped in the block that contains the
// stop time.
TEST(LinkPeers, StartStop1_PeerStartsAndStopsUs) {
    Ours ours(120.0);
    Peer peer(120.0);
    ours.session().set_start_stop_sync(true);
    ours.session().set_enabled(true);
    peer.enable(true);
    LINK_CONNECT_OR_SKIP(ours, peer);
    ours.run_for(300ms);
    ASSERT_FALSE(ours.last().m_info.is_playing());

    auto find_flag = [&](size_t from, uint32_t flag) -> const Block* {
        for (size_t i = from; i < ours.blocks().size(); ++i) {
            if (ours.blocks()[i].m_info.has(flag)) { return &ours.blocks()[i]; }
        }
        return nullptr;
    };
    const auto block_us = static_cast<int64_t>(std::llround(ours.frames() * 1e6 / k_sr));

    // Start at the first bar at least 300 ms ahead (LinkHut would start "now"
    // with a count-in; a future start makes the target block deterministic).
    size_t from = ours.blocks().size();
    const int64_t start = peer.bar_at_or_after(peer.now_us() + 300000);
    peer.set_playing(true, start);
    ASSERT_TRUE(ours.run_until([&] { return ours.last().m_info.is_playing(); }, k_sync_timeout));
    const Block* started = find_flag(from, TransportInfo::k_started);
    ASSERT_NE(started, nullptr);
    const int64_t our_start = started->m_state.m_play_time_us;
    report("startstop1_start_time_error_us",
           static_cast<double>(std::abs(our_start - start)),
           "us");
    EXPECT_LE(std::abs(our_start - start), 1000);
    EXPECT_LE(started->m_t0_us, our_start);
    EXPECT_LT(our_start, started->m_t0_us + block_us);
    // The start offset in the block is a bar of our timeline (phase 0).
    const auto offset = static_cast<uint32_t>(
        std::ceil(static_cast<double>(our_start - started->m_t0_us) * k_sr / 1e6));
    const double phase_ms = phase_diff_ms(started->m_info.beat_at(offset), 0.0, 120.0);
    report("startstop1_start_phase_ms", std::abs(phase_ms), "ms");
    EXPECT_LT(std::abs(phase_ms), k_phase_tolerance_ms + (1e3 / k_sr));
    EXPECT_EQ(count_flag(ours, from, TransportInfo::k_started), 1u);

    ours.run_for(300ms);
    from = ours.blocks().size();
    const int64_t stop = peer.bar_at_or_after(peer.now_us() + 300000);
    peer.set_playing(false, stop);
    ASSERT_TRUE(ours.run_until([&] { return !ours.last().m_info.is_playing(); }, k_sync_timeout));
    const Block* stopped = find_flag(from, TransportInfo::k_stopped);
    ASSERT_NE(stopped, nullptr);
    const int64_t our_stop = stopped->m_state.m_play_time_us;
    EXPECT_LE(std::abs(our_stop - stop), 1000);
    EXPECT_LE(stopped->m_t0_us, our_stop);
    EXPECT_LT(our_stop, stopped->m_t0_us + block_us);
    EXPECT_EQ(count_flag(ours, from, TransportInfo::k_stopped), 1u);
    EXPECT_EQ(count_flag(ours, from, TransportInfo::k_jumped), 0u);
}

// STARTSTOPSTATE-2: the peer plays; we join with start/stop sync. Our start joins
// the peer's playing session quantized to the bar, our stop stops the peer, and
// our next start starts it again at our start time.
TEST(LinkPeers, StartStop2_WeStartAndStopThePeer) {
    Peer peer(120.0);
    peer.enable(true);
    peer.set_playing(true, peer.now_us());
    std::this_thread::sleep_for(k_session_age);

    Ours ours(120.0);
    ours.session().set_start_stop_sync(true);
    ours.session().set_enabled(true);
    LINK_CONNECT_OR_SKIP(ours, peer);
    ours.run_for(300ms);
    EXPECT_TRUE(peer.is_playing());
    const bool playing_on_join = ours.last().m_info.is_playing();
    report("startstop2_playing_after_join", playing_on_join ? 1.0 : 0.0, "");

    // Start: k_started now, beat 0 at the next bar of the shared phase.
    if (!playing_on_join) {
        ours.clock().play();
        const Block& b = ours.next_block();
        EXPECT_TRUE(b.m_info.has(TransportInfo::k_started));
        EXPECT_TRUE(b.m_info.is_playing());
        EXPECT_GT(b.m_info.m_beat_position, -k_quantum);
        EXPECT_LE(b.m_info.m_beat_position, 0.0);
        const int64_t beat0 = peer.time_at_beat(peer.beat_at(b.m_t0_us) - b.m_info.m_beat_position);
        report("startstop2_beat0_phase_ms",
               std::abs(phase_diff_ms(peer.beat_at(beat0), 0.0, 120.0)),
               "ms");
        EXPECT_LT(std::abs(phase_diff_ms(peer.beat_at(beat0), 0.0, 120.0)), k_phase_tolerance_ms);
        EXPECT_TRUE(peer.is_playing());
    }
    ours.run_for(300ms);

    // Stop: the peer stops at our stop time.
    ours.clock().stop();
    const Block stop_block = ours.next_block();
    EXPECT_EQ(stop_block.m_info.discontinuities(), TransportInfo::k_stopped);
    ASSERT_TRUE(ours.run_until([&] { return !peer.is_playing(); }, k_sync_timeout));
    report("startstop2_stop_time_error_us",
           static_cast<double>(std::abs(peer.time_for_is_playing() - stop_block.m_t0_us)),
           "us");
    EXPECT_LE(std::abs(peer.time_for_is_playing() - stop_block.m_t0_us), 1000);
    ours.run_for(300ms);
    EXPECT_FALSE(ours.last().m_info.is_playing());

    // Start again: the peer starts at our start time.
    ours.clock().play();
    const Block start_block = ours.next_block();
    EXPECT_TRUE(start_block.m_info.has(TransportInfo::k_started));
    ASSERT_TRUE(ours.run_until([&] { return peer.is_playing(); }, k_sync_timeout));
    EXPECT_LE(std::abs(peer.time_for_is_playing() - start_block.m_t0_us), 1000);
}

// TEMPO-4: the peer goes to 20 and to 999 bpm (Link's range); we follow without
// k_jumped and stay in phase.
TEST(LinkPeers, Tempo4_ExtremeTempi) {
    Ours ours(120.0);
    Peer peer(120.0);
    ours.session().set_enabled(true);
    peer.enable(false);
    LINK_CONNECT_OR_SKIP(ours, peer);
    ours.clock().play();
    ours.run_for(k_grace_period);
    const size_t from = ours.blocks().size();

    for (const double bpm : {20.0, 999.0}) {
        peer.set_tempo(bpm);
        // Link sends the tempo as integer µs per beat: 999 bpm arrives as 999.001.
        ASSERT_TRUE(ours.run_until([&] { return std::abs(ours.last().m_info.m_bpm - bpm) < 1e-2; },
                                   k_sync_timeout))
            << "our tempo " << ours.last().m_info.m_bpm << " want " << bpm;
        const double err = measure_phase_error_ms(ours, peer, 500ms);
        report(bpm < 100.0 ? "tempo4_20bpm_phase_error_ms" : "tempo4_999bpm_phase_error_ms",
               err,
               "ms");
        EXPECT_LT(err, k_phase_tolerance_ms) << bpm << " bpm";
    }
    expect_continuous(ours, from);
}

// AUDIOENGINE-1 stand-in (no loopback cable): we render a click at every beat
// through simulated blocks with an output latency and check, against the peer's
// session state, the host time at which each click leaves the speaker:
// host time of the block + latency + offset / sr. TEST-PLAN tolerance: < 3 ms.
class LinkPeersLatency : public ::testing::TestWithParam<uint32_t> {};

TEST_P(LinkPeersLatency, AudioEngine1_ClicksLandOnThePeersBeats) {
    const uint32_t frames = GetParam();
    constexpr uint32_t k_latency = 480;  // 10 ms: uncompensated it would fail the 3 ms bound
    const double latency_us = k_latency * 1e6 / k_sr;

    Peer peer(240.0);  // 4 clicks per second
    peer.enable(false);
    Ours ours(240.0, frames);  // whichever session wins, the tempo is 240
    ours.clock().set_output_latency_samples(k_latency);
    ours.session().set_enabled(true);
    LINK_CONNECT_OR_SKIP(ours, peer);
    ASSERT_TRUE(ours.run_until([&] { return std::abs(ours.last().m_info.m_bpm - 240.0) < 1e-3; },
                               k_sync_timeout));
    ours.clock().play();
    ours.run_for(300ms);
    const size_t from = ours.blocks().size();
    ours.run_for(2000ms);

    double worst_us = 0.0;
    int clicks = 0;
    for (size_t i = from; i < ours.blocks().size(); ++i) {
        const auto& b = ours.blocks()[i];
        const auto& info = b.m_info;
        if (!info.is_playing() || info.m_beats_per_sample <= 0.0) { continue; }
        for (double k = std::ceil(info.m_beat_position); k < info.beat_end(); k += 1.0) {
            const double offset = std::ceil((k - info.m_beat_position) / info.m_beats_per_sample);
            if (offset >= frames) { break; }
            const double sounds_at =
                static_cast<double>(b.m_host_us) + latency_us + (offset * 1e6 / k_sr);
            const double peer_beat = std::round(peer.beat_at(std::llround(sounds_at)));
            const double expected = static_cast<double>(peer.time_at_beat(peer_beat));
            // Same beat modulo the quantum (bars line up), and close in time.
            EXPECT_NEAR(std::fmod(std::fmod(k - peer_beat, k_quantum) + k_quantum, k_quantum),
                        0.0,
                        1e-9);
            worst_us = std::max(worst_us, std::abs(sounds_at - expected));
            ++clicks;
        }
    }
    report(("audioengine1_max_error_us_" + std::to_string(frames)).c_str(), worst_us, "us");
    EXPECT_GE(clicks, 6);
    EXPECT_LT(worst_us, 3000.0);
}

INSTANTIATE_TEST_SUITE_P(BlockSizes,
                         LinkPeersLatency,
                         ::testing::Values(64u, 512u, 2048u),
                         [](const auto& p) { return "Block" + std::to_string(p.param); });
