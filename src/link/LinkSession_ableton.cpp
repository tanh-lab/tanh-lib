// LinkSession over the Ableton Link C++ SDK (macOS, Linux, Windows), with Link Audio.

#include <tanh/dsp/transport/LinkAudio.h>
#include <tanh/dsp/transport/LinkBackend.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <ableton/LinkAudio.hpp>
#include <ableton/link/NodeId.hpp>
#include <ableton/link_audio/ApiConfig.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "detail/LinkSharing.h"
#include "tanh/link/LinkSession.h"

namespace thl::link {

namespace {

using std::chrono::microseconds;

class AbletonBackend final : public dsp::transport::LinkBackend {
public:
    AbletonBackend(ableton::LinkAudio& link, const detail::SessionShared& shared)
        : m_link(link), m_shared(shared) {}

    void capture() override {
        m_state.emplace(m_link.captureAudioSessionState());
        m_captures.captured();
    }
    [[nodiscard]] double beat_at(int64_t t_us, double quantum) const override {
        return session().beatAtTime(microseconds(t_us), quantum);
    }
    [[nodiscard]] dsp::transport::LinkState state() const override {
        const auto& state = session();
        return {.m_tempo = state.tempo(),
                .m_playing = state.isPlaying(),
                .m_play_time_us = state.timeForIsPlaying().count()};
    }
    void set_tempo(double bpm, int64_t t_us) override {
        session().setTempo(bpm, microseconds(t_us));
    }
    void set_playing(bool playing, int64_t t_us) override {
        session().setIsPlaying(playing, microseconds(t_us));
    }
    void request_beat_at_start(double beat, double quantum) override {
        session().requestBeatAtStartPlayingTime(beat, quantum);
    }
    void request_beat_at(double beat, int64_t t_us, double quantum) override {
        session().requestBeatAtTime(beat, microseconds(t_us), quantum);
    }
    void commit() override {
        // Link 4.1 wakes its dispatcher thread with condition_variable::notify_one()
        // (pthread_cond_signal, no lock taken) inside commitAudioSessionState(),
        // which Ableton documents as realtime-safe. RTSan intercepts every
        // pthread_cond_signal, so this one SDK call is exempted.
        TANH_NONBLOCKING_SCOPED_DISABLER
        m_link.commitAudioSessionState(session());
    }
    [[nodiscard]] int64_t now_us() const override { return m_link.clock().micros().count(); }
    [[nodiscard]] uint64_t timeline_epoch() const override {
        return m_epoch.load(std::memory_order_acquire);
    }
    [[nodiscard]] double quantum() const override {
        return m_shared.m_quantum.load(std::memory_order_relaxed);
    }

    void bump_epoch() { m_epoch.fetch_add(1, std::memory_order_acq_rel); }

    /// Audio thread: the state the clock captured since the last call, once; null if none.
    [[nodiscard]] const ableton::LinkAudio::SessionState* take_capture() {
        return m_captures.take() && m_state ? &*m_state : nullptr;
    }

private:
    // The captured state. The seam's contract: the clock calls capture() before any
    // query or request of the block.
    [[nodiscard]] const ableton::LinkAudio::SessionState& session() const {
        assert(m_state.has_value() && "LinkBackend: session state used before capture()");
        return *m_state;  // NOLINT(bugprone-unchecked-optional-access): the contract above
    }
    [[nodiscard]] ableton::LinkAudio::SessionState& session() {
        assert(m_state.has_value() && "LinkBackend: session state used before capture()");
        return *m_state;  // NOLINT(bugprone-unchecked-optional-access): the contract above
    }

    ableton::LinkAudio& m_link;
    const detail::SessionShared& m_shared;
    detail::CaptureTracker m_captures;
    // SessionState has no default constructor; optional keeps it in place (no heap).
    std::optional<ableton::LinkAudio::SessionState> m_state;
    std::atomic<uint64_t> m_epoch{0};
};

// Link thread: a received buffer, mapped to the local beat grid, for the audio thread.
void on_buffer(ableton::LinkAudio& link,
               detail::SessionShared& shared,
               const ableton::LinkAudioSource::BufferHandle& buffer) {
    const auto& info = buffer.info;
    if (info.numChannels == 0 || info.numFrames == 0) { return; }
    const double quantum = shared.m_quantum.load(std::memory_order_relaxed);
    const auto state = link.captureAppSessionState();
    const auto begin = info.beginBeats(state, quantum);
    const auto end = info.endBeats(state, quantum);
    if (!begin || !end) { return; }  // from another session
    detail::push_received(shared.m_received,
                          buffer.samples,
                          info.numChannels,
                          info.numFrames,
                          *begin,
                          *end);
}

uint64_t to_u64(const ableton::ChannelId& id) {
    uint64_t value = 0;
    std::memcpy(&value, id.data(), sizeof(value));
    return value;
}

ableton::ChannelId to_channel_id(uint64_t value) {
    ableton::link::NodeIdArray bytes{};
    std::memcpy(bytes.data(), &value, sizeof(value));
    return {bytes};
}

// Link Audio on the audio thread: received buffers out of the queue, our output into the sink.
class AbletonSharing final : public dsp::transport::LinkAudioBackend {
public:
    AbletonSharing(ableton::LinkAudio& link,
                   AbletonBackend& clock,
                   detail::ReceivedQueue& received,
                   const std::string& output_name)
        : m_link(link)
        , m_clock(clock)
        , m_received(received)
        , m_sink(link, output_name, k_initial_sink_samples) {}

