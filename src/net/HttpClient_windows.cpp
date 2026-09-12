#include <tanh/net/HttpClient.h>

#include "HttpBackend.h"

// clang-format off
#include <windows.h>
#include <winhttp.h>
// clang-format on

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

namespace thl::net::detail {
namespace {

constexpr std::size_t k_read_chunk = 64 * 1024;

/// Closes a WinHTTP handle on scope exit. The three handles (session,
/// connection, request) must be closed in reverse order of creation, which
/// declaring them in order gives us for free.
struct Handle {
    HINTERNET m_value = nullptr;

    Handle() = default;
    explicit Handle(HINTERNET value) : m_value(value) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    ~Handle() {
        if (m_value != nullptr) { WinHttpCloseHandle(m_value); }
    }

    explicit operator bool() const { return m_value != nullptr; }
};

std::wstring widen(const std::string& text) {
    if (text.empty()) { return {}; }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) { return {}; }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

std::string last_error_message(const char* what) {
    return std::string(what) + " failed (GetLastError=" + std::to_string(GetLastError()) + ")";
}

}  // namespace

HttpResult perform(const Request& request) {
    HttpResult result;

    const std::wstring url = widen(request.m_url);
    if (url.empty()) {
        result.m_status = HttpStatus::NetworkError;
        result.m_message = "URL is not valid UTF-8.";
        return result;
    }

    // Split the URL rather than parsing it by hand: WinHttpCrackUrl handles
    // ports, escaping and IPv6 literals.
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    std::wstring host(256, L'\0');
    std::wstring path(2048, L'\0');
    components.lpszHostName = host.data();
    components.dwHostNameLength = static_cast<DWORD>(host.size());
    components.lpszUrlPath = path.data();
    components.dwUrlPathLength = static_cast<DWORD>(path.size());

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) {
        result.m_status = HttpStatus::NetworkError;
        result.m_message = last_error_message("WinHttpCrackUrl");
        return result;
    }

    host.resize(components.dwHostNameLength);
    path.resize(components.dwUrlPathLength);
    const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;

    // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY uses the system proxy configuration,
    // and certificate validation uses the system trust store — the same posture
    // as NSURLSession on Apple, with no CA bundle of our own.
    const Handle session(WinHttpOpen(L"tanh-net/1.0",
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS,
                                     0));
    if (!session) {
        result.m_status = HttpStatus::NetworkError;
        result.m_message = last_error_message("WinHttpOpen");
        return result;
    }

    if (request.m_timeout_seconds > 0) {
        const int milliseconds = request.m_timeout_seconds * 1000;
        // Resolve, connect, send, receive. The receive timeout bounds a stall
        // between reads, not the whole transfer, so a large asset on a slow
        // link is not killed for being big.
        WinHttpSetTimeouts(session.m_value, milliseconds, milliseconds, milliseconds, milliseconds);
    }

    const Handle connection(WinHttpConnect(session.m_value, host.c_str(), components.nPort, 0));
    if (!connection) {
        result.m_status = HttpStatus::NetworkError;
        result.m_message = last_error_message("WinHttpConnect");
        return result;
    }

    const Handle http_request(WinHttpOpenRequest(connection.m_value,
                                                 L"GET",
                                                 path.c_str(),
                                                 nullptr,
                                                 WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 secure ? WINHTTP_FLAG_SECURE : 0));
    if (!http_request) {
        result.m_status = HttpStatus::NetworkError;
        result.m_message = last_error_message("WinHttpOpenRequest");
        return result;
    }

    const bool to_file = !request.m_destination.empty();
    std::uint64_t resume_from = to_file ? request.m_resume_from : 0;

    if (resume_from > 0) {
        const std::wstring range = L"Range: bytes=" + std::to_wstring(resume_from) + L"-";
        WinHttpAddRequestHeaders(http_request.m_value,
                                 range.c_str(),
                                 static_cast<DWORD>(range.size()),
                                 WINHTTP_ADDREQ_FLAG_ADD);
    }

