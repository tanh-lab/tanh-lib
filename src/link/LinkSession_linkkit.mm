// LinkSession over LinkKit (iOS), with Link Audio. Compiled with ARC.
//
// LinkKit is a C API on mach host-time ticks; the seam speaks microseconds on the
// same clock, so the backend converts with mach_timebase_info. Enabling Link and
// start/stop sync are user settings in ABLLinkSettingsViewController.

#include "tanh/link/LinkSession.h"

#import <ABLLink.h>
#import <ABLLinkSettingsViewController.h>
#import <Foundation/Foundation.h>
#include <mach/mach_time.h>
#include <tanh/core/AbiNamespace.h>
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

#include "LinkSharing.h"

namespace thl::inline THL_ABI_NAMESPACE::link {

namespace {

class LinkKitBackend final : public dsp::transport::LinkBackend {
public:
    LinkKitBackend(ABLLinkRef link, const detail::SessionShared& shared)
        : m_link(link), m_shared(shared) {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        m_ticks_per_us =
            (1000.0 * static_cast<double>(info.denom)) / static_cast<double>(info.numer);
    }

    void capture() override {
        m_state = ABLLinkCaptureAudioSessionState(m_link);
        m_captures.captured();
    }
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

    [[nodiscard]] double quantum() const override {
        return m_shared.m_quantum.load(std::memory_order_relaxed);
    }

    void bump_epoch() { m_epoch.fetch_add(1, std::memory_order_acq_rel); }

    /// Audio thread: the state the clock captured since the last call, once; null if none.
    [[nodiscard]] ABLLinkSessionStateRef take_capture() {
        return m_captures.take() ? m_state : nullptr;
    }

    /// Audio thread: the state of the clock's latest capture (this callback's), null before one.
    [[nodiscard]] ABLLinkSessionStateRef audio_state() const { return m_state; }

private:
    [[nodiscard]] uint64_t to_ticks(int64_t us) const {
        return us <= 0 ? 0 : static_cast<uint64_t>(static_cast<double>(us) * m_ticks_per_us);
    }
    [[nodiscard]] int64_t to_us(uint64_t ticks) const {
        return static_cast<int64_t>(static_cast<double>(ticks) / m_ticks_per_us);
    }

    ABLLinkRef m_link;
    const detail::SessionShared& m_shared;
    detail::CaptureTracker m_captures;
    ABLLinkSessionStateRef m_state = nullptr;
    double m_ticks_per_us = 1.0;
    std::atomic<uint64_t> m_epoch{0};
};

// A received buffer as the Link thread queues it: the samples, and LinkKit's info to map
// it to local beats with. LinkKit's app session state is for the main thread only and its
// audio session state for the audio thread only, so the Link thread maps nothing: the
// audio thread does, with the state the clock captured in that callback.
struct ReceivedBuffer {
    dsp::transport::LinkAudioPacket m_packet;  // beats not yet set
    ABLLinkAudioSourceBufferInfo m_info;
};
using ReceivedBufferQueue = core::LockFreeQueue<ReceivedBuffer, 256>;

// Link Audio on the audio thread: received buffers out of the queue, our output into the sink.
class LinkKitSharing final : public dsp::transport::LinkAudioBackend {
public:
    LinkKitSharing(ABLLinkRef link,
                   LinkKitBackend& clock,
                   const detail::SessionShared& shared,
                   ReceivedBufferQueue& received,
                   const std::string& output_name)
        : m_link(link)
        , m_clock(clock)
        , m_shared(shared)
        , m_received(received)
        , m_sink(ABLLinkAudioSinkNew(link, output_name.c_str(), 4096)) {}
    ~LinkKitSharing() override { ABLLinkAudioSinkDelete(m_sink); }
    LinkKitSharing(const LinkKitSharing&) = delete;
    LinkKitSharing& operator=(const LinkKitSharing&) = delete;

    // Maps each buffer with this callback's capture; one from another session, or one before
    // the clock captured anything, is dropped.
    bool pop(dsp::transport::LinkAudioPacket& out) override {
        auto* state = m_clock.audio_state();
        const double quantum = m_shared.m_quantum.load(std::memory_order_relaxed);
        while (m_received.try_pop(m_buffer)) {
            double begin = 0.0;
            double end = 0.0;
            if (state == nullptr ||
                !ABLLinkAudioSourceBufferInfoBeginBeats(&m_buffer.m_info, state, quantum, &begin) ||
                !ABLLinkAudioSourceBufferInfoEndBeats(&m_buffer.m_info, state, quantum, &end) ||
                end <= begin) {
                continue;
            }
            out = m_buffer.m_packet;
            out.m_begin_beat = begin;
            // The packet may keep fewer frames than the buffer had: its end follows them.
            out.m_end_beat = begin + ((end - begin) * static_cast<double>(out.m_num_frames) /
                                      static_cast<double>(m_buffer.m_info.numFrames));
            return true;
        }
        return false;
    }

