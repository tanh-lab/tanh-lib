#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <map>
#include <gtest/gtest.h>
#include "tanh/core/threading/RCU.h"

using namespace thl;

TEST(RCU, BasicFunctionality) {
    RCU<std::map<std::string, int>> rcu_map;

    // Add some initial data
    rcu_map.update([](auto& map) {
        map["key1"] = 100;
        map["key2"] = 200;
    });

    // Test reading
    auto value = rcu_map.read([](const auto& map) {
        auto it = map.find("key1");
        return (it != map.end()) ? it->second : -1;
    });
    EXPECT_EQ(value, 100);
}

TEST(RCU, VectorFunctionality) {
    RCU<std::vector<int>> rcu_vec;

    // Add some initial data
    rcu_vec.update([](auto& vec) {
        vec.push_back(1);
        vec.push_back(2);
        vec.push_back(3);
    });

    // Test reading
    auto value = rcu_vec.read([](const auto& vec) {
        return (vec.size() == 3) ? vec[0] : -1;
    });
    EXPECT_EQ(value, 1);
}

TEST(RCU, MapFunctionality) {
    RCU<std::map<std::string, int>> rcu_map;

    // Add some initial data
    rcu_map.update([](auto& map) {
        map["key1"] = 100;
        map["key2"] = 200;
    });
    
    // Test lock-free reading
    auto value = rcu_map.read([](const auto& map) {
        auto it = map.find("key1");
        return (it != map.end()) ? it->second : -1;
    });
    
    EXPECT_EQ(value, 100);
    
    // Test updating while reading from multiple threads
    std::atomic<bool> running{true};
    std::atomic<int> read_count{0};
    std::atomic<int> errors{0};
    
    // Start reader threads
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&]() {
            while (running.load()) {
                auto val = rcu_map.read([](const auto& map) {
                    auto it = map.find("key1");
                    return (it != map.end()) ? it->second : -1;
                });
                
                if (val < 100) {
                    errors.fetch_add(1);
                }
                read_count.fetch_add(1);
                
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        });
    }
    
    // Start writer thread
    std::thread writer([&]() {
        for (int i = 0; i < 100; ++i) {
            rcu_map.update([i](auto& map) {
                map["key1"] = 100 + i;
                map["new_key_" + std::to_string(i)] = i;
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    
    // Let it run for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    running.store(false);
    
    // Join all threads
    writer.join();
    for (auto& reader : readers) {
        reader.join();
    }

    // Check for errors
    EXPECT_EQ(errors.load(), 0);

    // Check final state
    auto final_size = rcu_map.size();
    EXPECT_EQ(final_size, 102); // 100 updates + 2 initial keys
}

TEST(RCU, VectorFunctionalityBig) {
    RCU<std::vector<int>> rcu_vec;

    // Add some initial data
    rcu_vec.update([](auto& vec) {
        vec.push_back(1);
        vec.push_back(2);
        vec.push_back(3);
    });
    
    std::atomic<bool> running{true};
    std::atomic<int> notifications{0};
    
    // Simulate notification threads (like audio callbacks)
    std::vector<std::thread> notifiers;
    for (int i = 0; i < 3; ++i) {
        notifiers.emplace_back([&]() {
            while (running.load()) {
                // Simulate notifying all listeners (lock-free read)
                rcu_vec.read([&](const auto& listeners) {
                    for (auto listener_id : listeners) {
                        // Simulate calling listener
                        notifications.fetch_add(1);
                    }
                });
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });
    }
    
    // Simulate adding/removing listeners from UI thread
    std::thread ui_thread([&]() {
        for (int i = 0; i < 50; ++i) {
            // Add listener
            rcu_vec.update([i](auto& vec) {
                vec.push_back(100 + i);
            });
            
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            
            // Remove a listener
            if (i % 5 == 0) {
                rcu_vec.update([](auto& vec) {
                    if (!vec.empty()) {
                        vec.pop_back();
                    }
                });
            }
        }
    });
    
    // Let it run
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    running.store(false);
    
    // Join threads
    ui_thread.join();
    for (auto& notifier : notifiers) {
        notifier.join();
    }
    
    // Check notifications count
    EXPECT_GT(notifications.load(), 0);
    EXPECT_EQ(rcu_vec.size(), 43); // Initial 3 + 50 adds + 10 removes
}
