#include "tanh/state/State.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <tanh/core/Numbers.h>
#include <thread>
#include <vector>
#include <future>

using namespace thl;

// =============================================================================
// Core parameter operations
// =============================================================================

TEST(StateTests, BasicParameterOperations) {
    State state;

    // Test empty state
    EXPECT_TRUE(state.is_empty());

    // Test creating and getting parameters of different types
    state.create("double_param", std::numbers::pi);
    state.create("float_param", std::numbers::e_v<float>);
    state.create("int_param", 42);
    state.create("bool_param", true);
    state.create("string_param", "hello world");                // Short string falls within SSO
                                                                // buffer
    state.create("string_param_long", std::string(1000, 'a'));  // Long string
                                                                // exceeds SSO
                                                                // buffer

    // Test that the state is no longer empty
    EXPECT_FALSE(state.is_empty());

    // Test retrieving parameters with correct types
    EXPECT_DOUBLE_EQ(std::numbers::pi, state.get<double>("double_param"));
    EXPECT_FLOAT_EQ(std::numbers::e_v<float>, state.get<float>("float_param"));
    EXPECT_EQ(42, state.get<int>("int_param"));
    EXPECT_TRUE(state.get<bool>("bool_param"));
    EXPECT_EQ("hello world", state.get<std::string>("string_param", true));
    EXPECT_EQ(std::string(1000, 'a'),
              state.get<std::string>("string_param_long", true));  // May block
                                                                   // due to
                                                                   // heap
                                                                   // allocation

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

    // Create a parameter with a hierarchical path
    state.create("audio.mixer.volume", 0.75);

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

    // Disable RTSan for exception tests, as the desctructor of the exception
    // triggers RTSan (can't be scope disabled, because it happens during stack
    // unwinding) Also using __rtsan_disable() shall not be used in the
    // destructor as this will disable RTSan globally for the thread, and
    // __rtsan_enable() would mark all following code as non-real-time safe The
    // only solution could be a rtsan suppression file TODO: add suppression
    // file for tests
#ifndef TANH_WITH_RTSAN
    EXPECT_THROW(
        {
            state.get<double>("audio.nonexistent", true);  // Blocking because
                                                           // of the exception
        },
        StateKeyNotFoundException);

    EXPECT_THROW(
        {
            audio_group->get<double>("nonexistent.volume", true);  // Blocking
                                                                   // because of
                                                                   // the
                                                                   // exception
        },
        StateGroupNotFoundException);
#endif

    // Create another parameter in a different hierarchy
    state.create("audio.effects.reverb.size", 0.5);

    // Verify the new groups were created
    EXPECT_TRUE(audio_group->has_group("effects"));

    StateGroup* effects_group = audio_group->get_group("effects");
    ASSERT_NE(nullptr, effects_group);
    EXPECT_TRUE(effects_group->has_group("reverb"));

    // Verify the parameter was correctly set
    EXPECT_DOUBLE_EQ(0.5, state.get<double>("audio.effects.reverb.size"));

    // Test multi-level paths being created in one go
    state.create("visual.display.brightness.level", 85);

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

    // Test creating parameters at different levels
    state.create("global", 1.0);
    audio_group->create("volume", 0.8);
    effects_group->create("enabled", true);
    reverb_group->create("mix", 0.3f);
    reverb_group->create("room_size", 0.7f);

    // Test creating parameters with paths
    state.create("audio.effects.delay.time", 500);
    state.create("audio.effects.delay.feedback", 0.4f);

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
    EXPECT_FALSE(effects_group->is_empty());  // Still has "enabled" param and
                                              // delay group

    // Test clearing the whole state
    state.clear();
    EXPECT_TRUE(state.is_empty());
}

// Type conversion tests
TEST(StateTests, TypeConversions) {
    State state;

    // Create parameters of different types
    state.create("number", 42.5);

    // Test numeric type conversions
    EXPECT_DOUBLE_EQ(42.5, state.get<double>("number"));
    EXPECT_FLOAT_EQ(42.5f, state.get<float>("number"));
    EXPECT_EQ(42, state.get<int>("number"));
    EXPECT_TRUE(state.get<bool>("number"));

    // Test string conversions
    state.create("string_true", "true");
    state.create("string_one", "1");
    state.create("string_number", "123.45");

    EXPECT_TRUE(state.get<bool>("string_true"));
    EXPECT_TRUE(state.get<bool>("string_one"));
    EXPECT_DOUBLE_EQ(123.45, state.get<double>("string_number"));
    EXPECT_FLOAT_EQ(123.45f, state.get<float>("string_number"));
    EXPECT_EQ(123, state.get<int>("string_number"));

    // Test invalid conversions
    state.create("invalid_number", "not a number");
    EXPECT_DOUBLE_EQ(0.0, state.get<double>("invalid_number"));
    EXPECT_FLOAT_EQ(0.0f, state.get<float>("invalid_number"));
    EXPECT_EQ(0, state.get<int>("invalid_number"));
    EXPECT_FALSE(state.get<bool>("invalid_number"));

    // Test bool to other types
    state.create("bool_true", true);
    state.create("bool_false", false);

    EXPECT_DOUBLE_EQ(1.0, state.get<double>("bool_true"));
    EXPECT_DOUBLE_EQ(0.0, state.get<double>("bool_false"));
    EXPECT_EQ(1, state.get<int>("bool_true"));
    EXPECT_EQ(0, state.get<int>("bool_false"));
    EXPECT_EQ("true", state.get<std::string>("bool_true", true));
    EXPECT_EQ("false", state.get<std::string>("bool_false", true));
}

// Parameter object tests
TEST(StateTests, ParameterObjectTests) {
    State state;

    // Create parameters of different types
    state.create("double_param", std::numbers::pi);
    state.create("int_param", 42);
    state.create("bool_param", true);
    state.create("string_param", "hello world");

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
    EXPECT_DOUBLE_EQ(std::numbers::pi, double_param.to<double>());
    EXPECT_EQ(42, int_param.to<int>());
    EXPECT_TRUE(bool_param.to<bool>());
    EXPECT_EQ("hello world", string_param.to<std::string>(true));  // Allow
                                                                   // blocking
                                                                   // for string
                                                                   // conversion

    // Test cross-type conversions
    EXPECT_FLOAT_EQ(std::numbers::pi_v<float>, double_param.to<float>());
    EXPECT_EQ(3, double_param.to<int>());
    EXPECT_TRUE(double_param.to<bool>());

    EXPECT_DOUBLE_EQ(42.0, int_param.to<double>());
    EXPECT_TRUE(int_param.to<bool>());
}

// Test for create/set separation
TEST(StateTests, CreateParameterFlag) {
    State state;

    // Test create() - creates the parameter
    state.create("audio.volume", 0.75);
    EXPECT_DOUBLE_EQ(0.75, state.get<double>("audio.volume"));
    EXPECT_TRUE(state.has_group("audio"));

    // Update an existing parameter with set()
    // This should work since the parameter already exists
    state.set("audio.volume", 0.5);
    EXPECT_DOUBLE_EQ(0.5, state.get<double>("audio.volume"));

    // Test set() on a non-existent parameter
    // This should throw StateGroupNotFoundException since the group doesn't exist
    EXPECT_THROW({ state.set("nonexistent.parameter", 100); }, StateGroupNotFoundException);

    // Test deep nesting with create()
    state.create("visual.display.brightness", 85);
    EXPECT_EQ(85, state.get<int>("visual.display.brightness"));

    // Test set() on a non-existent nested parameter (group exists but key doesn't)
    EXPECT_THROW(
        {
            // "color" doesn't exist under visual.display
            state.set("visual.display.color", "blue");
        },
        StateKeyNotFoundException);

    // Test set() on a non-existent group
    EXPECT_THROW(
        {
            // "effects" doesn't exist under audio
            state.set("audio.effects.reverb", 0.3);
        },
        StateGroupNotFoundException);

    // Create that missing group and parameter
    state.create("audio.effects.reverb", 0.3);
    EXPECT_DOUBLE_EQ(0.3, state.get<double>("audio.effects.reverb"));

    // Now it should work with set() since it exists
    state.set("audio.effects.reverb", 0.4);
    EXPECT_DOUBLE_EQ(0.4, state.get<double>("audio.effects.reverb"));
}

// =============================================================================
// Notification and listener tests
// =============================================================================

// Test class for parameter notifications
class TestParameterListener : public ParameterListener {
public:
    void on_parameter_changed(const Parameter& param) override {
        m_last_path = param.key();

        // Store the value based on the parameter type
        switch (param.get_type()) {
            case ParameterType::Double: m_last_value_double = param.to<double>(); break;
            case ParameterType::Float: m_last_value_double = param.to<float>(); break;
            case ParameterType::Int: m_last_value_double = param.to<int>(); break;
            case ParameterType::Bool: m_last_value_double = param.to<bool>() ? 1.0 : 0.0; break;
            default: m_last_value_double = 0.0; break;
        }

        m_notification_count++;
    }

    std::string m_last_path;
    double m_last_value_double = 0.0;
    int m_notification_count = 0;
};

// Test for parameter notification functionality
TEST(StateTests, ParameterNotification) {
    State state;
    TestParameterListener listener;

    // Register the listener
    state.add_listener(&listener);

    // Create a parameter - this should trigger notification
    state.create("test.param", 42.0);

    // Verify the listener was notified
    EXPECT_EQ(1, listener.m_notification_count);
    EXPECT_EQ("test.param", listener.m_last_path);

    // Update the parameter - set() now always notifies
    state.set("test.param", 84.0);

    // Verify notification occurred (set() always notifies now)
    EXPECT_EQ(2, listener.m_notification_count);

    // Value should be updated
    EXPECT_DOUBLE_EQ(84.0, state.get<double>("test.param"));

    // Test hierarchical notifications
    StateGroup* audio_group = state.create_group("audio");
    TestParameterListener group_listener;
    audio_group->add_listener(&group_listener);

    // Create parameter in audio group
    state.create("audio.volume", 0.75);

    // Root listener should be notified
    EXPECT_EQ(3, listener.m_notification_count);
    // Group listener should be notified
    EXPECT_EQ(1, group_listener.m_notification_count);
    EXPECT_EQ("audio.volume", group_listener.m_last_path);

    // Create another parameter (creation always notifies)
    state.create("audio.bass", 0.5);

    // Both listeners should be notified
    EXPECT_EQ(4, listener.m_notification_count);
    EXPECT_EQ(2, group_listener.m_notification_count);

    // Manually notify
    Parameter param = state.get_parameter("audio.bass");
    param.notify();

    // Verify notifications occurred
    EXPECT_EQ(5, listener.m_notification_count);
    EXPECT_EQ(3, group_listener.m_notification_count);
}

TEST(StateTests, CallbackBasedNotifications) {
    State state;
    int callback_count = 0;
    std::string last_path;
    last_path.reserve(256);
    double last_value = 0.0;

    // Add a callback listener
    CallbackListener cb([&](const Parameter& param) {
        callback_count++;
        last_path = param.key();
        last_value = param.to<double>();
    });
    state.add_listener(&cb);

    // Create parameter
    state.create("test.callback.param", 123.0);

    // Verify callback was invoked
    EXPECT_EQ(1, callback_count);
    EXPECT_EQ("test.callback.param", last_path);
    EXPECT_DOUBLE_EQ(123.0, last_value);

    // Remove callback listener
    state.remove_listener(&cb);

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

    // Create a parameter (creation always notifies now)
    state.create("manual.test", 42.0);

    // Verify notification occurred from create
    EXPECT_EQ(1, listener.m_notification_count);

    // Manually notify about the parameter
    state.notify_parameter_change("manual.test");

    // Verify second notification occurred
    EXPECT_EQ(2, listener.m_notification_count);
    EXPECT_EQ("manual.test", listener.m_last_path);
    EXPECT_DOUBLE_EQ(42.0, listener.m_last_value_double);

    // Test direct Parameter notify method
    Parameter param = state.get_parameter("manual.test");
    param.notify();

    // Verify third notification
    EXPECT_EQ(3, listener.m_notification_count);
}

// =============================================================================
// Get parameters tests
// =============================================================================