    bool pop(dsp::transport::LinkAudioPacket& out) override { return m_received.try_pop(out); }

    bool send(const float* const* channels,
              uint32_t num_channels,
              uint32_t num_frames,
              double begin_beat,
              double quantum,
              uint32_t sample_rate) override {
        const auto shape = detail::send_shape(num_channels, num_frames, sample_rate);
        if (!shape) { return false; }
        ableton::LinkAudioSink::BufferHandle buffer(m_sink);  // invalid while nobody listens
        if (!buffer) { return false; }
        if (shape->m_samples > buffer.maxNumSamples) {
            m_sink.requestMaxNumSamples(shape->m_samples);  // larger buffers from the next block on
            return false;
        }
        detail::interleave(channels, *shape, buffer.samples);
        auto commit = [&](const ableton::LinkAudio::SessionState& state) {
            return buffer.commit(state,
                                 begin_beat,
                                 quantum,
                                 shape->m_frames,
                                 shape->m_channels,
                                 sample_rate);
        };
        // The state the block was rendered with (the clock's capture), as the SDK requires.
        if (const auto* captured = m_clock.take_capture()) { return commit(*captured); }
        return commit(m_link.captureAudioSessionState());
    }

private:
    static constexpr size_t k_initial_sink_samples = 4096;

    ableton::LinkAudio& m_link;
    AbletonBackend& m_clock;
    detail::ReceivedQueue& m_received;
    ableton::LinkAudioSink m_sink;
};

}  // namespace

struct LinkSession::Impl {
    Impl(double bpm, const std::string& peer_name, const std::string& output_name)
        : m_link(bpm, peer_name)
        , m_shared(std::make_shared<detail::SessionShared>())
        , m_backend(m_link, *m_shared)
        , m_sharing(m_link, m_backend, m_shared->m_received, output_name) {
        // Link thread: count peers, bump the epoch when the first one joins.
        m_link.setNumPeersCallback([this](std::size_t peers) {
            const size_t before = m_peers.exchange(peers, std::memory_order_acq_rel);
            if (before == 0 && peers > 0) { m_backend.bump_epoch(); }
        });
    }

    ableton::LinkAudio m_link;
    std::shared_ptr<detail::SessionShared> m_shared;  // also held by the receiving callback
    AbletonBackend m_backend;
    std::atomic<size_t> m_peers{0};
    AbletonSharing m_sharing;
    // Message thread.
    std::optional<ableton::LinkAudioSource> m_source;  // last: gone before what it feeds
    std::optional<uint64_t> m_source_id;
};

LinkSession::LinkSession(double initial_bpm,
                         const std::string& peer_name,
                         const std::string& output_name)
    : m_impl(std::make_unique<Impl>(initial_bpm, peer_name, output_name)) {}

// Only allocation can throw here (the SDK's callbacks are std::function); terminating
// then is the right outcome for a destructor.
// NOLINTNEXTLINE(bugprone-exception-escape)
LinkSession::~LinkSession() {
    m_impl->m_source.reset();
    m_impl->m_link.enableLinkAudio(false);
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

bool LinkSession::is_connected() const {
    return num_peers() > 0;
}

LinkSession::Capabilities LinkSession::capabilities() {
    return {.m_app_controls_settings = true,
            .m_counts_peers = true,
            .m_has_lifecycle = false,
            .m_has_settings_view = false};
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

void LinkSession::set_audio_enabled(bool enabled) {
    m_impl->m_link.enableLinkAudio(enabled);
}

bool LinkSession::is_audio_enabled() const {
    return m_impl->m_link.isLinkAudioEnabled();
}

std::vector<LinkSession::AudioChannel> LinkSession::audio_channels() const {
    std::vector<AudioChannel> result;
    for (const auto& channel : m_impl->m_link.channels()) {
        result.push_back({.m_id = to_u64(channel.id),
                          .m_name = channel.name,
                          .m_peer_id = to_u64(channel.peerId),
                          .m_peer_name = channel.peerName});
    }
    return result;
}

void LinkSession::set_audio_input(std::optional<uint64_t> id) {
    if (id == m_impl->m_source_id) { return; }
    m_impl->m_source.reset();
    m_impl->m_source_id = id;
    if (id) {
        m_impl->m_source.emplace(m_impl->m_link,
                                 to_channel_id(*id),
                                 [&link = m_impl->m_link, shared = m_impl->m_shared](
                                     ableton::LinkAudioSource::BufferHandle buffer) {
                                     on_buffer(link, *shared, buffer);
                                 });
    }
}

std::optional<uint64_t> LinkSession::audio_input() const {
    return m_impl->m_source_id;
}

void LinkSession::set_quantum(double quantum) {
    if (quantum > 0.0) { m_impl->m_shared->m_quantum.store(quantum, std::memory_order_relaxed); }
}

double LinkSession::quantum() const {
    return m_impl->m_shared->m_quantum.load(std::memory_order_relaxed);
}

dsp::transport::LinkAudioBackend& LinkSession::audio_sharing() {
    return m_impl->m_sharing;
}

}  // namespace thl::link
