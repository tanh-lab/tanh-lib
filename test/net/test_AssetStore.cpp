#include <gtest/gtest.h>
#include <tanh/net/AssetStore.h>
#include <tanh/net/HttpClient.h>
#include <tanh/net/Sha256.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "TestHttpServer.h"

using namespace thl::net;
using thl::net::test::TestHttpServer;
namespace fs = std::filesystem;

namespace {

class AssetStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(m_server.start());
        m_root = fs::temp_directory_path() /
                 ("tanh-assets-" +
                  std::to_string(::testing::UnitTest::GetInstance()->current_test_info()->line()));
        fs::remove_all(m_root);
    }

    void TearDown() override {
        m_server.stop();
        fs::remove_all(m_root);
    }

    /// Publish a body and return the descriptor entry that points at it.
    AssetFile publish(const std::string& name, const std::string& body) {
        m_server.add_route("/" + name, {body});
        AssetFile file;
        file.m_url = m_server.url("/" + name);
        file.m_name = name;
        file.m_size = body.size();
        file.m_sha256 = Sha256::to_hex(Sha256::of(body.data(), body.size()));
        return file;
    }

    static std::string read_file(const fs::path& path) {
        std::ifstream stream(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }

    AssetDescriptor two_file_descriptor() {
        AssetDescriptor descriptor;
        descriptor.m_id = "handpan";
        descriptor.m_version = "0.0.1";
        descriptor.m_files = {publish("encode.onnx", std::string(8 * 1024, 'a')),
                              publish("encode.onnx.data", std::string(32 * 1024, 'b'))};
        return descriptor;
    }

    TestHttpServer m_server;
    fs::path m_root;
};

#define SKIP_WITHOUT_BACKEND() \
    if (!HttpClient::supported()) { GTEST_SKIP() << "No HTTP backend on this platform."; }

}  // namespace

TEST_F(AssetStoreTest, InstallsAndMarksComplete) {
    SKIP_WITHOUT_BACKEND();
    AssetStore store(m_root);
    HttpClient client;
    const auto descriptor = two_file_descriptor();

    const auto result = store.install(descriptor, client);
    ASSERT_TRUE(result.ok()) << result.m_message;

    EXPECT_TRUE(store.is_installed("handpan", "0.0.1"));
    const auto directory = store.path_for("handpan", "0.0.1");
    EXPECT_TRUE(fs::exists(directory / "encode.onnx"));
    EXPECT_TRUE(fs::exists(directory / "encode.onnx.data"));
    EXPECT_TRUE(fs::exists(directory / AssetStore::k_marker_name));

    EXPECT_EQ(store.installed(),
              (std::vector<std::pair<std::string, std::string>>{{"handpan", "0.0.1"}}));
}

// The whole point of the digest: a body that arrives short looks like a success
// at the HTTP layer, and only verification catches it.
TEST_F(AssetStoreTest, RejectsATruncatedFileAndLeavesNothingInstalled) {
    SKIP_WITHOUT_BACKEND();
    const std::string body(20 * 1024, 'c');
    // A valid 200 carrying the wrong bytes: the transport cannot fault it, so
    // this is precisely the case the store's own verification exists for.
    m_server.add_route("/truncated.bin", {body, TestHttpServer::Behaviour::ShortBody});

    AssetDescriptor descriptor;
    descriptor.m_id = "djembe";
    descriptor.m_version = "0.0.1";
    AssetFile file;
    file.m_url = m_server.url("/truncated.bin");
    file.m_name = "truncated.bin";
    file.m_size = body.size();
    file.m_sha256 = Sha256::to_hex(Sha256::of(body.data(), body.size()));
    descriptor.m_files = {file};

    AssetStore store(m_root);
    HttpClient client;
    const auto result = store.install(descriptor, client);

    EXPECT_EQ(result.m_status, InstallStatus::VerificationFailed);
    EXPECT_EQ(result.m_file, "truncated.bin");
    EXPECT_FALSE(store.is_installed("djembe", "0.0.1"));
    EXPECT_TRUE(store.installed().empty());
    EXPECT_FALSE(fs::exists(store.path_for("djembe", "0.0.1") / "truncated.bin"))
        << "a file that failed verification must not be left behind";
}