// Test for StateGroup::get_parameters() method
TEST(StateTests, GetParametersMethod) {
    State state;

    // Create an empty state and check that get_parameters returns an empty map
    std::map<std::string, Parameter> empty_params = state.get_parameters();
    EXPECT_TRUE(empty_params.empty());

    // Add parameters at root level
    state.create("root_param1", 42);
    state.create("root_param2", "root value");

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
    EXPECT_EQ("root value",
              param2_it->second.to<std::string>(true));  // Allow blocking for
                                                         // string conversion

    // Create a nested structure with parameters
    StateGroup* audio_group = state.create_group("audio");
    audio_group->create("volume", 0.8);
    audio_group->create("muted", false);

    // Check that root level parameters still match
    root_params = state.get_parameters();
    EXPECT_EQ(4, root_params.size());  // 2 root + 2 from audio group

    // Check audio group parameters
    std::map<std::string, Parameter> audio_params = audio_group->get_parameters();
    EXPECT_EQ(2, audio_params.size());

    auto volume_it = audio_params.find("audio.volume");
    EXPECT_NE(volume_it, audio_params.end());
    EXPECT_DOUBLE_EQ(0.8, volume_it->second.to<double>());

    auto muted_it = audio_params.find("audio.muted");
    EXPECT_NE(muted_it, audio_params.end());
    EXPECT_FALSE(muted_it->second.to<bool>());

    // Create a deeper nested structure
    StateGroup* effects_group = audio_group->create_group("effects");
    effects_group->create("reverb", 0.5);
    effects_group->create("delay", 0.3);
    effects_group->create("chorus", 0.2);

    // Check effects group parameters
    std::map<std::string, Parameter> effects_params = effects_group->get_parameters();
    EXPECT_EQ(3, effects_params.size());
    EXPECT_TRUE(effects_params.find("audio.effects.reverb") != effects_params.end());
    EXPECT_TRUE(effects_params.find("audio.effects.delay") != effects_params.end());
    EXPECT_TRUE(effects_params.find("audio.effects.chorus") != effects_params.end());
    EXPECT_DOUBLE_EQ(0.5, effects_params.at("audio.effects.reverb").to<double>());

    // Check that audio group parameters now include effects parameters
    audio_params = audio_group->get_parameters();
    EXPECT_EQ(5, audio_params.size());  // 2 in audio + 3 in effects
    EXPECT_TRUE(audio_params.find("audio.effects.reverb") != audio_params.end());

    // Add another group with parameters
    StateGroup* visual_group = state.create_group("visual");
    visual_group->create("brightness", 75);
    visual_group->create("contrast", 100);

    // Check visual group parameters
    std::map<std::string, Parameter> visual_params = visual_group->get_parameters();
    EXPECT_EQ(2, visual_params.size());

    // Root should now have all parameters
    root_params = state.get_parameters();
    EXPECT_EQ(9, root_params.size());  // 2 root + 2 audio + 3 effects + 2
                                       // visual

    // Clear one group and verify its parameters are removed
    effects_group->clear();
    audio_params = audio_group->get_parameters();
    EXPECT_EQ(2, audio_params.size());  // Only the direct audio parameters
                                        // remain

    // Clear the entire state
    state.clear();
    root_params = state.get_parameters();
    EXPECT_TRUE(root_params.empty());
}

// Test edge cases for get_parameters
TEST(StateTests, GetParametersEdgeCases) {
    State state;

    // Create a deep hierarchy with parameters at multiple levels
    state.create("a.b.c.d.param1", 1);
    state.create("a.b.c.param2", 2);
    state.create("a.b.param3", 3);
    state.create("a.param4", 4);

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

    EXPECT_EQ(4, params_a.size());  // All parameters in hierarchy
    EXPECT_EQ(3, params_b.size());  // param3 and below
    EXPECT_EQ(2, params_c.size());  // param2 and param1
    EXPECT_EQ(1, params_d.size());  // Only param1

    // Test with special characters in parameter names
    state.create("special.param-with-dashes", 1);
    state.create("special.param_with_underscores", 2);
    state.create("special.param.with.dots", 3);  // This creates a deeper hierarchy

    StateGroup* special_group = state.get_group("special");
    std::map<std::string, Parameter> special_params = special_group->get_parameters();

    EXPECT_EQ(3, special_params.size());
    EXPECT_TRUE(special_params.find("special.param-with-dashes") != special_params.end());
    EXPECT_TRUE(special_params.find("special.param_with_underscores") != special_params.end());

    // Test with empty group (no direct parameters)
    StateGroup* empty_parent = state.create_group("empty_parent");
    empty_parent->create_group("child")->create("param", 100);

    std::map<std::string, Parameter> empty_parent_params = empty_parent->get_parameters();
    EXPECT_EQ(1, empty_parent_params.size());  // Contains only the child
                                               // parameter
    EXPECT_TRUE(empty_parent_params.find("empty_parent.child.param") != empty_parent_params.end());
}

// =============================================================================
// JSON update tests
// =============================================================================

// Test for JSON state updates
TEST(StateTests, UpdateFromJson) {
    State state;

    // Set up initial state
    state.create("volume", 0.5);
    state.create("muted", false);
    state.create("name", "default device");

    // Create a group for EQ settings
    StateGroup* eq = state.create_group("eq");
    eq->create("bass", 5);
    eq->create("treble", 3);

    // Basic JSON update
    nlohmann::json simple_update = {{"volume", 0.8}, {"muted", true}};

    state.update_from_json(simple_update);

    // Verify the updated values
    EXPECT_DOUBLE_EQ(0.8, state.get<double>("volume"));
    EXPECT_TRUE(state.get<bool>("muted"));
    EXPECT_EQ("default device", state.get<std::string>("name", true));  // Unchanged

    // Test nested JSON update
    nlohmann::json nested_update = {{"eq", {{"bass", 7}, {"treble", 4}}}};

    state.update_from_json(nested_update);

    // Verify the nested updates
    EXPECT_EQ(7, state.get<int>("eq.bass"));
    EXPECT_EQ(4, state.get<int>("eq.treble"));

    // Test mixed update with different types
    nlohmann::json mixed_update = {{"name", "new device"}, {"eq", {{"bass", 10}}}};

    state.update_from_json(mixed_update);

    EXPECT_EQ("new device", state.get<std::string>("name", true));
    EXPECT_EQ(10, state.get<int>("eq.bass"));
    EXPECT_EQ(4, state.get<int>("eq.treble"));  // Unchanged
}

// Test for JSON state updates with non-existent keys
TEST(StateTests, UpdateFromJsonNonExistentKey) {
    State state;

    // Set up initial state
    state.create("volume", 0.5);

    // JSON with non-existent key
    nlohmann::json invalid_update = {{"volume", 0.8}, {"non_existent", 42}};

    // The update should throw StateKeyNotFoundException
    EXPECT_THROW(
        {
            try {
                state.update_from_json(invalid_update);
            } catch (const StateKeyNotFoundException& e) {
                // Verify the error message contains the key name
                EXPECT_TRUE(std::string(e.what()).find("non_existent") != std::string::npos);
                EXPECT_EQ("non_existent", e.key());
                throw;
            }
        },
        StateKeyNotFoundException);

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

    reverb->create("wet", 0.3);
    reverb->create("dry", 0.7);
    reverb->create("room_size", 0.5);

    // Create a JSON with deep nesting
    nlohmann::json deep_update = {
        {"audio", {{"effects", {{"reverb", {{"wet", 0.4}, {"room_size", 0.8}}}}}}}};

    state.update_from_json(deep_update);

    // Verify the deep updates
    EXPECT_DOUBLE_EQ(0.4, state.get<double>("audio.effects.reverb.wet"));
    EXPECT_DOUBLE_EQ(0.8, state.get<double>("audio.effects.reverb.room_size"));
    EXPECT_DOUBLE_EQ(0.7,
                     state.get<double>("audio.effects.reverb.dry"));  // Unchanged
}

// Test for JSON state updates with different numeric types
TEST(StateTests, UpdateFromJsonNumericTypes) {
    State state;

    // Set up parameters of different types
    state.create("double_value", 1.0);
    state.create("int_value", 1);
    state.create("bool_value", false);

    // Update with numeric literals of different types
    nlohmann::json numeric_update = {
        {"double_value", 2},  // Integer literal to double parameter
        {"int_value", 2.5},   // Double literal to int parameter
        {"bool_value", 1}     // Integer literal to bool parameter
    };

    state.update_from_json(numeric_update);

    // Verify appropriate type conversions
    EXPECT_DOUBLE_EQ(2.0, state.get<double>("double_value"));
    EXPECT_EQ(2, state.get<int>("int_value"));   // Truncated to 2
    EXPECT_TRUE(state.get<bool>("bool_value"));  // Converted to true

    // Check that the stored types remain unchanged
    EXPECT_EQ(ParameterType::Double, state.get_parameter_type("double_value"));
    EXPECT_EQ(ParameterType::Int, state.get_parameter_type("int_value"));
    EXPECT_EQ(ParameterType::Bool, state.get_parameter_type("bool_value"));
}

// =============================================================================
// Parameter definition tests
// =============================================================================

// Test Range construction and conversions
TEST(StateTests, RangeConstructionAndConversion) {
    // Float range
    Range float_range(0.0f, 100.0f, 0.1f, 2.0f);
    EXPECT_FLOAT_EQ(0.0f, float_range.m_min);
    EXPECT_FLOAT_EQ(100.0f, float_range.m_max);
    EXPECT_FLOAT_EQ(0.1f, float_range.m_step);
    EXPECT_FLOAT_EQ(2.0f, float_range.m_skew);

    // Convert to int
    EXPECT_EQ(0, float_range.min_int());
    EXPECT_EQ(100, float_range.max_int());
    EXPECT_EQ(0, float_range.step_int());  // 0.1 rounds to 0

    // Int range
    Range int_range(0, 127, 1);
    EXPECT_FLOAT_EQ(0.0f, int_range.m_min);
    EXPECT_FLOAT_EQ(127.0f, int_range.m_max);
    EXPECT_FLOAT_EQ(1.0f, int_range.m_step);
    EXPECT_FLOAT_EQ(1.0f, int_range.m_skew);  // Linear by default

    EXPECT_EQ(0, int_range.min_int());
    EXPECT_EQ(127, int_range.max_int());
    EXPECT_EQ(1, int_range.step_int());

    // Bool range
    Range bool_range = Range::boolean();
    EXPECT_FLOAT_EQ(0.0f, bool_range.m_min);
    EXPECT_FLOAT_EQ(1.0f, bool_range.m_max);
    EXPECT_FLOAT_EQ(1.0f, bool_range.m_step);
    EXPECT_EQ(0, bool_range.min_int());
    EXPECT_EQ(1, bool_range.max_int());
}

// Test basic ParameterFloat definition
TEST(StateTests, ParameterDefinitionFloat) {
    State state;

    // Create a float parameter with definition
    ParameterFloat volume_def("Volume",
                              Range(0.0f, 1.0f, 0.01f, 1.0f),
                              0.75f,
                              2,     // 2 decimal places
                              true,  // automation
                              true   // modulation
    );

    // Create the parameter with definition
    state.create("audio.volume", volume_def);

    // Verify the value was set correctly
    EXPECT_FLOAT_EQ(0.75f, state.get<float>("audio.volume"));

    // Get the parameter and check its definition
    Parameter param = state.get_parameter("audio.volume");
    const auto& def = param.def();

    EXPECT_EQ("Volume", def.m_name);
    EXPECT_EQ(ParameterType::Float, def.m_type);
    EXPECT_FLOAT_EQ(0.0f, def.m_range.m_min);
    EXPECT_FLOAT_EQ(1.0f, def.m_range.m_max);
    EXPECT_FLOAT_EQ(0.01f, def.m_range.m_step);
    EXPECT_FLOAT_EQ(1.0f, def.m_range.m_skew);
    EXPECT_FLOAT_EQ(0.75f, def.m_default_value);
    EXPECT_EQ(2, def.m_decimal_places);
    EXPECT_TRUE(def.is_automatable());
    EXPECT_TRUE(def.is_modulatable());
}

// Test basic ParameterInt definition
TEST(StateTests, ParameterDefinitionInt) {
    State state;

    // Create an int parameter with definition
    ParameterInt filter_cutoff_def("Filter Cutoff",
                                   Range(20, 20000, 1),
                                   1000,
                                   true,  // automation
                                   true   // modulation
    );

    // Create the parameter with definition
    state.create("synth.filter.cutoff", filter_cutoff_def);

    // Verify the value was set correctly
    EXPECT_EQ(1000, state.get<int>("synth.filter.cutoff"));

    // Get the parameter and check its definition
    Parameter param = state.get_parameter("synth.filter.cutoff");
    const auto& def = param.def();

    EXPECT_EQ("Filter Cutoff", def.m_name);
    EXPECT_EQ(ParameterType::Int, def.m_type);
    EXPECT_EQ(20, def.m_range.min_int());
    EXPECT_EQ(20000, def.m_range.max_int());
    EXPECT_EQ(1, def.m_range.step_int());
    EXPECT_EQ(1000, def.as_int());
    EXPECT_EQ(0, def.m_decimal_places);  // Int should have 0 decimal places
    EXPECT_TRUE(def.is_automatable());
    EXPECT_TRUE(def.is_modulatable());
}

