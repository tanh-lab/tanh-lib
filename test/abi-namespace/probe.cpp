// SPDX-License-Identifier: Apache-2.0
// Compiled once per core library; ABI_PROBE_NAMESPACE (probe_main or probe_b) comes from
// CMake, the library's headers and THL_ABI_NAMESPACE from the core target it links.
#include "probe.h"

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Logger.h>

#include <atomic>

#define ABI_PROBE_STRINGIFY_IMPL(x) #x
#define ABI_PROBE_STRINGIFY(x) ABI_PROBE_STRINGIFY_IMPL(x)

namespace ABI_PROBE_NAMESPACE {

namespace {
std::atomic<int> records_seen{0};
}  // namespace

const char* abi_namespace() {
    return ABI_PROBE_STRINGIFY(THL_ABI_NAMESPACE);
}

void set_level(int level) {
    thl::Logger::set_level(static_cast<thl::Logger::LogLevel>(level));
}

int level() {
    return static_cast<int>(thl::Logger::get_level());
}

void set_platform_sink(bool enabled) {
    thl::Logger::LoggerConfig config;
    config.m_platform_enabled = enabled;
    config.m_file_enabled = false;
    config.m_rt_enabled = false;
    thl::Logger::set_config(config);
}

bool platform_sink_enabled() {
    return thl::Logger::get_config().m_platform_enabled;
}

void count_records() {
    thl::Logger::set_callback([](const thl::Logger::LogRecord&) { ++records_seen; });
}

void clear_callback() {
    thl::Logger::clear_callback();
}

int records() {
    return records_seen.load();
}

void log_error(const char* message) {
    thl::Logger::error("abi-namespace", message);
}

}  // namespace ABI_PROBE_NAMESPACE
