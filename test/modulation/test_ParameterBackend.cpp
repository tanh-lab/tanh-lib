// ModulationMatrix over a host ParameterBackend — no thl::State involved.
// FakeBackend stands in for a host parameter store (e.g. JUCE's
// AudioProcessorValueTreeState): plain std::atomic<float> base values and
// std::atomic<bool> gesture flags owned by the host.

#include <gtest/gtest.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/ModulationSource.h>
#include <tanh/modulation/ParameterBackend.h>
#include <tanh/modulation/SmartHandle.h>
#include <tanh/state/ParameterDefinitions.h>

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "TestHelpers.h"

using namespace thl::modulation;

namespace {

class FakeBackend : public ParameterBackend {
public:
    struct Entry {
        thl::ParameterDefinition m_def;
        std::atomic<float> m_value{0.0f};
        std::atomic<bool> m_gesture{false};
    };

    Entry& add(const std::string& key, thl::ParameterDefinition def) {
        auto entry = std::make_unique<Entry>();
        entry->m_value.store(def.m_default_value);
        entry->m_def = std::move(def);
        auto& ref = *entry;
        m_entries[key] = std::move(entry);
        return ref;
    }

    std::optional<ParameterBinding> find(std::string_view key) const override {
        auto it = m_entries.find(key);
        if (it == m_entries.end()) { return std::nullopt; }
        const Entry& e = *it->second;
        return ParameterBinding{
            .m_def = e.m_def,
            .m_base = &e.m_value,
            .m_in_gesture = m_bind_gesture ? &e.m_gesture : nullptr,
        };
    }

    bool m_bind_gesture = true;

private:
    std::map<std::string, std::unique_ptr<Entry>, std::less<>> m_entries;
};

class ConstSource : public ModulationSource {
public:
    float m_value = 1.0f;
    ConstSource() : ModulationSource(k_global_scope, true) {}
    void prepare(double /*sr*/, size_t spb, uint32_t voice_count) override {
        resize_buffers(spb, voice_count);
    }
    void process(size_t num_samples, size_t offset = 0) override {
        for (size_t i = offset; i < offset + num_samples; ++i) { m_output_buffer[i] = m_value; }
        if (num_samples > 0) { record_change_point(static_cast<uint32_t>(offset)); }
    }
};

thl::ParameterDefinition mod_float(thl::Range range, float default_value) {
    return thl::ParameterDefinition::make_float("p", range, default_value).modulatable(true);
}

}  // namespace

TEST(ParameterBackend, HandleReadsBaseAtomicWithoutRouting) {
    FakeBackend backend;
    auto& gain = backend.add("gain", mod_float(thl::Range::linear(0.0f, 1.0f), 0.25f));
    ModulationMatrix matrix(backend);

    auto handle = matrix.get_smart_handle<float>("gain");
    ASSERT_TRUE(handle.is_valid());
    EXPECT_FLOAT_EQ(handle.load(), 0.25f);
    EXPECT_FLOAT_EQ(handle.load_normalized(), 0.25f);
    EXPECT_EQ(handle.key(), "gain");
    EXPECT_EQ(handle.def().m_name, "p");
    EXPECT_FLOAT_EQ(handle.range().m_max, 1.0f);

    gain.m_value.store(0.75f);
    EXPECT_FLOAT_EQ(handle.load(), 0.75f);
    EXPECT_FALSE(matrix.has_state());
}

TEST(ParameterBackend, AdditiveNormalizedDepthLinearRange) {
    FakeBackend backend;
    backend.add("cutoff", mod_float(thl::Range::linear(0.0f, 100.0f), 20.0f));
    ModulationMatrix matrix(backend);

    ConstSource src;
    src.m_value = 0.5f;
    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<float>("cutoff");
    // depth 0.5 normalized → 0.5 * span(100) = 50 plain; source 0.5 → +25.
    ASSERT_NE(matrix.add_routing({"src", "cutoff", 0.5f}), k_invalid_routing_id);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    EXPECT_FLOAT_EQ(handle.load(0), 45.0f);
    EXPECT_FLOAT_EQ(handle.load(k_block_size - 1), 45.0f);
}

TEST(ParameterBackend, AdditiveNormalizedDepthNonLinearRange) {
    FakeBackend backend;
    const auto range = thl::Range::power_law(20.0f, 20000.0f, 0.3f);
    backend.add("freq", mod_float(range, 1000.0f));
    ModulationMatrix matrix(backend);

    ConstSource src;
    src.m_value = 0.5f;
    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<float>("freq");
    ASSERT_NE(matrix.add_routing({"src", "freq", 0.2f}), k_invalid_routing_id);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);
    EXPECT_TRUE(target->m_uses_normalized_buffer);

    // Modulation happens in normalized space: norm(1000) + 0.5 * 0.2.
    const float expected_norm = range.to_normalized(1000.0f) + 0.1f;
    EXPECT_NEAR(handle.load(0), range.from_normalized(expected_norm), 1e-2f);
    EXPECT_NEAR(handle.load_normalized(0), expected_norm, 1e-5f);
}

