#include "tanh/state/State.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <tanh/core/Numbers.h>

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
