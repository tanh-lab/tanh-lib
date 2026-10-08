// LinkSession over LinkKit (iOS), with Link Audio. Compiled with ARC.
//
// LinkKit is a C API on mach host-time ticks; the seam speaks microseconds on the
// same clock, so the backend converts with mach_timebase_info. Enabling Link and
// start/stop sync are user settings in ABLLinkSettingsViewController.

#include "tanh/link/LinkSession.h"

#import <ABLLink.h>
#import <ABLLinkSettingsViewController.h>
#include <mach/mach_time.h>
#include <tanh/core/threading/LockFreeQueue.h>
#include <tanh/dsp/transport/LinkAudio.h>
#include <tanh/dsp/transport/LinkBackend.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace thl::link {

namespace {

class LinkKitBackend final : public dsp::transport::LinkBackend {
public:
    explicit LinkKitBackend(ABLLinkRef link) : m_link(link) {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        m_ticks_per_us =
            (1000.0 * static_cast<double>(info.denom)) / static_cast<double>(info.numer);
    }

    void capture() override { m_state = ABLLinkCaptureAudioSessionState(m_link); }
    [[nodiscard]] double beat_at(int64_t t_us, double quantum) const override {
        return ABLLinkBeatAtTime(m_state, to_ticks(t_us), quantum);
    }
    [[nodiscard]] dsp::transport::LinkState state() const override {
        return {.m_tempo = ABLLinkGetTempo(m_state),
                .m_playing = ABLLinkIsPlaying(m_state),
                .m_play_time_us = to_us(ABLLinkTimeForIsPlaying(m_state))};
    }
    void set_tempo(double bpm, int64_t t_us) override {
        ABLLinkSetTempo(m_state, bpm, to_ticks(t_us));
    }
    void set_playing(bool playing, int64_t t_us) override {
        ABLLinkSetIsPlaying(m_state, playing, to_ticks(t_us));
    }
    void request_beat_at_start(double beat, double quantum) override {
        ABLLinkRequestBeatAtStartPlayingTime(m_state, beat, quantum);
    }
    void request_beat_at(double beat, int64_t t_us, double quantum) override {
        ABLLinkRequestBeatAtTime(m_state, beat, to_ticks(t_us), quantum);
    }
    void commit() override { ABLLinkCommitAudioSessionState(m_link, m_state); }
    [[nodiscard]] int64_t now_us() const override { return to_us(mach_absolute_time()); }
    [[nodiscard]] uint64_t timeline_epoch() const override {
        return m_epoch.load(std::memory_order_acquire);
    }

    void bump_epoch() { m_epoch.fetch_add(1, std::memory_order_acq_rel); }

    /// The state the clock captured for the current block (audio thread), or null.
    [[nodiscard]] ABLLinkSessionStateRef captured() const { return m_state; }

private:
    [[nodiscard]] uint64_t to_ticks(int64_t us) const {
        return us <= 0 ? 0 : static_cast<uint64_t>(static_cast<double>(us) * m_ticks_per_us);
    }
    [[nodiscard]] int64_t to_us(uint64_t ticks) const {
        return static_cast<int64_t>(static_cast<double>(ticks) / m_ticks_per_us);
    }

    ABLLinkRef m_link;
    ABLLinkSessionStateRef m_state = nullptr;
    double m_ticks_per_us = 1.0;
    std::atomic<uint64_t> m_epoch{0};
};

// Received buffers waiting for the audio thread (about 1.5 s of stereo at 48 kHz).
using PacketQueue = core::LockFreeQueue<dsp::transport::LinkAudioPacket, 256>;

// Link Audio on the audio thread: received buffers out of the queue, our output into the sink.
class LinkKitSharing final : public dsp::transport::LinkAudioBackend {
public:
    LinkKitSharing(ABLLinkRef link,
                   const LinkKitBackend& clock,
                   PacketQueue& queue,
                   const std::string& output_name)
        : m_link(link)
        , m_clock(clock)
        , m_queue(queue)
        , m_sink(ABLLinkAudioSinkNew(link, output_name.c_str(), 4096)) {}
    ~LinkKitSharing() override { ABLLinkAudioSinkDelete(m_sink); }
    LinkKitSharing(const LinkKitSharing&) = delete;
    LinkKitSharing& operator=(const LinkKitSharing&) = delete;

