#pragma once
#include <tanh/core/Exports.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace thl::net {

/// Why a transfer stopped. `Ok` is the only success.
enum class HttpStatus {
    Ok,
    /// No backend compiled in for this platform.
    Unsupported,
    /// Could not reach the host: DNS, connection refused, TLS failure, offline.
    NetworkError,
    /// Reached the server, which answered with something other than 2xx.
    HttpError,
    /// The local file could not be created or written.
    FileError,
    /// cancel() was called, or the progress callback asked to stop.
    Cancelled,
};

/// Outcome of a single request.
struct HttpResult {
    HttpStatus m_status = HttpStatus::NetworkError;
    /// Set when m_status is HttpError; 0 otherwise.
    int m_http_code = 0;
    /// Bytes written to the destination by this call — excludes bytes already
    /// present when resuming.
    std::uint64_t m_bytes_written = 0;
    /// Human-readable detail for logs. Never shown verbatim to a user.
    std::string m_message;

    [[nodiscard]] bool ok() const { return m_status == HttpStatus::Ok; }
};

/// Transfer progress. `m_total` is zero when the server does not say how big the
/// body is, so callers must tolerate an unknown total rather than dividing by it.
struct HttpProgress {
    std::uint64_t m_downloaded = 0;
    std::uint64_t m_total = 0;
};

/// Called on an unspecified worker thread as bytes arrive. Return false to
/// abort the transfer. Implementations must be cheap: it is called often.
using HttpProgressFn = std::function<bool(const HttpProgress&)>;

/// Minimal HTTPS GET-to-file client.
///
/// Deliberately not a general HTTP library. It does the one thing asset
/// delivery needs — fetch a URL into a file, resumably, with progress and
/// cancellation — and leaves everything else out.
///
/// Backends are platform-native (NSURLSession, WinHTTP) rather than a vendored
/// TLS stack, so certificate validation uses the OS trust store and there is no
/// CA bundle to ship or rotate. Where no backend exists the calls return
/// HttpStatus::Unsupported instead of failing to link.
///
/// Thread-safe for use from one thread at a time per instance; cancel() may be
/// called from any thread.
class TANH_API HttpClient {
public:
    struct Options {
        /// Give up if the transfer stalls this long. Zero uses the backend default.
        int m_timeout_seconds = 60;
        /// Resume into an existing partial file with a Range request when the
        /// destination already exists. Servers that ignore Range are detected
        /// (a 200 where 206 was expected) and the file is restarted.
        bool m_allow_resume = true;
    };

    HttpClient();
    explicit HttpClient(Options options);
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    /// Fetch `url` into `destination`, creating parent directories as needed.
    ///
    /// On success the file holds the complete body. On any failure the file is
    /// left as-is so a later call can resume it; callers that want a clean slate
    /// delete it themselves. Blocks until finished, cancelled or timed out.
    HttpResult get_to_file(const std::string& url,
                           const std::filesystem::path& destination,
                           const HttpProgressFn& on_progress = {});

    /// Fetch a small resource into memory — manifests, not payloads. Bodies
    /// larger than `max_bytes` fail with HttpError rather than allocating.
    HttpResult get_to_string(const std::string& url,
                             std::string& out_body,
                             std::size_t max_bytes = 4u * 1024u * 1024u);

    /// Ask the in-flight request to stop. Safe from any thread; a request that
    /// has already finished is unaffected. The flag stays set until reset().
    void cancel();

    /// Clear a previous cancel() so the instance can be reused.
    void reset();

    [[nodiscard]] bool cancelled() const { return m_cancelled.load(std::memory_order_relaxed); }

    /// True when this build has a real backend. False means every request will
    /// return HttpStatus::Unsupported.
    [[nodiscard]] static bool supported();

private:
    struct Backend;

    Options m_options;
    std::atomic<bool> m_cancelled{false};
    std::unique_ptr<Backend> m_backend;
};

}  // namespace thl::net
