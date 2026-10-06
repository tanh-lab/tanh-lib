#pragma once

#include <tanh/core/Exports.h>
#include <tanh/dsp/transport/LinkBackend.h>

#include <cstddef>
#include <memory>

namespace thl::link {

/**
 * @brief Owns one Ableton Link instance (tanh::Link, built with TANH_WITH_LINK).
 *
 * Desktop (macOS, Linux, Windows) uses the Ableton Link C++ SDK, iOS uses
 * LinkKit (ABLLink). No Link header appears here (pimpl). Drive the audio side
 * with a thl::dsp::transport::LinkTransportClock over audio_backend():
 *
 * @code
 * thl::link::LinkSession session(120.0);
 * thl::dsp::transport::LinkTransportClock clock(session.audio_backend());
 * session.set_enabled(true);
 * @endcode
 *
 * The session must outlive every clock built on it.
 *
 * Platform differences:
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
 * Construction, destruction and the setters run on the message thread and may
 * block. The getters are callable from any thread except the audio thread
 * (tempo() captures Link's app session state). audio_backend()'s methods are for
 * the audio thread only.
 *
 * Link is dual-licensed (GPLv2+ or a proprietary licence from Ableton). A product
 * that ships with TANH_WITH_LINK must be GPL-compatible or hold the proprietary
 * Link licence; LinkKit (iOS) is available under the proprietary licence only.
 */
class TANH_API LinkSession {
public:
    explicit LinkSession(double initial_bpm = 120.0);
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

    /// Peers in the session (iOS: 1 while connected).
    [[nodiscard]] size_t num_peers() const;

    /// iOS lifecycle (ABLLinkSetActive); no-op elsewhere.
    void set_active(bool active);

    /// iOS: the LinkKit settings view controller to present; nullptr elsewhere.
    [[nodiscard]] void* settings_view_controller();

    /// Session tempo as seen by the application thread.
    [[nodiscard]] double tempo() const;

    /// The audio-thread seam for LinkTransportClock.
    [[nodiscard]] dsp::transport::LinkBackend& audio_backend();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace thl::link
