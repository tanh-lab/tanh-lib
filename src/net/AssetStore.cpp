#include <tanh/net/AssetStore.h>
#include <tanh/net/HttpClient.h>
#include <tanh/net/Sha256.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace thl::net {
namespace {

/// A path segment we are willing to turn into a directory name. Rejects
/// separators, "." and ".." outright rather than trying to sanitise: the values
/// come from a server, and the only safe answer to a surprising one is no.
bool is_safe_segment(const std::string& segment) {
    if (segment.empty() || segment.size() > 128) { return false; }
    if (segment == "." || segment == "..") { return false; }
    for (const char c : segment) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!allowed) { return false; }
    }
    return true;
}

/// A file name may contain separators (nested layouts), but must stay inside the
/// asset directory: no absolute paths, no ".." component, no root name.
bool is_safe_relative_name(const std::string& name) {
    if (name.empty() || name.size() > 512) { return false; }
    const fs::path path(name);
    if (path.is_absolute() || !path.root_name().empty()) { return false; }
    for (const auto& part : path) {
        if (part == "..") { return false; }
    }
    return true;
}

bool descriptor_is_valid(const AssetDescriptor& descriptor) {
    if (!is_safe_segment(descriptor.m_id) || !is_safe_segment(descriptor.m_version)) {
        return false;
    }
    if (descriptor.m_files.empty()) { return false; }
    for (const auto& file : descriptor.m_files) {
        if (file.m_url.empty() || !is_safe_relative_name(file.m_name)) { return false; }
    }
    return true;
}

/// A file counts as present when it is the expected size and digest. Anything
/// less and we re-fetch: a half-written file from a killed process must not be
/// mistaken for a good one.
bool file_matches(const fs::path& path, const AssetFile& expected) {
    std::error_code error;
    if (!fs::is_regular_file(path, error)) { return false; }
    if (expected.m_size > 0) {
        const auto size = fs::file_size(path, error);
        if (error || size != expected.m_size) { return false; }
    }
    if (expected.m_sha256.empty()) {
        // Nothing to verify against; size alone decided it above.
        return expected.m_size > 0;
    }
    const auto digest = Sha256::of_file(path);
    return digest.m_ok && Sha256::matches_hex(digest.m_digest, expected.m_sha256);
}

std::uint64_t total_size(const AssetDescriptor& descriptor) {
    std::uint64_t total = 0;
    for (const auto& file : descriptor.m_files) {
        if (file.m_size == 0) { return 0; }  // unknown anywhere means unknown overall
        total += file.m_size;
    }
    return total;
}

}  // namespace

AssetStore::AssetStore(fs::path root) : m_root(std::move(root)) {
    std::error_code error;
    fs::create_directories(m_root, error);
}

fs::path AssetStore::path_for(const std::string& id, const std::string& version) const {
    if (!is_safe_segment(id) || !is_safe_segment(version)) { return {}; }
    return m_root / id / version;
}

bool AssetStore::is_installed(const std::string& id, const std::string& version) const {
    const auto directory = path_for(id, version);
    if (directory.empty()) { return false; }
    std::error_code error;
    return fs::is_regular_file(directory / k_marker_name, error);
}