    bool pop(dsp::transport::LinkAudioPacket& out) override { return m_queue.try_pop(out); }

    bool send(const float* const* channels,
              uint32_t num_channels,
              uint32_t num_frames,
              double begin_beat,
              double quantum,
              uint32_t sample_rate) override {
        num_channels = std::min<uint32_t>(num_channels, 2);  // Link Audio carries mono or stereo
        const uint32_t samples = num_frames * num_channels;
        if (num_channels == 0 || num_frames == 0 || sample_rate == 0) { return false; }
        auto* buffer = ABLLinkAudioRetainBuffer(m_sink);  // null while nobody listens
        if (buffer == nullptr) { return false; }
        if (!ABLLinkAudioSinkBufferHandleIsValid(buffer)) {
            ABLLinkAudioReleaseBuffer(buffer);
            return false;
        }
        if (samples > ABLLinkAudioSinkBufferHandleMaxNumSamples(buffer)) {
            ABLLinkAudioReleaseBuffer(buffer);
            ABLLinkAudioSinkRequestMaxNumSamples(m_sink, samples);  // from the next block on
            return false;
        }
        int16_t* out = ABLLinkAudioSinkBufferSamples(buffer);
        for (uint32_t f = 0; f < num_frames; ++f) {
            for (uint32_t ch = 0; ch < num_channels; ++ch) {
                out[(f * num_channels) + ch] =
                    dsp::transport::to_link_audio_sample(channels[ch][f]);
            }
        }
        // The state the block was rendered with (the clock's capture), as LinkKit requires.
        auto* state = m_clock.captured();
        return ABLLinkAudioReleaseAndCommitBuffer(
            m_sink,
            buffer,
            state != nullptr ? state : ABLLinkCaptureAudioSessionState(m_link),
            begin_beat,
            quantum,
            num_frames,
            num_channels,
            sample_rate);
    }

private:
    ABLLinkRef m_link;
    const LinkKitBackend& m_clock;
    PacketQueue& m_queue;
    ABLLinkAudioSinkRef m_sink;
};

}  // namespace

struct LinkSession::Impl {
    Impl(double bpm, const std::string& output_name)
        : m_link(ABLLinkNew(bpm))
        , m_backend(m_link)
        , m_queue(std::make_unique<PacketQueue>())
        , m_sharing(std::make_unique<LinkKitSharing>(m_link, m_backend, *m_queue, output_name)) {
        ABLLinkSetIsEnabledCallback(m_link, &Impl::on_enabled, this);
        ABLLinkSetIsConnectedCallback(m_link, &Impl::on_connected, this);
    }
    ~Impl() {
        stop_source();
        m_sharing.reset();
        m_settings = nil;
        ABLLinkDelete(m_link);
    }

    void stop_source() {
        if (m_source != nullptr) {
            ABLLinkAudioSourceDelete(m_source);
            m_source = nullptr;
        }
        m_source_id.reset();
    }

