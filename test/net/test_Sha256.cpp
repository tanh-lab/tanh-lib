#include <gtest/gtest.h>
#include <tanh/net/Sha256.h>

#include <cstdio>
#include <filesystem>
#include <string>

using thl::net::Sha256;
namespace fs = std::filesystem;

namespace {

std::string hex_of(const std::string& text) {
    return Sha256::to_hex(Sha256::of(text.data(), text.size()));
}

fs::path temp_file(const std::string& name, const std::string& contents) {
    const auto path = fs::temp_directory_path() / ("tanh-sha-" + name);
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file != nullptr) {
        if (!contents.empty()) { std::fwrite(contents.data(), 1, contents.size(), file); }
        std::fclose(file);
    }
    return path;
}

}  // namespace

// The published FIPS 180-4 vectors. If the implementation is wrong these are
// what catch it; everything else in this file only checks plumbing.
TEST(Sha256, MatchesPublishedVectors) {
    EXPECT_EQ(hex_of(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(hex_of("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

// The one-million-'a' vector exercises the multi-block path and the length
// encoding, which a short string cannot.
TEST(Sha256, MatchesLongVector) {
    Sha256 hash;
    const std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) { hash.update(chunk.data(), chunk.size()); }
    EXPECT_EQ(Sha256::to_hex(hash.finish()),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// Feeding the same bytes in awkward slices must not change the digest — that is
// where a buffering bug would show.
TEST(Sha256, ChunkingDoesNotChangeTheDigest) {
    const std::string message(5000, 'x');
    const auto whole = Sha256::of(message.data(), message.size());

    for (const std::size_t slice : {1u, 7u, 63u, 64u, 65u, 4096u}) {
        Sha256 hash;
        for (std::size_t offset = 0; offset < message.size(); offset += slice) {
            hash.update(message.data() + offset, std::min(slice, message.size() - offset));
        }
        EXPECT_EQ(hash.finish(), whole) << "slice size " << slice;
    }
}

TEST(Sha256, ResetAllowsReuse) {
    Sha256 hash;
    hash.update("abc", 3);
    const auto first = hash.finish();
    hash.reset();
    hash.update("abc", 3);
    EXPECT_EQ(hash.finish(), first);
}

TEST(Sha256, HashesAFileInChunks) {
    const std::string contents(300 * 1024, 'q');  // larger than the read buffer
    const auto path = temp_file("file", contents);

    const auto result = Sha256::of_file(path);
    ASSERT_TRUE(result.m_ok);
    EXPECT_EQ(result.m_digest, Sha256::of(contents.data(), contents.size()));

    fs::remove(path);
}

TEST(Sha256, EmptyFileHashesLikeEmptyInput) {
    const auto path = temp_file("empty", "");
    const auto result = Sha256::of_file(path);
    ASSERT_TRUE(result.m_ok);
    EXPECT_EQ(Sha256::to_hex(result.m_digest),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    fs::remove(path);
}

TEST(Sha256, MissingFileReportsFailureRatherThanAZeroDigest) {
    const auto result = Sha256::of_file(fs::temp_directory_path() / "tanh-sha-does-not-exist");
    EXPECT_FALSE(result.m_ok);
}

// Digests arrive as strings from a server, so malformed input must be rejected
// rather than trusted or thrown on.
TEST(Sha256, MatchesHexRejectsMalformedInput) {
    const auto digest = Sha256::of("abc", 3);
    const std::string correct = Sha256::to_hex(digest);

    EXPECT_TRUE(Sha256::matches_hex(digest, correct));

    std::string upper = correct;
    for (auto& c : upper) { c = static_cast<char>(std::toupper(c)); }
    EXPECT_TRUE(Sha256::matches_hex(digest, upper)) << "case must not matter";

    EXPECT_FALSE(Sha256::matches_hex(digest, ""));
    EXPECT_FALSE(Sha256::matches_hex(digest, correct.substr(0, 63)));
    EXPECT_FALSE(Sha256::matches_hex(digest, correct + "00"));
    EXPECT_FALSE(Sha256::matches_hex(digest, std::string(64, 'z')));

    std::string wrong = correct;
    wrong[0] = wrong[0] == 'a' ? 'b' : 'a';
    EXPECT_FALSE(Sha256::matches_hex(digest, wrong));
}