TEST_F(AssetStoreTest, RejectsACorruptedFile) {
    SKIP_WITHOUT_BACKEND();
    const std::string body(4 * 1024, 'd');
    m_server.add_route("/corrupt.bin", {body});

    AssetDescriptor descriptor;
    descriptor.m_id = "pack";
    descriptor.m_version = "1.0.0";
    AssetFile file;
    file.m_url = m_server.url("/corrupt.bin");
    file.m_name = "corrupt.bin";
    file.m_size = body.size();
    // Right size, wrong digest — the case a size check alone would wave through.
    file.m_sha256 = std::string(64, '0');
    descriptor.m_files = {file};

    AssetStore store(m_root);
    HttpClient client;
    const auto result = store.install(descriptor, client);

    EXPECT_EQ(result.m_status, InstallStatus::VerificationFailed);
    EXPECT_FALSE(store.is_installed("pack", "1.0.0"));
}

TEST_F(AssetStoreTest, AFailedInstallResumesWithoutRefetchingGoodFiles) {
    SKIP_WITHOUT_BACKEND();
    const std::string good(4 * 1024, 'e');
    const std::string bad(4 * 1024, 'f');

    AssetDescriptor descriptor;
    descriptor.m_id = "pack";
    descriptor.m_version = "0.1.0";
    descriptor.m_files = {publish("good.bin", good)};

    // Second file 404s, so the first install fails after fetching the first file.
    AssetFile missing;
    missing.m_url = m_server.url("/missing.bin");
    missing.m_name = "missing.bin";
    missing.m_size = bad.size();
    missing.m_sha256 = Sha256::to_hex(Sha256::of(bad.data(), bad.size()));
    descriptor.m_files.push_back(missing);

    AssetStore store(m_root);
    HttpClient client;

    const auto first = store.install(descriptor, client);
    EXPECT_EQ(first.m_status, InstallStatus::DownloadFailed);
    EXPECT_EQ(first.m_file, "missing.bin");
    EXPECT_FALSE(store.is_installed("pack", "0.1.0"));
    EXPECT_EQ(m_server.request_count("/good.bin"), 1);

    // Publish the missing file and try again: the verified one must not be
    // fetched a second time.
    m_server.add_route("/missing.bin", {bad});
    const auto second = store.install(descriptor, client);

    ASSERT_TRUE(second.ok()) << second.m_message;
    EXPECT_TRUE(store.is_installed("pack", "0.1.0"));
    EXPECT_EQ(m_server.request_count("/good.bin"), 1)
        << "an already-verified file must not be downloaded again";
}

TEST_F(AssetStoreTest, ReinstallingAnInstalledVersionTouchesNoNetwork) {
    SKIP_WITHOUT_BACKEND();
    AssetStore store(m_root);
    HttpClient client;
    const auto descriptor = two_file_descriptor();

    ASSERT_TRUE(store.install(descriptor, client).ok());
    const int before = m_server.request_count("/encode.onnx");

    ASSERT_TRUE(store.install(descriptor, client).ok());
    EXPECT_EQ(m_server.request_count("/encode.onnx"), before);
}

TEST_F(AssetStoreTest, ReportsProgressAcrossTheWholeInstall) {
    SKIP_WITHOUT_BACKEND();
    AssetStore store(m_root);
    HttpClient client;
    const auto descriptor = two_file_descriptor();
    const std::uint64_t expected_total = 8 * 1024 + 32 * 1024;

    std::uint64_t last_done = 0;
    std::uint64_t seen_total = 0;
    std::size_t seen_count = 0;

    const auto result = store.install(descriptor, client, [&](const AssetProgress& p) {
        EXPECT_GE(p.m_bytes_done, last_done) << "progress must not go backwards";
        last_done = p.m_bytes_done;
        seen_total = p.m_bytes_total;
        seen_count = p.m_file_count;
        return true;
    });

    ASSERT_TRUE(result.ok()) << result.m_message;
    EXPECT_EQ(seen_total, expected_total);
    EXPECT_EQ(seen_count, 2u);
    EXPECT_EQ(last_done, expected_total);
}

TEST_F(AssetStoreTest, ProgressCallbackCanCancelAndNothingIsMarked) {
    SKIP_WITHOUT_BACKEND();
    AssetStore store(m_root);
    HttpClient client;
    const auto descriptor = two_file_descriptor();

    const auto result =
        store.install(descriptor, client, [](const AssetProgress&) { return false; });

    EXPECT_EQ(result.m_status, InstallStatus::Cancelled);
    EXPECT_FALSE(store.is_installed("handpan", "0.0.1"));
}

