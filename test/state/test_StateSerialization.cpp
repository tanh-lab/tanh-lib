#include <gtest/gtest.h>
#include <tanh/core/Numbers.h>

#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <numbers>
#include <string>
#include <vector>

#include "tanh/state/Exceptions.h"
#include "tanh/state/State.h"

using namespace thl;

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

    state.from_json(simple_update);

    // Verify the updated values
    EXPECT_DOUBLE_EQ(0.8, state.get<double>("volume"));
    EXPECT_TRUE(state.get<bool>("muted"));
    EXPECT_EQ("default device", state.get<std::string>("name", true));  // Unchanged

    // Test nested JSON update
    nlohmann::json nested_update = {{"eq", {{"bass", 7}, {"treble", 4}}}};

    state.from_json(nested_update);

    // Verify the nested updates
    EXPECT_EQ(7, state.get<int>("eq.bass"));
    EXPECT_EQ(4, state.get<int>("eq.treble"));

    // Test mixed update with different types
    nlohmann::json mixed_update = {{"name", "new device"}, {"eq", {{"bass", 10}}}};

    state.from_json(mixed_update);

    EXPECT_EQ("new device", state.get<std::string>("name", true));
    EXPECT_EQ(10, state.get<int>("eq.bass"));
    EXPECT_EQ(4, state.get<int>("eq.treble"));  // Unchanged
}