TEST(ParameterBackend, ReplaceWithReplaceRange) {
    FakeBackend backend;
    auto& freq = backend.add("freq", mod_float(thl::Range::linear(0.0f, 1000.0f), 900.0f));
    ModulationMatrix matrix(backend);

    ConstSource src;
    src.m_value = 0.5f;
    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<float>("freq");

    ModulationRouting routing;
    routing.m_source_id = "src";
    routing.m_target_id = "freq";
    routing.m_combine_mode = CombineMode::Replace;
    routing.m_replace_range_min = 200.0f;
    routing.m_replace_range_max = 800.0f;
    routing.m_has_replace_range = true;
    ASSERT_NE(matrix.add_routing(routing), k_invalid_routing_id);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // 200 + 0.5 * (800 - 200) = 500, independent of the base value.
    EXPECT_FLOAT_EQ(handle.load(0), 500.0f);
    freq.m_value.store(100.0f);
    matrix.process(k_block_size);
    EXPECT_FLOAT_EQ(handle.load(0), 500.0f);
    EXPECT_FLOAT_EQ(handle.load_base(), 100.0f);
}

TEST(ParameterBackend, ReplaceRangeNormalizedUsesBackendRange) {
    FakeBackend backend;
    backend.add("freq", mod_float(thl::Range::linear(0.0f, 1000.0f), 900.0f));
    ModulationMatrix matrix(backend);

    ConstSource src;
    src.m_value = 0.5f;
    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<float>("freq");

    ModulationRouting routing;
    routing.m_source_id = "src";
    routing.m_target_id = "freq";
    routing.m_combine_mode = CombineMode::Replace;
    ASSERT_NE(matrix.add_routing(routing), k_invalid_routing_id);
    ASSERT_TRUE(matrix.update_routing_replace_range_normalized("src", "freq", 0.25f, 0.75f));
    EXPECT_FALSE(matrix.update_routing_replace_range_normalized("src", "missing", 0.0f, 1.0f));

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // [0.25, 0.75] normalized → [250, 750] plain; source 0.5 → 500.
    EXPECT_FLOAT_EQ(handle.load(0), 500.0f);
}

TEST(ParameterBackend, SkipDuringGestureReadsBackendFlag) {
    FakeBackend backend;
    auto& freq = backend.add("freq", mod_float(thl::Range::linear(0.0f, 1.0f), 0.0f));
    ModulationMatrix matrix(backend);

    ConstSource src;
    src.m_value = 1.0f;
    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<float>("freq");

    ModulationRouting routing;
    routing.m_source_id = "src";
    routing.m_target_id = "freq";
    routing.m_depth = 0.5f;
    routing.m_skip_during_gesture = true;
    ASSERT_NE(matrix.add_routing(routing), k_invalid_routing_id);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);
    EXPECT_FLOAT_EQ(handle.load(0), 0.5f);

    freq.m_gesture.store(true);
    matrix.process(k_block_size);
    EXPECT_FLOAT_EQ(handle.load(0), 0.0f);
    EXPECT_FLOAT_EQ(handle.load(k_block_size - 1), 0.0f);

    freq.m_gesture.store(false);
    matrix.process(k_block_size);
    EXPECT_FLOAT_EQ(handle.load(0), 0.5f);
}

TEST(ParameterBackend, NullGesturePointerNeverSkips) {
    FakeBackend backend;
    backend.m_bind_gesture = false;
    backend.add("freq", mod_float(thl::Range::linear(0.0f, 1.0f), 0.0f));
    ModulationMatrix matrix(backend);

    ConstSource src;
    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<float>("freq");

    ModulationRouting routing;
    routing.m_source_id = "src";
    routing.m_target_id = "freq";
    routing.m_depth = 0.5f;
    routing.m_skip_during_gesture = true;
    ASSERT_NE(matrix.add_routing(routing), k_invalid_routing_id);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);
    EXPECT_EQ(matrix.get_target("freq")->m_in_gesture, nullptr);
    EXPECT_FLOAT_EQ(handle.load(0), 0.5f);
}

TEST(ParameterBackend, ChangePoints) {
    FakeBackend backend;
    backend.add("a", mod_float(thl::Range::linear(0.0f, 1.0f), 0.0f));
    backend.add("b", mod_float(thl::Range::linear(0.0f, 1.0f), 0.0f));
    ModulationMatrix matrix(backend);

    ConstSource src;
    matrix.add_source("src", &src);
    auto a = matrix.get_smart_handle<float>("a");
    auto b = matrix.get_smart_handle<float>("b");
    EXPECT_EQ(a.change_points(), nullptr);

    ASSERT_NE(matrix.add_routing({"src", "a", 0.5f}), k_invalid_routing_id);
    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    ASSERT_NE(a.change_points(), nullptr);
    EXPECT_EQ(*a.change_points(), std::vector<uint32_t>{0});
    EXPECT_EQ(b.change_points(), nullptr);

    const std::array<SmartHandle<float>, 2> handles{a, b};
    std::vector<uint32_t> collected;
    collected.reserve(k_block_size);
    collect_change_points(std::span<const SmartHandle<float>>(handles), collected);
    EXPECT_EQ(collected, std::vector<uint32_t>{0});
}

