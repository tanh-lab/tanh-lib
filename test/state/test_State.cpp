#include "tanh/state/State.h"
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <future>

using namespace thl;

// Basic parameter operations tests
TEST(StateTests, BasicParameterOperations) {
    State state;
    
    // Test empty state
    EXPECT_TRUE(state.is_empty());
    
    // Test setting and getting parameters of different types
    state.set("double_param", 3.14159);
    state.set("float_param", 2.71828f);
    state.set("int_param", 42);
    state.set("bool_param", true);
    state.set("string_param", "hello world");
    
    // Test that the state is no longer empty
    EXPECT_FALSE(state.is_empty());
    
    // Test retrieving parameters with correct types
    EXPECT_DOUBLE_EQ(3.14159, state.get<double>("double_param"));
    EXPECT_FLOAT_EQ(2.71828f, state.get<float>("float_param"));
    EXPECT_EQ(42, state.get<int>("int_param"));
    EXPECT_TRUE(state.get<bool>("bool_param"));
    EXPECT_EQ("hello world", state.get<std::string>("string_param"));
    
    // Test parameter type identification
    EXPECT_EQ(ParameterType::Double, state.get_parameter_type("double_param"));
    EXPECT_EQ(ParameterType::Float, state.get_parameter_type("float_param"));
    EXPECT_EQ(ParameterType::Int, state.get_parameter_type("int_param"));
    EXPECT_EQ(ParameterType::Bool, state.get_parameter_type("bool_param"));
    EXPECT_EQ(ParameterType::String, state.get_parameter_type("string_param"));
    
    // Test clearing the state
    state.clear();
    EXPECT_TRUE(state.is_empty());
}

// Test for automatic group creation when setting hierarchical parameters
TEST(StateTests, AutoGroupCreation) {
    State state;
    
    // Initially, no groups should exist
    EXPECT_TRUE(state.is_empty());
    EXPECT_FALSE(state.has_group("audio"));

    // Set a parameter with a hierarchical path
    state.set("audio.mixer.volume", 0.75);
    
    // Check that the groups were automatically created
    EXPECT_TRUE(state.has_group("audio"));
    
    // Get the created groups and verify their existence
    StateGroup* audio_group = state.get_group("audio");
    ASSERT_NE(nullptr, audio_group);
    EXPECT_TRUE(audio_group->has_group("mixer"));
    
    StateGroup* mixer_group = audio_group->get_group("mixer");
    ASSERT_NE(nullptr, mixer_group);
    
    // Verify the parameter was correctly set
    EXPECT_DOUBLE_EQ(0.75, state.get<double>("audio.mixer.volume"));
    EXPECT_DOUBLE_EQ(0.75, audio_group->get<double>("mixer.volume"));
    EXPECT_DOUBLE_EQ(0.75, mixer_group->get<double>("volume"));
    
    // Set another parameter in a different hierarchy
    state.set("audio.effects.reverb.size", 0.5);
    
    // Verify the new groups were created
    EXPECT_TRUE(audio_group->has_group("effects"));
    
    StateGroup* effects_group = audio_group->get_group("effects");
    ASSERT_NE(nullptr, effects_group);
    EXPECT_TRUE(effects_group->has_group("reverb"));
    
    // Verify the parameter was correctly set
    EXPECT_DOUBLE_EQ(0.5, state.get<double>("audio.effects.reverb.size"));
    
    // Test multi-level paths being created in one go
    state.set("visual.display.brightness.level", 85);
    
    // Verify all groups were created
    EXPECT_TRUE(state.has_group("visual"));
    
    StateGroup* visual_group = state.get_group("visual");
    ASSERT_NE(nullptr, visual_group);
    EXPECT_TRUE(visual_group->has_group("display"));
    
    StateGroup* display_group = visual_group->get_group("display");
    ASSERT_NE(nullptr, display_group);
    EXPECT_TRUE(display_group->has_group("brightness"));
    
    // Verify the parameter was correctly set
    EXPECT_EQ(85, state.get<int>("visual.display.brightness.level"));
}

