#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace thl::net::test {

/// A tiny HTTP/1.1 server for the transfer tests.
///
/// The point is not to avoid the network — it is that the failures worth testing
/// cannot be produced on demand against a real bucket. Truncated bodies, a
/// server that ignores Range, a 500 halfway through, a connection dropped
/// mid-body: all trivial here, all impossible to arrange against R2. A real
/// endpoint can only ever demonstrate the happy path, and would make the suite
/// depend on the network and on nobody re-publishing an asset.
///
/// Deliberately minimal: one connection at a time, no keep-alive, POSIX sockets
/// only. TLS is out of scope — it belongs to the platform backend, and is
/// covered by the opt-in DISABLED_ live test instead.
class TestHttpServer {
public:
    /// How a route should misbehave.
    enum class Behaviour {
        Normal,
        /// Serve half the body with an honest Content-Length. The transfer is a
        /// well-formed 200 that no HTTP client can fault — only the caller's
        /// expected size or digest catches it. This is what a wrong file or a
        /// proxy error page looks like.
        ShortBody,
        /// Declare the full Content-Length and then close early. A correct
        /// client detects this itself, so it exercises the transport, not the
        /// verification.
        TruncateDeclared,
        /// Trickle the body so a transfer stays in flight long enough to cancel.
        Slow,
        /// Answer a Range request with 200 and the whole body.
        IgnoreRange,
        /// Reply with the configured status and no body.
        StatusOnly,
        /// Accept the connection and close it without writing anything.
        DropConnection,
    };

    struct Route {
        std::string m_body;
        Behaviour m_behaviour = Behaviour::Normal;
        int m_status = 200;
    };

    TestHttpServer() = default;

    ~TestHttpServer() { stop(); }

    TestHttpServer(const TestHttpServer&) = delete;
    TestHttpServer& operator=(const TestHttpServer&) = delete;

    /// Serve `body` at `path`. Overwrites any existing route.
    void add_route(const std::string& path, Route route) {
        const std::lock_guard lock(m_mutex);
        m_routes[path] = std::move(route);
    }

    /// Bind to an ephemeral port on the loopback interface and start serving.
    /// Returns false when the socket could not be set up.
    bool start() {
        // Cancelling a transfer closes the client socket mid-body, so the next
        // send() here raises SIGPIPE and would take the whole test binary down
        // with it. The tests cannot exercise cancellation without this.
        std::signal(SIGPIPE, SIG_IGN);

        const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener < 0) { return false; }
        m_listen_fd.store(listener, std::memory_order_release);

        int reuse = 1;
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;  // let the OS choose, so parallel tests never collide

        if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            stop();
            return false;
        }
        if (::listen(listener, 8) < 0) {
            stop();
            return false;
        }

        sockaddr_in bound{};
        socklen_t bound_size = sizeof(bound);
        if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_size) < 0) {
            stop();
            return false;
        }
        m_port = ntohs(bound.sin_port);

        m_running = true;
        m_thread = std::thread([this] { serve(); });
        return true;
    }

    void stop() {
        m_running = false;
        // exchange so a second stop() (the destructor after an explicit call)
        // cannot close the same descriptor twice.
        const int listener = m_listen_fd.exchange(-1, std::memory_order_acq_rel);
        if (listener >= 0) {
            ::shutdown(listener, SHUT_RDWR);
            ::close(listener);
        }
        if (m_thread.joinable()) { m_thread.join(); }
    }

    [[nodiscard]] std::uint16_t port() const { return m_port; }

    [[nodiscard]] std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(m_port) + path;
    }

    /// How many requests arrived for a path — lets a test prove a file was not
    /// fetched twice.
    [[nodiscard]] int request_count(const std::string& path) const {
        const std::lock_guard lock(m_mutex);
        const auto it = m_request_counts.find(path);
        return it == m_request_counts.end() ? 0 : it->second;
    }

    /// The Range header of the last request for a path, empty when absent.
    [[nodiscard]] std::string last_range(const std::string& path) const {
        const std::lock_guard lock(m_mutex);
        const auto it = m_last_range.find(path);
        return it == m_last_range.end() ? std::string{} : it->second;
    }

