// SPDX-License-Identifier: Apache-2.0
//
// The priority mapping and the SCHED_FIFO / nice fallback were ported from anira's
// HighPriorityThread (anira-project/anira, Apache-2.0).

#include <tanh/core/Logger.h>
#include <tanh/core/threading/Thread.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#endif

namespace thl::core {

namespace {

constexpr const char* k_group = "thl.thread";

#if defined(_WIN32)

bool apply_priority(ThreadPriority priority) noexcept {
    if (priority == ThreadPriority::Normal) { return true; }
    if (priority == ThreadPriority::Background) {
        if (SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != 0) {
            return true;
        }
        THL_LOG_WARNING(k_group, "background mode refused, error %lu", GetLastError());
        return false;
    }
    int native = THREAD_PRIORITY_NORMAL;
    switch (priority) {
        case ThreadPriority::Low: native = THREAD_PRIORITY_BELOW_NORMAL; break;
        case ThreadPriority::High: native = THREAD_PRIORITY_HIGHEST; break;
        case ThreadPriority::RealTime: native = THREAD_PRIORITY_TIME_CRITICAL; break;
        default: break;
    }
    if (SetThreadPriority(GetCurrentThread(), native) != 0) { return true; }
    THL_LOG_WARNING(k_group, "thread priority %d refused, error %lu", native, GetLastError());
    if (priority == ThreadPriority::RealTime) {
        // Degrade one step rather than run at normal priority.
        if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != 0) { return false; }
    }
    return false;
}

bool apply_name(const char* name) noexcept {
    // SetThreadDescription needs a wide string; names are ASCII in practice.
    wchar_t wide[64] = {};
    std::size_t i = 0;
    for (; i < 63 && name[i] != '\0'; ++i) { wide[i] = static_cast<wchar_t>(name[i]); }
    return SUCCEEDED(SetThreadDescription(GetCurrentThread(), wide));
}

#elif defined(__APPLE__)

bool apply_priority(ThreadPriority priority) noexcept {
    if (priority == ThreadPriority::Normal) { return true; }
    qos_class_t qos = QOS_CLASS_DEFAULT;
    switch (priority) {
        case ThreadPriority::Background: qos = QOS_CLASS_BACKGROUND; break;
        case ThreadPriority::Low: qos = QOS_CLASS_UTILITY; break;
        case ThreadPriority::High: qos = QOS_CLASS_USER_INITIATED; break;
        case ThreadPriority::RealTime: qos = QOS_CLASS_USER_INTERACTIVE; break;
        default: break;
    }
    const int ret = pthread_set_qos_class_self_np(qos, 0);
    if (ret == 0) { return true; }
    THL_LOG_WARNING(k_group, "QoS class %u refused, error %d", static_cast<unsigned>(qos), ret);
    return false;
}

bool apply_name(const char* name) noexcept {
    return pthread_setname_np(name) == 0;
}

#elif defined(__linux__)

bool set_nice(int value) noexcept {
    // On Linux, who == 0 with PRIO_PROCESS addresses the calling thread.
    if (setpriority(PRIO_PROCESS, 0, value) == 0) { return true; }
    THL_LOG_WARNING(k_group, "nice %d refused, errno %d", value, errno);
    return false;
}

bool apply_priority(ThreadPriority priority) noexcept {
    switch (priority) {
        case ThreadPriority::Normal: return true;
        case ThreadPriority::Low: return set_nice(10);
        case ThreadPriority::High: return set_nice(-5);
        case ThreadPriority::Background: {
            sched_param params{};
            if (sched_setscheduler(0, SCHED_IDLE, &params) == 0) { return true; }
            THL_LOG_WARNING(k_group, "SCHED_IDLE refused, errno %d", errno);
            set_nice(19);
            return false;
        }
        case ThreadPriority::RealTime: {
            sched_param params{};
            // PipeWire uses SCHED_FIFO 60 and the JUCE plugin host 55: stay below both.
            params.sched_priority = 50;
            if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &params) == 0) { return true; }
            THL_LOG_WARNING(k_group,
                            "SCHED_FIFO %d refused (errno %d): give the user rtprio rights "
                            "(realtime/audio group) — falling back to nice -10",
                            params.sched_priority,
                            errno);
            set_nice(-10);
            return false;
        }
    }
    return false;
}

bool apply_name(const char* name) noexcept {
    char truncated[16] = {};
    std::strncpy(truncated, name, sizeof(truncated) - 1);
    return pthread_setname_np(pthread_self(), truncated) == 0;
}

#else  // Emscripten and anything else: no scheduling control.

bool apply_priority(ThreadPriority) noexcept {
    return false;
}
bool apply_name(const char*) noexcept {
    return false;
}

#endif

}  // namespace

struct Thread::Impl {
    std::thread m_thread;
    std::atomic<bool> m_should_stop{false};
    std::atomic<bool> m_is_running{false};
};

Thread::Thread() : m_impl(std::make_unique<Impl>()) {}

Thread::~Thread() {
    request_stop();
    join();
}

bool Thread::start(const ThreadOptions& options, Body body) {
    if (m_impl->m_thread.joinable()) { return false; }
    m_impl->m_should_stop.store(false, std::memory_order_relaxed);
    m_impl->m_is_running.store(true, std::memory_order_release);
    try {
        const std::string name = options.m_name != nullptr ? options.m_name : "";
        const ThreadPriority priority = options.m_priority;
        m_impl->m_thread = std::thread([this, name, priority, body = std::move(body)]() {
            if (!name.empty()) { apply_name(name.c_str()); }
            apply_priority(priority);
            body(*this);
            m_impl->m_is_running.store(false, std::memory_order_release);
        });
    } catch (const std::exception& e) {
        m_impl->m_is_running.store(false, std::memory_order_release);
        THL_LOG_ERROR(k_group, "could not create thread: %s", e.what());
        return false;
    }
    return true;
}

void Thread::request_stop() noexcept TANH_NONBLOCKING_FUNCTION {
    m_impl->m_should_stop.store(true, std::memory_order_release);
}

void Thread::join() {
    if (m_impl->m_thread.joinable()) { m_impl->m_thread.join(); }
}

bool Thread::should_stop() const noexcept TANH_NONBLOCKING_FUNCTION {
    return m_impl->m_should_stop.load(std::memory_order_acquire);
}

bool Thread::is_running() const noexcept TANH_NONBLOCKING_FUNCTION {
    return m_impl->m_is_running.load(std::memory_order_acquire);
}

bool Thread::joinable() const noexcept {
    return m_impl->m_thread.joinable();
}

bool Thread::set_current_priority(ThreadPriority priority) noexcept {
    return apply_priority(priority);
}

bool Thread::set_current_name(const char* name) noexcept {
    return name != nullptr && apply_name(name);
}

}  // namespace thl::core
