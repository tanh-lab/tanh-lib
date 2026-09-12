#include <gtest/gtest.h>
#include <tanh/net/HttpClient.h>
#include <tanh/net/Sha256.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "TestHttpServer.h"

using namespace thl::net;
using thl::net::test::TestHttpServer;
namespace fs = std::filesystem;

namespace {

class HttpClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(m_server.start());
        m_dir = fs::temp_directory_path() /
                ("tanh-http-" +
                 std::to_string(::testing::UnitTest::GetInstance()->current_test_info()->line()));
        fs::remove_all(m_dir);
        fs::create_directories(m_dir);
    }

    void TearDown() override {
        m_server.stop();
        fs::remove_all(m_dir);
    }

    static std::string read_file(const fs::path& path) {
        std::ifstream stream(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }

    static void write_file(const fs::path& path, const std::string& contents) {
        std::ofstream stream(path, std::ios::binary);
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    TestHttpServer m_server;
    fs::path m_dir;
};

// Every test below needs a real backend; on a platform without one they would
// all fail identically and say nothing useful.
#define SKIP_WITHOUT_BACKEND() \
    if (!HttpClient::supported()) { GTEST_SKIP() << "No HTTP backend on this platform."; }

}  // namespace

TEST_F(HttpClientTest, FetchesABodyToFile) {
    SKIP_WITHOUT_BACKEND();
    const std::string body(100 * 1024, 'a');
    m_server.add_route("/asset.bin", {body});

    HttpClient client;
    const auto destination = m_dir / "asset.bin";
    const auto result = client.get_to_file(m_server.url("/asset.bin"), destination);

    ASSERT_TRUE(result.ok()) << result.m_message;
    EXPECT_EQ(read_file(destination), body);
}

TEST_F(HttpClientTest, ReportsProgressWithATotal) {
    SKIP_WITHOUT_BACKEND();
    const std::string body(64 * 1024, 'b');
    m_server.add_route("/p.bin", {body});

    std::uint64_t last_downloaded = 0;
    std::uint64_t seen_total = 0;
    int calls = 0;

    HttpClient client;
    const auto result =
        client.get_to_file(m_server.url("/p.bin"), m_dir / "p.bin", [&](const HttpProgress& p) {
            ++calls;
            EXPECT_GE(p.m_downloaded, last_downloaded) << "progress must not go backwards";
            last_downloaded = p.m_downloaded;
            seen_total = p.m_total;
            return true;
        });

    ASSERT_TRUE(result.ok()) << result.m_message;
    EXPECT_GT(calls, 0);
    EXPECT_EQ(last_downloaded, body.size());
    EXPECT_EQ(seen_total, body.size());
}

TEST_F(HttpClientTest, ResumesFromAPartialFile) {
    SKIP_WITHOUT_BACKEND();
    const std::string body(50 * 1024, 'c');
    m_server.add_route("/resume.bin", {body});

    // Pretend an earlier attempt stopped a third of the way in.
    const auto destination = m_dir / "resume.bin";
    const std::string prefix = body.substr(0, body.size() / 3);
    write_file(destination, prefix);

    HttpClient client;
    const auto result = client.get_to_file(m_server.url("/resume.bin"), destination);

    ASSERT_TRUE(result.ok()) << result.m_message;
    EXPECT_EQ(read_file(destination), body) << "resumed file must equal the whole body";
    EXPECT_EQ(m_server.last_range("/resume.bin"), "bytes=" + std::to_string(prefix.size()) + "-");
    EXPECT_EQ(result.m_bytes_written, body.size() - prefix.size())
        << "only the remainder should travel";
}

// A server may answer a Range request with the whole body. Appending then would
// silently corrupt the file, so the client must start over instead.
TEST_F(HttpClientTest, RestartsWhenTheServerIgnoresRange) {
    SKIP_WITHOUT_BACKEND();
    const std::string body(30 * 1024, 'd');
    m_server.add_route("/norange.bin", {body, TestHttpServer::Behaviour::IgnoreRange});

    const auto destination = m_dir / "norange.bin";
    write_file(destination, body.substr(0, 4096));

    HttpClient client;
    const auto result = client.get_to_file(m_server.url("/norange.bin"), destination);

    ASSERT_TRUE(result.ok()) << result.m_message;
    EXPECT_EQ(read_file(destination), body) << "must not be prefix + whole body";
}

TEST_F(HttpClientTest, ResumingACompleteFileIsHarmless) {
    SKIP_WITHOUT_BACKEND();
    const std::string body(8 * 1024, 'e');
    m_server.add_route("/done.bin", {body});

    const auto destination = m_dir / "done.bin";
    write_file(destination, body);

    HttpClient client;
    const auto result = client.get_to_file(m_server.url("/done.bin"), destination);

    // The server answers the past-the-end range with an empty 206; either way
    // the file must still be exactly the body.
    EXPECT_EQ(read_file(destination), body);
    EXPECT_TRUE(result.ok() || result.m_status == HttpStatus::HttpError) << result.m_message;
}

TEST_F(HttpClientTest, ReportsHttpErrors) {
    SKIP_WITHOUT_BACKEND();
    m_server.add_route("/gone", {"", TestHttpServer::Behaviour::StatusOnly, 404});

    HttpClient client;
    const auto result = client.get_to_file(m_server.url("/gone"), m_dir / "gone");

    EXPECT_EQ(result.m_status, HttpStatus::HttpError);
    EXPECT_EQ(result.m_http_code, 404);
}

