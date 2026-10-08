#pragma once

#include <tanh/core/Exports.h>
#include <tanh/dsp/transport/LinkAudio.h>
#include <tanh/dsp/transport/LinkBackend.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace thl::link {

/**
 * @brief Owns one Ableton Link instance (tanh::Link, built with TANH_BUILD_LINK).
 *
 * Desktop (macOS, Linux, Windows) uses the Ableton Link C++ SDK, iOS uses
 * LinkKit (ABLLink). No Link header appears here (pimpl). Drive the audio side
 * with a thl::dsp::transport::LinkTransportClock over timeline_backend():
 *
 * @code
 * thl::link::LinkSession session(120.0);
 * thl::dsp::transport::LinkTransportClock clock(session.timeline_backend());
 * session.set_enabled(true);
 * @endcode
 *
 * The session must outlive every clock built on it.
 *
 * Platform differences (capabilities() reports them):
 * - Desktop: set_enabled() and set_start_stop_sync() switch Link; num_peers() is
 *   the peer count; set_active() is a no-op; settings_view_controller() is null.
 * - iOS (LinkKit): the user enables Link and start/stop sync in LinkKit's settings
 *   view (settings_view_controller(), an `ABLLinkSettingsViewController*` to
 *   present), so set_enabled() and set_start_stop_sync() are no-ops and the
 *   getters report the user's choice. num_peers() is 1 while connected, else 0
 *   (LinkKit has no peer count). Call set_active(false) when the app goes to the
 *   background without playing audio and set_active(true) on return.
 *   The app bundles `LinkKitResources.bundle` and sets the Info.plist keys
 *   `NSLocalNetworkUsageDescription` and, for start/stop sync,
 *   `ABLLinkStartStopSyncSupported`.
 *
 * Link Audio (sharing audio channels): set_audio_enabled() (desktop) or the
 * settings view (iOS) shares audio. set_audio_input() subscribes to a peer's
 * channel; audio_backend().pop() hands its buffers to the audio thread, mapped to
 * local beats, for a dsp::transport::LinkAudioReceiver to play. audio_backend().send()
 * publishes the app's output as its own channel. On iOS the app also needs the
 * multicast networking entitlement and the Info.plist key ABLLinkAudioSupported.
 *
 * Construction, destruction and the setters run on the message thread and may
 * block. The getters are callable from any thread except the audio thread
 * (tempo() captures Link's app session state). timeline_backend()'s and
 * audio_backend()'s methods are for the audio thread only.
 *
 * Link is dual-licensed (GPLv2+ or a proprietary licence from Ableton). A product
 * that ships with TANH_BUILD_LINK must be GPL-compatible or hold the proprietary
 * Link licence; LinkKit (iOS) is available under the proprietary licence only.
 */
class TANH_API LinkSession {
public:
    /// @p peer_name names this app to the session's peers (Link Audio); on iOS the Info.plist
    /// key ABLLinkPeerName and the settings view set it instead. @p output_name names the channel
    /// audio_backend().send() publishes (fixed: the SDKs cannot rename it while audio runs).
    explicit LinkSession(double initial_bpm = 120.0,
                         const std::string& peer_name = "Link App",
                         const std::string& output_name = "Main");
    ~LinkSession();

    LinkSession(const LinkSession&) = delete;
    LinkSession& operator=(const LinkSession&) = delete;
    LinkSession(LinkSession&&) = delete;
    LinkSession& operator=(LinkSession&&) = delete;

    /// Join / leave the Link session (desktop). Enabling bumps the timeline epoch.
    void set_enabled(bool enabled);
    [[nodiscard]] bool is_enabled() const;

    /// Share start/stop with peers that enabled it too (desktop).
    void set_start_stop_sync(bool enabled);
    [[nodiscard]] bool is_start_stop_sync_enabled() const;

    /// What the platform's SDK lets the app do; where a flag is false the matching
    /// calls are no-ops or report less.
    struct Capabilities {
        /// set_enabled(), set_start_stop_sync() and set_audio_enabled() switch Link
        /// (desktop). False: they are user settings in settings_view_controller() (iOS).
        bool m_app_controls_settings = true;
        /// num_peers() counts the peers. False: it is 1 while connected (iOS).
        bool m_counts_peers = true;
        /// set_active() must follow the app's lifecycle (iOS).
        bool m_has_lifecycle = false;
        /// settings_view_controller() returns a view to present (iOS).
        bool m_has_settings_view = false;
    };
    [[nodiscard]] static Capabilities capabilities();

    /// Peers in the session (iOS: 1 while connected, see Capabilities::m_counts_peers).
    [[nodiscard]] size_t num_peers() const;
    /// At least one other peer is in the session.
    [[nodiscard]] bool is_connected() const;

    /// iOS lifecycle (ABLLinkSetActive); no-op elsewhere.
    void set_active(bool active);

    /// iOS: the LinkKit settings view controller to present; nullptr elsewhere.
    [[nodiscard]] void* settings_view_controller();

    /// Session tempo as seen by the application thread.
    [[nodiscard]] double tempo() const;

    /// The quantum in beats (the phase unit shared with peers, default 4), any thread. The
    /// clock over timeline_backend() quantizes with it and received Link Audio is mapped to
    /// beats with it.
    void set_quantum(double quantum);
    [[nodiscard]] double quantum() const;

    /// The audio-thread seam for LinkTransportClock.
    [[nodiscard]] dsp::transport::LinkBackend& timeline_backend();

    // ── Link Audio: sharing audio channels with peers ─────────────────────────────────────

    /// Share audio (desktop; iOS: a user setting in the settings view, shown with the
    /// Info.plist key ABLLinkAudioSupported). Off by default.
    void set_audio_enabled(bool enabled);
    [[nodiscard]] bool is_audio_enabled() const;

    /// An audio channel a peer announces.
    struct AudioChannel {
        uint64_t m_id = 0;  ///< stable for the channel's lifetime
        std::string m_name;
        uint64_t m_peer_id = 0;
        std::string m_peer_name;
    };
    /// The audio channels the session's other peers announce now (not our own output).
    [[nodiscard]] std::vector<AudioChannel> audio_channels() const;

    /// Receive channel @p id into audio_backend().pop(); nullopt stops receiving.
    void set_audio_input(std::optional<uint64_t> id);
    [[nodiscard]] std::optional<uint64_t> audio_input() const;

    /// The audio-thread seam for Link Audio: the received channel in, the output channel out.
    [[nodiscard]] dsp::transport::LinkAudioBackend& audio_backend();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace thl::link