// Test basic ParameterBool definition
TEST(StateTests, ParameterDefinitionBool) {
    State state;

    // Create a bool parameter with definition
    ParameterBool bypass_def("Bypass",
                             false,  // default value
                             true,   // automation
                             false   // modulation (typically off for bools)
    );

    // Create the parameter with definition
    state.create("effects.reverb.bypass", bypass_def);

    // Verify the value was set correctly
    EXPECT_FALSE(state.get<bool>("effects.reverb.bypass"));

    // Get the parameter and check its definition
    Parameter param = state.get_parameter("effects.reverb.bypass");
    const auto& def = param.def();

    EXPECT_EQ("Bypass", def.m_name);
    EXPECT_EQ(ParameterType::Bool, def.m_type);
    EXPECT_EQ(0, def.m_range.min_int());
    EXPECT_EQ(1, def.m_range.max_int());
    EXPECT_FALSE(def.as_bool());
    EXPECT_EQ(0, def.m_decimal_places);
    EXPECT_TRUE(def.is_automatable());
    EXPECT_FALSE(def.is_modulatable());
}

// Test basic ParameterChoice definition
TEST(StateTests, ParameterDefinitionChoice) {
    State state;

    // Create a choice parameter with definition
    std::vector<std::string> waveforms = {"Sine", "Saw", "Square", "Triangle"};
    ParameterChoice waveform_def("Waveform",
                                 waveforms,
                                 0,     // default index
                                 true,  // automation
                                 false  // modulation
    );

    // Create the parameter with definition
    state.create("oscillator.waveform", waveform_def);

    // Verify the value was set correctly (as int index)
    EXPECT_EQ(0, state.get<int>("oscillator.waveform"));

    // Get the parameter and check its definition
    Parameter param = state.get_parameter("oscillator.waveform");
    const auto& def = param.def();

    EXPECT_EQ("Waveform", def.m_name);
    EXPECT_EQ(ParameterType::Int, def.m_type);
    EXPECT_EQ(0, def.m_range.min_int());
    EXPECT_EQ(3, def.m_range.max_int());  // 4 choices: 0-3
    EXPECT_EQ(1, def.m_range.step_int());
    EXPECT_EQ(0, def.as_int());
    EXPECT_EQ(4, def.m_choices.size());
    EXPECT_EQ("Sine", def.m_choices[0]);
    EXPECT_EQ("Saw", def.m_choices[1]);
    EXPECT_EQ("Square", def.m_choices[2]);
    EXPECT_EQ("Triangle", def.m_choices[3]);
    EXPECT_TRUE(def.is_automatable());
    EXPECT_FALSE(def.is_modulatable());
}

// Test type conversions in ParameterDefinition
TEST(StateTests, ParameterDefinitionTypeConversions) {
    // Float parameter
    ParameterFloat float_param("Test Float", Range(0.0f, 1.0f), 0.5f);
    EXPECT_FLOAT_EQ(0.5f, float_param.as_float());
    EXPECT_EQ(0, float_param.as_int());  // 0.5 truncates to 0
    EXPECT_TRUE(float_param.as_bool());  // 0.5 != 0.0, so true

    // Int parameter
    ParameterInt int_param("Test Int", Range(0, 100), 42);
    EXPECT_FLOAT_EQ(42.0f, int_param.as_float());
    EXPECT_EQ(42, int_param.as_int());
    EXPECT_TRUE(int_param.as_bool());  // Non-zero

    // Bool parameter (true)
    ParameterBool bool_param_true("Test Bool True", true);
    EXPECT_FLOAT_EQ(1.0f, bool_param_true.as_float());
    EXPECT_EQ(1, bool_param_true.as_int());
    EXPECT_TRUE(bool_param_true.as_bool());

    // Bool parameter (false)
    ParameterBool bool_param_false("Test Bool False", false);
    EXPECT_FLOAT_EQ(0.0f, bool_param_false.as_float());
    EXPECT_EQ(0, bool_param_false.as_int());
    EXPECT_FALSE(bool_param_false.as_bool());

    // Choice parameter
    std::vector<std::string> choices = {"A", "B", "C"};
    ParameterChoice choice_param("Test Choice", choices, 1);
    EXPECT_FLOAT_EQ(1.0f, choice_param.as_float());
    EXPECT_EQ(1, choice_param.as_int());
    EXPECT_TRUE(choice_param.as_bool());  // Index 1 is non-zero
}

// Test updating parameter value preserves definition
TEST(StateTests, ParameterDefinitionPersistence) {
    State state;

    // Create and set a parameter with definition
    ParameterFloat gain_def("Gain", Range(0.0f, 2.0f, 0.01f, 1.0f), 1.0f, 2, true, true);

    state.create("gain", gain_def);

    // Verify initial value
    EXPECT_FLOAT_EQ(1.0f, state.get<float>("gain"));

    // Update the parameter value without definition
    state.set("gain", 1.5f);

    // Verify the value was updated
    EXPECT_FLOAT_EQ(1.5f, state.get<float>("gain"));

    // Verify the definition is still present and unchanged
    Parameter param = state.get_parameter("gain");
    const auto& def = param.def();

    EXPECT_EQ("Gain", def.m_name);
    EXPECT_FLOAT_EQ(1.0f, def.m_default_value);  // Default should still be 1.0
    EXPECT_FLOAT_EQ(0.0f, def.m_range.m_min);
    EXPECT_FLOAT_EQ(2.0f, def.m_range.m_max);
}

// Test that create() throws on duplicate key
TEST(StateTests, CreateThrowsOnDuplicateKey) {
    State state;

    // Create initial parameter with definition
    state.create("param", ParameterFloat("Initial", Range(0.0f, 1.0f), 0.5f, 2));

    // Attempting to create again should throw
    EXPECT_THROW(state.create("param", ParameterFloat("Updated", Range(0.0f, 10.0f), 5.0f, 3)),
                 ParameterAlreadyExistsException);

    // Original definition is preserved
    const auto& def = state.get_parameter("param").def();
    EXPECT_EQ("Initial", def.m_name);
    EXPECT_FLOAT_EQ(0.5f, def.m_default_value);
}

// Test multiple parameters with definitions
TEST(StateTests, MultipleParameterDefinitions) {
    State state;

    // Create a complex audio effect with multiple parameters
    state.create("reverb.dry_wet", ParameterFloat("Dry/Wet", Range(0.0f, 1.0f), 0.3f, 2));
    state.create("reverb.room_size", ParameterFloat("Room Size", Range(0.0f, 1.0f), 0.5f, 2));
    state.create("reverb.damping", ParameterFloat("Damping", Range(0.0f, 1.0f), 0.5f, 2));
    state.create("reverb.enabled", ParameterBool("Enabled", true));

    std::vector<std::string> room_types = {"Small", "Medium", "Large", "Hall"};
    state.create("reverb.type", ParameterChoice("Room Type", room_types, 1));

    // Verify all values
    EXPECT_FLOAT_EQ(0.3f, state.get<float>("reverb.dry_wet"));
    EXPECT_FLOAT_EQ(0.5f, state.get<float>("reverb.room_size"));
    EXPECT_FLOAT_EQ(0.5f, state.get<float>("reverb.damping"));
    EXPECT_TRUE(state.get<bool>("reverb.enabled"));
    EXPECT_EQ(1, state.get<int>("reverb.type"));

    // Verify all definitions exist (every parameter has a definition now)
    EXPECT_EQ("Dry/Wet", state.get_parameter("reverb.dry_wet").def().m_name);
    EXPECT_EQ("Room Size", state.get_parameter("reverb.room_size").def().m_name);
    EXPECT_EQ("Damping", state.get_parameter("reverb.damping").def().m_name);
    EXPECT_EQ("Enabled", state.get_parameter("reverb.enabled").def().m_name);
    EXPECT_EQ("Room Type", state.get_parameter("reverb.type").def().m_name);

    // Check specific definition properties
    const auto& type_def = state.get_parameter("reverb.type").def();
    EXPECT_EQ("Room Type", type_def.m_name);
    EXPECT_EQ(4, type_def.m_choices.size());
    EXPECT_EQ("Medium", type_def.m_choices[1]);
}

// Test parameter with default definition (value-created)
TEST(StateTests, ParameterWithDefaultDefinition) {
    State state;

    // Create a parameter with just a value (no explicit definition)
    state.create("simple.param", 42.0);

    // Verify the value is set
    EXPECT_DOUBLE_EQ(42.0, state.get<double>("simple.param"));

    // Value-created parameters have a default definition with empty name
    Parameter param = state.get_parameter("simple.param");
    const auto& def = param.def();
    EXPECT_TRUE(def.m_name.empty());
    EXPECT_EQ(ParameterType::Double, def.m_type);
}

// Test choice parameter with actual usage
TEST(StateTests, ChoiceParameterUsage) {
    State state;

    // Create a filter type parameter
    std::vector<std::string> filter_types = {"Low Pass", "High Pass", "Band Pass", "Notch"};

    ParameterChoice filter_type_def("Filter Type",
                                    filter_types,
                                    0  // Default to Low Pass
    );

    state.create("filter.type", filter_type_def);

    // Get the definition
    const auto& def = state.get_parameter("filter.type").def();

    // Verify range is correct
    EXPECT_EQ(0, def.m_range.min_int());
    EXPECT_EQ(3, def.m_range.max_int());

    // Verify choices
    EXPECT_EQ(4, def.m_choices.size());
    EXPECT_EQ("Low Pass", def.m_choices[0]);
    EXPECT_EQ("High Pass", def.m_choices[1]);
    EXPECT_EQ("Band Pass", def.m_choices[2]);
    EXPECT_EQ("Notch", def.m_choices[3]);

    // Change the value
    state.set("filter.type", 2);  // Select Band Pass
    EXPECT_EQ(2, state.get<int>("filter.type"));

    // Definition should still be intact
    const auto& def2 = state.get_parameter("filter.type").def();
    EXPECT_EQ("Band Pass", def2.m_choices[2]);
}

// Test automation and modulation flags
TEST(StateTests, AutomationModulationFlags) {
    State state;

    // Parameter with automation but no modulation
    state.create("param1", ParameterFloat("Param 1", Range(0.0f, 1.0f), 0.5f, 2, true, false));

    // Parameter with both
    state.create("param2", ParameterFloat("Param 2", Range(0.0f, 1.0f), 0.5f, 2, true, true));

    // Parameter with neither
    state.create("param3", ParameterFloat("Param 3", Range(0.0f, 1.0f), 0.5f, 2, false, false));

    // Check param1
    const auto& def1 = state.get_parameter("param1").def();
    EXPECT_TRUE(def1.is_automatable());
    EXPECT_FALSE(def1.is_modulatable());

    // Check param2
    const auto& def2 = state.get_parameter("param2").def();
    EXPECT_TRUE(def2.is_automatable());
    EXPECT_TRUE(def2.is_modulatable());

    // Check param3
    const auto& def3 = state.get_parameter("param3").def();
    EXPECT_FALSE(def3.is_automatable());
    EXPECT_FALSE(def3.is_modulatable());
}

// Test slider polarity
TEST(StateTests, SliderPolarityFlags) {
    State state;

    // Float parameter with unipolar polarity (default)
    state.create("unipolar_float", ParameterFloat("Unipolar Float", Range(0.0f, 1.0f), 0.5f));

    // Float parameter with bipolar polarity (explicit)
    state.create("bipolar_float",
                 ParameterFloat("Bipolar Float",
                                Range(-1.0f, 1.0f),
                                0.0f,
                                2,
                                true,
                                true,
                                SliderPolarity::Bipolar));

    // Int parameter with bipolar polarity
    state.create(
        "bipolar_int",
        ParameterInt("Bipolar Int", Range(-12, 12), 0, true, true, SliderPolarity::Bipolar));

    // Check unipolar float (should default to Unipolar)
    const auto& def1 = state.get_parameter("unipolar_float").def();
    EXPECT_EQ(SliderPolarity::Unipolar, def1.m_polarity);

    // Check bipolar float
    const auto& def2 = state.get_parameter("bipolar_float").def();
    EXPECT_EQ(SliderPolarity::Bipolar, def2.m_polarity);

    // Check bipolar int
    const auto& def3 = state.get_parameter("bipolar_int").def();
    EXPECT_EQ(SliderPolarity::Bipolar, def3.m_polarity);

    // Bool parameter (should default to Unipolar)
    state.create("bool_param", ParameterBool("Bool Param", false));
    const auto& def4 = state.get_parameter("bool_param").def();
    EXPECT_EQ(SliderPolarity::Unipolar, def4.m_polarity);

    // Choice parameter (should default to Unipolar)
    std::vector<std::string> choices = {"A", "B", "C"};
    state.create("choice_param", ParameterChoice("Choice Param", choices, 0));
    const auto& def5 = state.get_parameter("choice_param").def();
    EXPECT_EQ(SliderPolarity::Unipolar, def5.m_polarity);
}

