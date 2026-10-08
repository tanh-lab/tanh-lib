// LinkSession over the Ableton Link C++ SDK (macOS, Linux, Windows), with Link Audio.

#include <tanh/core/threading/LockFreeQueue.h>
#include <tanh/dsp/transport/LinkAudio.h>
#include <tanh/dsp/transport/LinkBackend.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <ableton/LinkAudio.hpp>
#include <ableton/link/NodeId.hpp>
#include <ableton/link_audio/ApiConfig.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tanh/link/LinkSession.h"

namespace thl::link {

namespace {

using std::chrono::microseconds;

class AbletonBackend final : public dsp::transport::LinkBackend {
public:
    explicit AbletonBackend(ableton::LinkAudio& link) : m_link(link) {}

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

    /// The state the clock captured for the current block (audio thread), if any.
    [[nodiscard]] const std::optional<ableton::LinkAudio::SessionState>& captured() const {
        return m_state;
    }

private:
    ableton::LinkAudio& m_link;
    // SessionState has no default constructor; optional keeps it in place (no heap).
    std::optional<ableton::LinkAudio::SessionState> m_state;
    std::atomic<uint64_t> m_epoch{0};
};

// Received buffers waiting for the audio thread (about 1.5 s of stereo at 48 kHz).
using PacketQueue = core::LockFreeQueue<dsp::transport::LinkAudioPacket, 256>;

// What the receiving callback writes. Shared with the callback, which the SDK may still run
// after its source is gone, so it outlives every callback.
struct Received {
    PacketQueue m_queue;
    std::atomic<double> m_quantum{4.0};
};

// Link thread: a received buffer, to float on the local beat grid, for the audio thread.
void on_buffer(ableton::LinkAudio& link,
               Received& received,
               const ableton::LinkAudioSource::BufferHandle& buffer) {
    const auto& info = buffer.info;
    if (info.numChannels == 0 || info.numFrames == 0) { return; }
    const double quantum = received.m_quantum.load(std::memory_order_relaxed);
    const auto state = link.captureAppSessionState();
    const auto begin = info.beginBeats(state, quantum);
    const auto end = info.endBeats(state, quantum);
    if (!begin || !end) { return; }  // from another session

    dsp::transport::LinkAudioPacket packet;
    if (!dsp::transport::make_link_audio_packet(buffer.samples,
                                                info.numChannels,
                                                info.numFrames,
                                                *begin,
                                                *end,
                                                packet)) {
        return;
    }
    (void)received.m_queue.try_push(packet);  // a full queue drops the newest
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
                   const AbletonBackend& clock,
                   PacketQueue& queue,
                   const std::string& output_name)
        : m_link(link)
        , m_clock(clock)
        , m_queue(queue)
        , m_sink(link, output_name, k_initial_sink_samples) {}

    bool pop(dsp::transport::LinkAudioPacket& out) override { return m_queue.try_pop(out); }

    bool send(const float* const* channels,
              uint32_t num_channels,
              uint32_t num_frames,
              double begin_beat,
              double quantum,
              uint32_t sample_rate) override {
        num_channels = std::min<uint32_t>(num_channels, 2);  // Link Audio carries mono or stereo
        const size_t samples = static_cast<size_t>(num_frames) * num_channels;
        if (num_channels == 0 || num_frames == 0 || sample_rate == 0) { return false; }
        ableton::LinkAudioSink::BufferHandle buffer(m_sink);  // invalid while nobody listens
        if (!buffer) { return false; }
        if (samples > buffer.maxNumSamples) {
            m_sink.requestMaxNumSamples(samples);  // larger buffers from the next block on
            return false;
        }
        for (uint32_t f = 0; f < num_frames; ++f) {
            for (uint32_t ch = 0; ch < num_channels; ++ch) {
                buffer.samples[(static_cast<size_t>(f) * num_channels) + ch] =
                    dsp::transport::to_link_audio_sample(channels[ch][f]);
            }
        }
        // The state the block was rendered with (the clock's capture), as the SDK requires; a
        // fresh capture only when no clock runs on this session.
        const auto& captured = m_clock.captured();
        return buffer.commit(captured ? *captured : m_link.captureAudioSessionState(),
                             begin_beat,
                             quantum,
                             num_frames,
                             num_channels,
                             sample_rate);
    }

private:
    static constexpr size_t k_initial_sink_samples = 4096;

    ableton::LinkAudio& m_link;
    const AbletonBackend& m_clock;
    PacketQueue& m_queue;
    ableton::LinkAudioSink m_sink;
};

}  // namespace

struct LinkSession::Impl {
    Impl(double bpm, const std::string& peer_name, const std::string& output_name)
        : m_link(bpm, peer_name)
        , m_backend(m_link)
        , m_received(std::make_shared<Received>())
        , m_sharing(m_link, m_backend, m_received->m_queue, output_name) {
        // Link thread: count peers, bump the epoch when the first one joins.
        m_link.setNumPeersCallback([this](std::size_t peers) {
            const size_t before = m_peers.exchange(peers, std::memory_order_acq_rel);
            if (before == 0 && peers > 0) { m_backend.bump_epoch(); }
        });
    }

    ableton::LinkAudio m_link;
    AbletonBackend m_backend;
    std::atomic<size_t> m_peers{0};
    std::shared_ptr<Received> m_received;
    AbletonSharing m_sharing;
    // Message thread.
    std::optional<ableton::LinkAudioSource> m_source;  // last: gone before what it feeds
    std::optional<uint64_t> m_source_id;
};

LinkSession::LinkSession(double initial_bpm,
                         const std::string& peer_name,
                         const std::string& output_name)
    : m_impl(std::make_unique<Impl>(initial_bpm, peer_name, output_name)) {}

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
                                 [&link = m_impl->m_link, received = m_impl->m_received](
                                     ableton::LinkAudioSource::BufferHandle buffer) {
                                     on_buffer(link, *received, buffer);
                                 });
    }
}

std::optional<uint64_t> LinkSession::audio_input() const {
    return m_impl->m_source_id;
}

void LinkSession::set_audio_quantum(double quantum) {
    m_impl->m_received->m_quantum.store(quantum, std::memory_order_relaxed);
}

dsp::transport::LinkAudioBackend& LinkSession::audio_sharing() {
    return m_impl->m_sharing;
}

}  // namespace thl::link
