#include <curl/curl.h>
#include <tanh/net/HttpClient.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>

#include "HttpBackend.h"

namespace thl::net::detail {
namespace {

/// State the curl callbacks write into. One per request.
struct Transfer {
    const Request* m_request = nullptr;

    std::ofstream m_file;
    std::string* m_body = nullptr;
    std::size_t m_max_body_bytes = 0;

    std::uint64_t m_received = 0;
    std::uint64_t m_resume_offset = 0;
    bool m_checked_range = false;
    bool m_write_failed = false;
    bool m_body_too_large = false;
    bool m_cancelled = false;

    CURL* m_handle = nullptr;
};

/// A 200 where we asked for a range means the server sent the whole body. What
/// is already on disk is not a prefix of it, so the file has to start over
/// rather than have the full body appended to the partial one.
void restart_if_range_ignored(Transfer& transfer) {
    if (transfer.m_checked_range) { return; }
    transfer.m_checked_range = true;

    if (transfer.m_resume_offset == 0 || transfer.m_body != nullptr) { return; }

    long code = 0;
    curl_easy_getinfo(transfer.m_handle, CURLINFO_RESPONSE_CODE, &code);
    if (code != 200) { return; }

    transfer.m_file.close();
    transfer.m_file.open(transfer.m_request->m_destination, std::ios::binary | std::ios::trunc);
    transfer.m_resume_offset = 0;
    if (!transfer.m_file.is_open()) { transfer.m_write_failed = true; }
}

std::size_t write_callback(char* data, std::size_t size, std::size_t count, void* user) {
    auto& transfer = *static_cast<Transfer*>(user);
    const std::size_t bytes = size * count;

    restart_if_range_ignored(transfer);
    if (transfer.m_write_failed) { return 0; }

    if (transfer.m_body != nullptr) {
        if (transfer.m_max_body_bytes > 0 &&
            transfer.m_body->size() + bytes > transfer.m_max_body_bytes) {
            transfer.m_body_too_large = true;
            return 0;  // non-matching count aborts the transfer
        }
        transfer.m_body->append(data, bytes);
    } else {
        transfer.m_file.write(data, static_cast<std::streamsize>(bytes));
        if (!transfer.m_file) {
            transfer.m_write_failed = true;
            return 0;
        }
    }

    transfer.m_received += bytes;
    return bytes;
}

/// Progress is also where cancellation is noticed: a non-zero return aborts.
int progress_callback(void* user, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
    auto& transfer = *static_cast<Transfer*>(user);
    const auto& request = *transfer.m_request;

    if (request.m_cancelled != nullptr && request.m_cancelled->load(std::memory_order_relaxed)) {
        transfer.m_cancelled = true;
        return 1;
    }

    if (request.m_on_progress != nullptr && *request.m_on_progress) {
        HttpProgress progress;
        progress.m_downloaded = transfer.m_resume_offset + static_cast<std::uint64_t>(now);
        progress.m_total =
            total > 0 ? transfer.m_resume_offset + static_cast<std::uint64_t>(total) : 0;
        if (!(*request.m_on_progress)(progress)) {
            transfer.m_cancelled = true;
            return 1;
        }
    }
    return 0;
}

}  // namespace

HttpResult perform(const Request& request) {
    HttpResult result;

    CURL* handle = curl_easy_init();
    if (handle == nullptr) {
        result.m_status = HttpStatus::NetworkError;
        result.m_message = "Could not initialise the transfer.";
        return result;
    }

    Transfer transfer;
    transfer.m_request = &request;
    transfer.m_handle = handle;

    const bool to_file = !request.m_destination.empty();
    if (to_file) {
        transfer.m_resume_offset = request.m_resume_from;
        transfer.m_file.open(request.m_destination,
                             transfer.m_resume_offset > 0 ? (std::ios::binary | std::ios::app)
                                                          : (std::ios::binary | std::ios::trunc));
        if (!transfer.m_file.is_open()) {
            curl_easy_cleanup(handle);
            result.m_status = HttpStatus::FileError;
            result.m_message = "Could not open the destination for writing.";
            return result;
        }
    } else {
        transfer.m_body = request.m_body;
        transfer.m_max_body_bytes = request.m_max_body_bytes;
    }

    curl_easy_setopt(handle, CURLOPT_URL, request.m_url.c_str());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &transfer);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(handle, CURLOPT_FAILONERROR, 0L);  // read the status ourselves
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);     // safe from any thread

    // Bound a stall rather than the transfer: a large asset on a slow link must
    // not be killed for being big.
    if (request.m_timeout_seconds > 0) {
        curl_easy_setopt(handle,
                         CURLOPT_CONNECTTIMEOUT,
                         static_cast<long>(request.m_timeout_seconds));
        curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(handle,
                         CURLOPT_LOW_SPEED_TIME,
                         static_cast<long>(request.m_timeout_seconds));
    }

    // The Range header is set by hand rather than with CURLOPT_RESUME_FROM_LARGE:
    // that option makes curl enforce range semantics itself and fail the whole
    // transfer with CURLE_RANGE_ERROR when a server answers 200, before any body
    // reaches the write callback. We want that body — a server ignoring Range is
    // a case to recover from, not to abort on — and setting the header directly
    // also matches what the Apple and WinHTTP backends do.
    curl_slist* headers = nullptr;
    if (transfer.m_resume_offset > 0) {
        const std::string range = "Range: bytes=" + std::to_string(transfer.m_resume_offset) + "-";
        headers = curl_slist_append(headers, range.c_str());
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    }

    const CURLcode code = curl_easy_perform(handle);

    if (headers != nullptr) { curl_slist_free_all(headers); }

    long http_code = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(handle);

    transfer.m_file.flush();
    const bool file_bad = to_file && transfer.m_file.bad();
    transfer.m_file.close();

    result.m_bytes_written = transfer.m_received;
    result.m_http_code = static_cast<int>(http_code);

    if (transfer.m_cancelled) {
        result.m_status = HttpStatus::Cancelled;
        result.m_message = "Cancelled.";
    } else if (transfer.m_write_failed || file_bad) {
        result.m_status = HttpStatus::FileError;
        result.m_message = "Could not write the received data.";
    } else if (transfer.m_body_too_large) {
        result.m_status = HttpStatus::HttpError;
        result.m_message = "Response body exceeded the caller's limit.";
    } else if (http_code > 0 && (http_code < 200 || http_code >= 300)) {
        result.m_status = HttpStatus::HttpError;
        result.m_message = "Server returned HTTP " + std::to_string(http_code);
    } else if (code != CURLE_OK) {
        result.m_status = HttpStatus::NetworkError;
        result.m_message = curl_easy_strerror(code);
    } else {
        result.m_status = HttpStatus::Ok;
    }

    return result;
}

bool backend_supported() {
    return true;
}

}  // namespace thl::net::detail