// Test data field for all parameter types
TEST(StateTests, ParameterRecordField) {
    State state;

    // Float parameter with custom data
    std::vector<std::string> float_data = {"unit:dB", "display:log"};
    state.create("gain_db",
                 ParameterFloat("Gain",
                                Range(-60.0f, 12.0f),
                                0.0f,
                                2,
                                true,
                                true,
                                SliderPolarity::Bipolar,
                                float_data));

    // Int parameter with custom data
    std::vector<std::string> int_data = {"midi:cc1", "channel:1"};
    state.create("midi_value",
                 ParameterInt("MIDI Value",
                              Range(0, 127),
                              64,
                              true,
                              true,
                              SliderPolarity::Unipolar,
                              int_data));

    // Bool parameter with custom data
    std::vector<std::string> bool_data = {"shortcut:space", "icon:play"};
    state.create("play_state",
                 ParameterBool("Play", false, true, false, SliderPolarity::Unipolar, bool_data));

    // Verify float parameter data
    const auto& float_def = state.get_parameter("gain_db").def();
    EXPECT_EQ(2, float_def.m_choices.size());
    EXPECT_EQ("unit:dB", float_def.m_choices[0]);
    EXPECT_EQ("display:log", float_def.m_choices[1]);
    EXPECT_EQ(SliderPolarity::Bipolar, float_def.m_polarity);

    // Verify int parameter data
    const auto& int_def = state.get_parameter("midi_value").def();
    EXPECT_EQ(2, int_def.m_choices.size());
    EXPECT_EQ("midi:cc1", int_def.m_choices[0]);
    EXPECT_EQ("channel:1", int_def.m_choices[1]);

    // Verify bool parameter data
    const auto& bool_def = state.get_parameter("play_state").def();
    EXPECT_EQ(2, bool_def.m_choices.size());
    EXPECT_EQ("shortcut:space", bool_def.m_choices[0]);
    EXPECT_EQ("icon:play", bool_def.m_choices[1]);

    // Choice parameter already uses data for choices - verify it still works
    std::vector<std::string> waveforms = {"Sine", "Saw", "Square"};
    state.create("waveform", ParameterChoice("Waveform", waveforms, 0));

    const auto& choice_def = state.get_parameter("waveform").def();
    EXPECT_EQ(3, choice_def.m_choices.size());
    EXPECT_EQ("Sine", choice_def.m_choices[0]);
    EXPECT_EQ("Saw", choice_def.m_choices[1]);
    EXPECT_EQ("Square", choice_def.m_choices[2]);
}

// =============================================================================
// State dump tests
// =============================================================================

// Test empty state dump
TEST(StateTests, EmptyStateDump) {
    State state;

    std::string dump = state.get_state_dump();
    nlohmann::json json_dump = nlohmann::json::parse(dump);

    EXPECT_TRUE(json_dump.is_array());
    EXPECT_TRUE(json_dump.empty());
}

// Test state dump with parameter definitions
TEST(StateTests, StateDumpWithDefinitions) {
    State state;

    // Create parameters with definitions
    state.create("synth.volume",
                 ParameterFloat("Volume", Range(0.0f, 1.0f, 0.01f, 1.0f), 0.75f, 2, true, true));
    state.create("synth.pitch", ParameterInt("Pitch", Range(-12, 12, 1), 0, true, false));
    state.create("synth.enabled", ParameterBool("Enabled", true, true, false));

    std::vector<std::string> waveforms = {"Sine", "Saw", "Square"};
    state.create("synth.waveform", ParameterChoice("Waveform", waveforms, 1, true, false));

    // Create a parameter without named definition (value-created)
    state.create("synth.internal_state", 42);

    // Get state dump
    std::string dump = state.get_state_dump();

    // Parse JSON
    nlohmann::json json_dump = nlohmann::json::parse(dump);

    // Verify it's an array
    EXPECT_TRUE(json_dump.is_array());
    EXPECT_EQ(5, json_dump.size());

    // Find and verify each parameter
    for (const auto& param_obj : json_dump) {
        EXPECT_TRUE(param_obj.contains("key"));
        EXPECT_TRUE(param_obj.contains("value"));

        std::string key = param_obj["key"];

        if (key == "synth.volume") {
            // Check value
            EXPECT_FLOAT_EQ(0.75f, param_obj["value"].get<float>());

            // Check definition exists
            ASSERT_TRUE(param_obj.contains("definition"));
            auto def = param_obj["definition"];

            EXPECT_EQ("Volume", def["name"]);
            EXPECT_EQ("float", def["type"]);
            EXPECT_FLOAT_EQ(0.0f, def["min"].get<float>());
            EXPECT_FLOAT_EQ(1.0f, def["max"].get<float>());
            EXPECT_FLOAT_EQ(0.01f, def["step"].get<float>());
            EXPECT_FLOAT_EQ(1.0f, def["skew"].get<float>());
            EXPECT_FLOAT_EQ(0.75f, def["default_value"].get<float>());
            EXPECT_EQ(2, def["decimal_places"].get<int>());
            EXPECT_TRUE(def["automation"].get<bool>());
            EXPECT_TRUE(def["modulation"].get<bool>());
            EXPECT_EQ("unipolar", def["slider_polarity"].get<std::string>());
        } else if (key == "synth.pitch") {
            // Check value
            EXPECT_EQ(0, param_obj["value"].get<int>());

            // Check definition
            ASSERT_TRUE(param_obj.contains("definition"));
            auto def = param_obj["definition"];

            EXPECT_EQ("Pitch", def["name"]);
            EXPECT_EQ("int", def["type"]);
            EXPECT_FLOAT_EQ(-12.0f, def["min"].get<float>());
            EXPECT_FLOAT_EQ(12.0f, def["max"].get<float>());
            EXPECT_FLOAT_EQ(1.0f, def["step"].get<float>());
            EXPECT_EQ(0, def["default_value"].get<int>());
            EXPECT_EQ(0, def["decimal_places"].get<int>());
            EXPECT_TRUE(def["automation"].get<bool>());
            EXPECT_FALSE(def["modulation"].get<bool>());
            EXPECT_EQ("unipolar", def["slider_polarity"].get<std::string>());
        } else if (key == "synth.enabled") {
            // Check value
            EXPECT_TRUE(param_obj["value"].get<bool>());

            // Check definition
            ASSERT_TRUE(param_obj.contains("definition"));
            auto def = param_obj["definition"];

            EXPECT_EQ("Enabled", def["name"]);
            EXPECT_EQ("bool", def["type"]);
            EXPECT_FLOAT_EQ(0.0f, def["min"].get<float>());
            EXPECT_FLOAT_EQ(1.0f, def["max"].get<float>());
            EXPECT_TRUE(def["automation"].get<bool>());
            EXPECT_FALSE(def["modulation"].get<bool>());
            EXPECT_EQ("unipolar", def["slider_polarity"].get<std::string>());
        } else if (key == "synth.waveform") {
            // Check value
            EXPECT_EQ(1, param_obj["value"].get<int>());

            // Check definition
            ASSERT_TRUE(param_obj.contains("definition"));
            auto def = param_obj["definition"];

            EXPECT_EQ("Waveform", def["name"]);
            EXPECT_EQ("choice", def["type"]);
            EXPECT_FLOAT_EQ(0.0f, def["min"].get<float>());
            EXPECT_FLOAT_EQ(2.0f, def["max"].get<float>());

            // Check choice data
            ASSERT_TRUE(def.contains("data"));
            EXPECT_EQ(3, def["data"].size());
            EXPECT_EQ("Sine", def["data"][0]);
            EXPECT_EQ("Saw", def["data"][1]);
            EXPECT_EQ("Square", def["data"][2]);
            EXPECT_EQ("unipolar", def["slider_polarity"].get<std::string>());
        } else if (key == "synth.internal_state") {
            // Check value
            EXPECT_EQ(42, param_obj["value"].get<int>());

            // Should NOT have definition (empty name = no definition in dump)
            EXPECT_FALSE(param_obj.contains("definition"));
        }
    }
}

// Test state dump with mixed parameters
TEST(StateTests, StateDumpMixedParameters) {
    State state;

    // Some with definitions
    state.create("audio.gain", ParameterFloat("Gain", Range(0.0f, 2.0f), 1.0f, 2));

    // Some without
    state.create("audio.sample_rate", 44100);
    state.create("audio.buffer_size", 512);

    // Get dump
    std::string dump = state.get_state_dump();
    nlohmann::json json_dump = nlohmann::json::parse(dump);

    // Verify structure
    EXPECT_TRUE(json_dump.is_array());
    EXPECT_EQ(3, json_dump.size());

    // Count parameters with and without definitions
    int with_def = 0;
    int without_def = 0;

    for (const auto& param : json_dump) {
        if (param.contains("definition")) {
            with_def++;
        } else {
            without_def++;
        }
    }

    EXPECT_EQ(1, with_def);
    EXPECT_EQ(2, without_def);
}

// Test slider polarity in JSON dump
TEST(StateTests, SliderPolarityInJsonDump) {
    State state;

    // Create parameters with different polarities
    state.create("unipolar_param", ParameterFloat("Unipolar", Range(0.0f, 1.0f), 0.5f));
    state.create("bipolar_param",
                 ParameterFloat("Bipolar",
                                Range(-1.0f, 1.0f),
                                0.0f,
                                2,
                                true,
                                true,
                                SliderPolarity::Bipolar));

    // Get state dump
    std::string dump = state.get_state_dump();
    nlohmann::json json_dump = nlohmann::json::parse(dump);

    // Find and verify each parameter
    for (const auto& param_obj : json_dump) {
        std::string key = param_obj["key"];

        if (key == "unipolar_param") {
            ASSERT_TRUE(param_obj.contains("definition"));
            auto def = param_obj["definition"];
            EXPECT_EQ("unipolar", def["slider_polarity"].get<std::string>());
        } else if (key == "bipolar_param") {
            ASSERT_TRUE(param_obj.contains("definition"));
            auto def = param_obj["definition"];
            EXPECT_EQ("bipolar", def["slider_polarity"].get<std::string>());
        }
    }
}

TEST(StateTests, GetStateDumpWithDefinitions) {
    State state;
    state.create("synth.osc.frequency",
                 ParameterFloat("Frequency", Range(20.0f, 20000.0f, 1.0f, 0.3f), 440.0f));
    state.create("synth.osc.volume", ParameterFloat("Volume", Range(0.0f, 1.0f, 0.01f), 0.8f));
    state.create("synth.filter.cutoff",
                 ParameterFloat("Cutoff", Range(20.0f, 20000.0f, 1.0f), 1000.0f));

    auto dump = state.get_state_dump(true);
    auto json = nlohmann::json::parse(dump);

    EXPECT_EQ(3, json.size());
    for (const auto& entry : json) {
        EXPECT_TRUE(entry.contains("key"));
        EXPECT_TRUE(entry.contains("value"));
        EXPECT_TRUE(entry.contains("definition"));
        const auto& def = entry["definition"];
        EXPECT_TRUE(def.contains("name"));
        EXPECT_TRUE(def.contains("min"));
        EXPECT_TRUE(def.contains("max"));
    }
}

TEST(StateTests, GetStateDumpWithoutDefinitions) {
    State state;
    state.create("synth.osc.frequency",
                 ParameterFloat("Frequency", Range(20.0f, 20000.0f, 1.0f), 440.0f));
    state.create("synth.osc.volume", ParameterFloat("Volume", Range(0.0f, 1.0f, 0.01f), 0.8f));

    auto dump = state.get_state_dump(false);
    auto json = nlohmann::json::parse(dump);

    EXPECT_EQ(2, json.size());
    for (const auto& entry : json) {
        EXPECT_TRUE(entry.contains("key"));
        EXPECT_TRUE(entry.contains("value"));
        EXPECT_FALSE(entry.contains("definition"));
    }
}

TEST(StateTests, GetStateDumpDefaultIncludesDefinitions) {
    State state;
    state.create("param", ParameterFloat("Param", Range(0.0f, 1.0f, 0.01f), 0.5f));

    auto dump = state.get_state_dump();  // no arg = include definitions
    auto json = nlohmann::json::parse(dump);
    EXPECT_TRUE(json[0].contains("definition"));
}

TEST(StateTests, GetStateDumpPreservesAllValueTypes) {
    State state;
    state.create("f", 3.14f);
    state.create("d", std::numbers::e);
    state.create("i", 42);
    state.create("b", true);
    state.create("s", std::string("hello"));

    auto dump = state.get_state_dump(false);
    auto json = nlohmann::json::parse(dump);

    EXPECT_EQ(5, json.size());

    std::map<std::string, nlohmann::json> entries;
    for (const auto& e : json) { entries[e["key"]] = e["value"]; }

    EXPECT_NEAR(3.14f, entries["f"].get<float>(), 0.001f);
    EXPECT_NEAR(std::numbers::e, entries["d"].get<double>(), 0.00001);
    EXPECT_EQ(42, entries["i"].get<int>());
    EXPECT_TRUE(entries["b"].get<bool>());
    EXPECT_EQ("hello", entries["s"].get<std::string>());
}

