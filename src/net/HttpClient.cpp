#include <tanh/net/HttpClient.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#include "HttpBackend.h"

namespace fs = std::filesystem;

namespace thl::net {

struct HttpClient::Backend {};

HttpClient::HttpClient() : HttpClient(Options{}) {}

HttpClient::HttpClient(Options options) : m_options(options) {}

HttpClient::~HttpClient() = default;

void HttpClient::cancel() {
    m_cancelled.store(true, std::memory_order_relaxed);
}

void HttpClient::reset() {
    m_cancelled.store(false, std::memory_order_relaxed);
}

bool HttpClient::supported() {
    return detail::backend_supported();
}

HttpResult HttpClient::get_to_file(const std::string& url,
                                   const fs::path& destination,
                                   const HttpProgressFn& on_progress) {
    HttpResult result;

    if (!url.starts_with("https://") && !url.starts_with("http://")) {
        result.m_status = HttpStatus::NetworkError;
        result.m_message = "Only http and https URLs are accepted.";
        return result;
    }

    std::error_code error;
    fs::create_directories(destination.parent_path(), error);
    if (error) {
        result.m_status = HttpStatus::FileError;
        result.m_message = error.message();
        return result;
    }

    // Resume only from a non-empty partial file: a zero-length one gains
    // nothing and makes the Range header pointless.
    std::uint64_t resume_from = 0;
    if (m_options.m_allow_resume && fs::is_regular_file(destination, error)) {
        const auto existing = fs::file_size(destination, error);
        if (!error) { resume_from = existing; }
    }

    detail::Request request;
    request.m_url = url;
    request.m_destination = destination;
    request.m_resume_from = resume_from;
    request.m_timeout_seconds = m_options.m_timeout_seconds;
    request.m_cancelled = &m_cancelled;
    request.m_on_progress = &on_progress;

    return detail::perform(request);
}

HttpResult HttpClient::get_to_string(const std::string& url,
                                     std::string& out_body,
                                     std::size_t max_bytes) {
    out_body.clear();

    HttpResult result;
    if (!url.starts_with("https://") && !url.starts_with("http://")) {
        result.m_status = HttpStatus::NetworkError;
        result.m_message = "Only http and https URLs are accepted.";
        return result;
    }

    detail::Request request;
    request.m_url = url;
    request.m_timeout_seconds = m_options.m_timeout_seconds;
    request.m_cancelled = &m_cancelled;
    request.m_body = &out_body;
    request.m_max_body_bytes = max_bytes;

    return detail::perform(request);
}

}  // namespace thl::net
