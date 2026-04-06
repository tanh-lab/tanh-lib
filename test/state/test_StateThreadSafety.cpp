#include "tanh/state/State.h"
#include <atomic>
#include <future>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace thl;

// =============================================================================
// Thread safety tests
// =============================================================================

TEST(StateTests, ThreadSafety) {
    State state;
    const int k_num_threads = 10;
    const int k_num_operations = 1000;

    // Set up some initial parameters
    state.create("counter", 0);
    state.create("string_param", "initial");

    // Create a group with parameters
    state.create_group("audio")->create("volume", 0.5);

    // Use atomic flag for controlling thread execution
    std::atomic<bool> start_flag(false);
    std::atomic<int> ready_thread_count(0);

    // Shared atomic counter to track total operations performed - real-time
    // safe
    std::atomic<int> operations_completed(0);

    // Shared atomic counter for volume accumulation - real-time safe
    std::atomic<int> volume_increments(0);

    // Function for writer threads (increment counter) - made real-time safe
    auto writer = [&state,
                   &start_flag,
                   &ready_thread_count,
                   &operations_completed,
                   &volume_increments,
                   k_num_operations](int id) {
        // Signal that thread is ready
        ready_thread_count.fetch_add(1);

        // Wait for start signal
        while (!start_flag.load(std::memory_order_acquire)) {
            // Real-time safe spin wait
            std::atomic_thread_fence(std::memory_order_acquire);
        }

        // Ensure thread is registered after start signal to account for the
        // nested group created after initial thread creation
        state.ensure_thread_registered();

        for (int i = 0; i < k_num_operations; ++i) {
            // Atomically increment the shared operation counter
            operations_completed.fetch_add(1, std::memory_order_relaxed);

            // Atomically increment the volume counter
            volume_increments.fetch_add(1, std::memory_order_relaxed);

            // Update parameters separately - now real-time safe
            // We don't need read-modify-write patterns anymore, just set values
            state.set("counter", operations_completed.load(std::memory_order_relaxed));

            // Convert volume_increments to a double for the volume parameter
            double new_volume = 0.5 + (volume_increments.load(std::memory_order_relaxed) * 0.001);
            state.set("audio.volume", new_volume);

            // For even thread IDs, also update string parameter
            // String operations are not real-time safe, but are contained in a
            // controlled branch
            if (id % 2 == 0) {
                std::string new_value = "thread_" + std::to_string(id) + "_" + std::to_string(i);
                state.set("string_param", new_value);
            }

            // Sleep to simulate work - not real-time safe
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    };

    // Function for reader threads - made real-time safe
    auto reader = [&state, &start_flag, &ready_thread_count, k_num_operations](int id) {
        // Signal that thread is ready
        ready_thread_count.fetch_add(1);

        // Wait for start signal
        while (!start_flag.load(std::memory_order_acquire)) {
            // Real-time safe spin wait
            std::atomic_thread_fence(std::memory_order_acquire);
        }

        // Ensure thread is registered after start signal to account for the
        // nested group created after initial thread creation
        state.ensure_thread_registered();

        for (int i = 0; i < k_num_operations; ++i) {
            // Read various parameters - these are real-time safe operations
            auto counter = state.get<int>("counter");
            auto volume = state.get<double>("audio.volume");
            auto test_param = state.get<int>("nested.group.param");

            // String read is not real-time safe, but is necessary for the test
            auto str = state.get<std::string>("string_param", true);

            // Just to avoid compiler optimization
            EXPECT_GE(counter, 0);
            EXPECT_GE(volume, 0.0);
            EXPECT_EQ(test_param, 10);
            EXPECT_FALSE(str.empty());

            // Sleep to simulate work - not real-time safe
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }

        // Exit real-time context before thread cleanup to avoid RTSan false
        // positive Thread destruction involves free(), which RTSan would flag
        // if still in RT context
        TANH_NONBLOCKING_SCOPED_DISABLER

        return true;  // All reads completed successfully
    };

    // Launch threads
    std::vector<std::future<void>> write_threads;
    std::vector<std::future<bool>> read_threads;

    for (int i = 0; i < k_num_threads; ++i) {
        write_threads.push_back(std::async(std::launch::async, writer, i));
        read_threads.push_back(std::async(std::launch::async, reader, i));
    }

    // Wait for all threads to be ready
    while (ready_thread_count.load() < k_num_threads * 2) {
        // Small sleep here is acceptable as it's not in the real-time path
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Nested group with parameter
    state.create_group("nested")->create_group("group")->create("param", 10);

    // Start all threads simultaneously - real-time safe trigger
    start_flag.store(true, std::memory_order_release);

    // Wait for all threads to finish - not part of real-time execution
    for (auto& f : write_threads) { f.get(); }

    // Check that all read threads completed successfully - not part of
    // real-time execution
    for (auto& f : read_threads) { EXPECT_TRUE(f.get()); }

    // Check final state - we should have exactly k_num_threads * k_num_operations
    // operations
    EXPECT_EQ(k_num_threads * k_num_operations, operations_completed.load());

    // Make sure the parameters match our atomic tracking variables
    EXPECT_EQ(operations_completed.load(), state.get<int>("counter"));

    double expected_volume = 0.5 + (k_num_threads * k_num_operations * 0.001);
    EXPECT_DOUBLE_EQ(expected_volume, state.get<double>("audio.volume"));

    // String parameter should have been updated multiple times, but we can't
    // know the final value
    EXPECT_FALSE(state.get<std::string>("string_param", true).empty());
}
