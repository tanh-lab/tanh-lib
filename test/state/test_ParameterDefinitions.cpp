#include "tanh/state/State.h"
#include "tanh/state/Exceptions.h"
#include <gtest/gtest.h>

using namespace thl;

// =============================================================================
// Range tests
// =============================================================================

TEST(StateTests, RangeLinearFactory) {
    Range r = Range::linear(0.0f, 100.0f, 0.1f);
    EXPECT_FLOAT_EQ(0.0f, r.m_min);
    EXPECT_FLOAT_EQ(100.0f, r.m_max);
    EXPECT_FLOAT_EQ(0.1f, r.m_step);
    EXPECT_TRUE(r.is_linear());
    EXPECT_FALSE(r.m_periodic);
}

TEST(StateTests, RangePowerLawFactory) {
    Range r = Range::power_law(0.0f, 100.0f, 2.0f, 0.1f);
    EXPECT_FLOAT_EQ(0.0f, r.m_min);
    EXPECT_FLOAT_EQ(100.0f, r.m_max);
    EXPECT_FLOAT_EQ(0.1f, r.m_step);
    EXPECT_FLOAT_EQ(2.0f, r.m_curve.m_skew);
    EXPECT_FALSE(r.is_linear());
}

TEST(StateTests, RangeDiscreteFactory) {
    Range r = Range::discrete(0, 127, 1);
    EXPECT_FLOAT_EQ(0.0f, r.m_min);
    EXPECT_FLOAT_EQ(127.0f, r.m_max);
    EXPECT_FLOAT_EQ(1.0f, r.m_step);
    EXPECT_TRUE(r.is_linear());
}

TEST(StateTests, RangeBooleanFactory) {
    Range r = Range::boolean();
    EXPECT_FLOAT_EQ(0.0f, r.m_min);
    EXPECT_FLOAT_EQ(1.0f, r.m_max);
    EXPECT_FLOAT_EQ(1.0f, r.m_step);
}

TEST(StateTests, RangePeriodicFactory) {
    Range r = Range::periodic(0.0f, 360.0f, 1.0f);
    EXPECT_FLOAT_EQ(0.0f, r.m_min);
    EXPECT_FLOAT_EQ(360.0f, r.m_max);
    EXPECT_TRUE(r.m_periodic);
    EXPECT_TRUE(r.is_linear());
}

TEST(StateTests, RangeCustomFactory) {
    auto to_fn = [](float p) -> float { return p * p; };
    auto from_fn = [](float p) -> float { return std::sqrt(p); };
    Range r = Range::custom(20.0f, 20000.0f, to_fn, from_fn, 1.0f);
    EXPECT_FLOAT_EQ(20.0f, r.m_min);
    EXPECT_FLOAT_EQ(20000.0f, r.m_max);
    EXPECT_FALSE(r.is_linear());
    EXPECT_EQ(NormalizationCurve::Type::Custom, r.m_curve.m_type);
}

TEST(StateTests, RangeDefaultConstruction) {
    Range r;
    EXPECT_FLOAT_EQ(0.0f, r.m_min);
    EXPECT_FLOAT_EQ(1.0f, r.m_max);
    EXPECT_FLOAT_EQ(0.01f, r.m_step);
    EXPECT_TRUE(r.is_linear());
    EXPECT_FALSE(r.m_periodic);
}

TEST(StateTests, RangeClamp) {
    Range r = Range::linear(0.0f, 100.0f);
    EXPECT_FLOAT_EQ(0.0f, r.clamp(-5.0f));
    EXPECT_FLOAT_EQ(50.0f, r.clamp(50.0f));
    EXPECT_FLOAT_EQ(100.0f, r.clamp(200.0f));
}

TEST(StateTests, RangeWrap) {
    Range r = Range::periodic(0.0f, 360.0f);
    EXPECT_FLOAT_EQ(10.0f, r.wrap(370.0f));
    EXPECT_FLOAT_EQ(350.0f, r.wrap(-10.0f));
    EXPECT_FLOAT_EQ(0.0f, r.wrap(0.0f));
    EXPECT_FLOAT_EQ(0.0f, r.wrap(360.0f));
}