    // Link thread: a received buffer, to float on the local beat grid, for the audio thread.
    static void on_buffer(const ABLLinkAudioSourceBuffer* buffer, void* context) {
        auto* impl = static_cast<Impl*>(context);
        const auto& info = buffer->info;
        if (info.numChannels == 0 || info.numFrames == 0) { return; }
        const double quantum = impl->m_quantum.load(std::memory_order_relaxed);
        auto* state = ABLLinkCaptureAppSessionState(impl->m_link);
        double begin = 0.0;
        double end = 0.0;
        if (!ABLLinkAudioSourceBufferInfoBeginBeats(&info, state, quantum, &begin) ||
            !ABLLinkAudioSourceBufferInfoEndBeats(&info, state, quantum, &end)) {
            return;  // from another session
        }

        dsp::transport::LinkAudioPacket packet;
        if (!dsp::transport::make_link_audio_packet(buffer->samples,
                                                    info.numChannels,
                                                    info.numFrames,
                                                    begin,
                                                    end,
                                                    packet)) {
            return;
        }
        (void)impl->m_queue->try_push(packet);  // a full queue drops the newest
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // LinkKit invokes these on the main thread.
    static void on_enabled(bool enabled, void* context) {
        if (enabled) { static_cast<Impl*>(context)->m_backend.bump_epoch(); }
    }
    static void on_connected(bool connected, void* context) {
        auto* impl = static_cast<Impl*>(context);
        const bool before = impl->m_connected.exchange(connected, std::memory_order_acq_rel);
        if (!before && connected) { impl->m_backend.bump_epoch(); }
    }

    ABLLinkRef m_link;
    LinkKitBackend m_backend;
    std::atomic<bool> m_connected{false};
    ABLLinkSettingsViewController* m_settings = nil;
    std::unique_ptr<PacketQueue> m_queue;
    std::unique_ptr<LinkKitSharing> m_sharing;
    std::atomic<double> m_quantum{4.0};
    ABLLinkAudioSourceRef m_source = nullptr;  // message thread
    std::optional<uint64_t> m_source_id;
};

// The peer name comes from the Info.plist key ABLLinkPeerName and the settings view.
LinkSession::LinkSession(double initial_bpm,
                         const std::string& /*peer_name*/,
                         const std::string& output_name)
    : m_impl(std::make_unique<Impl>(initial_bpm, output_name)) {}

LinkSession::~LinkSession() = default;

void LinkSession::set_enabled(bool /*enabled*/) {}  // a user setting in the settings view

bool LinkSession::is_enabled() const {
    return ABLLinkIsEnabled(m_impl->m_link);
}

void LinkSession::set_start_stop_sync(bool /*enabled*/) {}  // a user setting as well

bool LinkSession::is_start_stop_sync_enabled() const {
    return ABLLinkIsStartStopSyncEnabled(m_impl->m_link);
}

size_t LinkSession::num_peers() const {
    return m_impl->m_connected.load(std::memory_order_acquire) ? 1 : 0;
}

void LinkSession::set_active(bool active) {
    ABLLinkSetActive(m_impl->m_link, active);
}

void* LinkSession::settings_view_controller() {
    if (m_impl->m_settings == nil) {
        m_impl->m_settings = [ABLLinkSettingsViewController instance:m_impl->m_link];
    }
    return (__bridge void*)m_impl->m_settings;
}

double LinkSession::tempo() const {
    return ABLLinkGetTempo(ABLLinkCaptureAppSessionState(m_impl->m_link));
}

dsp::transport::LinkBackend& LinkSession::audio_backend() {
    return m_impl->m_backend;
}

void LinkSession::set_audio_enabled(bool /*enabled*/) {}  // a user setting in the settings view

bool LinkSession::is_audio_enabled() const {
    return ABLLinkIsAudioEnabled(m_impl->m_link);
}

std::vector<LinkSession::AudioChannel> LinkSession::audio_channels() const {
    std::vector<AudioChannel> result;
    const auto list = ABLLinkAudioGetChannelList(m_impl->m_link);
    for (size_t i = 0; i < list.count; ++i) {
        const auto& channel = list.channels[i];
        result.push_back({.m_id = channel.id,
                          .m_name = channel.name != nullptr ? channel.name : "",
                          .m_peer_id = channel.peerId,
                          .m_peer_name = channel.peerName != nullptr ? channel.peerName : ""});
    }
    ABLLinkAudioFreeChannelList(list);
    return result;
}

void LinkSession::set_audio_input(std::optional<uint64_t> id) {
    if (id == m_impl->m_source_id) { return; }
    m_impl->stop_source();
    if (id) {
        m_impl->m_source =
            ABLLinkAudioSourceNew(m_impl->m_link, *id, &Impl::on_buffer, m_impl.get());
        m_impl->m_source_id = id;
    }
}

std::optional<uint64_t> LinkSession::audio_input() const {
    return m_impl->m_source_id;
}

void LinkSession::set_audio_quantum(double quantum) {
    m_impl->m_quantum.store(quantum, std::memory_order_relaxed);
}

dsp::transport::LinkAudioBackend& LinkSession::audio_sharing() {
    return *m_impl->m_sharing;
}

}  // namespace thl::link