TEST(StateTests, GetStateDumpRoundTrip) {
    State state;
    state.create("synth.freq", 440.0f);
    state.create("synth.vol", 0.8f);
    state.create("mixer.vol", 0.5f);

    auto dump = state.get_state_dump(false);
    auto json = nlohmann::json::parse(dump);

    state.clear();
    EXPECT_TRUE(state.is_empty());

    for (const auto& entry : json) {
        std::string key = entry["key"];
        float value = entry["value"];
        state.create(key, value);
    }

    EXPECT_FLOAT_EQ(440.0f, state.get<float>("synth.freq"));
    EXPECT_FLOAT_EQ(0.8f, state.get<float>("synth.vol"));
    EXPECT_FLOAT_EQ(0.5f, state.get<float>("mixer.vol"));
}

TEST(StateTests, GetStateDumpDefinitionValues) {
    State state;
    state.create(
        "synth.freq",
        ParameterFloat("Frequency", Range(20.0f, 20000.0f, 1.0f, 0.3f), 440.0f, 1, true, true));

    // With definitions: verify actual values
    auto dump_with = state.get_state_dump(true);
    auto json_with = nlohmann::json::parse(dump_with);
    ASSERT_EQ(1, json_with.size());

    const auto& entry = json_with[0];
    EXPECT_EQ("synth.freq", entry["key"].get<std::string>());
    EXPECT_NEAR(440.0f, entry["value"].get<float>(), 0.01f);

    ASSERT_TRUE(entry.contains("definition"));
    const auto& def = entry["definition"];
    EXPECT_EQ("Frequency", def["name"].get<std::string>());
    EXPECT_EQ("float", def["type"].get<std::string>());
    EXPECT_NEAR(20.0f, def["min"].get<float>(), 0.01f);
    EXPECT_NEAR(20000.0f, def["max"].get<float>(), 0.01f);
    EXPECT_NEAR(1.0f, def["step"].get<float>(), 0.01f);
    EXPECT_NEAR(0.3f, def["skew"].get<float>(), 0.01f);
    EXPECT_NEAR(440.0f, def["default_value"].get<float>(), 0.01f);
    EXPECT_EQ(1, def["decimal_places"].get<int>());
    EXPECT_TRUE(def["automation"].get<bool>());
    EXPECT_TRUE(def["modulation"].get<bool>());
    EXPECT_EQ("unipolar", def["slider_polarity"].get<std::string>());

    // Without definitions: verify no definition key at all
    auto dump_without = state.get_state_dump(false);
    auto json_without = nlohmann::json::parse(dump_without);
    ASSERT_EQ(1, json_without.size());
    EXPECT_EQ("synth.freq", json_without[0]["key"].get<std::string>());
    EXPECT_NEAR(440.0f, json_without[0]["value"].get<float>(), 0.01f);
    EXPECT_FALSE(json_without[0].contains("definition"));
}

TEST(StateTests, GetStateDumpDefinitionChoiceAndBipolar) {
    State state;
    state.create("synth.model", ParameterChoice("Model", {"Saw", "Square", "Sine"}, 0));
    state.create("synth.pan",
                 ParameterFloat("Pan",
                                Range(0.0f, 1.0f, 0.01f),
                                0.5f,
                                2,
                                true,
                                true,
                                SliderPolarity::Bipolar));

    auto dump = state.get_state_dump(true);
    auto json = nlohmann::json::parse(dump);
    ASSERT_EQ(2, json.size());

    // Find entries by key
    std::map<std::string, nlohmann::json> entries;
    for (const auto& e : json) { entries[e["key"]] = e; }

    // Choice param
    const auto& model_def = entries["synth.model"]["definition"];
    EXPECT_EQ("choice", model_def["type"].get<std::string>());
    EXPECT_EQ("Model", model_def["name"].get<std::string>());
    ASSERT_TRUE(model_def.contains("data"));
    auto data = model_def["data"].get<std::vector<std::string>>();
    ASSERT_EQ(3, data.size());
    EXPECT_EQ("Saw", data[0]);
    EXPECT_EQ("Square", data[1]);
    EXPECT_EQ("Sine", data[2]);

    // Bipolar param
    const auto& pan_def = entries["synth.pan"]["definition"];
    EXPECT_EQ("bipolar", pan_def["slider_polarity"].get<std::string>());
}

TEST(StateTests, GetGroupStateDump) {
    State state;
    state.create("engine0.grain0.volume", 1.0f);
    state.create("engine0.grain0.size", 0.5f);
    state.create("engine0.grain1.volume", 0.8f);
    state.create("engine1.grain0.volume", 0.7f);
    state.create("mixer.volume", 0.9f);

    auto dump = state.get_group_state_dump("engine0", false);
    auto json = nlohmann::json::parse(dump);

    EXPECT_EQ(3, json.size());
    for (const auto& entry : json) {
        std::string key = entry["key"];
        EXPECT_EQ(0, key.find("engine0"));
    }
}

TEST(StateTests, GetGroupStateDumpDeepPrefix) {
    State state;
    state.create("engine0.grain0.volume", 1.0f);
    state.create("engine0.grain0.size", 0.5f);
    state.create("engine0.grain1.volume", 0.8f);
    state.create("engine1.grain0.volume", 0.7f);

    auto dump = state.get_group_state_dump("engine0.grain0", false);
    auto json = nlohmann::json::parse(dump);

    EXPECT_EQ(2, json.size());
    for (const auto& entry : json) {
        std::string key = entry["key"];
        EXPECT_EQ(0, key.find("engine0.grain0"));
    }
}

TEST(StateTests, GetGroupStateDumpNoMatch) {
    State state;
    state.create("engine0.volume", 1.0f);

    auto dump = state.get_group_state_dump("nonexistent", false);
    auto json = nlohmann::json::parse(dump);

    EXPECT_EQ(0, json.size());
}

TEST(StateTests, GetGroupStateDumpEmptyPrefixReturnsAll) {
    State state;
    state.create("a.b", 1.0f);
    state.create("c.d", 2.0f);

    auto all_dump = state.get_state_dump(false);
    auto group_dump = state.get_group_state_dump("", false);

    EXPECT_EQ(all_dump, group_dump);
}

TEST(StateTests, GetGroupStateDumpWithDefinitions) {
    State state;
    state.create("synth.osc.freq",
                 ParameterFloat("Frequency", Range(20.0f, 20000.0f, 1.0f), 440.0f));
    state.create("synth.osc.vol", ParameterFloat("Volume", Range(0.0f, 1.0f, 0.01f), 0.8f));
    state.create("mixer.vol", ParameterFloat("Mixer Vol", Range(0.0f, 1.0f, 0.01f), 0.5f));

    auto dump = state.get_group_state_dump("synth", true);
    auto json = nlohmann::json::parse(dump);

    EXPECT_EQ(2, json.size());
    for (const auto& entry : json) { EXPECT_TRUE(entry.contains("definition")); }
}

TEST(StateTests, GetGroupStateDumpWithoutDefinitions) {
    State state;
    state.create("synth.freq", ParameterFloat("Freq", Range(0.0f, 1.0f, 0.01f), 0.5f));

    auto dump = state.get_group_state_dump("synth", false);
    auto json = nlohmann::json::parse(dump);

    EXPECT_EQ(1, json.size());
    EXPECT_FALSE(json[0].contains("definition"));
}

TEST(StateTests, GetGroupStateDumpPrefixBoundary) {
    State state;
    state.create("engine0.volume", 1.0f);
    state.create("engine0_extra.volume", 0.5f);

    auto dump = state.get_group_state_dump("engine0.", false);
    auto json = nlohmann::json::parse(dump);

    EXPECT_EQ(1, json.size());
    EXPECT_EQ("engine0.volume", json[0]["key"].get<std::string>());
}

// =============================================================================
// ParameterHandle tests
// =============================================================================

TEST(StateTests, HandleDefaultConstructor) {
    ParameterHandle<double> handle;
    EXPECT_FALSE(handle.is_valid());
}

// TEST(StateTests, HandleNonExistentKey) {
//     State state;
//     EXPECT_THROW(state.get_handle<double>("nonexistent"), StateKeyNotFoundException);
// }

TEST(StateTests, HandleBasicLoadStore) {
    State state;
    state.create("volume", 0.75);

    auto handle = state.get_handle<double>("volume");
    EXPECT_TRUE(handle.is_valid());
    EXPECT_DOUBLE_EQ(0.75, handle.load());

    handle.store(0.5);
    EXPECT_DOUBLE_EQ(0.5, handle.load());
}

TEST(StateTests, HandleAllNumericTypes) {
    State state;
    state.create("d_param", 3.14);
    state.create("f_param", 2.71f);
    state.create("i_param", 42);
    state.create("b_param", true);

    auto dh = state.get_handle<double>("d_param");
    auto fh = state.get_handle<float>("f_param");
    auto ih = state.get_handle<int>("i_param");
    auto bh = state.get_handle<bool>("b_param");

    EXPECT_DOUBLE_EQ(3.14, dh.load());
    EXPECT_FLOAT_EQ(2.71f, fh.load());
    EXPECT_EQ(42, ih.load());
    EXPECT_TRUE(bh.load());
}

TEST(StateTests, HandleReflectsSetInRoot) {
    State state;
    state.create("gain", 1.0);

    auto handle = state.get_handle<double>("gain");
    EXPECT_DOUBLE_EQ(1.0, handle.load());

    // set_in_root updates both RCU and atomic cache
    state.set("gain", 2.0);
    EXPECT_DOUBLE_EQ(2.0, handle.load());
}

TEST(StateTests, HandleStoreKeepsStateInSync) {
    State state;
    state.create("gain", 1.0);

    auto handle = state.get_handle<double>("gain");
    handle.store(3.0);

    // Handle sees the new value
    EXPECT_DOUBLE_EQ(3.0, handle.load());

    // get_from_root also reads from the same atomic -- state stays in sync
    EXPECT_DOUBLE_EQ(3.0, state.get_from_root<double>("gain"));
}

TEST(StateTests, HandleSetUpdatesNativeAtomicOnly) {
    State state;
    state.create("param", 3.14);

    // Only native-type handle is allowed
    auto dh = state.get_handle<double>("param");
    EXPECT_DOUBLE_EQ(3.14, dh.load());

    // Cross-type handles should throw
    EXPECT_THROW(state.get_handle<float>("param"), ParameterTypeMismatchException);
    EXPECT_THROW(state.get_handle<int>("param"), ParameterTypeMismatchException);
    EXPECT_THROW(state.get_handle<bool>("param"), ParameterTypeMismatchException);

    // set_in_root updates only the native atomic (double)
    state.set("param", 0.0);
    EXPECT_DOUBLE_EQ(0.0, dh.load());

    // Cross-type get still works via on-the-fly conversion
    EXPECT_FLOAT_EQ(0.0f, state.get<float>("param"));
    EXPECT_EQ(0, state.get<int>("param"));
    EXPECT_FALSE(state.get<bool>("param"));
}

TEST(StateTests, HandleSurvivesOtherParameterCreation) {
    State state;
    state.create("param_a", 1.0);

    auto handle = state.get_handle<double>("param_a");
    EXPECT_DOUBLE_EQ(1.0, handle.load());

    // Create many more parameters -- must not invalidate handle
    for (int i = 0; i < 100; ++i) {
        state.create("param_" + std::to_string(i), static_cast<double>(i));
    }

    // Handle still works
    EXPECT_DOUBLE_EQ(1.0, handle.load());

    state.set("param_a", 99.0);
    EXPECT_DOUBLE_EQ(99.0, handle.load());
}

TEST(StateTests, HandleWithHierarchicalPath) {
    State state;
    state.create("audio.effects.reverb.mix", 0.5);

    auto handle = state.get_handle<double>("audio.effects.reverb.mix");
    EXPECT_DOUBLE_EQ(0.5, handle.load());

    state.set("audio.effects.reverb.mix", 0.8);
    EXPECT_DOUBLE_EQ(0.8, handle.load());
}

TEST(StateTests, HandleNativeTypeEnforced) {
    State state;
    state.create("dval", 10.0);
    state.create("fval", 2.5f);
    state.create("ival", 42);
    state.create("bval", true);

    // Native-type handles succeed
    EXPECT_NO_THROW(state.get_handle<double>("dval"));
    EXPECT_NO_THROW(state.get_handle<float>("fval"));
    EXPECT_NO_THROW(state.get_handle<int>("ival"));
    EXPECT_NO_THROW(state.get_handle<bool>("bval"));

    // Cross-type handles throw
    EXPECT_THROW(state.get_handle<float>("dval"), ParameterTypeMismatchException);
    EXPECT_THROW(state.get_handle<double>("fval"), ParameterTypeMismatchException);
    EXPECT_THROW(state.get_handle<bool>("ival"), ParameterTypeMismatchException);
    EXPECT_THROW(state.get_handle<int>("bval"), ParameterTypeMismatchException);
}

