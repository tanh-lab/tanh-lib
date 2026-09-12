#pragma once
#include <tanh/net/HttpClient.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

/// Internal seam between HttpClient and its per-platform implementations.
/// Not installed and not part of the public API.
namespace thl::net::detail {

/// One transfer. Either m_destination (stream to file) or m_body (collect in
/// memory) is set, never both.
struct Request {
    std::string m_url;

    std::filesystem::path m_destination;
    /// Byte offset to resume from; 0 starts fresh. Only meaningful with
    /// m_destination.
    std::uint64_t m_resume_from = 0;

    /// When set, the body is appended here instead of written to a file.
    std::string* m_body = nullptr;
    std::size_t m_max_body_bytes = 0;

    int m_timeout_seconds = 60;

    /// Polled by the backend; never null.
    const std::atomic<bool>* m_cancelled = nullptr;
    /// May point at an empty std::function; backends must check before calling.
    const HttpProgressFn* m_on_progress = nullptr;
};

/// Run a request to completion. Blocks.
HttpResult perform(const Request& request);

/// False when this build has no real backend.
bool backend_supported();

}  // namespace thl::net::detail
