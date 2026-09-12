#pragma once
#include <tanh/core/Exports.h>
#include <tanh/net/HttpClient.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace thl::net {

/// One file inside an asset. The caller fills this in from whatever index it
/// uses — this component has no opinion about manifests.
struct AssetFile {
    /// Absolute URL to fetch.
    std::string m_url;
    /// Filename to store, relative to the asset directory. May contain
    /// separators for nested layouts; must not escape the directory.
    std::string m_name;
    /// Expected size in bytes. Zero means unknown — the size check is skipped.
    std::uint64_t m_size = 0;
    /// Expected SHA-256, lowercase hex. Empty means unverified, which the store
    /// allows but callers should avoid.
    std::string m_sha256;
};

/// A complete, versioned asset: the unit that gets installed or removed.
struct AssetDescriptor {
    /// Stable identifier, used as a directory name. Must be a single path
    /// segment: no separators, no "..".
    std::string m_id;
    /// Version, used as the second directory level. Same constraints.
    std::string m_version;
    std::vector<AssetFile> m_files;
};

/// Progress across a whole install, so callers can drive one bar rather than
/// one per file.
struct AssetProgress {
    std::size_t m_file_index = 0;
    std::size_t m_file_count = 0;
    /// Name of the file currently transferring.
    std::string m_current_file;
    /// Bytes finished across all files, including those already verified.
    std::uint64_t m_bytes_done = 0;
    /// Sum of every m_size in the descriptor. Zero when any size was unknown.
    std::uint64_t m_bytes_total = 0;
};

/// Return false to abort the install.
using AssetProgressFn = std::function<bool(const AssetProgress&)>;

enum class InstallStatus {
    Ok,
    /// The descriptor is malformed: bad id/version, empty file list, a name that
    /// escapes the directory.
    InvalidDescriptor,
    /// A transfer failed. `m_http` carries the detail.
    DownloadFailed,
    /// A file arrived but its size or digest did not match the descriptor.
    VerificationFailed,
    /// The local filesystem refused something: no space, no permission.
    FileError,
    Cancelled,
};

struct InstallResult {
    InstallStatus m_status = InstallStatus::FileError;
    /// Which file failed, when one did.
    std::string m_file;
    HttpResult m_http;
    std::string m_message;

    [[nodiscard]] bool ok() const { return m_status == InstallStatus::Ok; }
};

/// A local store of verified, versioned assets.
///
/// Layout under the root:
///
///     <root>/<id>/<version>/<file names...>
///     <root>/<id>/<version>/.tanh-installed
///
/// The marker is written last and only after every file has been verified, so a
/// directory without it is a partial install. Nothing knows what the files are
/// for — models, sample packs, impulse responses are all the same to this class.
///
/// Installs are atomic in the sense that matters: a version is either complete
/// and marked, or it is absent from installed(). A crash mid-download leaves a
/// partial directory that the next install() or prune_partial() removes.
///
/// Not thread-safe. Drive one instance from one worker thread.
class TANH_API AssetStore {
public:
    /// `root` is created if missing.
    explicit AssetStore(std::filesystem::path root);

    /// Download, verify and mark a version installed.
    ///
    /// Files already present and matching the descriptor are not fetched again,
    /// so a failed install resumes cheaply. An already-complete version returns
    /// Ok without touching the network.
    InstallResult install(const AssetDescriptor& descriptor,
                          HttpClient& client,
                          const AssetProgressFn& on_progress = {});

    /// True when the version is present, complete and marked.
    [[nodiscard]] bool is_installed(const std::string& id, const std::string& version) const;

    /// Directory for a version, whether or not it is installed. Empty when the
    /// id or version is malformed.
    [[nodiscard]] std::filesystem::path path_for(const std::string& id,
                                                 const std::string& version) const;

    /// Every complete version present, as (id, version) pairs, sorted by id then
    /// version string. Partial directories are skipped.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> installed() const;

    /// Remove one version. Returns false when it was not there or could not be
    /// deleted. Removing a version that is not installed still deletes a partial
    /// directory for it.
    bool remove(const std::string& id, const std::string& version);

    /// Delete every unmarked version directory under the root. Returns how many
    /// were removed. Worth calling at startup.
    std::size_t prune_partial();

    [[nodiscard]] const std::filesystem::path& root() const { return m_root; }

    /// Name of the completion marker, exposed for tests and for tools that need
    /// to recognise the layout.
    static constexpr const char* k_marker_name = ".tanh-installed";

private:
    std::filesystem::path m_root;
};

}  // namespace thl::net