TEST(StateTests, RangeConstrain) {
    Range linear = Range::linear(0.0f, 100.0f);
    EXPECT_FLOAT_EQ(0.0f, linear.constrain(-5.0f));     // clamps
    EXPECT_FLOAT_EQ(100.0f, linear.constrain(105.0f));  // clamps

    Range periodic = Range::periodic(0.0f, 360.0f);
    EXPECT_FLOAT_EQ(10.0f, periodic.constrain(370.0f));   // wraps
    EXPECT_FLOAT_EQ(350.0f, periodic.constrain(-10.0f));  // wraps
}

TEST(StateTests, RangeToNormalizedLinear) {
    Range r = Range::linear(0.0f, 100.0f);
    EXPECT_FLOAT_EQ(0.0f, r.to_normalized(0.0f));
    EXPECT_FLOAT_EQ(0.5f, r.to_normalized(50.0f));
    EXPECT_FLOAT_EQ(1.0f, r.to_normalized(100.0f));
}

TEST(StateTests, RangeFromNormalizedLinear) {
    Range r = Range::linear(0.0f, 100.0f);
    EXPECT_FLOAT_EQ(0.0f, r.from_normalized(0.0f));
    EXPECT_FLOAT_EQ(50.0f, r.from_normalized(0.5f));
    EXPECT_FLOAT_EQ(100.0f, r.from_normalized(1.0f));
}

TEST(StateTests, RangeNormalizedRoundtripPowerLaw) {
    Range r = Range::power_law(20.0f, 20000.0f, 3.0f);
    // Roundtrip: from_normalized(to_normalized(x)) == x
    for (float plain : {20.0f, 440.0f, 1000.0f, 5000.0f, 20000.0f}) {
        float normalized = r.to_normalized(plain);
        float recovered = r.from_normalized(normalized);
        EXPECT_NEAR(plain, recovered, 0.1f) << "Roundtrip failed for " << plain;
    }
}

TEST(StateTests, RangeNormalizedRoundtripCustom) {
    auto to_fn = [](float p) -> float { return p * p; };
    auto from_fn = [](float p) -> float { return std::sqrt(p); };
    Range r = Range::custom(0.0f, 1000.0f, to_fn, from_fn);
    for (float plain : {0.0f, 100.0f, 500.0f, 1000.0f}) {
        float normalized = r.to_normalized(plain);
        float recovered = r.from_normalized(normalized);
        EXPECT_NEAR(plain, recovered, 0.1f) << "Roundtrip failed for " << plain;
    }
}

TEST(StateTests, RangeSnap) {
    Range r = Range::linear(0.0f, 10.0f, 0.5f);
    EXPECT_FLOAT_EQ(0.0f, r.snap(0.1f));
    EXPECT_FLOAT_EQ(0.5f, r.snap(0.3f));
    EXPECT_FLOAT_EQ(1.0f, r.snap(0.8f));
    EXPECT_FLOAT_EQ(5.0f, r.snap(5.0f));
}

// =============================================================================
// Parameter definition tests
// =============================================================================

TEST(StateTests, ParameterDefinitionFloat) {
    State state;

    auto volume_def =
        ParameterDefinition::make_float("Volume", Range::linear(0.0f, 1.0f, 0.01f), 0.75f);
    state.create("audio.volume", volume_def);

    EXPECT_FLOAT_EQ(0.75f, state.get<float>("audio.volume"));

    Parameter param = state.get_parameter("audio.volume");
    const auto& def = param.def();

    EXPECT_EQ("Volume", def.m_name);
    EXPECT_EQ(ParameterType::Float, def.m_type);
    EXPECT_FLOAT_EQ(0.0f, def.m_range.m_min);
    EXPECT_FLOAT_EQ(1.0f, def.m_range.m_max);
    EXPECT_FLOAT_EQ(0.01f, def.m_range.m_step);
    EXPECT_TRUE(def.m_range.is_linear());
    EXPECT_FLOAT_EQ(0.75f, def.m_default_value);
    EXPECT_EQ(2u, def.m_decimal_places);
    EXPECT_TRUE(def.is_automatable());
    EXPECT_FALSE(def.is_modulatable());
}