// Test for JSON state updates with non-existent keys
TEST(StateTests, UpdateFromJsonNonExistentKey) {
    State state;

    // Set up initial state
    state.create("volume", 0.5);

    // JSON with a non-existent key. It sorts after "volume", so the known key comes first.
    nlohmann::json invalid_update = {{"volume", 0.8}, {"z_non_existent", 42}};

    // The update should throw StateKeyNotFoundException
    EXPECT_THROW(
        {
            try {
                state.from_json(invalid_update);
            } catch (const StateKeyNotFoundException& e) {
                // Verify the error message contains the key name
                EXPECT_TRUE(std::string(e.what()).find("z_non_existent") != std::string::npos);
                EXPECT_EQ("z_non_existent", e.key());
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

    state.from_json(deep_update);

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

    state.from_json(numeric_update);

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
// State dump tests
// =============================================================================

// Test empty state dump
TEST(StateTests, EmptyStateDump) {
    State state;

    auto dump = state.to_json();
    const auto& json_dump = dump;  // dump is already nlohmann::json

    EXPECT_TRUE(json_dump.is_array());
    EXPECT_TRUE(json_dump.empty());
}

// Test state dump with parameter definitions
TEST(StateTests, StateDumpWithDefinitions) {
    State state;

    // Create parameters with definitions
    state.create("synth.volume",
                 ParameterDefinition::make_float("Volume", Range::linear(0.0f, 1.0f, 0.01f), 0.75f)
                     .modulatable(true));
    state.create(
        "synth.pitch",
        ParameterDefinition::make_int("Pitch", Range::discrete(-12, 12), 0).modulatable(false));
    state.create("synth.enabled",
                 ParameterDefinition::make_bool("Enabled", true).modulatable(false));

    std::vector<std::string> waveforms = {"Sine", "Saw", "Square"};
    state.create("synth.waveform",
                 ParameterDefinition::make_choice("Waveform", waveforms, 1).modulatable(false));

    // Create a parameter without named definition (value-created)
    state.create("synth.internal_state", 42);

    // Get state dump
    auto dump = state.to_json();

    // Parse JSON
    const auto& json_dump = dump;  // dump is already nlohmann::json

    // Verify it's an array
    EXPECT_TRUE(json_dump.is_array());
    EXPECT_EQ(5u, json_dump.size());

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
            // Linear range — no "skew" key emitted
            EXPECT_FALSE(def.contains("skew"));
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
            EXPECT_EQ(3u, def["data"].size());
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
    state.create("audio.gain",
                 ParameterDefinition::make_float("Gain", Range::linear(0.0f, 2.0f), 1.0f));

    // Some without
    state.create("audio.sample_rate", 44100);
    state.create("audio.buffer_size", 512);

    // Get dump
    auto dump = state.to_json();
    const auto& json_dump = dump;  // dump is already nlohmann::json

    // Verify structure
    EXPECT_TRUE(json_dump.is_array());
    EXPECT_EQ(3u, json_dump.size());

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
    state.create("unipolar_param",
                 ParameterDefinition::make_float("Unipolar", Range::linear(0.0f, 1.0f), 0.5f));
    state.create("bipolar_param",
                 ParameterDefinition::make_float("Bipolar", Range::linear(-1.0f, 1.0f), 0.0f)
                     .polarity(SliderPolarity::Bipolar)
                     .modulatable(true));

    // Get state dump
    auto dump = state.to_json();
    const auto& json_dump = dump;  // dump is already nlohmann::json

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
                 ParameterDefinition::make_float("Frequency",
                                                 Range::power_law(20.0f, 20000.0f, 0.3f, 1.0f),
                                                 440.0f));
    state.create("synth.osc.volume",
                 ParameterDefinition::make_float("Volume", Range::linear(0.0f, 1.0f, 0.01f), 0.8f));
    state.create(
        "synth.filter.cutoff",
        ParameterDefinition::make_float("Cutoff", Range::linear(20.0f, 20000.0f, 1.0f), 1000.0f));

    auto dump = state.to_json(true);
    const auto& json = dump;

    EXPECT_EQ(3u, json.size());
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
    state.create(
        "synth.osc.frequency",
        ParameterDefinition::make_float("Frequency", Range::linear(20.0f, 20000.0f, 1.0f), 440.0f));
    state.create("synth.osc.volume",
                 ParameterDefinition::make_float("Volume", Range::linear(0.0f, 1.0f, 0.01f), 0.8f));

    auto dump = state.to_json(false);
    const auto& json = dump;

    EXPECT_EQ(2u, json.size());
    for (const auto& entry : json) {
        EXPECT_TRUE(entry.contains("key"));
        EXPECT_TRUE(entry.contains("value"));
        EXPECT_FALSE(entry.contains("definition"));
    }
}

TEST(StateTests, GetStateDumpDefaultIncludesDefinitions) {
    State state;
    state.create("param",
                 ParameterDefinition::make_float("Param", Range::linear(0.0f, 1.0f, 0.01f), 0.5f));

    auto dump = state.to_json();  // no arg = include definitions
    const auto& json = dump;
    EXPECT_TRUE(json[0].contains("definition"));
}

TEST(StateTests, GetStateDumpPreservesAllValueTypes) {
    State state;
    state.create("f", 3.14f);
    state.create("d", std::numbers::e);
    state.create("i", 42);
    state.create("b", true);
    state.create("s", std::string("hello"));

    auto dump = state.to_json(false);
    const auto& json = dump;

    EXPECT_EQ(5u, json.size());

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

    auto dump = state.to_json(false);
    const auto& json = dump;

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
    state.create("synth.freq",
                 ParameterDefinition::make_float("Frequency",
                                                 Range::power_law(20.0f, 20000.0f, 0.3f, 1.0f),
                                                 440.0f,
                                                 1)
                     .modulatable(true));

    // With definitions: verify actual values
    auto dump_with = state.to_json(true);
    const auto& json_with = dump_with;
    ASSERT_EQ(1u, json_with.size());

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
    // Power-law range emits skew
    ASSERT_TRUE(def.contains("skew"));
    EXPECT_NEAR(0.3f, def["skew"].get<float>(), 0.01f);
    EXPECT_NEAR(440.0f, def["default_value"].get<float>(), 0.01f);
    EXPECT_EQ(1, def["decimal_places"].get<int>());
    EXPECT_TRUE(def["automation"].get<bool>());
    EXPECT_TRUE(def["modulation"].get<bool>());
    EXPECT_EQ("unipolar", def["slider_polarity"].get<std::string>());

    // Without definitions: verify no definition key at all
    auto dump_without = state.to_json(false);
    const auto& json_without = dump_without;
    ASSERT_EQ(1u, json_without.size());
    EXPECT_EQ("synth.freq", json_without[0]["key"].get<std::string>());
    EXPECT_NEAR(440.0f, json_without[0]["value"].get<float>(), 0.01f);
    EXPECT_FALSE(json_without[0].contains("definition"));
}

TEST(StateTests, GetStateDumpDefinitionChoiceAndBipolar) {
    State state;
    state.create("synth.model",
                 ParameterDefinition::make_choice("Model", {"Saw", "Square", "Sine"}, 0));
    state.create("synth.pan",
                 ParameterDefinition::make_float("Pan", Range::linear(0.0f, 1.0f, 0.01f), 0.5f)
                     .polarity(SliderPolarity::Bipolar)
                     .modulatable(true));

    auto dump = state.to_json(true);
    const auto& json = dump;
    ASSERT_EQ(2u, json.size());

    // Find entries by key
    std::map<std::string, nlohmann::json> entries;
    for (const auto& e : json) { entries[e["key"]] = e; }

    // Choice param
    const auto& model_def = entries["synth.model"]["definition"];
    EXPECT_EQ("choice", model_def["type"].get<std::string>());
    EXPECT_EQ("Model", model_def["name"].get<std::string>());
    ASSERT_TRUE(model_def.contains("data"));
    auto data = model_def["data"].get<std::vector<std::string>>();
    ASSERT_EQ(3u, data.size());
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

    auto dump = state.group_to_json("engine0", false);
    const auto& json = dump;

    EXPECT_EQ(3u, json.size());
    for (const auto& entry : json) {
        std::string key = entry["key"];
        EXPECT_EQ(0u, key.find("engine0"));
    }
}

TEST(StateTests, GetGroupStateDumpDeepPrefix) {
    State state;
    state.create("engine0.grain0.volume", 1.0f);
    state.create("engine0.grain0.size", 0.5f);
    state.create("engine0.grain1.volume", 0.8f);
    state.create("engine1.grain0.volume", 0.7f);

    auto dump = state.group_to_json("engine0.grain0", false);
    const auto& json = dump;

    EXPECT_EQ(2u, json.size());
    for (const auto& entry : json) {
        std::string key = entry["key"];
        EXPECT_EQ(0u, key.find("engine0.grain0"));
    }
}

TEST(StateTests, GetGroupStateDumpNoMatch) {
    State state;
    state.create("engine0.volume", 1.0f);

    auto dump = state.group_to_json("nonexistent", false);
    const auto& json = dump;

    EXPECT_EQ(0u, json.size());
}

TEST(StateTests, GetGroupStateDumpEmptyPrefixReturnsAll) {
    State state;
    state.create("a.b", 1.0f);
    state.create("c.d", 2.0f);

    auto all_dump = state.to_json(false);
    auto group_dump = state.group_to_json("", false);

    EXPECT_EQ(all_dump, group_dump);
}

TEST(StateTests, GetGroupStateDumpWithDefinitions) {
    State state;
    state.create(
        "synth.osc.freq",
        ParameterDefinition::make_float("Frequency", Range::linear(20.0f, 20000.0f, 1.0f), 440.0f));
    state.create("synth.osc.vol",
                 ParameterDefinition::make_float("Volume", Range::linear(0.0f, 1.0f, 0.01f), 0.8f));
    state.create(
        "mixer.vol",
        ParameterDefinition::make_float("Mixer Vol", Range::linear(0.0f, 1.0f, 0.01f), 0.5f));

    auto dump = state.group_to_json("synth", true);
    const auto& json = dump;

    EXPECT_EQ(2u, json.size());
    for (const auto& entry : json) { EXPECT_TRUE(entry.contains("definition")); }
}

TEST(StateTests, GetGroupStateDumpWithoutDefinitions) {
    State state;
    state.create("synth.freq",
                 ParameterDefinition::make_float("Freq", Range::linear(0.0f, 1.0f, 0.01f), 0.5f));

    auto dump = state.group_to_json("synth", false);
    const auto& json = dump;

    EXPECT_EQ(1u, json.size());
    EXPECT_FALSE(json[0].contains("definition"));
}

// =============================================================================
// Save and restore: what the writers write, from_json() reads back
// =============================================================================

namespace {

// One parameter of every type, in groups, away from its initial value.
void fill_all_types(State& state) {
    state.create(
        "synth.osc.freq",
        ParameterDefinition::make_float("Frequency", Range::linear(20.0f, 20000.0f, 1.0f), 440.0f));
    state.create("synth.osc.detune", 0.1);
    state.create("synth.voices", 4);
    state.create("synth.enabled", false);
    state.create("synth.name", std::string("init"));
    state.create("volume", 0.5f);

    state.set("synth.osc.freq", 0.1f);  // not exact in binary: must survive the text form
    state.set("synth.osc.detune", std::numbers::pi);
    state.set("synth.voices", 7);
    state.set("synth.enabled", true);
    state.set("synth.name", std::string("lead \"one\""));
    state.set("volume", 0.8f);
}

void reset_all_types(State& state) {
    state.set("synth.osc.freq", 1.0f);
    state.set("synth.osc.detune", 0.0);
    state.set("synth.voices", 0);
    state.set("synth.enabled", false);
    state.set("synth.name", std::string(""));
    state.set("volume", 0.0f);
}

void expect_all_types_restored(State& state) {
    EXPECT_EQ(0.1f, state.get<float>("synth.osc.freq"));
    EXPECT_EQ(std::numbers::pi, state.get<double>("synth.osc.detune"));
    EXPECT_EQ(7, state.get<int>("synth.voices"));
    EXPECT_TRUE(state.get<bool>("synth.enabled"));
    EXPECT_EQ("lead \"one\"", state.get<std::string>("synth.name", true));
    EXPECT_EQ(0.8f, state.get<float>("volume"));
}

// Counts the notifications a restore sends.
class CountingListener : public ParameterListener {
public:
    void on_parameter_changed(const Parameter& /*param*/) override { ++m_count; }
    int m_count = 0;
};

}  // namespace

// The call from issue #53.
TEST(StateJsonRoundTrip, ToJsonIsReadBack) {
    for (const bool with_definitions : {false, true}) {
        State state;
        fill_all_types(state);
        const nlohmann::json saved = state.to_json(with_definitions);
        ASSERT_TRUE(saved.is_array());

        reset_all_types(state);
        const State::JsonReadReport report = state.from_json(saved);

        expect_all_types_restored(state);
        EXPECT_EQ(6u, report.m_applied);
        EXPECT_TRUE(report.m_skipped.empty());
    }
}

// Through text, as a file or a plugin state chunk holds it.
TEST(StateJsonRoundTrip, ToJsonIsReadBackThroughText) {
    State state;
    fill_all_types(state);
    const std::string text = state.to_json(false).dump();

    reset_all_types(state);
    state.from_json(nlohmann::json::parse(text));

    expect_all_types_restored(state);
}

TEST(StateJsonRoundTrip, ValuesToJsonIsNested) {
    State state;
    state.create("volume", 0.5);
    state.create("muted", true);
    state.create("eq.bass", 5);
    state.create("eq.mid.gain", 0.25);
    state.create("name", std::string("device"));

    const nlohmann::json expected = {{"eq", {{"bass", 5}, {"mid", {{"gain", 0.25}}}}},
                                     {"muted", true},
                                     {"name", "device"},
                                     {"volume", 0.5}};
    EXPECT_EQ(expected, state.values_to_json());
}

TEST(StateJsonRoundTrip, ValuesToJsonIsReadBackThroughText) {
    State state;
    fill_all_types(state);
    const std::string text = state.values_to_json().dump();

    reset_all_types(state);
    const State::JsonReadReport report = state.from_json(nlohmann::json::parse(text));

    expect_all_types_restored(state);
    EXPECT_EQ(6u, report.m_applied);
    EXPECT_TRUE(report.m_skipped.empty());
}

TEST(StateJsonRoundTrip, EmptyStateWritesAnEmptyObject) {
    State state;
    const nlohmann::json values = state.values_to_json();
    EXPECT_TRUE(values.is_object());
    EXPECT_TRUE(values.empty());
    EXPECT_EQ(0u, state.from_json(values).m_applied);
}

// One group is saved with its paths from the root and restored without touching the rest.
TEST(StateJsonRoundTrip, GroupIsRestoredAlone) {
    for (const bool nested : {true, false}) {
        State state;
        state.create("engine0.volume", 0.25f);
        state.create("engine0.grain.size", 0.5f);
        state.create("engine1.volume", 0.75f);
        state.create("mixer.volume", 1.0f);

        const nlohmann::json saved =
            nested ? state.values_to_json("engine0") : state.group_to_json("engine0", false);
        if (nested) {
            const nlohmann::json expected = {
                {"engine0", {{"grain", {{"size", 0.5f}}}, {"volume", 0.25f}}}};
            EXPECT_EQ(expected, saved);
        }

        state.set("engine0.volume", 0.0f);
        state.set("engine0.grain.size", 0.0f);
        state.set("engine1.volume", 0.0f);
        state.set("mixer.volume", 0.0f);

        EXPECT_EQ(2u, state.from_json(saved).m_applied);

        EXPECT_EQ(0.25f, state.get<float>("engine0.volume"));
        EXPECT_EQ(0.5f, state.get<float>("engine0.grain.size"));
        EXPECT_EQ(0.0f, state.get<float>("engine1.volume"));
        EXPECT_EQ(0.0f, state.get<float>("mixer.volume"));
    }
}

TEST(StateJsonRoundTrip, FlatKeysAndMixedForms) {
    State state;
    state.create("eq.bass", 5);
    state.create("eq.mid.gain", 0.0);
    state.create("volume", 0.0);

    state.from_json(nlohmann::json{{"eq.bass", 7}, {"eq", {{"mid.gain", 0.5}}}, {"volume", 1.0}});

    EXPECT_EQ(7, state.get<int>("eq.bass"));
    EXPECT_DOUBLE_EQ(0.5, state.get<double>("eq.mid.gain"));
    EXPECT_DOUBLE_EQ(1.0, state.get<double>("volume"));
}

// A record carries more than key and value (a definition, what a consumer added): ignored.
TEST(StateJsonRoundTrip, RecordsWithExtraFields) {
    State state;
    state.create("gain", 0.0f);

    nlohmann::json records = nlohmann::json::array();
    records.push_back({{"key", "gain"},
                       {"value", 0.5},
                       {"value_normalized", 0.25},
                       {"definition", {{"name", "Gain"}}}});
    EXPECT_EQ(1u, state.from_json(records).m_applied);
    EXPECT_FLOAT_EQ(0.5f, state.get<float>("gain"));
}

// Restoring notifies once per parameter that was written, and passes the source on.
TEST(StateJsonRoundTrip, NotifiesOncePerParameter) {
    State state;
    state.create("a", 0.0);
    state.create("b.c", 0.0);
    state.create("untouched", 0.0);

    CountingListener listener;
    state.add_listener(&listener);
    state.from_json(nlohmann::json{{"a", 1.0}, {"b", {{"c", 2.0}}}});
    EXPECT_EQ(2, listener.m_count);
    state.remove_listener(&listener);
}

// A document with no known shape, and records without a key, are refused in both modes.
TEST(StateJsonRoundTrip, RefusesOtherShapes) {
    State state;
    state.create("a", 1.0);

    for (const State::JsonMode mode : {State::JsonMode::Strict, State::JsonMode::Lenient}) {
        EXPECT_THROW(state.from_json(nlohmann::json(42), nullptr, mode), StateJsonException);
        EXPECT_THROW(state.from_json(nlohmann::json("a"), nullptr, mode), StateJsonException);
        EXPECT_THROW(state.from_json(nlohmann::json::array({1, 2}), nullptr, mode),
                     StateJsonException);
        EXPECT_THROW(state.from_json(nlohmann::json::array({{{"value", 2.0}}}), nullptr, mode),
                     StateJsonException);
        EXPECT_THROW(
            state.from_json(nlohmann::json::array({{{"key", 7}, {"value", 2.0}}}), nullptr, mode),
            StateJsonException);
        // A record that is refused must not let the ones before it through.
        nlohmann::json half = nlohmann::json::array();
        half.push_back({{"key", "a"}, {"value", 5.0}});
        half.push_back({{"value", 6.0}});
        EXPECT_THROW(state.from_json(half, nullptr, mode), StateJsonException);
        EXPECT_DOUBLE_EQ(1.0, state.get<double>("a"));
    }
    // Nothing to apply is not an error.
    EXPECT_EQ(0u, state.from_json(nlohmann::json()).m_applied);
    EXPECT_EQ(0u, state.from_json(nlohmann::json::array()).m_applied);
}

// Strict: nothing is written when one entry cannot be applied, wherever it sits.
TEST(StateJsonRoundTrip, StrictWritesNothingOnAnError) {
    State state;
    state.create("a", 1.0);
    state.create("m.x", 1.0);
    state.create("z", 1.0);
    state.create("name", std::string("keep"));

    CountingListener listener;
    state.add_listener(&listener);

    // Unknown key in the middle, known keys before and after it.
    EXPECT_THROW(state.from_json(nlohmann::json{{"a", 2.0}, {"m", {{"unknown", 5}}}, {"z", 2.0}}),
                 StateKeyNotFoundException);
    // A value that does not fit, after a key that could be written.
    try {
        state.from_json(nlohmann::json{{"a", 2.0}, {"name", 42}});
        ADD_FAILURE() << "a number was accepted for a string parameter";
    } catch (const StateJsonException& e) { EXPECT_EQ("name", e.key()); }
    EXPECT_THROW(state.from_json(nlohmann::json{{"a", 2.0}, {"z", nlohmann::json::array({1})}}),
                 StateJsonException);

    EXPECT_DOUBLE_EQ(1.0, state.get<double>("a"));
    EXPECT_DOUBLE_EQ(1.0, state.get<double>("m.x"));
    EXPECT_DOUBLE_EQ(1.0, state.get<double>("z"));
    EXPECT_EQ("keep", state.get<std::string>("name", true));
    EXPECT_EQ(0, listener.m_count);
    state.remove_listener(&listener);
}

// Lenient: a state saved by another version loads as far as it still applies.
TEST(StateJsonRoundTrip, LenientSkipsWhatItCannotApply) {
    State state;
    state.create("a", 1.0);
    state.create("m.x", 1.0);
    state.create("name", std::string("keep"));
    state.create("z", 1.0);

    const nlohmann::json saved = {{"a", 2.0},
                                  {"m", {{"retired", 5}, {"x", 3.0}}},
                                  {"name", 42},
                                  {"z", 4.0}};
    const State::JsonReadReport report = state.from_json(saved, nullptr, State::JsonMode::Lenient);

    EXPECT_EQ(3u, report.m_applied);
    EXPECT_EQ((std::vector<std::string>{"m.retired", "name"}), report.m_skipped);
    EXPECT_DOUBLE_EQ(2.0, state.get<double>("a"));
    EXPECT_DOUBLE_EQ(3.0, state.get<double>("m.x"));
    EXPECT_EQ("keep", state.get<std::string>("name", true));
    EXPECT_DOUBLE_EQ(4.0, state.get<double>("z"));

    // The same for the array form.
    nlohmann::json records = nlohmann::json::array();
    records.push_back({{"key", "gone"}, {"value", 1}});
    records.push_back({{"key", "a"}, {"value", 9.0}});
    const State::JsonReadReport second =
        state.from_json(records, nullptr, State::JsonMode::Lenient);
    EXPECT_EQ(1u, second.m_applied);
    EXPECT_EQ((std::vector<std::string>{"gone"}), second.m_skipped);
    EXPECT_DOUBLE_EQ(9.0, state.get<double>("a"));
}

// JSON text has no NaN and no infinity: they are written as null, and a null leaves the
// parameter alone instead of failing the restore.
TEST(StateJsonRoundTrip, NonFiniteValuesComeBackAsNull) {
    State state;
    state.create("x", 0.5f);
    state.create("y", 0.5);
    state.create("ok", 1.0);
    state.set("x", std::numeric_limits<float>::quiet_NaN());
    state.set("y", std::numeric_limits<double>::infinity());

    const std::string text = state.values_to_json().dump();
    state.set("x", 0.25f);
    state.set("y", 0.25);
    state.set("ok", 0.0);

    const State::JsonReadReport report = state.from_json(nlohmann::json::parse(text));
    EXPECT_EQ(1u, report.m_applied);
    EXPECT_EQ((std::vector<std::string>{"x", "y"}), report.m_skipped);
    EXPECT_FLOAT_EQ(0.25f, state.get<float>("x"));
    EXPECT_DOUBLE_EQ(0.25, state.get<double>("y"));
    EXPECT_DOUBLE_EQ(1.0, state.get<double>("ok"));

    // A record without a value is the same as a null.
    nlohmann::json records = nlohmann::json::array();
    records.push_back({{"key", "ok"}});
    EXPECT_EQ((std::vector<std::string>{"ok"}), state.from_json(records).m_skipped);
    EXPECT_DOUBLE_EQ(1.0, state.get<double>("ok"));
}

// Numbers are converted by the parameter's type. JavaScript writes 3000000000.0 as
// 3000000000: an integer in JSON that does not fit an int.
TEST(StateJsonRoundTrip, NumbersBeyondInt) {
    State state;
    state.create("d", 0.0);
    state.create("f", 0.0f);
    state.create("i", 0);
    state.create("b", false);

    state.from_json(
        nlohmann::json::parse(R"({"d": 3000000000, "f": 5000000000, "i": 3000000000})"));
    EXPECT_DOUBLE_EQ(3000000000.0, state.get<double>("d"));
    EXPECT_FLOAT_EQ(5000000000.0f, state.get<float>("f"));
    EXPECT_EQ(std::numeric_limits<int>::max(), state.get<int>("i"));

    state.from_json(
        nlohmann::json::parse(R"({"d": -5000000000, "i": -5000000000, "b": 4294967296})"));
    EXPECT_DOUBLE_EQ(-5000000000.0, state.get<double>("d"));
    EXPECT_EQ(std::numeric_limits<int>::min(), state.get<int>("i"));
    EXPECT_TRUE(state.get<bool>("b"));

    state.from_json(nlohmann::json::parse(R"({"i": 1e300})"));
    EXPECT_EQ(std::numeric_limits<int>::max(), state.get<int>("i"));
    state.from_json(nlohmann::json::parse(R"({"i": -1e300})"));
    EXPECT_EQ(std::numeric_limits<int>::min(), state.get<int>("i"));
    state.from_json(nlohmann::json::parse(R"({"i": -2.9, "b": 0.0})"));
    EXPECT_EQ(-2, state.get<int>("i"));
    EXPECT_FALSE(state.get<bool>("b"));
}

// What set() accepts for a numeric parameter, from_json() accepts too.
TEST(StateJsonRoundTrip, BoolsAndStringsForNumericParameters) {
    State state;
    state.create("d", 0.0);
    state.create("i", 0);
    state.create("b", false);

    state.from_json(nlohmann::json{{"d", "0.5"}, {"i", true}, {"b", "true"}});
    EXPECT_DOUBLE_EQ(0.5, state.get<double>("d"));
    EXPECT_EQ(1, state.get<int>("i"));
    EXPECT_TRUE(state.get<bool>("b"));
}

// Text that is no number does not fit a numeric parameter (set() would read it as 0), and
// a number in text is clamped like a JSON number.
TEST(StateJsonRoundTrip, TextThatIsNoNumberDoesNotFit) {
    State state;
    state.create("mode", 3);
    state.create("d", 0.25);

    for (const char* text : {"saw", "", "0.5x", "1 2"}) {
        try {
            state.from_json(nlohmann::json{{"mode", text}});
            ADD_FAILURE() << "'" << text << "' was applied";
        } catch (const StateJsonException& e) { EXPECT_EQ("mode", e.key()); }
    }
    EXPECT_EQ(3, state.get<int>("mode"));

    const State::JsonReadReport report =
        state.from_json(nlohmann::json{{"d", "0.5"}, {"mode", "saw"}},
                        nullptr,
                        State::JsonMode::Lenient);
    EXPECT_EQ(1u, report.m_applied);
    EXPECT_EQ((std::vector<std::string>{"mode"}), report.m_skipped);
    EXPECT_EQ(3, state.get<int>("mode"));
    EXPECT_DOUBLE_EQ(0.5, state.get<double>("d"));

    state.from_json(nlohmann::json{{"mode", "3000000000"}});
    EXPECT_EQ(std::numeric_limits<int>::max(), state.get<int>("mode"));
    state.from_json(nlohmann::json{{"mode", "-7.9"}});
    EXPECT_EQ(-7, state.get<int>("mode"));
}

// An object where a parameter is does not fit; an empty object anywhere else is a group
// without values.
TEST(StateJsonRoundTrip, AnObjectDoesNotFitAParameter) {
    State state;
    state.create("gain", 0.5);

    try {
        state.from_json(nlohmann::json{{"gain", nlohmann::json::object()}});
        ADD_FAILURE() << "an object was applied to a parameter";
    } catch (const StateJsonException& e) { EXPECT_EQ("gain", e.key()); }

    const State::JsonReadReport lenient =
        state.from_json(nlohmann::json{{"gain", nlohmann::json::object()}},
                        nullptr,
                        State::JsonMode::Lenient);
    EXPECT_EQ((std::vector<std::string>{"gain"}), lenient.m_skipped);

    const State::JsonReadReport empty_group =
        state.from_json(nlohmann::json{{"eq", nlohmann::json::object()}});
    EXPECT_EQ(0u, empty_group.m_applied);
    EXPECT_TRUE(empty_group.m_skipped.empty());
    EXPECT_DOUBLE_EQ(0.5, state.get<double>("gain"));
}

// A listener may remove parameters while a restore is writing: what is gone is skipped.
TEST(StateJsonRoundTrip, AListenerMayRemoveParameters) {
    State state;
    state.create("a", 0.0);
    state.create("voices.gain", 0.0);
    state.create("z", 0.0);

    CallbackListener listener([&](const Parameter& param) {
        if (param.key() == "a") { state.get_group("voices")->clear(); }
    });
    state.add_listener(&listener);
    const State::JsonReadReport report =
        state.from_json(nlohmann::json{{"a", 1.0}, {"voices", {{"gain", 0.5}}}, {"z", 2.0}});
    state.remove_listener(&listener);

    EXPECT_EQ(2u, report.m_applied);
    EXPECT_EQ((std::vector<std::string>{"voices.gain"}), report.m_skipped);
    EXPECT_DOUBLE_EQ(1.0, state.get<double>("a"));
    EXPECT_DOUBLE_EQ(2.0, state.get<double>("z"));
}

// Both forms in one document can name a parameter twice: every entry is written.
TEST(StateJsonRoundTrip, AParameterNamedTwiceIsWrittenTwice) {
    State state;
    state.create("eq.bass", 0.0);

    CountingListener listener;
    state.add_listener(&listener);
    const State::JsonReadReport report =
        state.from_json(nlohmann::json{{"eq", {{"bass", 8.0}}}, {"eq.bass", 7.0}});
    state.remove_listener(&listener);

    EXPECT_EQ(2u, report.m_applied);
    EXPECT_EQ(2, listener.m_count);
    EXPECT_DOUBLE_EQ(7.0, state.get<double>("eq.bass"));
}

// The nested form has no place for a parameter below another parameter.
TEST(StateJsonRoundTrip, ValuesToJsonRefusesAParameterBelowAParameter) {
    State state;
    state.create("a", 1.0);
    state.create("a-b", 1.0);  // sorts between "a" and "a.b"
    state.create("a.b", 2.0);
    state.create("c.d", 3.0);

    try {
        (void)state.values_to_json();
        ADD_FAILURE() << "two parameters in one place";
    } catch (const StateJsonException& e) { EXPECT_EQ("a.b", e.key()); }

    // A part without the clash is written, and the array form holds everything.
    EXPECT_EQ((nlohmann::json{{"c", {{"d", 3.0}}}}), state.values_to_json("c"));
    const nlohmann::json all = state.to_json(false);
    state.set("a", 0.0);
    state.set("a.b", 0.0);
    state.from_json(all);
    EXPECT_DOUBLE_EQ(1.0, state.get<double>("a"));
    EXPECT_DOUBLE_EQ(2.0, state.get<double>("a.b"));
}

// A group is its own key and what lies below it, not every key that starts the same.
TEST(StateTests, GetGroupStateDumpMatchesWholeSegments) {
    State state;
    state.create("engine1", 1.0f);
    state.create("engine1.volume", 1.0f);
    state.create("engine10.volume", 0.5f);
    state.create("engine1x", 0.5f);

    const nlohmann::json dump = state.group_to_json("engine1", false);
    ASSERT_EQ(2u, dump.size());
    EXPECT_EQ("engine1", dump[0]["key"].get<std::string>());
    EXPECT_EQ("engine1.volume", dump[1]["key"].get<std::string>());

    EXPECT_EQ((nlohmann::json{{"engine10", {{"volume", 0.5f}}}}), state.values_to_json("engine10"));
}

TEST(StateTests, GetGroupStateDumpPrefixBoundary) {
    State state;
    state.create("engine0.volume", 1.0f);
    state.create("engine0_extra.volume", 0.5f);

    auto dump = state.group_to_json("engine0.", false);
    const auto& json = dump;

    EXPECT_EQ(1u, json.size());
    EXPECT_EQ("engine0.volume", json[0]["key"].get<std::string>());
}

// The same for a group that is cleared or asked whether it is empty.
TEST(StateTests, GroupClearMatchesWholeSegments) {
    State state;
    state.create("engine1.volume", 1.0f);
    state.create("engine10.volume", 0.5f);

    StateGroup* const engine1 = state.get_group("engine1");
    ASSERT_NE(nullptr, engine1);
    engine1->clear();

    EXPECT_TRUE(engine1->is_empty());
    const auto left = state.get_parameters();
    ASSERT_EQ(1u, left.size());
    EXPECT_EQ("engine10.volume", left.begin()->first);
    EXPECT_FLOAT_EQ(0.5f, state.get<float>("engine10.volume"));
}