TEST_F(HttpClientTest, ReportsAnUnroutedPathAs404) {
    SKIP_WITHOUT_BACKEND();
    HttpClient client;
    const auto result = client.get_to_file(m_server.url("/nothing-here"), m_dir / "x");
    EXPECT_EQ(result.m_status, HttpStatus::HttpError);
    EXPECT_EQ(result.m_http_code, 404);
}

// A well-formed response whose body is simply wrong — the shape a proxy error
// page or a mis-published file takes. Nothing at the HTTP layer can fault it,
// which is exactly why AssetStore verifies sizes and digests itself.
TEST_F(HttpClientTest, AShortButWellFormedBodySucceedsAtTheHttpLayer) {
    SKIP_WITHOUT_BACKEND();
    const std::string body(20 * 1024, 'f');
    m_server.add_route("/short.bin", {body, TestHttpServer::Behaviour::ShortBody});

    HttpClient client;
    const auto destination = m_dir / "short.bin";
    const auto result = client.get_to_file(m_server.url("/short.bin"), destination);

    EXPECT_TRUE(result.ok()) << "a consistent Content-Length is valid HTTP";
    EXPECT_LT(read_file(destination).size(), body.size());
}

// The opposite case: Content-Length promises more than arrives. The transport
// detects this on its own, so it must be reported as a failure rather than
// handed on as a complete file.
TEST_F(HttpClientTest, ADeclaredLengthThatIsNotDeliveredFails) {
    SKIP_WITHOUT_BACKEND();
    const std::string body(20 * 1024, 'g');
    m_server.add_route("/cut.bin", {body, TestHttpServer::Behaviour::TruncateDeclared});

    HttpClient client;
    const auto result = client.get_to_file(m_server.url("/cut.bin"), m_dir / "cut.bin");

    EXPECT_FALSE(result.ok());
}

TEST_F(HttpClientTest, NetworkFailureIsReported) {
    SKIP_WITHOUT_BACKEND();
    m_server.add_route("/drop", {"", TestHttpServer::Behaviour::DropConnection});

    HttpClient client;
    const auto result = client.get_to_file(m_server.url("/drop"), m_dir / "drop");

    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.m_status, HttpStatus::Ok);
}

TEST_F(HttpClientTest, ProgressCallbackCanCancel) {
    SKIP_WITHOUT_BACKEND();
    const std::string body(2 * 1024 * 1024, 'g');
    m_server.add_route("/big.bin", {body, TestHttpServer::Behaviour::Slow});

    HttpClient client;
    const auto result = client.get_to_file(m_server.url("/big.bin"),
                                           m_dir / "big.bin",
                                           [](const HttpProgress&) { return false; });

    EXPECT_EQ(result.m_status, HttpStatus::Cancelled);
}

TEST_F(HttpClientTest, CancelFromAnotherThreadStopsTheTransfer) {
    SKIP_WITHOUT_BACKEND();
    // Trickled by the server: over loopback a plain body finishes long before a
    // 20 ms sleep, so the cancel would land after the transfer and prove nothing.
    const std::string body(4 * 1024 * 1024, 'h');
    m_server.add_route("/cancel.bin", {body, TestHttpServer::Behaviour::Slow});

    HttpClient client;
    std::thread canceller([&client] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        client.cancel();
    });

    const auto result = client.get_to_file(m_server.url("/cancel.bin"), m_dir / "cancel.bin");
    canceller.join();

    EXPECT_EQ(result.m_status, HttpStatus::Cancelled);
    EXPECT_TRUE(client.cancelled());

    client.reset();
    EXPECT_FALSE(client.cancelled());
}

TEST_F(HttpClientTest, FetchesSmallBodiesIntoAString) {
    SKIP_WITHOUT_BACKEND();
    const std::string manifest = R"({"schema_version":1})";
    m_server.add_route("/manifest.json", {manifest});

    HttpClient client;
    std::string body;
    const auto result = client.get_to_string(m_server.url("/manifest.json"), body);

    ASSERT_TRUE(result.ok()) << result.m_message;
    EXPECT_EQ(body, manifest);
}

TEST_F(HttpClientTest, RefusesABodyOverTheLimit) {
    SKIP_WITHOUT_BACKEND();
    m_server.add_route("/large.json", {std::string(64 * 1024, 'j')});

    HttpClient client;
    std::string body;
    const auto result = client.get_to_string(m_server.url("/large.json"), body, 1024);

    EXPECT_FALSE(result.ok()) << "an oversized body must not be allocated";
}

TEST_F(HttpClientTest, RejectsNonHttpUrls) {
    HttpClient client;
    const auto result = client.get_to_file("ftp://example.invalid/x", m_dir / "x");
    EXPECT_EQ(result.m_status, HttpStatus::NetworkError);
    EXPECT_EQ(result.m_bytes_written, 0u);
}

// Opt in with --gtest_also_run_disabled_tests. This is the only test that
// touches the real service, and the only one that exercises TLS — the local
// server is plain HTTP by design. Keep it out of the default suite so the
// results never depend on the network or on what is published.
TEST_F(HttpClientTest, DISABLED_LiveManifestOverTls) {
    SKIP_WITHOUT_BACKEND();
    HttpClient client;
    std::string body;
    const auto result =
        client.get_to_string("https://pub-c5816cb666e54b7081b801ecb7b71ff7.r2.dev/manifest.json",
                             body);

    ASSERT_TRUE(result.ok()) << result.m_message;
    EXPECT_NE(body.find("\"schema_version\""), std::string::npos);
}