TEST(StateTests, ParameterDefinitionInt) {
    State state;

    auto filter_cutoff_def =
        ParameterDefinition::make_int("Filter Cutoff", Range::discrete(20, 20000), 1000);
    state.create("synth.filter.cutoff", filter_cutoff_def);

    EXPECT_EQ(1000, state.get<int>("synth.filter.cutoff"));

    Parameter param = state.get_parameter("synth.filter.cutoff");
    const auto& def = param.def();

    EXPECT_EQ("Filter Cutoff", def.m_name);
    EXPECT_EQ(ParameterType::Int, def.m_type);
    EXPECT_FLOAT_EQ(20.0f, def.m_range.m_min);
    EXPECT_FLOAT_EQ(20000.0f, def.m_range.m_max);
    EXPECT_FLOAT_EQ(1.0f, def.m_range.m_step);
    EXPECT_EQ(1000, def.as_int());
    EXPECT_EQ(0u, def.m_decimal_places);
    EXPECT_TRUE(def.is_automatable());
    EXPECT_FALSE(def.is_modulatable());
}

TEST(StateTests, ParameterDefinitionBool) {
    State state;

    auto bypass_def = ParameterDefinition::make_bool("Bypass", false).modulatable(false);
    state.create("effects.reverb.bypass", bypass_def);

    EXPECT_FALSE(state.get<bool>("effects.reverb.bypass"));

    Parameter param = state.get_parameter("effects.reverb.bypass");
    const auto& def = param.def();

    EXPECT_EQ("Bypass", def.m_name);
    EXPECT_EQ(ParameterType::Bool, def.m_type);
    EXPECT_FLOAT_EQ(0.0f, def.m_range.m_min);
    EXPECT_FLOAT_EQ(1.0f, def.m_range.m_max);
    EXPECT_FALSE(def.as_bool());
    EXPECT_EQ(0u, def.m_decimal_places);
    EXPECT_TRUE(def.is_automatable());
    EXPECT_FALSE(def.is_modulatable());
}

TEST(StateTests, ParameterDefinitionChoice) {
    State state;

    std::vector<std::string> waveforms = {"Sine", "Saw", "Square", "Triangle"};
    auto waveform_def =
        ParameterDefinition::make_choice("Waveform", waveforms, 0).modulatable(false);
    state.create("oscillator.waveform", waveform_def);

    EXPECT_EQ(0, state.get<int>("oscillator.waveform"));

    Parameter param = state.get_parameter("oscillator.waveform");
    const auto& def = param.def();

    EXPECT_EQ("Waveform", def.m_name);
    EXPECT_EQ(ParameterType::Int, def.m_type);
    EXPECT_FLOAT_EQ(0.0f, def.m_range.m_min);
    EXPECT_FLOAT_EQ(3.0f, def.m_range.m_max);
    EXPECT_FLOAT_EQ(1.0f, def.m_range.m_step);
    EXPECT_EQ(0, def.as_int());
    EXPECT_EQ(4u, def.m_choices.size());
    EXPECT_EQ("Sine", def.m_choices[0]);
    EXPECT_EQ("Saw", def.m_choices[1]);
    EXPECT_EQ("Square", def.m_choices[2]);
    EXPECT_EQ("Triangle", def.m_choices[3]);
    EXPECT_TRUE(def.is_automatable());
    EXPECT_FALSE(def.is_modulatable());
}

TEST(StateTests, ParameterDefinitionTypeConversions) {
    auto float_param =
        ParameterDefinition::make_float("Test Float", Range::linear(0.0f, 1.0f), 0.5f);
    EXPECT_FLOAT_EQ(0.5f, float_param.as_float());
    EXPECT_EQ(0, float_param.as_int());
    EXPECT_TRUE(float_param.as_bool());

    auto int_param = ParameterDefinition::make_int("Test Int", Range::discrete(0, 100), 42);
    EXPECT_FLOAT_EQ(42.0f, int_param.as_float());
    EXPECT_EQ(42, int_param.as_int());
    EXPECT_TRUE(int_param.as_bool());

    auto bool_param_true = ParameterDefinition::make_bool("Test Bool True", true);
    EXPECT_FLOAT_EQ(1.0f, bool_param_true.as_float());
    EXPECT_EQ(1, bool_param_true.as_int());
    EXPECT_TRUE(bool_param_true.as_bool());

    auto bool_param_false = ParameterDefinition::make_bool("Test Bool False", false);
    EXPECT_FLOAT_EQ(0.0f, bool_param_false.as_float());
    EXPECT_EQ(0, bool_param_false.as_int());
    EXPECT_FALSE(bool_param_false.as_bool());

    std::vector<std::string> choices = {"A", "B", "C"};
    auto choice_param = ParameterDefinition::make_choice("Test Choice", choices, 1);
    EXPECT_FLOAT_EQ(1.0f, choice_param.as_float());
    EXPECT_EQ(1, choice_param.as_int());
    EXPECT_TRUE(choice_param.as_bool());
}

