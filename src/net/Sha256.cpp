#include <tanh/net/Sha256.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <vector>

namespace thl::net {
namespace {

constexpr std::array<std::uint32_t, 64> k_round_constants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::array<std::uint32_t, 8> k_initial_state = {0x6a09e667,
                                                          0xbb67ae85,
                                                          0x3c6ef372,
                                                          0xa54ff53a,
                                                          0x510e527f,
                                                          0x9b05688c,
                                                          0x1f83d9ab,
                                                          0x5be0cd19};

// Hashing a 100 MB file a few hundred KB at a time keeps peak memory flat and
// still amortises the read syscalls.
constexpr std::size_t k_file_chunk = std::size_t{256} * 1024;

constexpr std::uint32_t rotr(std::uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

std::uint32_t load_be32(const std::uint8_t* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) | static_cast<std::uint32_t>(bytes[3]);
}

void store_be32(std::uint8_t* bytes, std::uint32_t value) {
    bytes[0] = static_cast<std::uint8_t>(value >> 24);
    bytes[1] = static_cast<std::uint8_t>(value >> 16);
    bytes[2] = static_cast<std::uint8_t>(value >> 8);
    bytes[3] = static_cast<std::uint8_t>(value);
}

}  // namespace

Sha256::Sha256() {
    reset();
}

void Sha256::reset() {
    m_state = k_initial_state;
    m_buffered = 0;
    m_total_bits = 0;
}

void Sha256::compress(const std::uint8_t* block) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) { w[i] = load_be32(block + i * 4); }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
    std::uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + ch + k_round_constants[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

void Sha256::update(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    m_total_bits += static_cast<std::uint64_t>(size) * 8;

    // Top up a partial block first, then run whole blocks straight from the
    // caller's buffer, then keep whatever is left over.
    if (m_buffered > 0) {
        const std::size_t take = std::min(size, m_buffer.size() - m_buffered);
        std::memcpy(m_buffer.data() + m_buffered, bytes, take);
        m_buffered += take;
        bytes += take;
        size -= take;
        if (m_buffered == m_buffer.size()) {
            compress(m_buffer.data());
            m_buffered = 0;
        }
    }

    while (size >= m_buffer.size()) {
        compress(bytes);
        bytes += m_buffer.size();
        size -= m_buffer.size();
    }

    if (size > 0) {
        std::memcpy(m_buffer.data(), bytes, size);
        m_buffered = size;
    }
}

Sha256::Digest Sha256::finish() {
    const std::uint64_t total_bits = m_total_bits;

    // Padding is 0x80, then zeros up to 56 bytes into the final block, then the
    // message length in bits as a big-endian u64. Assembled in one buffer and
    // fed once: byte-at-a-time updates were slower and left the bounds of each
    // call unprovable to static analysis.
    //
    // Worst case is 1 + 63 + 8 bytes. m_total_bits is bumped again by this
    // update, which does not matter — the length was captured above and the
    // object is finished.
    std::array<std::uint8_t, 72> tail{};
    std::size_t length = 0;
    tail[length++] = 0x80;

    const std::size_t used = (m_buffered + 1) % 64;
    const std::size_t zeros = (56 + 64 - used) % 64;
    length += zeros;  // tail is value-initialised, so the zeros are already there

    store_be32(tail.data() + length, static_cast<std::uint32_t>(total_bits >> 32));
    store_be32(tail.data() + length + 4, static_cast<std::uint32_t>(total_bits & 0xffffffffu));
    length += 8;

    update(tail.data(), length);

    Digest digest{};
    for (std::size_t i = 0; i < m_state.size(); ++i) {
        store_be32(digest.data() + i * 4, m_state[i]);
    }
    return digest;
}

std::string Sha256::to_hex(const Digest& digest) {
    static constexpr std::array<char, 16> k_digits =
        {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string hex;
    hex.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        hex.push_back(k_digits[byte >> 4]);
        hex.push_back(k_digits[byte & 0x0f]);
    }
    return hex;
}

Sha256::Digest Sha256::of(const void* data, std::size_t size) {
    Sha256 hash;
    hash.update(data, size);
    return hash.finish();
}

Sha256::FileResult Sha256::of_file(const std::filesystem::path& path) {
    FileResult result;

    // ifstream rather than C stdio: it takes the path directly, so there is no
    // narrow/wide fopen split for Windows, and the stream state is checked
    // rather than inferred from a short read.
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) { return result; }

    Sha256 hash;
    std::vector<char> buffer(k_file_chunk);
    while (stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
           stream.gcount() > 0) {
        hash.update(buffer.data(), static_cast<std::size_t>(stream.gcount()));
        if (stream.eof()) { break; }
    }

    // eof is the expected end; anything else is a read failure.
    if (stream.bad() || (stream.fail() && !stream.eof())) { return result; }

    result.m_digest = hash.finish();
    result.m_ok = true;
    return result;
}

bool Sha256::matches_hex(const Digest& digest, std::string_view hex) {
    if (hex.size() != digest.size() * 2) { return false; }
    const auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') { return c - '0'; }
        if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
        if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
        return -1;
    };
    for (std::size_t i = 0; i < digest.size(); ++i) {
        const int high = value(hex[i * 2]);
        const int low = value(hex[i * 2 + 1]);
        if (high < 0 || low < 0) { return false; }
        if (digest[i] != static_cast<std::uint8_t>((high << 4) | low)) { return false; }
    }
    return true;
}

}  // namespace thl::net