// Ids and versions become directory names and arrive from a server, so a
// traversal attempt must be refused rather than sanitised.
TEST_F(AssetStoreTest, RefusesUnsafeIdsVersionsAndFileNames) {
    AssetStore store(m_root);
    HttpClient client;

    const auto attempt =
        [&](const std::string& id, const std::string& version, const std::string& name) {
            AssetDescriptor descriptor;
            descriptor.m_id = id;
            descriptor.m_version = version;
            AssetFile file;
            file.m_url = "http://127.0.0.1/x";
            file.m_name = name;
            descriptor.m_files = {file};
            return store.install(descriptor, client).m_status;
        };

    EXPECT_EQ(attempt("..", "0.0.1", "a.bin"), InstallStatus::InvalidDescriptor);
    EXPECT_EQ(attempt("a/b", "0.0.1", "a.bin"), InstallStatus::InvalidDescriptor);
    EXPECT_EQ(attempt("pack", "../../etc", "a.bin"), InstallStatus::InvalidDescriptor);
    EXPECT_EQ(attempt("pack", "0.0.1", "../escape.bin"), InstallStatus::InvalidDescriptor);
    EXPECT_EQ(attempt("pack", "0.0.1", "/etc/passwd"), InstallStatus::InvalidDescriptor);
    EXPECT_EQ(attempt("", "0.0.1", "a.bin"), InstallStatus::InvalidDescriptor);

    // Nested names are legitimate as long as they stay inside.
    EXPECT_NE(attempt("pack", "0.0.1", "sub/a.bin"), InstallStatus::InvalidDescriptor);
}

TEST_F(AssetStoreTest, EmptyFileListIsRejected) {
    AssetStore store(m_root);
    HttpClient client;
    AssetDescriptor descriptor;
    descriptor.m_id = "pack";
    descriptor.m_version = "0.0.1";
    EXPECT_EQ(store.install(descriptor, client).m_status, InstallStatus::InvalidDescriptor);
}

TEST_F(AssetStoreTest, VersionsCoexistAndRemoveIndependently) {
    SKIP_WITHOUT_BACKEND();
    AssetStore store(m_root);
    HttpClient client;

    auto first = two_file_descriptor();
    auto second = first;
    second.m_version = "0.0.2";

    ASSERT_TRUE(store.install(first, client).ok());
    ASSERT_TRUE(store.install(second, client).ok());

    EXPECT_EQ(store.installed().size(), 2u);

    EXPECT_TRUE(store.remove("handpan", "0.0.1"));
    EXPECT_FALSE(store.is_installed("handpan", "0.0.1"));
    EXPECT_TRUE(store.is_installed("handpan", "0.0.2")) << "removing one version keeps the other";

    EXPECT_FALSE(store.remove("handpan", "0.0.1")) << "removing twice reports false";
}

// A directory left behind by a crash has files but no marker. It must never be
// mistaken for an install, and prune_partial must clear it.
TEST_F(AssetStoreTest, PartialDirectoriesAreNotInstalledAndArePruned) {
    AssetStore store(m_root);

    const auto directory = store.path_for("ghost", "0.0.1");
    fs::create_directories(directory);
    std::ofstream(directory / "encode.onnx", std::ios::binary) << "partial";

    EXPECT_FALSE(store.is_installed("ghost", "0.0.1"));
    EXPECT_TRUE(store.installed().empty());

    EXPECT_EQ(store.prune_partial(), 1u);
    EXPECT_FALSE(fs::exists(directory));
}

TEST_F(AssetStoreTest, PruneKeepsCompleteInstalls) {
    SKIP_WITHOUT_BACKEND();
    AssetStore store(m_root);
    HttpClient client;
    ASSERT_TRUE(store.install(two_file_descriptor(), client).ok());

    const auto partial = store.path_for("handpan", "0.0.9");
    fs::create_directories(partial);

    EXPECT_EQ(store.prune_partial(), 1u);
    EXPECT_TRUE(store.is_installed("handpan", "0.0.1")) << "a complete install must survive";
}

TEST_F(AssetStoreTest, PathForRejectsUnsafeSegments) {
    AssetStore store(m_root);
    EXPECT_TRUE(store.path_for("..", "0.0.1").empty());
    EXPECT_TRUE(store.path_for("pack", "a/b").empty());
    EXPECT_FALSE(store.path_for("pack", "0.0.1").empty());
}
