// SPDX-License-Identifier: Apache-2.0
#pragma once

// One core library seen from the test: probe.cpp is compiled twice, once against tanh_core
// (namespace probe_main) and once against tanh_core_b (namespace probe_b), each translation
// unit with its own library's headers. No tanh-lib type crosses this interface, so the test
// itself includes neither library's headers.

namespace probe_main {
const char* abi_namespace();           ///< THL_ABI_NAMESPACE of the library, as a string.
void set_level(int level);             ///< thl::Logger::set_level(), 1 Error .. 4 Debug.
int level();                           ///< thl::Logger::get_level().
void set_platform_sink(bool enabled);  ///< thl::Logger::set_config(), file and rt sinks off.
bool platform_sink_enabled();          ///< thl::Logger::get_config().m_platform_enabled.
void count_records();                  ///< thl::Logger::set_callback() with a counting callback.
void clear_callback();                 ///< thl::Logger::clear_callback().
int records();                         ///< Records the callback counted so far.
void log_error(const char* message);   ///< thl::Logger::error().
}  // namespace probe_main

namespace probe_b {
const char* abi_namespace();
void set_level(int level);
int level();
void set_platform_sink(bool enabled);
bool platform_sink_enabled();
void count_records();
void clear_callback();
int records();
void log_error(const char* message);
}  // namespace probe_b
