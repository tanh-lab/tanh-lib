#include <gtest/gtest.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/ModulationRouting.h>
#include <tanh/modulation/ModulationSource.h>
#include <tanh/modulation/ParameterBackend.h>
#include <tanh/modulation/SmartHandle.h>
#include <tanh/parameters/ParameterDefinitions.h>

#include <atomic>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

// ModulationRouting::m_offset and the chain (CombineMode::Multiply,
// m_chain_position): a target's routings folded in order.

using namespace thl::modulation;

namespace {

constexpr double k_sr = 48000.0;
constexpr size_t k_bs = 64;

class FakeBackend : public ParameterBackend {
public:
    // A parameter over -10 ... 10: a normalised contribution c moves it by 20 c.
    void add(const std::string& key, float default_value) {
        auto e = std::make_unique<Entry>();
        e->m_def = thl::ParameterDefinition::make_float(key,
                                                        thl::Range::linear(-10.0f, 10.0f),
                                                        default_value)
                       .modulatable(true);
        e->m_value.store(default_value);
        m_entries[key] = std::move(e);
    }
    std::optional<ParameterBinding> find(std::string_view key) const override {
        auto it = m_entries.find(key);
        if (it == m_entries.end()) { return std::nullopt; }
        return ParameterBinding{.m_def = it->second->m_def, .m_base = &it->second->m_value};
    }

private:
    struct Entry {
        thl::ParameterDefinition m_def;
        std::atomic<float> m_value{0.0f};
    };
    std::map<std::string, std::unique_ptr<Entry>, std::less<>> m_entries;
};

/// A global source with a constant value.
class ConstSource : public ModulationSource {
public:
    explicit ConstSource(float value) : ModulationSource(k_global_scope), m_value(value) {}
    void prepare(double, size_t samples_per_block, uint32_t voice_count) override {
        resize_buffers(samples_per_block, voice_count);
    }
    void process(size_t num_samples, size_t offset) override {
        for (size_t i = offset; i < offset + num_samples; ++i) { m_output_buffer[i] = m_value; }
        if (offset == 0) { record_change_point(0); }
    }
    float m_value;
};

ModulationRouting chained(std::string_view src,
                          float depth,
                          CombineMode mode,
                          int32_t position,
                          float offset = 0.0f) {
    ModulationRouting r(src, "p", depth);
    r.m_combine_mode = mode;
    r.m_chain_position = position;
    r.m_offset = offset;
    return r;
}

struct Rig {
    Rig() : matrix(backend_with_p()) {}
    static FakeBackend& backend_with_p() {
        static FakeBackend b;
        static bool once = [] {
            b.add("p", 0.0f);
            return true;
        }();
        (void)once;
        return b;
    }
    ModulationMatrix matrix;
    ConstSource a{0.5f}, b{0.5f}, c{0.25f};
};

}  // namespace

TEST(RoutingChain, AnOffsetAddsToTheContribution) {
    Rig rig;
    rig.matrix.add_source("a", &rig.a);
    auto p = rig.matrix.get_smart_handle<float>("p");
    // (0.5 * 0.1 + 0.1) of the range: 3.
    rig.matrix.add_routing(chained("a", 0.1f, CombineMode::Additive, 0, 0.1f));
    rig.matrix.prepare(k_sr, k_bs);
    rig.matrix.process(k_bs);
    EXPECT_NEAR(p.load(0), 3.0f, 1.0e-5f);

    // Without a rebuild: (0.5 * 0.1 - 0.05) of the range: 0.
    EXPECT_TRUE(rig.matrix.update_routing_offset("a", "p", -0.05f));
    rig.matrix.process(k_bs);
    EXPECT_NEAR(p.load(k_bs - 1), 0.0f, 1.0e-5f);
    EXPECT_FALSE(rig.matrix.update_routing_offset("a", "nope", 0.0f));
}

TEST(RoutingChain, MultiplyFoldsTheChainInOrder) {
    Rig rig;
    for (auto [id, source] :
         {std::pair{"a", &rig.a}, std::pair{"b", &rig.b}, std::pair{"c", &rig.c}}) {
        rig.matrix.add_source(id, source);
    }
    auto p = rig.matrix.get_smart_handle<float>("p");
    // a: 0.2, then times b: 0.5, then plus c: 0.1 -> 0.2 of the range: 4.
    rig.matrix.add_routing(chained("a", 0.4f, CombineMode::Additive, 0));
    rig.matrix.add_routing(chained("b", 1.0f, CombineMode::Multiply, 1));
    rig.matrix.add_routing(chained("c", 0.4f, CombineMode::Additive, 2));
    rig.matrix.prepare(k_sr, k_bs);
    rig.matrix.process(k_bs);
    EXPECT_NEAR(p.load(0), 4.0f, 1.0e-5f);
    EXPECT_NEAR(p.load(k_bs - 1), 4.0f, 1.0e-5f);
}