    if (!WinHttpSendRequest(http_request.m_value,
                            WINHTTP_NO_ADDITIONAL_HEADERS,
                            0,
                            WINHTTP_NO_REQUEST_DATA,
                            0,
                            0,
                            0) ||
        !WinHttpReceiveResponse(http_request.m_value, nullptr)) {
        result.m_status = HttpStatus::NetworkError;
        result.m_message = last_error_message("WinHttpSendRequest/ReceiveResponse");
        return result;
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    WinHttpQueryHeaders(http_request.m_value,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &status_code,
                        &status_size,
                        WINHTTP_NO_HEADER_INDEX);
    result.m_http_code = static_cast<int>(status_code);

    if (status_code < 200 || status_code >= 300) {
        result.m_status = HttpStatus::HttpError;
        result.m_message = "Server returned HTTP " + std::to_string(status_code);
        return result;
    }

    // A 200 where a range was asked for means the server sent the whole body;
    // what is on disk is not a prefix of it, so the file starts over rather
    // than having the complete body appended to the partial one.
    if (resume_from > 0 && status_code == 200) { resume_from = 0; }

    std::uint64_t declared = 0;
    {
        DWORD length = 0;
        DWORD length_size = sizeof(length);
        if (WinHttpQueryHeaders(http_request.m_value,
                                WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX,
                                &length,
                                &length_size,
                                WINHTTP_NO_HEADER_INDEX)) {
            declared = length;
        }
    }

    std::ofstream file;
    if (to_file) {
        file.open(request.m_destination,
                  resume_from > 0 ? (std::ios::binary | std::ios::app)
                                  : (std::ios::binary | std::ios::trunc));
        if (!file.is_open()) {
            result.m_status = HttpStatus::FileError;
            result.m_message = "Could not open the destination for writing.";
            return result;
        }
    }

    std::vector<char> buffer(k_read_chunk);
    std::uint64_t received = 0;

    while (true) {
        if (request.m_cancelled != nullptr &&
            request.m_cancelled->load(std::memory_order_relaxed)) {
            result.m_status = HttpStatus::Cancelled;
            result.m_message = "Cancelled.";
            result.m_bytes_written = received;
            return result;
        }

        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(http_request.m_value, &available)) {
            result.m_status = HttpStatus::NetworkError;
            result.m_message = last_error_message("WinHttpQueryDataAvailable");
            result.m_bytes_written = received;
            return result;
        }
        if (available == 0) { break; }

        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(available, buffer.size()));
        DWORD read = 0;
        if (!WinHttpReadData(http_request.m_value, buffer.data(), want, &read)) {
            result.m_status = HttpStatus::NetworkError;
            result.m_message = last_error_message("WinHttpReadData");
            result.m_bytes_written = received;
            return result;
        }
        if (read == 0) { break; }

        if (to_file) {
            file.write(buffer.data(), static_cast<std::streamsize>(read));
            if (!file) {
                result.m_status = HttpStatus::FileError;
                result.m_message = "Could not write the received data.";
                result.m_bytes_written = received;
                return result;
            }
        } else if (request.m_body != nullptr) {
            if (request.m_max_body_bytes > 0 &&
                request.m_body->size() + read > request.m_max_body_bytes) {
                result.m_status = HttpStatus::HttpError;
                result.m_message = "Response body exceeded the caller's limit.";
                result.m_bytes_written = received;
                return result;
            }
            request.m_body->append(buffer.data(), read);
        }

        received += read;

        if (request.m_on_progress != nullptr && *request.m_on_progress) {
            HttpProgress progress;
            progress.m_downloaded = resume_from + received;
            progress.m_total = declared > 0 ? resume_from + declared : 0;
            if (!(*request.m_on_progress)(progress)) {
                result.m_status = HttpStatus::Cancelled;
                result.m_message = "Cancelled.";
                result.m_bytes_written = received;
                return result;
            }
        }
    }

    if (to_file) {
        file.flush();
        const bool bad = file.bad();
        file.close();
        if (bad) {
            result.m_status = HttpStatus::FileError;
            result.m_message = "Could not write the received data.";
            result.m_bytes_written = received;
            return result;
        }
    }

    result.m_bytes_written = received;
    result.m_status = HttpStatus::Ok;
    return result;
}

bool backend_supported() {
    return true;
}

}  // namespace thl::net::detail
