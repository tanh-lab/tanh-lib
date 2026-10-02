// LinkSession over LinkKit (iOS). Compiled with ARC.
//
// LinkKit is a C API on mach host-time ticks; the seam speaks microseconds on the
// same clock, so the backend converts with mach_timebase_info. Enabling Link and
// start/stop sync are user settings in ABLLinkSettingsViewController.

#import <ABLLink.h>
#import <ABLLinkSettingsViewController.h>
#include <mach/mach_time.h>
#include <tanh/dsp/transport/LinkBackend.h>
#include <tanh/link/LinkSession.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

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

    void capture() noexcept override { m_state = ABLLinkCaptureAudioSessionState(m_link); }
    [[nodiscard]] double beat_at(int64_t t_us, double quantum) const noexcept override {
        return ABLLinkBeatAtTime(m_state, to_ticks(t_us), quantum);
    }
    [[nodiscard]] dsp::transport::LinkState state() const noexcept override {
        return {.m_tempo = ABLLinkGetTempo(m_state),
                .m_playing = ABLLinkIsPlaying(m_state),
                .m_play_time_us = to_us(ABLLinkTimeForIsPlaying(m_state))};
    }
    void set_tempo(double bpm, int64_t t_us) noexcept override {
        ABLLinkSetTempo(m_state, bpm, to_ticks(t_us));
    }
    void set_playing(bool playing, int64_t t_us) noexcept override {
        ABLLinkSetIsPlaying(m_state, playing, to_ticks(t_us));
    }
    void request_beat_at_start(double beat, double quantum) noexcept override {
        ABLLinkRequestBeatAtStartPlayingTime(m_state, beat, quantum);
    }
    void request_beat_at(double beat, int64_t t_us, double quantum) noexcept override {
        ABLLinkRequestBeatAtTime(m_state, beat, to_ticks(t_us), quantum);
    }
    void commit() noexcept override { ABLLinkCommitAudioSessionState(m_link, m_state); }
    [[nodiscard]] int64_t now_us() const noexcept override { return to_us(mach_absolute_time()); }
    [[nodiscard]] uint64_t timeline_epoch() const noexcept override {
        return m_epoch.load(std::memory_order_acquire);
    }

    void bump_epoch() noexcept { m_epoch.fetch_add(1, std::memory_order_acq_rel); }

private:
    [[nodiscard]] uint64_t to_ticks(int64_t us) const noexcept {
        return us <= 0 ? 0 : static_cast<uint64_t>(static_cast<double>(us) * m_ticks_per_us);
    }
    [[nodiscard]] int64_t to_us(uint64_t ticks) const noexcept {
        return static_cast<int64_t>(static_cast<double>(ticks) / m_ticks_per_us);
    }

    ABLLinkRef m_link;
    ABLLinkSessionStateRef m_state = nullptr;
    double m_ticks_per_us = 1.0;
    std::atomic<uint64_t> m_epoch{0};
};

}  // namespace

struct LinkSession::Impl {
    explicit Impl(double bpm) : m_link(ABLLinkNew(bpm)), m_backend(m_link) {
        ABLLinkSetIsEnabledCallback(m_link, &Impl::on_enabled, this);
        ABLLinkSetIsConnectedCallback(m_link, &Impl::on_connected, this);
    }
    ~Impl() {
        m_settings = nil;
        ABLLinkDelete(m_link);
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
};

LinkSession::LinkSession(double initial_bpm) : m_impl(std::make_unique<Impl>(initial_bpm)) {}

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

}  // namespace thl::link