// Type conversion tests
TEST(StateTests, TypeConversions) {
    State state;
    
    // Set parameters of different types
    state.set("number", 42.5);
    
    // Test numeric type conversions
    EXPECT_DOUBLE_EQ(42.5, state.get<double>("number"));
    EXPECT_FLOAT_EQ(42.5f, state.get<float>("number"));
    EXPECT_EQ(42, state.get<int>("number"));
    EXPECT_TRUE(state.get<bool>("number"));
    
    // Test string conversions
    state.set("string_true", "true");
    state.set("string_one", "1");
    state.set("string_number", "123.45");
    
    EXPECT_TRUE(state.get<bool>("string_true"));
    EXPECT_TRUE(state.get<bool>("string_one"));
    EXPECT_DOUBLE_EQ(123.45, state.get<double>("string_number"));
    EXPECT_FLOAT_EQ(123.45f, state.get<float>("string_number"));
    EXPECT_EQ(123, state.get<int>("string_number"));
    
    // Test invalid conversions
    state.set("invalid_number", "not a number");
    EXPECT_DOUBLE_EQ(0.0, state.get<double>("invalid_number"));
    EXPECT_FLOAT_EQ(0.0f, state.get<float>("invalid_number"));
    EXPECT_EQ(0, state.get<int>("invalid_number"));
    EXPECT_FALSE(state.get<bool>("invalid_number"));
    
    // Test bool to other types
    state.set("bool_true", true);
    state.set("bool_false", false);
    
    EXPECT_DOUBLE_EQ(1.0, state.get<double>("bool_true"));
    EXPECT_DOUBLE_EQ(0.0, state.get<double>("bool_false"));
    EXPECT_EQ(1, state.get<int>("bool_true"));
    EXPECT_EQ(0, state.get<int>("bool_false"));
    EXPECT_EQ("true", state.get<std::string>("bool_true"));
    EXPECT_EQ("false", state.get<std::string>("bool_false"));
}

// Parameter object tests
TEST(StateTests, ParameterObjectTests) {
    State state;
    
    // Set parameters of different types
    state.set("double_param", 3.14159);
    state.set("int_param", 42);
    state.set("bool_param", true);
    state.set("string_param", "hello world");
    
    // Get parameter objects
    Parameter double_param = state.get_parameter("double_param");
    Parameter int_param = state.get_parameter("int_param");
    Parameter bool_param = state.get_parameter("bool_param");
    Parameter string_param = state.get_parameter("string_param");
    
    // Test type checking methods
    EXPECT_TRUE(double_param.is_double());
    EXPECT_FALSE(double_param.is_int());
    
    EXPECT_TRUE(int_param.is_int());
    EXPECT_FALSE(int_param.is_string());
    
    EXPECT_TRUE(bool_param.is_bool());
    EXPECT_FALSE(bool_param.is_double());
    
    EXPECT_TRUE(string_param.is_string());
    EXPECT_FALSE(string_param.is_bool());
    
    // Test conversion methods
    EXPECT_DOUBLE_EQ(3.14159, double_param.to<double>());
    EXPECT_EQ(42, int_param.to<int>());
    EXPECT_TRUE(bool_param.to<bool>());
    EXPECT_EQ("hello world", string_param.to<std::string>());
    
    // Test cross-type conversions
    EXPECT_FLOAT_EQ(3.14159f, double_param.to<float>());
    EXPECT_EQ(3, double_param.to<int>());
    EXPECT_TRUE(double_param.to<bool>());
    
    EXPECT_DOUBLE_EQ(42.0, int_param.to<double>());
    EXPECT_TRUE(int_param.to<bool>());
}

