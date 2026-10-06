// LinkSession over the Ableton Link C++ SDK (macOS, Linux, Windows).

#include <tanh/dsp/transport/LinkBackend.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <ableton/Link.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "tanh/link/LinkSession.h"

namespace thl::link {

namespace {

using std::chrono::microseconds;

class AbletonBackend final : public dsp::transport::LinkBackend {
public:
    explicit AbletonBackend(ableton::Link& link) : m_link(link) {}

    void capture() override { m_state.emplace(m_link.captureAudioSessionState()); }
    [[nodiscard]] double beat_at(int64_t t_us, double quantum) const override {
        return m_state->beatAtTime(microseconds(t_us), quantum);
    }
    [[nodiscard]] dsp::transport::LinkState state() const override {
        return {.m_tempo = m_state->tempo(),
                .m_playing = m_state->isPlaying(),
                .m_play_time_us = m_state->timeForIsPlaying().count()};
    }
    void set_tempo(double bpm, int64_t t_us) override {
        m_state->setTempo(bpm, microseconds(t_us));
    }
    void set_playing(bool playing, int64_t t_us) override {
        m_state->setIsPlaying(playing, microseconds(t_us));
    }
    void request_beat_at_start(double beat, double quantum) override {
        m_state->requestBeatAtStartPlayingTime(beat, quantum);
    }
    void request_beat_at(double beat, int64_t t_us, double quantum) override {
        m_state->requestBeatAtTime(beat, microseconds(t_us), quantum);
    }
    void commit() override {
        // Link 4.1 wakes its dispatcher thread with condition_variable::notify_one()
        // (pthread_cond_signal, no lock taken) inside commitAudioSessionState(),
        // which Ableton documents as realtime-safe. RTSan intercepts every
        // pthread_cond_signal, so this one SDK call is exempted.
        TANH_NONBLOCKING_SCOPED_DISABLER
        m_link.commitAudioSessionState(*m_state);
    }
    [[nodiscard]] int64_t now_us() const override { return m_link.clock().micros().count(); }
    [[nodiscard]] uint64_t timeline_epoch() const override {
        return m_epoch.load(std::memory_order_acquire);
    }

    void bump_epoch() { m_epoch.fetch_add(1, std::memory_order_acq_rel); }

private:
    ableton::Link& m_link;
    // SessionState has no default constructor; optional keeps it in place (no heap).
    std::optional<ableton::Link::SessionState> m_state;
    std::atomic<uint64_t> m_epoch{0};
};

}  // namespace

struct LinkSession::Impl {
    explicit Impl(double bpm) : m_link(bpm), m_backend(m_link) {
        // Link thread: count peers, bump the epoch when the first one joins.
        m_link.setNumPeersCallback([this](std::size_t peers) {
            const size_t before = m_peers.exchange(peers, std::memory_order_acq_rel);
            if (before == 0 && peers > 0) { m_backend.bump_epoch(); }
        });
    }

    ableton::Link m_link;
    AbletonBackend m_backend;
    std::atomic<size_t> m_peers{0};
};

LinkSession::LinkSession(double initial_bpm) : m_impl(std::make_unique<Impl>(initial_bpm)) {}

LinkSession::~LinkSession() {
    m_impl->m_link.setNumPeersCallback([](std::size_t) {});
    m_impl->m_link.enable(false);
}

void LinkSession::set_enabled(bool enabled) {
    if (enabled == m_impl->m_link.isEnabled()) { return; }
    m_impl->m_link.enable(enabled);
    if (enabled) { m_impl->m_backend.bump_epoch(); }
}

bool LinkSession::is_enabled() const {
    return m_impl->m_link.isEnabled();
}

void LinkSession::set_start_stop_sync(bool enabled) {
    m_impl->m_link.enableStartStopSync(enabled);
}

bool LinkSession::is_start_stop_sync_enabled() const {
    return m_impl->m_link.isStartStopSyncEnabled();
}

size_t LinkSession::num_peers() const {
    return m_impl->m_peers.load(std::memory_order_acquire);
}

void LinkSession::set_active(bool /*active*/) {}

void* LinkSession::settings_view_controller() {
    return nullptr;
}

double LinkSession::tempo() const {
    return m_impl->m_link.captureAppSessionState().tempo();
}

dsp::transport::LinkBackend& LinkSession::audio_backend() {
    return m_impl->m_backend;
}

}  // namespace thl::link
