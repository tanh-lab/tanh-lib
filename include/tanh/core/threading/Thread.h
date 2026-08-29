// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <functional>
#include <memory>

#include "tanh/core/Exports.h"
#include "tanh/utils/RealtimeSanitizer.h"

namespace thl::core {

/// Scheduling class requested for a thread. A priority is a request, never a
/// requirement: the platform may deny it (e.g. SCHED_FIFO without rtprio rights),
/// in which case the thread runs at the closest class that was granted and the
/// failure is logged once under the `thl.thread` group.
///
/// | | Windows | Linux / Android | macOS / iOS |
/// |---|---|---|---|
/// | Background | THREAD_MODE_BACKGROUND | SCHED_IDLE | QOS_CLASS_BACKGROUND |
/// | Low | BELOW_NORMAL | nice +10 | QOS_CLASS_UTILITY |
/// | Normal | (unchanged) | (unchanged) | (unchanged) |
/// | High | HIGHEST | nice -5 | QOS_CLASS_USER_INITIATED |
/// | RealTime | TIME_CRITICAL | SCHED_FIFO 50, else nice -10 | QOS_CLASS_USER_INTERACTIVE |
enum class ThreadPriority { Background, Low, Normal, High, RealTime };

/// Options for Thread::start().
struct ThreadOptions {
    ThreadPriority m_priority = ThreadPriority::Normal;
    /// Thread name as shown by debuggers and profilers (truncated to the platform
    /// limit, 15 characters on Linux). nullptr leaves the default.
    const char* m_name = nullptr;
};

/// @brief A joinable OS thread with a requested scheduling class and a stop flag.
///
/// Composition, not inheritance: the body is a callable that polls
/// `should_stop()` and returns. `start()` applies the priority and name on the
/// new thread before running the body; the destructor requests a stop and
/// joins. Not real-time safe except where noted.
///
/// ```cpp
/// thl::core::Thread worker;
/// worker.start({ThreadPriority::Low, "drain"}, [](const thl::core::Thread& self) {
///     while (!self.should_stop()) { work(); }
/// });
/// ```
class TANH_API Thread {
public:
    using Body = std::function<void(const Thread&)>;

    Thread();
    ~Thread();  ///< request_stop() + join().

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    Thread(Thread&&) = delete;
    Thread& operator=(Thread&&) = delete;

    /// Start the thread. Returns false (and does nothing) if it is already
    /// running or the OS refused to create a thread.
    bool start(const ThreadOptions& options, Body body);

    /// Ask the body to return. Real-time safe (one atomic store).
    void request_stop() noexcept TANH_NONBLOCKING_FUNCTION;

    /// Wait for the body to return. Safe to call when not running.
    void join();

    /// Give up ownership of the OS thread without waiting for it. For the one
    /// case join() cannot serve: tearing the Thread down from its own body.
    void detach();

    /// True after request_stop() (or ~Thread). Real-time safe.
    [[nodiscard]] bool should_stop() const noexcept TANH_NONBLOCKING_FUNCTION;

    /// True from start() until the body has returned. Real-time safe.
    [[nodiscard]] bool is_running() const noexcept TANH_NONBLOCKING_FUNCTION;

    /// True while the OS thread exists (started and not yet joined).
    [[nodiscard]] bool joinable() const noexcept;

    /// Apply a scheduling class to the calling thread (one we did not create:
    /// a host's audio thread, a benchmark's main thread). Returns whether the
    /// request was granted as asked.
    static bool set_current_priority(ThreadPriority priority) noexcept;

    /// Name the calling thread for debuggers and profilers.
    static bool set_current_name(const char* name) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace thl::core