// Test for the create parameter in set() method
// Test for the create parameter in set() method
TEST(StateTests, CreateParameterFlag) {
    State state;
    
    // Test with create=true (default behavior)
    // This should create the parameter if it doesn't exist
    state.set("audio.volume", 0.75, NotifyStrategies::all, nullptr, true);
    EXPECT_DOUBLE_EQ(0.75, state.get<double>("audio.volume"));
    EXPECT_TRUE(state.has_group("audio"));
    
    // Update an existing parameter with create=false
    // This should work since the parameter already exists
    state.set("audio.volume", 0.5, NotifyStrategies::all, nullptr, false);
    EXPECT_DOUBLE_EQ(0.5, state.get<double>("audio.volume"));
    
    // Test with create=false on a non-existent parameter
    // This should throw StateKeyNotFoundException
    EXPECT_THROW({
        state.set("nonexistent.parameter", 100, NotifyStrategies::all, nullptr, false);
    }, StateKeyNotFoundException);
    
    // Test deep nesting with create=true
    state.set("visual.display.brightness", 85, NotifyStrategies::all, nullptr, true);
    EXPECT_EQ(85, state.get<int>("visual.display.brightness"));
    
    // Test with create=false on a non-existent nested parameter
    EXPECT_THROW({
        // "color" doesn't exist under visual.display
        state.set("visual.display.color", "blue", NotifyStrategies::all, nullptr, false);
    }, StateKeyNotFoundException);
    
    // Test with create=false on a non-existent group
    EXPECT_THROW({
        // "effects" doesn't exist under audio
        state.set("audio.effects.reverb", 0.3, NotifyStrategies::all, nullptr, false);
    }, StateKeyNotFoundException);
    
    // Create that missing group and then test again
    state.set("audio.effects.reverb", 0.3, NotifyStrategies::all, nullptr, true);
    EXPECT_DOUBLE_EQ(0.3, state.get<double>("audio.effects.reverb"));
    
    // Now it should work with create=false since it exists
    state.set("audio.effects.reverb", 0.4, NotifyStrategies::all, nullptr, false);
    EXPECT_DOUBLE_EQ(0.4, state.get<double>("audio.effects.reverb"));
}

// Hierarchical structure tests
TEST(StateTests, HierarchicalStructure) {
    State state;
    
    // Test creating groups
    StateGroup* audio_group = state.create_group("audio");
    EXPECT_TRUE(state.has_group("audio"));
    EXPECT_FALSE(state.has_group("nonexistent"));
    
    // Test getting existing groups
    StateGroup* retrieved_audio_group = state.get_group("audio");
    EXPECT_EQ(audio_group, retrieved_audio_group);
    
    // Test nesting groups
    StateGroup* effects_group = audio_group->create_group("effects");
    StateGroup* reverb_group = effects_group->create_group("reverb");
    
    // Test path construction
    EXPECT_EQ("audio", audio_group->get_full_path());
    EXPECT_EQ("audio.effects", effects_group->get_full_path());
    EXPECT_EQ("audio.effects.reverb", reverb_group->get_full_path());
    
    // Test setting parameters at different levels
    state.set("global", 1.0);
    audio_group->set("volume", 0.8);
    effects_group->set("enabled", true);
    reverb_group->set("mix", 0.3f);
    reverb_group->set("room_size", 0.7f);
    
    // Test setting parameters with paths
    state.set("audio.effects.delay.time", 500);
    state.set("audio.effects.delay.feedback", 0.4f);
    
    // Test getting parameters with paths
    EXPECT_DOUBLE_EQ(1.0, state.get<double>("global"));
    EXPECT_DOUBLE_EQ(0.8, state.get<double>("audio.volume"));
    EXPECT_TRUE(state.get<bool>("audio.effects.enabled"));
    EXPECT_FLOAT_EQ(0.3f, state.get<float>("audio.effects.reverb.mix"));
    EXPECT_FLOAT_EQ(0.7f, state.get<float>("audio.effects.reverb.room_size"));
    EXPECT_EQ(500, state.get<int>("audio.effects.delay.time"));
    EXPECT_FLOAT_EQ(0.4f, state.get<float>("audio.effects.delay.feedback"));
    
    // Test getting parameters through groups
    EXPECT_DOUBLE_EQ(0.8, audio_group->get<double>("volume"));
    EXPECT_TRUE(effects_group->get<bool>("enabled"));
    EXPECT_FLOAT_EQ(0.3f, reverb_group->get<float>("mix"));
    
    // Test parameter objects with paths
    Parameter volume_param = state.get_parameter("audio.volume");
    EXPECT_TRUE(volume_param.is_double());
    EXPECT_DOUBLE_EQ(0.8, volume_param.to<double>());
    
    // Test clear on groups
    reverb_group->clear();
    EXPECT_TRUE(reverb_group->is_empty());
    EXPECT_FALSE(effects_group->is_empty()); // Still has "enabled" param and delay group
    
    // Test clearing the whole state
    state.clear();
    EXPECT_TRUE(state.is_empty());
}