TEST(StateTests, ParameterDefinitionPersistence) {
    State state;

    auto gain_def = ParameterDefinition::make_float("Gain", Range::linear(0.0f, 2.0f, 0.01f), 1.0f);
    state.create("gain", gain_def);

    EXPECT_FLOAT_EQ(1.0f, state.get<float>("gain"));

    state.set("gain", 1.5f);
    EXPECT_FLOAT_EQ(1.5f, state.get<float>("gain"));

    Parameter param = state.get_parameter("gain");
    const auto& def = param.def();

    EXPECT_EQ("Gain", def.m_name);
    EXPECT_FLOAT_EQ(1.0f, def.m_default_value);
    EXPECT_FLOAT_EQ(0.0f, def.m_range.m_min);
    EXPECT_FLOAT_EQ(2.0f, def.m_range.m_max);
}

TEST(StateTests, CreateThrowsOnDuplicateKey) {
    State state;

    state.create("param",
                 ParameterDefinition::make_float("Initial", Range::linear(0.0f, 1.0f), 0.5f));

    EXPECT_THROW(
        state.create("param",
                     ParameterDefinition::make_float("Updated", Range::linear(0.0f, 10.0f), 5.0f)),
        ParameterAlreadyExistsException);

    const auto& def = state.get_parameter("param").def();
    EXPECT_EQ("Initial", def.m_name);
    EXPECT_FLOAT_EQ(0.5f, def.m_default_value);
}

TEST(StateTests, MultipleParameterDefinitions) {
    State state;

    state.create("reverb.dry_wet",
                 ParameterDefinition::make_float("Dry/Wet", Range::linear(0.0f, 1.0f), 0.3f));
    state.create("reverb.room_size",
                 ParameterDefinition::make_float("Room Size", Range::linear(0.0f, 1.0f), 0.5f));
    state.create("reverb.damping",
                 ParameterDefinition::make_float("Damping", Range::linear(0.0f, 1.0f), 0.5f));
    state.create("reverb.enabled", ParameterDefinition::make_bool("Enabled", true));

    std::vector<std::string> room_types = {"Small", "Medium", "Large", "Hall"};
    state.create("reverb.type", ParameterDefinition::make_choice("Room Type", room_types, 1));

    EXPECT_FLOAT_EQ(0.3f, state.get<float>("reverb.dry_wet"));
    EXPECT_FLOAT_EQ(0.5f, state.get<float>("reverb.room_size"));
    EXPECT_FLOAT_EQ(0.5f, state.get<float>("reverb.damping"));
    EXPECT_TRUE(state.get<bool>("reverb.enabled"));
    EXPECT_EQ(1, state.get<int>("reverb.type"));

    EXPECT_EQ("Dry/Wet", state.get_parameter("reverb.dry_wet").def().m_name);
    EXPECT_EQ("Room Size", state.get_parameter("reverb.room_size").def().m_name);
    EXPECT_EQ("Damping", state.get_parameter("reverb.damping").def().m_name);
    EXPECT_EQ("Enabled", state.get_parameter("reverb.enabled").def().m_name);
    EXPECT_EQ("Room Type", state.get_parameter("reverb.type").def().m_name);

    const auto& type_def = state.get_parameter("reverb.type").def();
    EXPECT_EQ("Room Type", type_def.m_name);
    EXPECT_EQ(4u, type_def.m_choices.size());
    EXPECT_EQ("Medium", type_def.m_choices[1]);
}