    bool send(const float* const* channels,
              uint32_t num_channels,
              uint32_t num_frames,
              double begin_beat,
              double quantum,
              uint32_t sample_rate) override {
        const auto shape = detail::send_shape(num_channels, num_frames, sample_rate);
        if (!shape) { return false; }
        auto* buffer = ABLLinkAudioRetainBuffer(m_sink);  // null while nobody listens
        if (buffer == nullptr) { return false; }
        if (!ABLLinkAudioSinkBufferHandleIsValid(buffer)) {
            ABLLinkAudioReleaseBuffer(buffer);
            return false;
        }
        if (shape->m_samples > ABLLinkAudioSinkBufferHandleMaxNumSamples(buffer)) {
            ABLLinkAudioReleaseBuffer(buffer);
            // From the next block on (at most 2 x a block of frames: fits 32 bits).
            ABLLinkAudioSinkRequestMaxNumSamples(m_sink, static_cast<uint32_t>(shape->m_samples));
            return false;
        }
        detail::interleave(channels, *shape, ABLLinkAudioSinkBufferSamples(buffer));
        // The state the block was rendered with (the clock's capture), as LinkKit requires.
        auto* state = m_clock.take_capture();
        return ABLLinkAudioReleaseAndCommitBuffer(
            m_sink,
            buffer,
            state != nullptr ? state : ABLLinkCaptureAudioSessionState(m_link),
            begin_beat,
            quantum,
            shape->m_frames,
            shape->m_channels,
            sample_rate);
    }

private:
    ABLLinkRef m_link;
    LinkKitBackend& m_clock;
    const detail::SessionShared& m_shared;
    ReceivedBufferQueue& m_received;
    ReceivedBuffer m_buffer;  // the one being taken (no copy on the audio thread's stack)
    ABLLinkAudioSinkRef m_sink;
};

}  // namespace

struct LinkSession::Impl {
    Impl(double bpm, const std::string& output_name)
        : m_link(ABLLinkNew(bpm))
        , m_shared(std::make_unique<detail::SessionShared>())
        , m_backend(m_link, *m_shared)
        , m_received(std::make_unique<ReceivedBufferQueue>())
        , m_sharing(std::make_unique<LinkKitSharing>(m_link,
                                                     m_backend,
                                                     *m_shared,
                                                     *m_received,
                                                     output_name)) {
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

    // Link thread: a received buffer, to float, for the audio thread to map to local beats
    // (no session state is ours to capture here: see ReceivedBuffer). A full queue drops it.
    static void on_buffer(const ABLLinkAudioSourceBuffer* buffer, void* context) {
        auto* impl = static_cast<Impl*>(context);
        const auto& info = buffer->info;
        if (info.numChannels == 0 || info.numFrames == 0) { return; }
        ReceivedBuffer& received = impl->m_incoming;  // the Link thread's own, no stack copy
        // Placeholder beats [0, 1): pop() maps the buffer on the audio thread.
        if (!detail::make_link_audio_packet(buffer->samples,
                                            info.numChannels,
                                            info.numFrames,
                                            0.0,
                                            1.0,
                                            received.m_packet)) {
            return;
        }
        received.m_info = info;
        (void)impl->m_received->try_push(received);
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
    std::unique_ptr<detail::SessionShared> m_shared;
    LinkKitBackend m_backend;
    std::unique_ptr<ReceivedBufferQueue> m_received;  // Link thread -> audio thread
    ReceivedBuffer m_incoming;                        // Link thread only
    std::atomic<bool> m_connected{false};
    ABLLinkSettingsViewController* m_settings = nil;
    std::unique_ptr<LinkKitSharing> m_sharing;
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

bool LinkSession::is_connected() const {
    return m_impl->m_connected.load(std::memory_order_acquire);
}

LinkSession::Capabilities LinkSession::capabilities() {
    return {.m_app_controls_settings = false,
            .m_counts_peers = false,
            .m_has_lifecycle = true,
            .m_has_settings_view = true};
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

// LinkKit's app session state is for the main thread only (ABLLink.h).
double LinkSession::tempo() const {
    NSCAssert([NSThread isMainThread], @"LinkSession::tempo() is main thread only on iOS");
    return ABLLinkGetTempo(ABLLinkCaptureAppSessionState(m_impl->m_link));
}

dsp::transport::LinkBackend& LinkSession::timeline_backend() {
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

void LinkSession::set_quantum(double quantum) {
    if (quantum > 0.0) { m_impl->m_shared->m_quantum.store(quantum, std::memory_order_relaxed); }
}

double LinkSession::quantum() const {
    return m_impl->m_shared->m_quantum.load(std::memory_order_relaxed);
}

dsp::transport::LinkAudioBackend& LinkSession::audio_backend() {
    return *m_impl->m_sharing;
}

}  // namespace thl::inline THL_ABI_NAMESPACE::link