// Thread safety tests
TEST(StateTests, ThreadSafety) {
    State state;
    const int NUM_THREADS = 10;
    const int NUM_OPERATIONS = 1000;
    
    // Set up some initial parameters
    state.set("counter", 0);
    state.set("string_param", "initial");
    
    // Create a group with parameters
    state.create_group("audio")->set("volume", 0.5);
    
    // Use atomic flag for controlling thread execution
    std::atomic<bool> start_flag(false);
    std::atomic<int> ready_thread_count(0);

    // Shared atomic counter to track total operations performed - real-time safe
    std::atomic<int> operations_completed(0);
    
    // Shared atomic counter for volume accumulation - real-time safe
    std::atomic<int> volume_increments(0);

    // Function for writer threads (increment counter) - made real-time safe
    auto writer = [&state, &start_flag, &ready_thread_count, &operations_completed, &volume_increments](int id) {
        state.ensure_rcu_initialized(); // Ensure RCU is initialized for this thread
        // Signal that thread is ready
        ready_thread_count.fetch_add(1);
        
        // Wait for start signal
        while (!start_flag.load(std::memory_order_acquire)) {
            // Real-time safe spin wait
            std::atomic_thread_fence(std::memory_order_acquire);
        }
        
        for (int i = 0; i < NUM_OPERATIONS; ++i) {
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
            // String operations are not real-time safe, but are contained in a controlled branch
            if (id % 2 == 0) {
                std::string new_value = "thread_" + std::to_string(id) + "_" + std::to_string(i);
                state.set("string_param", new_value);
            }
            
            // Sleep to simulate work - not real-time safe
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    };
    
    // Function for reader threads - made real-time safe
    auto reader = [&state, &start_flag, &ready_thread_count](int id) {
        state.ensure_rcu_initialized(); // Ensure RCU is initialized for this thread

        // Signal that thread is ready
        ready_thread_count.fetch_add(1);
        
        // Wait for start signal
        while (!start_flag.load(std::memory_order_acquire)) {
            // Real-time safe spin wait
            std::atomic_thread_fence(std::memory_order_acquire);
        }
        
        for (int i = 0; i < NUM_OPERATIONS; ++i) {
            // Read various parameters - these are real-time safe operations
            int counter = state.get<int>("counter");
            double volume = state.get<double>("audio.volume");
            
            // String read is not real-time safe, but is necessary for the test
            std::string str = state.get<std::string>("string_param");
            
            // Just to avoid compiler optimization
            EXPECT_GE(counter, 0);
            EXPECT_GE(volume, 0.0);
            EXPECT_FALSE(str.empty());
            
            // Sleep to simulate work - not real-time safe
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
        return true; // All reads completed successfully
    };
    
    // Launch threads
    std::vector<std::future<void>> write_threads;
    std::vector<std::future<bool>> read_threads;
    
    for (int i = 0; i < NUM_THREADS; ++i) {
        write_threads.push_back(std::async(std::launch::async, writer, i));
        read_threads.push_back(std::async(std::launch::async, reader, i));
    }
    
    // Wait for all threads to be ready
    while (ready_thread_count.load() < NUM_THREADS * 2) {
        // Small sleep here is acceptable as it's not in the real-time path
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    // Start all threads simultaneously - real-time safe trigger
    start_flag.store(true, std::memory_order_release);
    
    // Wait for all threads to finish - not part of real-time execution
    for (auto& f : write_threads) {
        f.get();
    }
    
    // Check that all read threads completed successfully - not part of real-time execution
    for (auto& f : read_threads) {
        EXPECT_TRUE(f.get());
    }
    
    // Check final state - we should have exactly NUM_THREADS * NUM_OPERATIONS operations
    EXPECT_EQ(NUM_THREADS * NUM_OPERATIONS, operations_completed.load());
    
    // Make sure the parameters match our atomic tracking variables
    EXPECT_EQ(operations_completed.load(), state.get<int>("counter"));
    
    double expected_volume = 0.5 + (NUM_THREADS * NUM_OPERATIONS * 0.001);
    EXPECT_DOUBLE_EQ(expected_volume, state.get<double>("audio.volume"));
    
    // String parameter should have been updated multiple times, but we can't know the final value
    EXPECT_FALSE(state.get<std::string>("string_param").empty());
}

// Test for JSON state updates
TEST(StateTests, UpdateFromJson) {
    State state;
    
    // Set up initial state
    state.set("volume", 0.5);
    state.set("muted", false);
    state.set("name", "default device");
    
    // Create a group for EQ settings
    StateGroup* eq = state.create_group("eq");
    eq->set("bass", 5);
    eq->set("treble", 3);
    
    // Basic JSON update
    nlohmann::json simple_update = {
        {"volume", 0.8},
        {"muted", true}
    };
    
    state.update_from_json(simple_update);
    
    // Verify the updated values
    EXPECT_DOUBLE_EQ(0.8, state.get<double>("volume"));
    EXPECT_TRUE(state.get<bool>("muted"));
    EXPECT_EQ("default device", state.get<std::string>("name")); // Unchanged
    
    // Test nested JSON update
    nlohmann::json nested_update = {
        {"eq", {
            {"bass", 7},
            {"treble", 4}
        }}
    };
    
    state.update_from_json(nested_update);
    
    // Verify the nested updates
    EXPECT_EQ(7, state.get<int>("eq.bass"));
    EXPECT_EQ(4, state.get<int>("eq.treble"));
    
    // Test mixed update with different types
    nlohmann::json mixed_update = {
        {"name", "new device"},
        {"eq", {
            {"bass", 10}
        }}
    };
    
    state.update_from_json(mixed_update);
    
    EXPECT_EQ("new device", state.get<std::string>("name"));
    EXPECT_EQ(10, state.get<int>("eq.bass"));
    EXPECT_EQ(4, state.get<int>("eq.treble")); // Unchanged
}

// Test for JSON state updates with non-existent keys
TEST(StateTests, UpdateFromJsonNonExistentKey) {
    State state;
    
    // Set up initial state
    state.set("volume", 0.5);
    
    // JSON with non-existent key
    nlohmann::json invalid_update = {
        {"volume", 0.8},
        {"non_existent", 42}
    };
    
    // The update should throw StateKeyNotFoundException
    EXPECT_THROW({
        try {
            state.update_from_json(invalid_update);
        } catch (const StateKeyNotFoundException& e) {
            // Verify the error message contains the key name
            EXPECT_TRUE(std::string(e.what()).find("non_existent") != std::string::npos);
            EXPECT_EQ("non_existent", e.key());
            throw;
        }
    }, StateKeyNotFoundException);
    
    // Verify that no partial updates occurred
    EXPECT_DOUBLE_EQ(0.5, state.get<double>("volume"));
}

// Test for JSON state updates with deep nesting
TEST(StateTests, UpdateFromJsonDeepNesting) {
    State state;
    
    // Set up a deeply nested state
    StateGroup* audio = state.create_group("audio");
    StateGroup* effects = audio->create_group("effects");
    StateGroup* reverb = effects->create_group("reverb");
    
    reverb->set("wet", 0.3);
    reverb->set("dry", 0.7);
    reverb->set("room_size", 0.5);
    
    // Create a JSON with deep nesting
    nlohmann::json deep_update = {
        {"audio", {
            {"effects", {
                {"reverb", {
                    {"wet", 0.4},
                    {"room_size", 0.8}
                }}
            }}
        }}
    };
    
    state.update_from_json(deep_update);
    
    // Verify the deep updates
    EXPECT_DOUBLE_EQ(0.4, state.get<double>("audio.effects.reverb.wet"));
    EXPECT_DOUBLE_EQ(0.8, state.get<double>("audio.effects.reverb.room_size"));
    EXPECT_DOUBLE_EQ(0.7, state.get<double>("audio.effects.reverb.dry")); // Unchanged
}

// Test for JSON state updates with different numeric types
TEST(StateTests, UpdateFromJsonNumericTypes) {
    State state;
    
    // Set up parameters of different types
    state.set("double_value", 1.0);
    state.set("int_value", 1);
    state.set("bool_value", false);
    
    // Update with numeric literals of different types
    nlohmann::json numeric_update = {
        {"double_value", 2},      // Integer literal to double parameter
        {"int_value", 2.5},       // Double literal to int parameter
        {"bool_value", 1}         // Integer literal to bool parameter
    };
    
    state.update_from_json(numeric_update);
    
    // Verify appropriate type conversions
    EXPECT_DOUBLE_EQ(2.0, state.get<double>("double_value"));
    EXPECT_EQ(2, state.get<int>("int_value"));       // Truncated to 2
    EXPECT_TRUE(state.get<bool>("bool_value"));      // Converted to true
    
    // Check that the stored types remain unchanged
    EXPECT_EQ(ParameterType::Double, state.get_parameter_type("double_value"));
    EXPECT_EQ(ParameterType::Int, state.get_parameter_type("int_value"));
    EXPECT_EQ(ParameterType::Bool, state.get_parameter_type("bool_value"));
}

// Test class for parameter notifications
class TestParameterListener : public ParameterListener {
public:
    void on_parameter_changed(std::string_view path, const Parameter& param) override {
        last_path = path;
        
        // Store the value based on the parameter type
        switch(param.get_type()) {
            case ParameterType::Double:
                last_value_double = param.to<double>();
                break;
            case ParameterType::Float:
                last_value_double = param.to<float>();
                break;
            case ParameterType::Int:
                last_value_double = param.to<int>();
                break;
            case ParameterType::Bool:
                last_value_double = param.to<bool>() ? 1.0 : 0.0;
                break;
            default:
                last_value_double = 0.0;
                break;
        }
        
        notification_count++;
    }

    std::string last_path;
    double last_value_double = 0.0;
    int notification_count = 0;
};

// Test for parameter notification functionality
TEST(StateTests, ParameterNotification) {
    State state;
    TestParameterListener listener;
    
    // Register the listener
    state.add_listener(&listener);
    
    // Set a parameter - this should trigger notification
    state.set("test.param", 42.0);
    
    // Verify the listener was notified
    EXPECT_EQ(1, listener.notification_count);
    EXPECT_EQ("test.param", listener.last_path);
    
    // Test optional notification - should not notify
    state.set("test.param", 84.0, NotifyStrategies::none);
    
    // Verify no additional notification occurred
    EXPECT_EQ(1, listener.notification_count);
    
    // But value should be updated
    EXPECT_DOUBLE_EQ(84.0, state.get<double>("test.param"));
    
    // Test hierarchical notifications
    StateGroup* audio_group = state.create_group("audio");
    TestParameterListener group_listener;
    audio_group->add_listener(&group_listener);
    
    // Set parameter in audio group
    state.set("audio.volume", 0.75);
    
    // Root listener should be notified
    EXPECT_EQ(2, listener.notification_count);
    // Group listener should be notified
    EXPECT_EQ(1, group_listener.notification_count);
    EXPECT_EQ("audio.volume", group_listener.last_path);
    
    // Test manual notification through Parameter object
    state.set("audio.bass", 0.5, NotifyStrategies::none);  // Set without notification
    
    // Verify no notification occurred
    EXPECT_EQ(2, listener.notification_count);
    EXPECT_EQ(1, group_listener.notification_count);
    
    // Manually notify
    Parameter param = state.get_parameter("audio.bass");
    param.notify();
    
    // Verify notifications occurred
    EXPECT_EQ(3, listener.notification_count);
    EXPECT_EQ(2, group_listener.notification_count);
}

TEST(StateTests, CallbackBasedNotifications) {
    State state;
    int callback_count = 0;
    std::string last_path;
    last_path.reserve(256);
    double last_value = 0.0;
    
    // Add a callback listener
    size_t listener_id = state.add_callback_listener([&](std::string_view path, const Parameter& param) {
        callback_count++;
        last_path = path;
        last_value = param.to<double>();
    });
    
    // Set parameter
    state.set("test.callback.param", 123.0);
    
    // Verify callback was invoked
    EXPECT_EQ(1, callback_count);
    EXPECT_EQ("test.callback.param", last_path);
    EXPECT_DOUBLE_EQ(123.0, last_value);
    
    // Remove callback listener
    state.remove_callback_listener(listener_id);
    
    // Set parameter again
    state.set("test.callback.param", 456.0);
    
    // Verify callback was not invoked again
    EXPECT_EQ(1, callback_count);
    
    // But value should be updated
    EXPECT_DOUBLE_EQ(456.0, state.get<double>("test.callback.param"));
}

TEST(StateTests, ManualNotification) {
    State state;
    TestParameterListener listener;
    
    // Register listener
    state.add_listener(&listener);
    
    // Set a parameter without notification
    state.set("manual.test", 42.0, NotifyStrategies::none);
    
    // Verify no notification occurred
    EXPECT_EQ(0, listener.notification_count);
    
    // Manually notify about the parameter
    state.notify_parameter_change("manual.test");
    
    // Verify notification occurred
    EXPECT_EQ(1, listener.notification_count);
    EXPECT_EQ("manual.test", listener.last_path);
    EXPECT_DOUBLE_EQ(42.0, listener.last_value_double);
    
    // Test direct Parameter notify method
    Parameter param = state.get_parameter("manual.test");
    param.notify();
    
    // Verify second notification
    EXPECT_EQ(2, listener.notification_count);
}

// Test for StateGroup::get_parameters() method
TEST(StateTests, GetParametersMethod) {
    State state;
    
    // Create an empty state and check that get_parameters returns an empty map
    std::map<std::string, Parameter> empty_params = state.get_parameters();
    EXPECT_TRUE(empty_params.empty());
    
    // Add parameters at root level
    state.set("root_param1", 42);
    state.set("root_param2", "root value");
    
    // Check root parameters
    std::map<std::string, Parameter> root_params = state.get_parameters();
    EXPECT_EQ(2, root_params.size());
    EXPECT_TRUE(root_params.find("root_param1") != root_params.end());
    EXPECT_TRUE(root_params.find("root_param2") != root_params.end());
    
    // Access the Parameter using find() and then access the value
    auto param1_it = root_params.find("root_param1");
    EXPECT_NE(param1_it, root_params.end());
    EXPECT_EQ(42, param1_it->second.to<int>());
    
    auto param2_it = root_params.find("root_param2");
    EXPECT_NE(param2_it, root_params.end());
    EXPECT_EQ("root value", param2_it->second.to<std::string>());
    
    // Create a nested structure with parameters
    StateGroup* audio_group = state.create_group("audio");
    audio_group->set("volume", 0.8);
    audio_group->set("muted", false);
    
    // Check that root level parameters still match
    root_params = state.get_parameters();
    EXPECT_EQ(4, root_params.size()); // 2 root + 2 from audio group
    
    // Check audio group parameters
    std::map<std::string, Parameter> audio_params = audio_group->get_parameters();
    EXPECT_EQ(2, audio_params.size());
    
    auto volume_it = audio_params.find("audio.volume");
    EXPECT_NE(volume_it, audio_params.end());
    EXPECT_DOUBLE_EQ(0.8, volume_it->second.to<double>());
    
    auto muted_it = audio_params.find("audio.muted");
    EXPECT_NE(muted_it, audio_params.end());
    EXPECT_FALSE(muted_it->second.to<bool>());
    
    // // Create a deeper nested structure
    // StateGroup* effects_group = audio_group->create_group("effects");
    // effects_group->set("reverb", 0.5);
    // effects_group->set("delay", 0.3);
    // effects_group->set("chorus", 0.2);
    
    // // Check effects group parameters
    // std::map<std::string, Parameter> effects_params = effects_group->get_parameters();
    // EXPECT_EQ(3, effects_params.size());
    // EXPECT_TRUE(effects_params.find("audio.effects.reverb") != effects_params.end());
    // EXPECT_TRUE(effects_params.find("audio.effects.delay") != effects_params.end());
    // EXPECT_TRUE(effects_params.find("audio.effects.chorus") != effects_params.end());
    // EXPECT_DOUBLE_EQ(0.5, effects_params["audio.effects.reverb"].to<double>());
    
    // // Check that audio group parameters now include effects parameters
    // audio_params = audio_group->get_parameters();
    // EXPECT_EQ(5, audio_params.size()); // 2 in audio + 3 in effects
    // EXPECT_TRUE(audio_params.find("audio.effects.reverb") != audio_params.end());
    
    // // Add another group with parameters
    // StateGroup* visual_group = state.create_group("visual");
    // visual_group->set("brightness", 75);
    // visual_group->set("contrast", 100);
    
    // // Check visual group parameters
    // std::map<std::string, Parameter> visual_params = visual_group->get_parameters();
    // EXPECT_EQ(2, visual_params.size());
    
    // // Root should now have all parameters
    // root_params = state.get_parameters();
    // EXPECT_EQ(9, root_params.size()); // 2 root + 2 audio + 3 effects + 2 visual
    
    // // Clear one group and verify its parameters are removed
    // effects_group->clear();
    // audio_params = audio_group->get_parameters();
    // EXPECT_EQ(2, audio_params.size()); // Only the direct audio parameters remain
    
    // // Clear the entire state
    // state.clear();
    // root_params = state.get_parameters();
    // EXPECT_TRUE(root_params.empty());
}

// Test edge cases for get_parameters
TEST(StateTests, GetParametersEdgeCases) {
    State state;
    
    // Create a deep hierarchy with parameters at multiple levels
    state.set("a.b.c.d.param1", 1);
    state.set("a.b.c.param2", 2);
    state.set("a.b.param3", 3);
    state.set("a.param4", 4);
    
    // Get the groups
    StateGroup* group_a = state.get_group("a");
    StateGroup* group_b = group_a->get_group("b");
    StateGroup* group_c = group_b->get_group("c");
    StateGroup* group_d = group_c->get_group("d");
    
    // Check parameters at each level
    std::map<std::string, Parameter> params_a = group_a->get_parameters();
    std::map<std::string, Parameter> params_b = group_b->get_parameters();
    std::map<std::string, Parameter> params_c = group_c->get_parameters();
    std::map<std::string, Parameter> params_d = group_d->get_parameters();
    
    EXPECT_EQ(4, params_a.size()); // All parameters in hierarchy
    EXPECT_EQ(3, params_b.size()); // param3 and below
    EXPECT_EQ(2, params_c.size()); // param2 and param1
    EXPECT_EQ(1, params_d.size()); // Only param1
    
    // Test with special characters in parameter names
    state.set("special.param-with-dashes", 1);
    state.set("special.param_with_underscores", 2);
    state.set("special.param.with.dots", 3); // This creates a deeper hierarchy
    
    StateGroup* special_group = state.get_group("special");
    std::map<std::string, Parameter> special_params = special_group->get_parameters();
    
    EXPECT_EQ(3, special_params.size());
    EXPECT_TRUE(special_params.find("special.param-with-dashes") != special_params.end());
    EXPECT_TRUE(special_params.find("special.param_with_underscores") != special_params.end());
    
    // Test with empty group (no direct parameters)
    StateGroup* empty_parent = state.create_group("empty_parent");
    empty_parent->create_group("child")->set("param", 100);
    
    std::map<std::string, Parameter> empty_parent_params = empty_parent->get_parameters();
    EXPECT_EQ(1, empty_parent_params.size()); // Contains only the child parameter
    EXPECT_TRUE(empty_parent_params.find("empty_parent.child.param") != empty_parent_params.end());
}