TEST(RoutingChain, TheOrderMatters) {
    Rig rig;
    for (auto [id, source] :
         {std::pair{"a", &rig.a}, std::pair{"b", &rig.b}, std::pair{"c", &rig.c}}) {
        rig.matrix.add_source(id, source);
    }
    auto p = rig.matrix.get_smart_handle<float>("p");
    // c first: (0.1 + 0.2) * 0.5 = 0.15 of the range: 3.
    rig.matrix.add_routing(chained("a", 0.4f, CombineMode::Additive, 1));
    rig.matrix.add_routing(chained("b", 1.0f, CombineMode::Multiply, 2));
    rig.matrix.add_routing(chained("c", 0.4f, CombineMode::Additive, 0));
    rig.matrix.prepare(k_sr, k_bs);
    rig.matrix.process(k_bs);
    EXPECT_NEAR(p.load(0), 3.0f, 1.0e-5f);
}

TEST(RoutingChain, ADisabledRoutingLeavesTheChain) {
    Rig rig;
    rig.matrix.add_source("a", &rig.a);
    rig.matrix.add_source("b", &rig.b);
    auto p = rig.matrix.get_smart_handle<float>("p");
    rig.matrix.add_routing(chained("a", 0.4f, CombineMode::Additive, 0));
    rig.matrix.add_routing(chained("b", 1.0f, CombineMode::Multiply, 1));
    rig.matrix.prepare(k_sr, k_bs);
    rig.matrix.process(k_bs);
    EXPECT_NEAR(p.load(0), 2.0f, 1.0e-5f);  // 0.2 * 0.5

    ASSERT_TRUE(rig.matrix.set_routing_enabled("b", "p", false));
    rig.matrix.process(k_bs);
    EXPECT_NEAR(p.load(0), 4.0f, 1.0e-5f);  // 0.2 alone

    ASSERT_TRUE(rig.matrix.set_routing_enabled("a", "p", false));
    ASSERT_TRUE(rig.matrix.set_routing_enabled("b", "p", true));
    rig.matrix.process(k_bs);
    EXPECT_NEAR(p.load(0), 10.0f, 1.0e-5f);  // 0.5 starts the chain: 0.5 of the range
}

TEST(RoutingChain, MultiplyAndOffsetMakeOneFactor) {
    Rig rig;
    rig.matrix.add_source("a", &rig.a);
    rig.matrix.add_source("b", &rig.b);
    auto p = rig.matrix.get_smart_handle<float>("p");
    // 0.2, times (0.5 * 0.5 + 0.25) = 0.5: 0.1 of the range.
    rig.matrix.add_routing(chained("a", 0.4f, CombineMode::Additive, 0));
    rig.matrix.add_routing(chained("b", 0.5f, CombineMode::Multiply, 1, 0.25f));
    rig.matrix.prepare(k_sr, k_bs);
    rig.matrix.process(k_bs);
    EXPECT_NEAR(p.load(0), 2.0f, 1.0e-5f);
}

TEST(RoutingChain, WithoutMultiplyTheTargetJustAdds) {
    Rig rig;
    rig.matrix.add_source("a", &rig.a);
    rig.matrix.add_source("c", &rig.c);
    auto p = rig.matrix.get_smart_handle<float>("p");
    rig.matrix.add_routing(chained("a", 0.4f, CombineMode::Additive, 5));
    rig.matrix.add_routing(chained("c", 0.4f, CombineMode::Additive, 0));
    rig.matrix.prepare(k_sr, k_bs);
    rig.matrix.process(k_bs);
    EXPECT_NEAR(p.load(0), 6.0f, 1.0e-5f);  // (0.2 + 0.1) of the range
}

TEST(RoutingChain, OffsetPositionAndMultiplyAreSaved) {
    Rig rig;
    rig.matrix.add_source("a", &rig.a);
    rig.matrix.add_source("b", &rig.b);
    rig.matrix.add_routing(chained("a", 0.4f, CombineMode::Additive, 3, 0.1f));
    rig.matrix.add_routing(chained("b", 1.0f, CombineMode::Multiply, 7));
    const auto json = rig.matrix.to_json();

    Rig restored;
    restored.matrix.add_source("a", &restored.a);
    restored.matrix.add_source("b", &restored.b);
    restored.matrix.from_json(json);
    EXPECT_EQ(restored.matrix.to_json(), json);
    const auto routings = json.at("modulation_routings");
    ASSERT_EQ(routings.size(), 2u);
    EXPECT_FLOAT_EQ(routings[0].value("offset", 0.0f), 0.1f);
    EXPECT_EQ(routings[0].value("chain_position", 0), 3);
    EXPECT_EQ(routings[1].value("combine_mode", std::string()), "multiply");
}