TEST(StateTests, HandleCopySemantics) {
    State state;
    state.create("param", 42.0);

    auto h1 = state.get_handle<double>("param");
    auto h2 = h1;  // Copy

    EXPECT_TRUE(h2.is_valid());
    EXPECT_DOUBLE_EQ(42.0, h2.load());

    h1.store(99.0);
    // Both handles point to the same AtomicCacheEntry
    EXPECT_DOUBLE_EQ(99.0, h2.load());
}

TEST(StateTests, HandleConcurrentLoadStore) {
    State state;
    state.create("param", 0.0);

    auto handle = state.get_handle<double>("param");

    const int num_threads = 4;
    const int ops_per_thread = 10000;
    std::atomic<bool> go{false};

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    // Writer threads use handle.store
    for (int t = 0; t < num_threads / 2; ++t) {
        threads.emplace_back([&, t] {
            while (!go.load(std::memory_order_acquire)) {}
            for (int i = 0; i < ops_per_thread; ++i) {
                handle.store(static_cast<double>(t * ops_per_thread + i));
            }
        });
    }

    // Reader threads use handle.load
    for (int t = 0; t < num_threads / 2; ++t) {
        threads.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) {}
            for (int i = 0; i < ops_per_thread; ++i) {
                volatile double val = handle.load();
                (void)val;
            }
        });
    }

    go.store(true, std::memory_order_release);
    for (auto& t : threads) { t.join(); }

    // Just verify handle is still usable
    double final_val = handle.load();
    EXPECT_GE(final_val, 0.0);
}

// =============================================================================
// ID-based access tests
// =============================================================================

// Helper: create a ParameterFloat with a specific m_id.
static ParameterFloat make_float_with_id(const std::string& name,
                                         Range range,
                                         float default_val,
                                         uint32_t id) {
    ParameterFloat def(name, range, default_val);
    def.m_id = id;
    return def;
}

TEST(StateTests, GetHandleByIdBasic) {
    State state;

    state.create("synth.freq", make_float_with_id("Frequency", Range(20.0f, 20000.0f), 440.0f, 1));
    state.create("synth.gain", make_float_with_id("Gain", Range(0.0f, 1.0f), 0.8f, 2));

    auto freq_handle = state.get_handle_by_id<float>(1);
    auto gain_handle = state.get_handle_by_id<float>(2);

    EXPECT_TRUE(freq_handle.is_valid());
    EXPECT_TRUE(gain_handle.is_valid());
    EXPECT_FLOAT_EQ(440.0f, freq_handle.load());
    EXPECT_FLOAT_EQ(0.8f, gain_handle.load());

    // Metadata accessible via handle
    EXPECT_EQ("Frequency", freq_handle.def().m_name);
    EXPECT_EQ(1u, freq_handle.id());
    EXPECT_EQ("synth.freq", freq_handle.key());
    EXPECT_EQ(ParameterType::Float, freq_handle.type());
}

TEST(StateTests, GetByIdReturnsParameter) {
    State state;

    state.create("vol", make_float_with_id("Volume", Range(0.0f, 1.0f), 0.5f, 10));

    Parameter param = state.get_parameter_by_id(10);
    EXPECT_FLOAT_EQ(0.5f, param.to<float>());
    EXPECT_EQ("Volume", param.def().m_name);
    EXPECT_EQ("vol", param.key());
}

TEST(StateTests, GetHandleByIdThrowsOnUnknownId) {
    State state;
    state.create("param", make_float_with_id("P", Range(), 0.0f, 1));

    EXPECT_THROW(state.get_handle_by_id<float>(999), StateKeyNotFoundException);
    EXPECT_THROW(state.get_parameter_by_id(999), StateKeyNotFoundException);
}

TEST(StateTests, GetHandleByIdThrowsOnTypeMismatch) {
    State state;
    state.create("param", make_float_with_id("P", Range(), 0.0f, 1));

    // Parameter is float, requesting double handle should throw
    EXPECT_THROW(state.get_handle_by_id<double>(1), ParameterTypeMismatchException);
    EXPECT_THROW(state.get_handle_by_id<int>(1), ParameterTypeMismatchException);
}

TEST(StateTests, DuplicateIdThrows) {
    State state;

    state.create("param1", make_float_with_id("P1", Range(), 0.0f, 5));

    // Same ID 5 on a different parameter should throw
    EXPECT_THROW(state.create("param2", make_float_with_id("P2", Range(), 0.0f, 5)),
                 DuplicateParameterIdException);
}

TEST(StateTests, AutoIdAssignment) {
    State state;

    // Value-created params get auto-assigned IDs starting from 0
    state.create("plain", 42.0);
    state.create("another", 7.0);

    // Auto-assigned IDs are sequential starting from 0
    EXPECT_DOUBLE_EQ(42.0, state.get_handle_by_id<double>(0).load());
    EXPECT_DOUBLE_EQ(7.0, state.get_handle_by_id<double>(1).load());

    // UINT32_MAX should not be findable (no longer used as a real ID)
    EXPECT_THROW(state.get_handle_by_id<double>(UINT32_MAX), StateKeyNotFoundException);
}

TEST(StateTests, ClearRemovesIdIndex) {
    State state;

    state.create("param", make_float_with_id("P", Range(), 0.0f, 1));

    EXPECT_FLOAT_EQ(0.0f, state.get_handle_by_id<float>(1).load());

    state.clear();

    EXPECT_THROW(state.get_handle_by_id<float>(1), StateKeyNotFoundException);
}

TEST(StateTests, HandleByIdAllNumericTypes) {
    State state;

    state.create("f", make_float_with_id("F", Range(), 3.14f, 1));

    ParameterInt int_def("I", Range(0, 100), 42);
    int_def.m_id = 2;
    state.create("i", int_def);

    ParameterBool bool_def("B", true);
    bool_def.m_id = 3;
    state.create("b", bool_def);

    auto fh = state.get_handle_by_id<float>(1);
    auto ih = state.get_handle_by_id<int>(2);
    auto bh = state.get_handle_by_id<bool>(3);

    EXPECT_FLOAT_EQ(3.14f, fh.load());
    EXPECT_EQ(42, ih.load());
    EXPECT_TRUE(bh.load());
}

// =============================================================================
// get_by_id tests
// =============================================================================

TEST(StateTests, GetByIdFloat) {
    State state;
    state.create("freq", make_float_with_id("Frequency", Range(20.0f, 20000.0f), 440.0f, 1));

    EXPECT_FLOAT_EQ(440.0f, state.get_by_id<float>(1));

    state.set("freq", 880.0f);
    EXPECT_FLOAT_EQ(880.0f, state.get_by_id<float>(1));
}

TEST(StateTests, GetByIdInt) {
    State state;
    ParameterInt int_def("Voices", Range(1, 16), 4);
    int_def.m_id = 10;
    state.create("voices", int_def);

    EXPECT_EQ(4, state.get_by_id<int>(10));

    state.set("voices", 8);
    EXPECT_EQ(8, state.get_by_id<int>(10));
}

TEST(StateTests, GetByIdBool) {
    State state;
    ParameterBool bool_def("Bypass", false);
    bool_def.m_id = 20;
    state.create("bypass", bool_def);

    EXPECT_FALSE(state.get_by_id<bool>(20));

    state.set("bypass", true);
    EXPECT_TRUE(state.get_by_id<bool>(20));
}

TEST(StateTests, GetByIdString) {
    State state;
    ParameterDefinition str_def;
    str_def.m_type = ParameterType::String;
    str_def.m_name = "Preset";
    str_def.m_id = 30;
    state.create("preset", str_def);
    state.set("preset", std::string("Init"));

    EXPECT_EQ("Init", state.get_by_id<std::string>(30, true));
}

TEST(StateTests, GetByIdCrossTypeConversion) {
    State state;
    state.create("freq", make_float_with_id("Frequency", Range(20.0f, 20000.0f), 440.0f, 1));

    // Reading float param as double/int should convert
    EXPECT_FLOAT_EQ(440.0f, static_cast<float>(state.get_by_id<double>(1)));
    EXPECT_EQ(440, state.get_by_id<int>(1));
}

TEST(StateTests, GetByIdThrowsOnUnknownId) {
    State state;
    state.create("param", make_float_with_id("P", Range(), 0.0f, 1));

    EXPECT_THROW(state.get_by_id<float>(999), StateKeyNotFoundException);
}

TEST(StateTests, GetByIdStringWithoutAllowBlockingThrows) {
    State state;
    ParameterDefinition str_def;
    str_def.m_type = ParameterType::String;
    str_def.m_name = "Preset";
    str_def.m_id = 30;
    state.create("preset", str_def);

    EXPECT_THROW(state.get_by_id<std::string>(30), BlockingException);
    EXPECT_NO_THROW(state.get_by_id<std::string>(30, true));
}

// =============================================================================
// Gesture notification filtering tests
// =============================================================================

TEST(StateTests, GestureFilterSkipsListenerDuringGesture) {
    State state;

    // Listener that opts out of gesture notifications
    class NoGestureListener : public ParameterListener {
    public:
        NoGestureListener() : ParameterListener(NotifyStrategies::All, false) {}
        void on_parameter_changed(const Parameter&) override { m_count++; }
        int m_count = 0;
    };

    TestParameterListener always_listener;  // default: receives_during_gesture = true
    NoGestureListener filtered_listener;

    state.add_listener(&always_listener);
    state.add_listener(&filtered_listener);

    // Normal set -- both listeners fire
    state.create("test.param", 1.0f);
    EXPECT_EQ(1, always_listener.m_notification_count);
    EXPECT_EQ(1, filtered_listener.m_count);

    // Begin gesture, then set -- only the always-listener fires
    state.set_gesture("test.param", true);
    state.set("test.param", 2.0f);
    EXPECT_EQ(2, always_listener.m_notification_count);
    EXPECT_EQ(1, filtered_listener.m_count);  // still 1

    // End gesture, then set -- both fire again
    state.set_gesture("test.param", false);
    state.set("test.param", 3.0f);
    EXPECT_EQ(3, always_listener.m_notification_count);
    EXPECT_EQ(2, filtered_listener.m_count);
}

TEST(StateTests, GestureFilterWithGroupListeners) {
    State state;
    StateGroup* group = state.create_group("synth");

    class NoGestureListener : public ParameterListener {
    public:
        NoGestureListener() : ParameterListener(NotifyStrategies::All, false) {}
        void on_parameter_changed(const Parameter&) override { m_count++; }
        int m_count = 0;
    };

    TestParameterListener root_listener;
    NoGestureListener group_filtered;

    state.add_listener(&root_listener);
    group->add_listener(&group_filtered);

    state.create("synth.cutoff", 0.5f);
    EXPECT_EQ(1, root_listener.m_notification_count);
    EXPECT_EQ(1, group_filtered.m_count);

    // Gesture active on this parameter
    state.set_gesture("synth.cutoff", true);
    state.set("synth.cutoff", 0.8f);
    EXPECT_EQ(2, root_listener.m_notification_count);
    EXPECT_EQ(1, group_filtered.m_count);  // skipped

    state.set_gesture("synth.cutoff", false);
    state.set("synth.cutoff", 0.3f);
    EXPECT_EQ(3, root_listener.m_notification_count);
    EXPECT_EQ(2, group_filtered.m_count);  // fires again
}

TEST(StateTests, GestureFilterIsPerParameter) {
    State state;

    class NoGestureListener : public ParameterListener {
    public:
        NoGestureListener() : ParameterListener(NotifyStrategies::All, false) {}
        void on_parameter_changed(const Parameter&) override { m_count++; }
        int m_count = 0;
    };

    NoGestureListener filtered;
    state.add_listener(&filtered);

    state.create("param.a", 1.0f);
    state.create("param.b", 1.0f);
    EXPECT_EQ(2, filtered.m_count);

    // Only param.a is in gesture
    state.set_gesture("param.a", true);

    state.set("param.a", 2.0f);
    EXPECT_EQ(2, filtered.m_count);  // skipped

    state.set("param.b", 2.0f);
    EXPECT_EQ(3, filtered.m_count);  // not in gesture -- fires

    state.set_gesture("param.a", false);
    state.set("param.a", 3.0f);
    EXPECT_EQ(4, filtered.m_count);  // fires again
}

TEST(StateTests, GestureValueStillUpdatedWhenListenerSkipped) {
    State state;

    class NoGestureListener : public ParameterListener {
    public:
        NoGestureListener() : ParameterListener(NotifyStrategies::All, false) {}
        void on_parameter_changed(const Parameter&) override { m_count++; }
        int m_count = 0;
    };

    NoGestureListener filtered;
    state.add_listener(&filtered);

    state.create("vol", 0.5f);
    EXPECT_EQ(1, filtered.m_count);

    state.set_gesture("vol", true);
    state.set("vol", 0.9f);
    EXPECT_EQ(1, filtered.m_count);  // notification skipped

    // But the value IS updated
    EXPECT_FLOAT_EQ(0.9f, state.get<float>("vol"));
}

