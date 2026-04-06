#include "tanh/state/State.h"
#include "tanh/state/Exceptions.h"
#include <gtest/gtest.h>
#include <tanh/core/Numbers.h>

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