TEST(StateTests, ParameterWithDefaultDefinition) {
    State state;

    state.create("simple.param", 42.0);

    EXPECT_DOUBLE_EQ(42.0, state.get<double>("simple.param"));

    Parameter param = state.get_parameter("simple.param");
    const auto& def = param.def();
    EXPECT_TRUE(def.m_name.empty());
    EXPECT_EQ(ParameterType::Double, def.m_type);
}

TEST(StateTests, ChoiceParameterUsage) {
    State state;

    std::vector<std::string> filter_types = {"Low Pass", "High Pass", "Band Pass", "Notch"};
    state.create("filter.type", ParameterDefinition::make_choice("Filter Type", filter_types, 0));

    const auto& def = state.get_parameter("filter.type").def();

    EXPECT_FLOAT_EQ(0.0f, def.m_range.m_min);
    EXPECT_FLOAT_EQ(3.0f, def.m_range.m_max);

    EXPECT_EQ(4u, def.m_choices.size());
    EXPECT_EQ("Low Pass", def.m_choices[0]);
    EXPECT_EQ("High Pass", def.m_choices[1]);
    EXPECT_EQ("Band Pass", def.m_choices[2]);
    EXPECT_EQ("Notch", def.m_choices[3]);

    state.set("filter.type", 2);
    EXPECT_EQ(2, state.get<int>("filter.type"));

    const auto& def2 = state.get_parameter("filter.type").def();
    EXPECT_EQ("Band Pass", def2.m_choices[2]);
}

TEST(StateTests, AutomationModulationFlags) {
    State state;

    // automatable but not modulatable
    state.create("param1",
                 ParameterDefinition::make_float("Param 1", Range::linear(0.0f, 1.0f), 0.5f)
                     .modulatable(false));

    // both
    state.create("param2",
                 ParameterDefinition::make_float("Param 2", Range::linear(0.0f, 1.0f), 0.5f)
                     .modulatable(true));

    // neither
    state.create("param3",
                 ParameterDefinition::make_float("Param 3", Range::linear(0.0f, 1.0f), 0.5f)
                     .automatable(false)
                     .modulatable(false));

    const auto& def1 = state.get_parameter("param1").def();
    EXPECT_TRUE(def1.is_automatable());
    EXPECT_FALSE(def1.is_modulatable());

    const auto& def2 = state.get_parameter("param2").def();
    EXPECT_TRUE(def2.is_automatable());
    EXPECT_TRUE(def2.is_modulatable());

    const auto& def3 = state.get_parameter("param3").def();
    EXPECT_FALSE(def3.is_automatable());
    EXPECT_FALSE(def3.is_modulatable());
}

TEST(StateTests, SliderPolarityFlags) {
    State state;

    // Default polarity
    state.create(
        "unipolar_float",
        ParameterDefinition::make_float("Unipolar Float", Range::linear(0.0f, 1.0f), 0.5f));

    // Explicit bipolar
    state.create("bipolar_float",
                 ParameterDefinition::make_float("Bipolar Float", Range::linear(-1.0f, 1.0f), 0.0f)
                     .polarity(SliderPolarity::Bipolar)
                     .modulatable(true));

    // Int bipolar
    state.create("bipolar_int",
                 ParameterDefinition::make_int("Bipolar Int", Range::discrete(-12, 12), 0)
                     .polarity(SliderPolarity::Bipolar)
                     .modulatable(true));

    const auto& def1 = state.get_parameter("unipolar_float").def();
    EXPECT_EQ(SliderPolarity::Unipolar, def1.m_polarity);

    const auto& def2 = state.get_parameter("bipolar_float").def();
    EXPECT_EQ(SliderPolarity::Bipolar, def2.m_polarity);

    const auto& def3 = state.get_parameter("bipolar_int").def();
    EXPECT_EQ(SliderPolarity::Bipolar, def3.m_polarity);

    // Bool parameter (should default to Unipolar)
    state.create("bool_param", ParameterDefinition::make_bool("Bool Param", false));
    const auto& def4 = state.get_parameter("bool_param").def();
    EXPECT_EQ(SliderPolarity::Unipolar, def4.m_polarity);

    // Choice parameter (should default to Unipolar)
    std::vector<std::string> choices = {"A", "B", "C"};
    state.create("choice_param", ParameterDefinition::make_choice("Choice Param", choices, 0));
    const auto& def5 = state.get_parameter("choice_param").def();
    EXPECT_EQ(SliderPolarity::Unipolar, def5.m_polarity);
}

