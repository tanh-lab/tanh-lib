#pragma once
#include <tanh/core/Exports.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace thl::net {

/// SHA-256 (FIPS 180-4).
///
/// Implemented in plain C++ rather than against CommonCrypto / BCrypt / OpenSSL
/// on purpose: it removes a whole platform matrix from a component that already
/// needs one for HTTP, and it keeps the library free of a system crypto
/// dependency. Throughput is a few hundred MB/s, which is irrelevant next to the
/// download it verifies.
///
/// Not real-time safe and not intended to be — this runs on a worker thread
/// after a transfer.
class TANH_API Sha256 {
public:
    /// Raw digest, 32 bytes.
    using Digest = std::array<std::uint8_t, 32>;

    Sha256();

    /// Feed more bytes. May be called any number of times before finish().
    void update(const void* data, std::size_t size);

    /// Finish and return the digest. The object must not be reused afterwards
    /// without reset().
    [[nodiscard]] Digest finish();

    /// Return to the initial state so the object can hash something else.
    void reset();

    /// Lowercase hex, 64 characters — the form the manifests use.
    [[nodiscard]] static std::string to_hex(const Digest& digest);

    /// Convenience: hash a whole buffer.
    [[nodiscard]] static Digest of(const void* data, std::size_t size);

    /// Convenience: hash a file in chunks, so size is bounded by the buffer and
    /// not by the file. Returns an empty optional-shaped result — an all-zero
    /// digest with `ok == false` — when the file cannot be read.
    struct FileResult {
        Digest m_digest{};
        bool m_ok = false;
    };
    [[nodiscard]] static FileResult of_file(const std::filesystem::path& path);

    /// Case-insensitive comparison of a digest against a hex string. Returns
    /// false for malformed input rather than throwing, since the string comes
    /// from a server.
    [[nodiscard]] static bool matches_hex(const Digest& digest, std::string_view hex);

private:
    void compress(const std::uint8_t* block);

    std::array<std::uint32_t, 8> m_state{};
    std::array<std::uint8_t, 64> m_buffer{};
    std::size_t m_buffered = 0;
    std::uint64_t m_total_bits = 0;
};

}  // namespace thl::net