// =============================================================================
// StringView invalidation tests
// =============================================================================

// Test for string_view invalidation when listener calls set() in callback
TEST(StateTests, StringViewInvalidationInNestedSetCalls) {
    State state;

    // Create a listener that captures the path via param.key() and then calls set() on
    // another parameter
    class NestedSetListener : public ParameterListener {
    public:
        // Each invocation records a (before, after) pair in m_matched_pairs
        struct PathPair {
            std::string m_before;
            std::string m_after;
        };

        void on_parameter_changed(const Parameter& param) override {
            std::string_view path = param.key();
            // Store the path immediately - this creates a copy
            std::string path_copy_immediate(path);

            // Also store pointer and size of the string_view to check later
            const char* view_data = path.data();
            size_t view_size = path.size();

            // Now trigger a nested set() call which might invalidate the string_view
            if (path == "trigger.param1") {
                m_state_ref->set("response.param1", 100.0);
                m_state_ref->set("response.param2", 200.0);
                m_state_ref->set("response.param3", 300.0);
            } else if (path == "trigger.deeply.nested.param") {
                m_state_ref->set("response.deep.level1.param", 1.0);
                m_state_ref->set("response.deep.level2.param", 2.0);
                m_state_ref->set("response.deep.level3.param", 3.0);
            }

            // Now try to access the original string_view - this is where it might be
            // corrupted
            std::string path_copy_after(path);

            // Store matched pair for this invocation
            m_matched_pairs.push_back({path_copy_immediate, path_copy_after});

            // Verify the string_view still points to valid data
            EXPECT_EQ(path_copy_immediate, path_copy_after)
                << "string_view was corrupted during nested set() call!";

            // Verify the string_view hasn't changed pointer or size
            EXPECT_EQ(view_data, path.data())
                << "string_view data pointer changed during nested set() call!";
            EXPECT_EQ(view_size, path.size())
                << "string_view size changed during nested set() call!";
        }

        State* m_state_ref = nullptr;
        std::vector<PathPair> m_matched_pairs;
    };

    NestedSetListener listener;
    listener.m_state_ref = &state;
    state.add_listener(&listener);

    // Pre-create the response parameters so nested set() calls don't need create()
    state.create("response.param1", 0.0);
    state.create("response.param2", 0.0);
    state.create("response.param3", 0.0);
    state.create("response.deep.level1.param", 0.0);
    state.create("response.deep.level2.param", 0.0);
    state.create("response.deep.level3.param", 0.0);

    // Clear notification count from creates
    listener.m_matched_pairs.clear();

    // Test 1: Simple nested set
    state.create("trigger.param1", 42.0);

    // trigger.param1 + 3 response params = 4 notifications
    ASSERT_EQ(4, listener.m_matched_pairs.size());

    // Verify every invocation's before/after match (string_view not corrupted)
    for (size_t i = 0; i < listener.m_matched_pairs.size(); ++i) {
        EXPECT_EQ(listener.m_matched_pairs[i].m_before, listener.m_matched_pairs[i].m_after)
            << "Path mismatch at invocation " << i;
    }

    // Test 2: Deeply nested path
    state.create("trigger.deeply.nested.param", 123.0);

    // Previous 4 + trigger.deeply.nested.param + 3 response.deep params = 8 total
    ASSERT_EQ(8, listener.m_matched_pairs.size());

    // Test 3: Multiple consecutive sets that trigger nested calls
    state.set("trigger.param1", 99.0);

    // Verify all invocations' before/after pairs match
    for (size_t i = 0; i < listener.m_matched_pairs.size(); ++i) {
        EXPECT_EQ(listener.m_matched_pairs[i].m_before, listener.m_matched_pairs[i].m_after)
            << "Path mismatch at invocation " << i;
    }

    // Verify all response parameters were actually set
    EXPECT_DOUBLE_EQ(100.0, state.get<double>("response.param1"));
    EXPECT_DOUBLE_EQ(200.0, state.get<double>("response.param2"));
    EXPECT_DOUBLE_EQ(300.0, state.get<double>("response.param3"));
    EXPECT_DOUBLE_EQ(1.0, state.get<double>("response.deep.level1.param"));
    EXPECT_DOUBLE_EQ(2.0, state.get<double>("response.deep.level2.param"));
    EXPECT_DOUBLE_EQ(3.0, state.get<double>("response.deep.level3.param"));
}

// Test string_view invalidation with callback-based listeners
TEST(StateTests, StringViewInvalidationInCallbackListeners) {
    State state;

    struct PathPair {
        std::string m_before;
        std::string m_after;
    };
    std::vector<PathPair> matched_pairs;

    // Pre-create target parameters
    state.create("target.b", 0.0);
    state.create("target.c", 0.0);

    // Add a callback listener that performs nested set() calls
    CallbackListener cb([&](const Parameter& param) {
        std::string_view path = param.key();
        // Capture path immediately
        std::string immediate_copy(path);

        const char* view_ptr = path.data();

        // Perform nested set that might invalidate the string_view
        if (path == "source.a") {
            state.set("target.b", 20.0);
            state.set("target.c", 30.0);
        }

        // Try to use the string_view again
        std::string after_copy(path);
        matched_pairs.push_back({immediate_copy, after_copy});

        // Verify they match
        EXPECT_EQ(immediate_copy, after_copy) << "string_view corrupted in callback listener!";

        // Verify pointer hasn't changed
        EXPECT_EQ(view_ptr, path.data()) << "string_view pointer changed in callback listener!";
    });
    state.add_listener(&cb);

    // Clear matched_pairs from pre-create notifications
    matched_pairs.clear();

    // Trigger the callback
    state.create("source.a", 10.0);

    // Verify we got notifications for all three parameters
    ASSERT_EQ(3, matched_pairs.size());

    // Verify all invocations' before/after pairs match
    for (size_t i = 0; i < matched_pairs.size(); ++i) {
        EXPECT_EQ(matched_pairs[i].m_before, matched_pairs[i].m_after)
            << "Path mismatch at invocation " << i;
    }
}

// Test string_view invalidation with very long parameter paths
TEST(StateTests, StringViewInvalidationWithLongPaths) {
    State state;

    class LongPathListener : public ParameterListener {
    public:
        void on_parameter_changed(const Parameter& param) override {
            std::string_view path = param.key();
            std::string path_before(path);
            const char* ptr_before = path.data();
            size_t size_before = path.size();

            // Only trigger nested sets for the original trigger path (exact prefix match)
            // to avoid infinite recursion from response paths also matching
            if (path.starts_with("very.")) {
                m_state_ref->set("another.very.long.hierarchical.path.level1.level2.level3.param1",
                                 1.0);
                m_state_ref->set("another.very.long.hierarchical.path.level1.level2.level3.param2",
                                 2.0);
                m_state_ref->set("another.very.long.hierarchical.path.level1.level2.level3.param3",
                                 3.0);
                m_state_ref->set("yet.another.extremely.long.path.with.many.nested.groups.param",
                                 4.0);
            }

            // Verify string_view is still valid
            std::string path_after(path);
            EXPECT_EQ(path_before, path_after) << "Long path string_view was corrupted!";
            EXPECT_EQ(ptr_before, path.data()) << "Long path string_view pointer changed!";
            EXPECT_EQ(size_before, path.size()) << "Long path string_view size changed!";

            m_paths.push_back(path_before);
        }

        State* m_state_ref = nullptr;
        std::vector<std::string> m_paths;
    };

    LongPathListener listener;
    listener.m_state_ref = &state;

    // Pre-create response parameters
    state.create("another.very.long.hierarchical.path.level1.level2.level3.param1", 0.0);
    state.create("another.very.long.hierarchical.path.level1.level2.level3.param2", 0.0);
    state.create("another.very.long.hierarchical.path.level1.level2.level3.param3", 0.0);
    state.create("yet.another.extremely.long.path.with.many.nested.groups.param", 0.0);

    state.add_listener(&listener);

    // Create a very long parameter path
    state.create("very.long.hierarchical.path.with.multiple.levels.and.groups.final.param", 99.0);

    // Verify the listener received notifications with valid paths
    // (m_paths vector is in DFS post-order due to recursion, so check by content)
    ASSERT_GE(listener.m_paths.size(), 1);
    bool found_trigger = false;
    for (const auto& p : listener.m_paths) {
        if (p == "very.long.hierarchical.path.with.multiple.levels.and.groups.final.param") {
            found_trigger = true;
        }
    }
    EXPECT_TRUE(found_trigger) << "Trigger path not found in listener notifications";

    // Verify all the nested parameters were set
    EXPECT_DOUBLE_EQ(
        1.0,
        state.get<double>("another.very.long.hierarchical.path.level1.level2.level3.param1"));
    EXPECT_DOUBLE_EQ(
        4.0,
        state.get<double>("yet.another.extremely.long.path.with.many.nested.groups.param"));
}

// Test chain of listener notifications where each triggers more sets
TEST(StateTests, StringViewInvalidationWithListenerChain) {
    State state;

    class ChainListener : public ParameterListener {
    public:
        explicit ChainListener(int id) : m_listener_id(id) {}

        void on_parameter_changed(const Parameter& param) override {
            std::string_view path = param.key();
            std::string path_copy(path);
            const char* ptr = path.data();

            // Each listener in the chain triggers the next
            if (path == "chain.step1" && m_listener_id == 1) {
                m_state_ref->set("chain.step2", 2.0);
            } else if (path == "chain.step2" && m_listener_id == 2) {
                m_state_ref->set("chain.step3", 3.0);
            } else if (path == "chain.step3" && m_listener_id == 3) {
                m_state_ref->set("chain.step4", 4.0);
            }

            // Verify string_view is still valid after potential nested calls
            std::string path_after(path);
            EXPECT_EQ(path_copy, path_after)
                << "string_view corrupted in listener chain at listener " << m_listener_id;
            EXPECT_EQ(ptr, path.data())
                << "string_view pointer changed in listener chain at listener " << m_listener_id;

            m_notifications.push_back(path_copy);
        }

        State* m_state_ref{nullptr};
        int m_listener_id;
        std::vector<std::string> m_notifications;
    };

    // Pre-create the chain parameters
    state.create("chain.step1", 0.0);
    state.create("chain.step2", 0.0);
    state.create("chain.step3", 0.0);
    state.create("chain.step4", 0.0);

    ChainListener listener1(1);
    ChainListener listener2(2);
    ChainListener listener3(3);

    listener1.m_state_ref = &state;
    listener2.m_state_ref = &state;
    listener3.m_state_ref = &state;

    state.add_listener(&listener1);
    state.add_listener(&listener2);
    state.add_listener(&listener3);

    // Start the chain
    state.set("chain.step1", 1.0);

    // All listeners should have received notifications
    EXPECT_GE(listener1.m_notifications.size(), 1);
    EXPECT_GE(listener2.m_notifications.size(), 1);
    EXPECT_GE(listener3.m_notifications.size(), 1);

    // Verify the chain completed
    EXPECT_DOUBLE_EQ(1.0, state.get<double>("chain.step1"));
    EXPECT_DOUBLE_EQ(2.0, state.get<double>("chain.step2"));
    EXPECT_DOUBLE_EQ(3.0, state.get<double>("chain.step3"));
    EXPECT_DOUBLE_EQ(4.0, state.get<double>("chain.step4"));
}

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

// =============================================================================
// Display formatting tests (Phase 5: m_value_to_text / m_text_to_value)
// =============================================================================

TEST(StateTests, FloatValueToText) {
    ParameterFloat def("Gain", Range(0.0f, 10.0f), 3.14f, 2);
    EXPECT_EQ("3.14", def.m_value_to_text(3.14f));
    EXPECT_EQ("0.00", def.m_value_to_text(0.0f));
    EXPECT_EQ("10.00", def.m_value_to_text(10.0f));
}

TEST(StateTests, FloatValueToTextDecimalPlaces) {
    ParameterFloat def0("P", Range(), 0.0f, 0);
    EXPECT_EQ("3", def0.m_value_to_text(3.14f));

    ParameterFloat def4("P", Range(), 0.0f, 4);
    EXPECT_EQ("3.1416", def4.m_value_to_text(3.14159f));
}

TEST(StateTests, FloatTextToValue) {
    ParameterFloat def("Gain", Range(0.0f, 10.0f), 0.0f);
    EXPECT_FLOAT_EQ(3.14f, def.m_text_to_value("3.14"));
    EXPECT_FLOAT_EQ(0.0f, def.m_text_to_value("0"));
    EXPECT_FLOAT_EQ(0.0f, def.m_text_to_value("not_a_number"));
}

TEST(StateTests, IntValueToText) {
    ParameterInt def("Steps", Range(0, 100), 42);
    EXPECT_EQ("42", def.m_value_to_text(42.0f));
    EXPECT_EQ("0", def.m_value_to_text(0.0f));
    EXPECT_EQ("-5", def.m_value_to_text(-5.0f));
}

