#include "tanh/state/State.h"
#include <gtest/gtest.h>

using namespace thl;

// =============================================================================
// Range tests
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

// =============================================================================
// Parameter definition tests
// =============================================================================

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
// Display formatting tests
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