TEST(StateTests, ChoiceParameterChoicesField) {
    State state;

    std::vector<std::string> waveforms = {"Sine", "Saw", "Square"};
    state.create("waveform", ParameterDefinition::make_choice("Waveform", waveforms, 0));

    const auto& choice_def = state.get_parameter("waveform").def();
    EXPECT_EQ(3u, choice_def.m_choices.size());
    EXPECT_EQ("Sine", choice_def.m_choices[0]);
    EXPECT_EQ("Saw", choice_def.m_choices[1]);
    EXPECT_EQ("Square", choice_def.m_choices[2]);
}

// =============================================================================
// Display formatting tests
// =============================================================================

TEST(StateTests, FloatValueToText) {
    auto def = ParameterDefinition::make_float("Gain", Range::linear(0.0f, 10.0f), 3.14f);
    EXPECT_EQ("3.14", def.m_value_to_text(3.14f));
    EXPECT_EQ("0.00", def.m_value_to_text(0.0f));
    EXPECT_EQ("10.00", def.m_value_to_text(10.0f));
}

TEST(StateTests, FloatValueToTextDecimalPlaces) {
    auto def0 = ParameterDefinition::make_float("P", Range(), 0.0f, 0);
    EXPECT_EQ("3", def0.m_value_to_text(3.14f));

    auto def4 = ParameterDefinition::make_float("P", Range(), 0.0f, 4);
    EXPECT_EQ("3.1416", def4.m_value_to_text(3.14159f));
}

TEST(StateTests, FloatTextToValue) {
    auto def = ParameterDefinition::make_float("Gain", Range::linear(0.0f, 10.0f), 0.0f);
    EXPECT_FLOAT_EQ(3.14f, def.m_text_to_value("3.14"));
    EXPECT_FLOAT_EQ(0.0f, def.m_text_to_value("0"));
    EXPECT_FLOAT_EQ(0.0f, def.m_text_to_value("not_a_number"));
}

TEST(StateTests, IntValueToText) {
    auto def = ParameterDefinition::make_int("Steps", Range::discrete(0, 100), 42);
    EXPECT_EQ("42", def.m_value_to_text(42.0f));
    EXPECT_EQ("0", def.m_value_to_text(0.0f));
    EXPECT_EQ("-5", def.m_value_to_text(-5.0f));
}

TEST(StateTests, IntTextToValue) {
    auto def = ParameterDefinition::make_int("Steps", Range::discrete(0, 100), 0);
    EXPECT_FLOAT_EQ(42.0f, def.m_text_to_value("42"));
    EXPECT_FLOAT_EQ(0.0f, def.m_text_to_value("not_a_number"));
}

TEST(StateTests, BoolValueToText) {
    auto def = ParameterDefinition::make_bool("Enable", true);
    EXPECT_EQ("On", def.m_value_to_text(1.0f));
    EXPECT_EQ("Off", def.m_value_to_text(0.0f));
}

TEST(StateTests, BoolTextToValue) {
    auto def = ParameterDefinition::make_bool("Enable");
    EXPECT_FLOAT_EQ(1.0f, def.m_text_to_value("On"));
    EXPECT_FLOAT_EQ(1.0f, def.m_text_to_value("on"));
    EXPECT_FLOAT_EQ(1.0f, def.m_text_to_value("true"));
    EXPECT_FLOAT_EQ(1.0f, def.m_text_to_value("1"));
    EXPECT_FLOAT_EQ(0.0f, def.m_text_to_value("Off"));
    EXPECT_FLOAT_EQ(0.0f, def.m_text_to_value("anything_else"));
}

TEST(StateTests, ChoiceValueToText) {
    auto def = ParameterDefinition::make_choice("Waveform", {"Sine", "Saw", "Square"}, 0);
    EXPECT_EQ("Sine", def.m_value_to_text(0.0f));
    EXPECT_EQ("Saw", def.m_value_to_text(1.0f));
    EXPECT_EQ("Square", def.m_value_to_text(2.0f));
    // Out of range falls back to int string
    EXPECT_EQ("5", def.m_value_to_text(5.0f));
}