InstallResult AssetStore::install(const AssetDescriptor& descriptor,
                                  HttpClient& client,
                                  const AssetProgressFn& on_progress) {
    InstallResult result;

    if (!descriptor_is_valid(descriptor)) {
        result.m_status = InstallStatus::InvalidDescriptor;
        result.m_message = "Descriptor rejected: bad id, version or file name.";
        return result;
    }

    if (is_installed(descriptor.m_id, descriptor.m_version)) {
        result.m_status = InstallStatus::Ok;
        return result;
    }

    const auto directory = path_for(descriptor.m_id, descriptor.m_version);
    std::error_code error;
    fs::create_directories(directory, error);
    if (error) {
        result.m_status = InstallStatus::FileError;
        result.m_message = "Could not create " + directory.string() + ": " + error.message();
        return result;
    }

    AssetProgress progress;
    progress.m_file_count = descriptor.m_files.size();
    progress.m_bytes_total = total_size(descriptor);

    for (std::size_t i = 0; i < descriptor.m_files.size(); ++i) {
        const auto& file = descriptor.m_files[i];
        const auto destination = directory / fs::path(file.m_name);

        progress.m_file_index = i;
        progress.m_current_file = file.m_name;

        // Already good from an earlier attempt: count it and move on.
        if (file_matches(destination, file)) {
            progress.m_bytes_done += file.m_size;
            if (on_progress && !on_progress(progress)) {
                result.m_status = InstallStatus::Cancelled;
                return result;
            }
            continue;
        }

        fs::create_directories(destination.parent_path(), error);
        if (error) {
            result.m_status = InstallStatus::FileError;
            result.m_file = file.m_name;
            result.m_message = error.message();
            return result;
        }

        const std::uint64_t done_before = progress.m_bytes_done;
        const auto transfer =
            client.get_to_file(file.m_url, destination, [&](const HttpProgress& http) {
                if (!on_progress) { return true; }
                progress.m_bytes_done = done_before + http.m_downloaded;
                return on_progress(progress);
            });

        if (!transfer.ok()) {
            result.m_http = transfer;
            result.m_file = file.m_name;
            result.m_status = transfer.m_status == HttpStatus::Cancelled
                                  ? InstallStatus::Cancelled
                                  : InstallStatus::DownloadFailed;
            result.m_message = transfer.m_message;
            return result;
        }

        // Verify what actually landed, not what the server claimed. A truncated
        // body, a proxy error page served with 200, or a corrupted byte all look
        // like success at the HTTP layer and are caught only here.
        if (!file_matches(destination, file)) {
            fs::remove(destination, error);
            result.m_status = InstallStatus::VerificationFailed;
            result.m_file = file.m_name;
            result.m_message = "Size or digest mismatch for " + file.m_name;
            return result;
        }

        progress.m_bytes_done = done_before + file.m_size;
        if (on_progress && !on_progress(progress)) {
            result.m_status = InstallStatus::Cancelled;
            return result;
        }
    }

    // Marker last: until this exists the directory is a partial install, and
    // is_installed() keeps saying no.
    {
        const auto marker = directory / k_marker_name;
        // Opened and closed by the destructor; the marker's existence is the
        // entire payload, so nothing is written to it.
        const std::ofstream stream(marker, std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) {
            result.m_status = InstallStatus::FileError;
            result.m_message = "Could not write the completion marker.";
            return result;
        }
    }

    result.m_status = InstallStatus::Ok;
    return result;
}

std::vector<std::pair<std::string, std::string>> AssetStore::installed() const {
    std::vector<std::pair<std::string, std::string>> found;
    std::error_code error;

    for (const auto& id_entry : fs::directory_iterator(m_root, error)) {
        if (!id_entry.is_directory(error)) { continue; }
        const auto id = id_entry.path().filename().string();
        if (!is_safe_segment(id)) { continue; }

        for (const auto& version_entry : fs::directory_iterator(id_entry.path(), error)) {
            if (!version_entry.is_directory(error)) { continue; }
            const auto version = version_entry.path().filename().string();
            if (!is_safe_segment(version)) { continue; }
            if (!fs::is_regular_file(version_entry.path() / k_marker_name, error)) { continue; }
            found.emplace_back(id, version);
        }
    }

    std::ranges::sort(found);
    return found;
}

bool AssetStore::remove(const std::string& id, const std::string& version) {
    const auto directory = path_for(id, version);
    if (directory.empty()) { return false; }
    std::error_code error;
    if (!fs::exists(directory, error)) { return false; }
    const auto removed = fs::remove_all(directory, error);
    return !error && removed > 0;
}

std::size_t AssetStore::prune_partial() {
    std::size_t pruned = 0;
    std::error_code error;

    for (const auto& id_entry : fs::directory_iterator(m_root, error)) {
        if (!id_entry.is_directory(error)) { continue; }
        for (const auto& version_entry : fs::directory_iterator(id_entry.path(), error)) {
            if (!version_entry.is_directory(error)) { continue; }
            if (fs::is_regular_file(version_entry.path() / k_marker_name, error)) { continue; }
            if (fs::remove_all(version_entry.path(), error) > 0 && !error) { ++pruned; }
        }
    }

    return pruned;
}

}  // namespace thl::net