private:
    void serve() {
        while (m_running) {
            const int listener = m_listen_fd.load(std::memory_order_acquire);
            if (listener < 0) { break; }
            // stop() may close the socket between this load and the accept; the
            // call then fails with EBADF and the loop exits on m_running.
            const int client = ::accept(listener, nullptr, nullptr);
            if (client < 0) { continue; }
#ifdef SO_NOSIGPIPE
            int nosigpipe = 1;
            ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif
            handle(client);
            ::close(client);
        }
    }

    static std::string read_request(int client) {
        std::string request;
        char buffer[2048];
        while (request.find("\r\n\r\n") == std::string::npos) {
            const auto read = ::recv(client, buffer, sizeof(buffer), 0);
            if (read <= 0) { break; }
            request.append(buffer, static_cast<std::size_t>(read));
            if (request.size() > 64 * 1024) { break; }
        }
        return request;
    }

    static std::string header_value(const std::string& request, const std::string& name) {
        const auto start = request.find(name + ":");
        if (start == std::string::npos) { return {}; }
        const auto value_start = request.find_first_not_of(" ", start + name.size() + 1);
        const auto end = request.find("\r\n", value_start);
        if (value_start == std::string::npos || end == std::string::npos) { return {}; }
        return request.substr(value_start, end - value_start);
    }

    static void send_all(int client, const std::string& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const auto wrote = ::send(client, data.data() + sent, data.size() - sent, 0);
            if (wrote <= 0) { return; }
            sent += static_cast<std::size_t>(wrote);
        }
    }

    void handle(int client) {
        const auto request = read_request(client);
        if (request.empty()) { return; }

        const auto path_start = request.find(' ');
        const auto path_end = request.find(' ', path_start + 1);
        if (path_start == std::string::npos || path_end == std::string::npos) { return; }
        const auto path = request.substr(path_start + 1, path_end - path_start - 1);
        const auto range = header_value(request, "Range");

        Route route;
        bool found = false;
        {
            const std::lock_guard lock(m_mutex);
            ++m_request_counts[path];
            m_last_range[path] = range;
            const auto it = m_routes.find(path);
            if (it != m_routes.end()) {
                route = it->second;
                found = true;
            }
        }

        if (!found) {
            send_all(client,
                     "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            return;
        }

        if (route.m_behaviour == Behaviour::DropConnection) { return; }

        if (route.m_behaviour == Behaviour::StatusOnly) {
            send_all(client,
                     "HTTP/1.1 " + std::to_string(route.m_status) +
                         " Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            return;
        }

        // Honour Range unless the route is set up to ignore it.
        std::size_t offset = 0;
        const bool ranged = !range.empty() && route.m_behaviour != Behaviour::IgnoreRange;
        if (ranged) {
            const auto equals = range.find('=');
            const auto dash = range.find('-');
            if (equals != std::string::npos && dash != std::string::npos) {
                offset = static_cast<std::size_t>(
                    std::stoull(range.substr(equals + 1, dash - equals - 1)));
            }
            if (offset > route.m_body.size()) { offset = route.m_body.size(); }
        }

        std::string payload = route.m_body.substr(offset);

        // ShortBody keeps Content-Length honest about what it sends, so the
        // response is valid HTTP and only the caller's expectations catch it.
        if (route.m_behaviour == Behaviour::ShortBody) { payload.resize(payload.size() / 2); }

        const std::size_t declared = payload.size();
        const std::size_t actual =
            route.m_behaviour == Behaviour::TruncateDeclared ? declared / 2 : declared;

        std::string head = ranged ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
        head += "Content-Length: " + std::to_string(declared) + "\r\n";
        if (ranged) {
            head += "Content-Range: bytes " + std::to_string(offset) + "-" +
                    std::to_string(route.m_body.size() - 1) + "/" +
                    std::to_string(route.m_body.size()) + "\r\n";
        }
        head += "Connection: close\r\n\r\n";

        send_all(client, head);

        if (route.m_behaviour == Behaviour::Slow) {
            // Small writes with a pause between them: enough wall-clock for a
            // cancel from another thread to land mid-transfer.
            constexpr std::size_t k_chunk = 4096;
            for (std::size_t sent = 0; sent < actual; sent += k_chunk) {
                send_all(client, payload.substr(sent, std::min(k_chunk, actual - sent)));
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return;
        }

        send_all(client, payload.substr(0, actual));
    }

    mutable std::mutex m_mutex;
    std::map<std::string, Route> m_routes;
    std::map<std::string, int> m_request_counts;
    std::map<std::string, std::string> m_last_range;

    /// Read by the serve thread and cleared by stop() on another: atomic, or
    /// TSan flags the accept/close pair (and rightly — it is a real race).
    std::atomic<int> m_listen_fd{-1};
    std::uint16_t m_port = 0;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
};

}  // namespace thl::net::test
