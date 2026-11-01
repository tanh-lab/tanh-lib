// SPDX-License-Identifier: Apache-2.0
// Two core libraries in one executable: tanh_core and tanh_core_b (cmake/core-library.cmake,
// namespace thl::test_b), linked side by side, as a host's tanh-lib and an embedder's private
// copy are. Each probe reaches one of them; their loggers must not share any state.
#include <gtest/gtest.h>

#include <string>

#include "probe.h"

namespace {

constexpr int k_error = 1;
constexpr int k_warning = 2;
constexpr int k_debug = 4;

TEST(AbiNamespace, EachLibraryDeclaresInItsOwnNamespace) {
    EXPECT_EQ(std::string(probe_main::abi_namespace()), ABI_EXPECTED_MAIN_NAMESPACE);
    EXPECT_EQ(std::string(probe_b::abi_namespace()), ABI_EXPECTED_B_NAMESPACE);
    EXPECT_NE(std::string(probe_main::abi_namespace()), std::string(probe_b::abi_namespace()));
}

TEST(AbiNamespace, LevelsAreSeparate) {
    probe_main::set_level(k_error);
    probe_b::set_level(k_debug);
    EXPECT_EQ(probe_main::level(), k_error);
    EXPECT_EQ(probe_b::level(), k_debug);

    probe_b::set_level(k_warning);
    EXPECT_EQ(probe_main::level(), k_error);
    EXPECT_EQ(probe_b::level(), k_warning);

    probe_main::set_level(k_debug);
    EXPECT_EQ(probe_b::level(), k_warning);
}

TEST(AbiNamespace, ConfigsAreSeparate) {
    probe_b::set_platform_sink(true);
    probe_main::set_platform_sink(false);
    EXPECT_FALSE(probe_main::platform_sink_enabled());
    EXPECT_TRUE(probe_b::platform_sink_enabled());

    probe_b::set_platform_sink(false);
    probe_main::set_platform_sink(true);
    EXPECT_TRUE(probe_main::platform_sink_enabled());
    EXPECT_FALSE(probe_b::platform_sink_enabled());
}

TEST(AbiNamespace, CallbacksAreSeparate) {
    probe_main::set_platform_sink(false);
    probe_b::set_platform_sink(false);
    probe_main::set_level(k_debug);
    probe_b::set_level(k_debug);

    // A library replays the records it buffered without a callback when one is set, so
    // only the differences from here on count.
    probe_main::count_records();
    const int main_before = probe_main::records();
    const int b_before = probe_b::records();

    probe_main::log_error("to the main library");
    EXPECT_EQ(probe_main::records(), main_before + 1);
    EXPECT_EQ(probe_b::records(), b_before);

    probe_b::count_records();
    const int b_counting = probe_b::records();
    probe_b::log_error("to the second library");
    EXPECT_EQ(probe_b::records(), b_counting + 1);
    EXPECT_EQ(probe_main::records(), main_before + 1);

    probe_main::clear_callback();
    probe_main::log_error("to the main library, without its callback");
    probe_b::log_error("to the second library, which keeps its callback");
    EXPECT_EQ(probe_main::records(), main_before + 1);
    EXPECT_EQ(probe_b::records(), b_counting + 2);

    probe_b::clear_callback();
}

}  // namespace