TEST(StateTests, IntTextToValue) {
    ParameterInt def("Steps", Range(0, 100), 0);
    EXPECT_FLOAT_EQ(42.0f, def.m_text_to_value("42"));
    EXPECT_FLOAT_EQ(0.0f, def.m_text_to_value("not_a_number"));
}

TEST(StateTests, BoolValueToText) {
    ParameterBool def("Enable", true);
    EXPECT_EQ("On", def.m_value_to_text(1.0f));
    EXPECT_EQ("Off", def.m_value_to_text(0.0f));
}

TEST(StateTests, BoolTextToValue) {
    ParameterBool def("Enable");
    EXPECT_FLOAT_EQ(1.0f, def.m_text_to_value("On"));
    EXPECT_FLOAT_EQ(1.0f, def.m_text_to_value("on"));
    EXPECT_FLOAT_EQ(1.0f, def.m_text_to_value("true"));
    EXPECT_FLOAT_EQ(1.0f, def.m_text_to_value("1"));
    EXPECT_FLOAT_EQ(0.0f, def.m_text_to_value("Off"));
    EXPECT_FLOAT_EQ(0.0f, def.m_text_to_value("anything_else"));
}

TEST(StateTests, ChoiceValueToText) {
    ParameterChoice def("Waveform", {"Sine", "Saw", "Square"}, 0);
    EXPECT_EQ("Sine", def.m_value_to_text(0.0f));
    EXPECT_EQ("Saw", def.m_value_to_text(1.0f));
    EXPECT_EQ("Square", def.m_value_to_text(2.0f));
    // Out of range falls back to int string
    EXPECT_EQ("5", def.m_value_to_text(5.0f));
}

TEST(StateTests, ChoiceTextToValue) {
    ParameterChoice def("Waveform", {"Sine", "Saw", "Square"}, 0);
    EXPECT_FLOAT_EQ(0.0f, def.m_text_to_value("Sine"));
    EXPECT_FLOAT_EQ(1.0f, def.m_text_to_value("Saw"));
    EXPECT_FLOAT_EQ(2.0f, def.m_text_to_value("Square"));
    // Numeric fallback
    EXPECT_FLOAT_EQ(1.0f, def.m_text_to_value("1"));
    // Unknown label falls back to parsing
    EXPECT_FLOAT_EQ(0.0f, def.m_text_to_value("Unknown"));
}

TEST(StateTests, DisplayFormattingViaParameterDef) {
    // Verify formatting works when accessed through State::get_from_root
    State state;
    state.create("gain", ParameterFloat("Gain", Range(0.0f, 1.0f), 0.75f, 3));

    Parameter param = state.get_parameter_from_root("gain");
    const auto& def = param.def();
    ASSERT_TRUE(def.m_value_to_text);
    EXPECT_EQ("0.750", def.m_value_to_text(param.to<float>()));
}

TEST(StateTests, DisplayFormattingViaHandle) {
    // Verify formatting works when accessed through ParameterHandle
    State state;
    state.create("freq", ParameterFloat("Frequency", Range(20.0f, 20000.0f), 440.0f, 1));

    auto handle = state.get_handle<float>("freq");
    ASSERT_TRUE(handle.def().m_value_to_text);
    EXPECT_EQ("440.0", handle.def().m_value_to_text(handle.load()));
}

// =============================================================================
// set_by_id tests
// =============================================================================

TEST(StateTests, SetByIdFloat) {
    State state;
    state.create("freq", make_float_with_id("Frequency", Range(20.0f, 20000.0f), 440.0f, 1));

    state.set_by_id<float>(1, 880.0f);
    EXPECT_FLOAT_EQ(880.0f, state.get<float>("freq"));
}

TEST(StateTests, SetByIdInt) {
    State state;
    ParameterDefinition int_def;
    int_def.m_type = ParameterType::Int;
    int_def.m_name = "Note";
    int_def.m_id = 10;
    state.create("note", int_def);

    state.set_by_id<int>(10, 72);
    EXPECT_EQ(72, state.get<int>("note"));
}

TEST(StateTests, SetByIdBool) {
    State state;
    ParameterDefinition bool_def;
    bool_def.m_type = ParameterType::Bool;
    bool_def.m_name = "Active";
    bool_def.m_id = 20;
    state.create("active", bool_def);

    state.set_by_id<bool>(20, true);
    EXPECT_TRUE(state.get<bool>("active"));

    state.set_by_id<bool>(20, false);
    EXPECT_FALSE(state.get<bool>("active"));
}

TEST(StateTests, SetByIdDouble) {
    State state;
    ParameterDefinition dbl_def;
    dbl_def.m_type = ParameterType::Double;
    dbl_def.m_name = "Gain";
    dbl_def.m_id = 5;
    state.create("gain", dbl_def);

    state.set_by_id<double>(5, 0.75);
    EXPECT_DOUBLE_EQ(0.75, state.get<double>("gain"));
}

TEST(StateTests, SetByIdString) {
    State state;
    ParameterDefinition str_def;
    str_def.m_type = ParameterType::String;
    str_def.m_name = "Preset";
    str_def.m_id = 30;
    state.create("preset", str_def);

    state.set_by_id<std::string>(30, "Warm Pad");
    EXPECT_EQ("Warm Pad", state.get<std::string>("preset", true));
}

TEST(StateTests, SetByIdThrowsOnUnknownId) {
    State state;
    state.create("param", make_float_with_id("P", Range(), 0.0f, 1));

    EXPECT_THROW(state.set_by_id<float>(999, 1.0f), StateKeyNotFoundException);
}

TEST(StateTests, SetByIdNotifiesListeners) {
    State state;
    state.create("vol", make_float_with_id("Volume", Range(0.0f, 1.0f), 0.5f, 1));

    TestParameterListener listener;
    state.add_listener(&listener);

    state.set_by_id<float>(1, 0.9f);
    EXPECT_EQ(1, listener.m_notification_count);
    EXPECT_FLOAT_EQ(0.9f, state.get<float>("vol"));
}

TEST(StateTests, SetByIdCrossTypeConversion) {
    State state;
    state.create("freq", make_float_with_id("Frequency", Range(20.0f, 20000.0f), 440.0f, 1));

    // Set float parameter using int value
    state.set_by_id<int>(1, 880);
    EXPECT_FLOAT_EQ(880.0f, state.get<float>("freq"));
}

TEST(StateTests, SetByIdRoundTrip) {
    State state;
    state.create("param", make_float_with_id("P", Range(), 0.0f, 1));

    state.set_by_id<float>(1, 3.14f);
    EXPECT_FLOAT_EQ(3.14f, state.get_by_id<float>(1));
}

// =============================================================================
// Subgroup get_handle tests
// =============================================================================

TEST(StateTests, GetHandleFromSubgroup) {
    State state;
    StateGroup* synth = state.create_group("synth");
    synth->create("freq", 440.0f);

    // Get handle via subgroup (relative path)
    auto handle = synth->get_handle<float>("freq");
    EXPECT_FLOAT_EQ(440.0f, handle.load());

    // Modify via root state, verify handle sees the change
    state.set("synth.freq", 880.0f);
    EXPECT_FLOAT_EQ(880.0f, handle.load());
}

TEST(StateTests, GetHandleFromNestedSubgroup) {
    State state;
    StateGroup* audio = state.create_group("audio");
    StateGroup* effects = audio->create_group("effects");
    StateGroup* reverb = effects->create_group("reverb");
    reverb->create("mix", 0.3f);

    // Get handle from deeply nested group
    auto handle = reverb->get_handle<float>("mix");
    EXPECT_FLOAT_EQ(0.3f, handle.load());

    // Get handle via intermediate group (relative path)
    auto handle2 = effects->get_handle<float>("reverb.mix");
    EXPECT_FLOAT_EQ(0.3f, handle2.load());

    // Get handle via root (full path)
    auto handle3 = state.get_handle<float>("audio.effects.reverb.mix");
    EXPECT_FLOAT_EQ(0.3f, handle3.load());

    // All handles point to the same underlying record
    state.set("audio.effects.reverb.mix", 0.9f);
    EXPECT_FLOAT_EQ(0.9f, handle.load());
    EXPECT_FLOAT_EQ(0.9f, handle2.load());
    EXPECT_FLOAT_EQ(0.9f, handle3.load());
}

TEST(StateTests, GetHandleFromSubgroupAllTypes) {
    State state;
    StateGroup* group = state.create_group("params");
    group->create("d", 1.0);
    group->create("f", 2.5f);
    group->create("i", 42);
    group->create("b", true);

    auto dh = group->get_handle<double>("d");
    auto fh = group->get_handle<float>("f");
    auto ih = group->get_handle<int>("i");
    auto bh = group->get_handle<bool>("b");

    EXPECT_DOUBLE_EQ(1.0, dh.load());
    EXPECT_FLOAT_EQ(2.5f, fh.load());
    EXPECT_EQ(42, ih.load());
    EXPECT_TRUE(bh.load());
}

TEST(StateTests, GetHandleFromSubgroupThrowsTypeMismatch) {
    State state;
    StateGroup* group = state.create_group("params");
    group->create("val", 1.0f);

    EXPECT_THROW(group->get_handle<double>("val"), ParameterTypeMismatchException);
    EXPECT_THROW(group->get_handle<int>("val"), ParameterTypeMismatchException);
    EXPECT_THROW(group->get_handle<bool>("val"), ParameterTypeMismatchException);
}

TEST(StateTests, GetHandleFromSubgroupThrowsOnMissing) {
    State state;
    StateGroup* group = state.create_group("params");
    group->create("val", 1.0f);

    EXPECT_THROW(group->get_handle<float>("nonexistent"), StateKeyNotFoundException);
}

// =============================================================================
// Subgroup set_gesture tests
// =============================================================================

TEST(StateTests, SetGestureFromSubgroup) {
    State state;

    class NoGestureListener : public ParameterListener {
    public:
        NoGestureListener() : ParameterListener(NotifyStrategies::All, false) {}
        void on_parameter_changed(const Parameter&) override { m_count++; }
        int m_count = 0;
    };

    StateGroup* synth = state.create_group("synth");
    synth->create("cutoff", 0.5f);

    TestParameterListener always_listener;
    NoGestureListener filtered_listener;
    state.add_listener(&always_listener);
    state.add_listener(&filtered_listener);

    // set via subgroup, then gesture via subgroup
    synth->set("cutoff", 0.6f);
    EXPECT_EQ(1, always_listener.m_notification_count);
    EXPECT_EQ(1, filtered_listener.m_count);

    synth->set_gesture("cutoff", true);
    synth->set("cutoff", 0.8f);
    EXPECT_EQ(2, always_listener.m_notification_count);
    EXPECT_EQ(1, filtered_listener.m_count);  // skipped

    synth->set_gesture("cutoff", false);
    synth->set("cutoff", 0.3f);
    EXPECT_EQ(3, always_listener.m_notification_count);
    EXPECT_EQ(2, filtered_listener.m_count);  // fires again
}

TEST(StateTests, SetGestureFromNestedSubgroup) {
    State state;

    class NoGestureListener : public ParameterListener {
    public:
        NoGestureListener() : ParameterListener(NotifyStrategies::All, false) {}
        void on_parameter_changed(const Parameter&) override { m_count++; }
        int m_count = 0;
    };

    StateGroup* audio = state.create_group("audio");
    StateGroup* effects = audio->create_group("effects");
    effects->create("mix", 0.5f);

    NoGestureListener filtered;
    state.add_listener(&filtered);

    // Gesture set via intermediate group
    audio->set_gesture("effects.mix", true);
    state.set("audio.effects.mix", 0.9f);
    EXPECT_EQ(0, filtered.m_count);  // skipped (create notified before listener added)

    audio->set_gesture("effects.mix", false);
    state.set("audio.effects.mix", 0.1f);
    EXPECT_EQ(1, filtered.m_count);  // fires
}

TEST(StateTests, SetGestureFromSubgroupMatchesRootGesture) {
    // Gesture set via subgroup should be visible via root and vice versa
    State state;

    class NoGestureListener : public ParameterListener {
    public:
        NoGestureListener() : ParameterListener(NotifyStrategies::All, false) {}
        void on_parameter_changed(const Parameter&) override { m_count++; }
        int m_count = 0;
    };

    StateGroup* synth = state.create_group("synth");
    synth->create("vol", 0.5f);

    NoGestureListener filtered;
    state.add_listener(&filtered);

    // Set gesture via subgroup, verify root state recognizes it
    synth->set_gesture("vol", true);
    state.set("synth.vol", 0.7f);
    EXPECT_EQ(0, filtered.m_count);  // skipped

    // End gesture via root state
    state.set_gesture("synth.vol", false);
    state.set("synth.vol", 0.4f);
    EXPECT_EQ(1, filtered.m_count);  // fires again
}