TEST(ParameterBackend, BaseChangeBetweenBlocksIsReflected) {
    FakeBackend backend;
    auto& cutoff = backend.add("cutoff", mod_float(thl::Range::linear(0.0f, 100.0f), 10.0f));
    ModulationMatrix matrix(backend);

    ConstSource src;
    src.m_value = 1.0f;
    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<float>("cutoff");
    ASSERT_NE(matrix.add_routing({"src", "cutoff", 0.1f}), k_invalid_routing_id);  // +10

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);
    EXPECT_FLOAT_EQ(handle.load(0), 20.0f);

    cutoff.m_value.store(50.0f);
    matrix.process(k_block_size);
    EXPECT_FLOAT_EQ(handle.load(0), 60.0f);
    EXPECT_FLOAT_EQ(handle.load(k_block_size - 1), 60.0f);
}

TEST(ParameterBackend, BindingIsResolvedOnceAndCopied) {
    FakeBackend backend;
    backend.add("gain", mod_float(thl::Range::linear(0.0f, 1.0f), 0.5f));
    ModulationMatrix matrix(backend);

    auto h1 = matrix.get_smart_handle<float>("gain");
    auto h2 = matrix.get_smart_handle<float>("gain");
    EXPECT_EQ(h1.target(), h2.target());
    EXPECT_EQ(&h1.range(), matrix.get_target("gain")->m_range);
}

TEST(ParameterBackend, LookupErrors) {
    FakeBackend backend;
    backend.add("fixed",
                thl::ParameterDefinition::make_float("f", thl::Range::linear(0.0f, 1.0f), 0.0f)
                    .modulatable(false));
    backend.add("gain", mod_float(thl::Range::linear(0.0f, 1.0f), 0.0f));
    ModulationMatrix matrix(backend);
    ConstSource src;
    matrix.add_source("src", &src);

    EXPECT_THROW(matrix.get_smart_handle<float>("missing"), std::out_of_range);
    EXPECT_THROW(matrix.get_smart_handle<float>("fixed"), std::invalid_argument);
    EXPECT_THROW(matrix.get_smart_handle<int>("gain"), std::invalid_argument);

    EXPECT_EQ(matrix.add_routing({"src", "missing"}), k_invalid_routing_id);
    EXPECT_EQ(matrix.add_routing({"src", "fixed"}), k_invalid_routing_id);
    EXPECT_NE(matrix.add_routing({"src", "gain"}), k_invalid_routing_id);
}

TEST(ParameterBackend, JsonRoundTripWithoutState) {
    FakeBackend backend;
    backend.add("gain", mod_float(thl::Range::linear(0.0f, 1.0f), 0.0f));
    ModulationMatrix matrix(backend);
    ConstSource src;
    matrix.add_source("src", &src);
    ASSERT_NE(matrix.add_routing({"src", "gain", 0.3f}), k_invalid_routing_id);

    EXPECT_THROW((void)matrix.state(), std::logic_error);

    const auto json = matrix.to_json();
    EXPECT_FALSE(json.contains("parameters"));
    ASSERT_TRUE(json.contains("modulation_routings"));
    EXPECT_EQ(json["modulation_routings"].size(), 1u);

    ModulationMatrix restored(backend);
    ConstSource src2;
    restored.add_source("src", &src2);
    nlohmann::json with_params = json;
    with_params["parameters"] = nlohmann::json::array();  // ignored without State
    restored.from_json(with_params);
    const auto out = restored.to_json(false);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0]["target_id"], "gain");
    EXPECT_FLOAT_EQ(out[0]["depth"].get<float>(), 0.3f);
}

// Hosts may render each block on a different thread. The audio path (process(),
// audio_read_scope()) uses the matrix's own reader slot, so a fresh thread per
// block needs no registration — under RTSan the old per-thread registration
// (allocation + mutex inside the nonblocking process()) would abort here.
TEST(ParameterBackend, ProcessOnChangingThreadsNeedsNoRegistration) {
    FakeBackend backend;
    backend.add("gain", mod_float(thl::Range::linear(0.0f, 1.0f), 0.25f));
    ModulationMatrix matrix(backend);
    ConstSource src;
    src.m_value = 0.5f;
    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<float>("gain");
    ASSERT_NE(matrix.add_routing({"src", "gain", 0.5f}), k_invalid_routing_id);
    matrix.prepare(k_sample_rate, k_block_size);

    for (int block = 0; block < 20; ++block) {
        std::thread render([&]() {
            if (block % 2 == 0) {
                matrix.process(k_block_size);
            } else {
                const auto scope = matrix.audio_read_scope();
                matrix.process_with_scope(scope.data(), k_block_size);
                EXPECT_FLOAT_EQ(handle.load(0), 0.5f);  // 0.25 + 0.5 * 0.5
            }
        });
        render.join();
    }
}