TEST(StateTests, ChoiceTextToValue) {
    auto def = ParameterDefinition::make_choice("Waveform", {"Sine", "Saw", "Square"}, 0);
    EXPECT_FLOAT_EQ(0.0f, def.m_text_to_value("Sine"));
    EXPECT_FLOAT_EQ(1.0f, def.m_text_to_value("Saw"));
    EXPECT_FLOAT_EQ(2.0f, def.m_text_to_value("Square"));
    // Numeric fallback
    EXPECT_FLOAT_EQ(1.0f, def.m_text_to_value("1"));
    // Unknown label falls back to parsing
    EXPECT_FLOAT_EQ(0.0f, def.m_text_to_value("Unknown"));
}

TEST(StateTests, DisplayFormattingViaParameterDef) {
    State state;
    state.create("gain",
                 ParameterDefinition::make_float("Gain", Range::linear(0.0f, 1.0f), 0.75f, 3));

    Parameter param = state.get_parameter_from_root("gain");
    const auto& def = param.def();
    ASSERT_TRUE(def.m_value_to_text);
    EXPECT_EQ("0.750", def.m_value_to_text(param.to<float>()));
}

TEST(StateTests, DisplayFormattingViaHandle) {
    State state;
    state.create(
        "freq",
        ParameterDefinition::make_float("Frequency", Range::linear(20.0f, 20000.0f), 440.0f, 1));

    auto handle = state.get_handle<float>("freq");
    ASSERT_TRUE(handle.def().m_value_to_text);
    EXPECT_EQ("440.0", handle.def().m_value_to_text(handle.load()));
}

// =============================================================================
// Chainable setter tests
// =============================================================================

TEST(StateTests, ChainableSetters) {
    auto def = ParameterDefinition::make_float("Freq", Range::linear(20.0f, 20000.0f), 440.0f, 1)
                   .unit("Hz")
                   .short_name("F")
                   .polarity(SliderPolarity::Unipolar)
                   .automatable(true)
                   .modulatable(true)
                   .param_id(42);

    EXPECT_EQ("Hz", def.m_unit);
    EXPECT_EQ("F", def.m_short_name);
    EXPECT_EQ(SliderPolarity::Unipolar, def.m_polarity);
    EXPECT_TRUE(def.is_automatable());
    EXPECT_TRUE(def.is_modulatable());
    EXPECT_EQ(42u, def.m_id);
}

// =============================================================================
// Default formatter on value-created params
// =============================================================================

TEST(StateTests, ValueCreatedParamGetsFormatters) {
    State state;
    state.create("val", 42.0f);

    Parameter param = state.get_parameter("val");
    const auto& def = param.def();

    // Value-created params should get default formatters via ensure_formatters()
    ASSERT_TRUE(def.m_value_to_text);
    ASSERT_TRUE(def.m_text_to_value);
    EXPECT_EQ("42.00", def.m_value_to_text(42.0f));
    EXPECT_FLOAT_EQ(42.0f, def.m_text_to_value("42"));
}

TEST(StateTests, ValueCreatedBoolGetsFormatters) {
    State state;
    state.create("flag", true);

    Parameter param = state.get_parameter("flag");
    const auto& def = param.def();

    ASSERT_TRUE(def.m_value_to_text);
    ASSERT_TRUE(def.m_text_to_value);
    EXPECT_EQ("On", def.m_value_to_text(1.0f));
    EXPECT_EQ("Off", def.m_value_to_text(0.0f));
}

TEST(StateTests, ValueCreatedIntGetsFormatters) {
    State state;
    state.create("count", 7);

    Parameter param = state.get_parameter("count");
    const auto& def = param.def();

    ASSERT_TRUE(def.m_value_to_text);
    ASSERT_TRUE(def.m_text_to_value);
    EXPECT_EQ("7", def.m_value_to_text(7.0f));
    EXPECT_FLOAT_EQ(7.0f, def.m_text_to_value("7"));
}
