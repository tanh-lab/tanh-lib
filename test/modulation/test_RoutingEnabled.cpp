#include <gtest/gtest.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/ModulationRouting.h>
#include <tanh/modulation/ModulationSource.h>
#include <tanh/modulation/ParameterBackend.h>
#include <tanh/modulation/SmartHandle.h>
#include <tanh/state/ParameterDefinitions.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace thl::modulation;

namespace {

constexpr double k_sr = 48000.0;
constexpr size_t k_bs = 64;

class FakeBackend : public ParameterBackend {
public:
    void add(const std::string& key, float default_value) {
        auto e = std::make_unique<Entry>();
        e->m_def =
            thl::ParameterDefinition::make_float(key, thl::Range::linear(0.0f, 1.0f), default_value)
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

/// Global source with a settable constant value and gate.
class ConstSource : public ModulationSource {
public:
    explicit ConstSource(float value, bool active = true, std::vector<std::string> keys = {})
        : ModulationSource(k_global_scope, /*fully_active=*/false)
        , m_value(value)
        , m_active(active)
        , m_keys(std::move(keys)) {}

    void prepare(double, size_t samples_per_block, uint32_t voice_count) override {
        resize_buffers(samples_per_block, voice_count);
    }
    void process(size_t num_samples, size_t offset) override {
        const float v = m_value.load(std::memory_order_relaxed);
        const bool a = m_active.load(std::memory_order_relaxed);
        for (size_t i = offset; i < offset + num_samples; ++i) {
            m_output_buffer[i] = v;
            get_output_active()[i] = a ? 1 : 0;
        }
        if (offset == 0 && m_record_change_points) { record_change_point(0); }
    }
    std::vector<std::string> parameter_keys() const override { return m_keys; }

    std::atomic<float> m_value;
    std::atomic<bool> m_active;
    std::vector<std::string> m_keys;
    bool m_record_change_points = true;
};

ModulationRouting routing(std::string_view src,
                          std::string_view dst,
                          CombineMode mode,
                          uint32_t priority = 0,
                          float depth = 1.0f) {
    ModulationRouting r(src, dst, depth);
    r.m_combine_mode = mode;
    r.m_replace_priority = priority;
    return r;
}

const ProcessingConfig* live_config(const ModulationMatrix& m) {
    const auto scope = m.read_scope();
    return &scope.data();
}

}  // namespace

// The finding behind the API: a Replace routing at depth 0 still wins the
// target (it writes src * 0); only disabling it gives the base value back.
TEST(RoutingEnabled, DepthZeroDoesNotSilenceReplaceButDisableDoes) {
    FakeBackend backend;
    backend.add("p", 0.4f);
    ModulationMatrix matrix(backend);
    ConstSource src(0.9f);
    matrix.add_source("src", &src);
    auto p = matrix.get_smart_handle<float>("p");
    const uint32_t id = matrix.add_routing(routing("src", "p", CombineMode::Replace));
    ASSERT_NE(id, k_invalid_routing_id);
    matrix.prepare(k_sr, k_bs);

    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(p.load(0), 0.9f);

    ASSERT_TRUE(matrix.update_routing_depth(id, 0.0f));
    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(p.load(0), 0.0f);  // depth 0 still replaces

    ASSERT_TRUE(matrix.update_routing_depth(id, 1.0f));
    ASSERT_TRUE(matrix.set_routing_enabled(id, false));
    matrix.process(k_bs);
    for (uint32_t i = 0; i < k_bs; ++i) { EXPECT_FLOAT_EQ(p.load(i), 0.4f); }

    ASSERT_TRUE(matrix.set_routing_enabled(id, true));
    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(p.load(0), 0.9f);
    matrix.remove_source("src");
}

TEST(RoutingEnabled, DisabledAdditiveContributesNothing) {
    FakeBackend backend;
    backend.add("p", 0.25f);
    ModulationMatrix matrix(backend);
    ConstSource src(0.5f);
    matrix.add_source("src", &src);
    auto p = matrix.get_smart_handle<float>("p");
    const uint32_t id = matrix.add_routing(routing("src", "p", CombineMode::Additive));
    matrix.prepare(k_sr, k_bs);

    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(p.load(3), 0.75f);
    EXPECT_TRUE(matrix.set_routing_enabled("src", "p", false));
    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(p.load(3), 0.25f);
    EXPECT_TRUE(matrix.set_routing_enabled("src", "p", true));
    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(p.load(3), 0.75f);
    EXPECT_FALSE(matrix.set_routing_enabled(id + 100, false));
    EXPECT_FALSE(matrix.set_routing_enabled("src", "nope", false));
    matrix.remove_source("src");
}

TEST(RoutingEnabled, DisabledReplaceHoldWritesNothingAndReenableDoesNotReviveStaleHold) {
    FakeBackend backend;
    backend.add("p", 0.1f);
    ModulationMatrix matrix(backend);
    ConstSource src(0.8f);
    matrix.add_source("src", &src);
    auto p = matrix.get_smart_handle<float>("p");
    const uint32_t id = matrix.add_routing(routing("src", "p", CombineMode::ReplaceHold));
    matrix.prepare(k_sr, k_bs);

    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(p.load(0), 0.8f);
    src.m_active = false;
    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(p.load(k_bs - 1), 0.8f);  // held

    matrix.set_routing_enabled(id, false);
    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(p.load(k_bs - 1), 0.1f);  // the hold is not written

    matrix.set_routing_enabled(id, true);
    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(p.load(k_bs - 1), 0.1f);  // and not revived

    src.m_value = 0.6f;
    src.m_active = true;
    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(p.load(0), 0.6f);
    src.m_active = false;
    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(p.load(0), 0.6f);  // a fresh hold works again
    matrix.remove_source("src");
}

// Multi-Replace: a disabled high-priority routing must not block a lower one.
TEST(RoutingEnabled, DisabledHighPriorityReplaceYieldsToLowerPriority) {
    FakeBackend backend;
    backend.add("p", 0.0f);
    ModulationMatrix matrix(backend);
    ConstSource lo(0.3f);
    ConstSource hi(0.7f);
    matrix.add_source("lo", &lo);
    matrix.add_source("hi", &hi);
    auto p = matrix.get_smart_handle<float>("p");
    matrix.add_routing(routing("lo", "p", CombineMode::ReplaceHold, 10));
    const uint32_t hi_id = matrix.add_routing(routing("hi", "p", CombineMode::ReplaceHold, 20));
    matrix.prepare(k_sr, k_bs);

    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(p.load(5), 0.7f);
    matrix.set_routing_enabled(hi_id, false);
    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(p.load(5), 0.3f);
    hi.m_active = false;  // hi would hold 0.7 at priority 20 if it were enabled
    matrix.set_routing_enabled(hi_id, true);
    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(p.load(5), 0.3f);  // its hold was cleared on disable
    matrix.remove_source("lo");
    matrix.remove_source("hi");
}

// A routing inside a cyclic step (self-edge) is inert as well.
TEST(RoutingEnabled, DisabledRoutingInCyclicStepIsInert) {
    FakeBackend backend;
    backend.add("p", 0.2f);
    ModulationMatrix matrix(backend);
    ConstSource src(0.9f, true, {"p"});
    matrix.add_source("src", &src);
    auto p = matrix.get_smart_handle<float>("p");
    const uint32_t id = matrix.add_routing(routing("src", "p", CombineMode::Replace));
    matrix.prepare(k_sr, k_bs);
    bool cyclic = false;
    for (const auto& step : matrix.get_schedule()) {
        cyclic = cyclic || std::holds_alternative<CyclicStep>(step);
    }
    ASSERT_TRUE(cyclic);

    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(p.load(7), 0.9f);
    matrix.set_routing_enabled(id, false);
    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(p.load(7), 0.2f);
    matrix.set_routing_enabled(id, true);
    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(p.load(7), 0.9f);
    matrix.remove_source("src");
}

TEST(RoutingEnabled, EdgesFlagAChangePointAtOffsetZero) {
    FakeBackend backend;
    backend.add("p", 0.2f);
    ModulationMatrix matrix(backend);
    ConstSource src(0.9f);
    matrix.add_source("src", &src);
    auto p = matrix.get_smart_handle<float>("p");
    const uint32_t id = matrix.add_routing(routing("src", "p", CombineMode::Replace));
    matrix.prepare(k_sr, k_bs);
    matrix.process(k_bs);
    const auto* mb = matrix.get_target("p")->m_mono.load(std::memory_order_acquire);
    ASSERT_NE(mb, nullptr);

    // A disabled routing propagates none of its source's change points: the
    // only one in the disable block is the edge itself.
    matrix.set_routing_enabled(id, false);
    matrix.process(k_bs);
    ASSERT_EQ(mb->m_change_points.size(), 1u);
    EXPECT_EQ(mb->m_change_points.front(), 0u);
    EXPECT_FLOAT_EQ(p.load(0), 0.2f);

    matrix.process(k_bs);
    EXPECT_TRUE(mb->m_change_points.empty());

    matrix.set_routing_enabled(id, true);
    matrix.process(k_bs);
    ASSERT_FALSE(mb->m_change_points.empty());
    EXPECT_EQ(mb->m_change_points.front(), 0u);
    matrix.remove_source("src");
}

// A flag change that lands together with a schedule rebuild has no previous
// block in the new config to compare against; it must still flag offset 0.
TEST(RoutingEnabled, EdgeLandingWithRebuildFlagsOffsetZero) {
    FakeBackend backend;
    backend.add("p", 0.2f);
    backend.add("q", 0.2f);
    ModulationMatrix matrix(backend);
    ConstSource src(0.9f);
    src.m_record_change_points = false;
    matrix.add_source("src", &src);
    auto p = matrix.get_smart_handle<float>("p");
    const uint32_t id = matrix.add_routing(routing("src", "p", CombineMode::Additive));
    matrix.prepare(k_sr, k_bs);
    matrix.process(k_bs);
    matrix.process(k_bs);
    ASSERT_NE(p.change_points(), nullptr);
    EXPECT_TRUE(p.change_points()->empty());

    matrix.set_routing_enabled(id, false);
    ASSERT_NE(matrix.add_routing(routing("src", "q", CombineMode::Additive)), k_invalid_routing_id);
    matrix.process(k_bs);
    ASSERT_NE(p.change_points(), nullptr);
    EXPECT_EQ(*p.change_points(), std::vector<uint32_t>{0});
    EXPECT_FLOAT_EQ(p.load(0), 0.2f);
    matrix.remove_source("src");
}

// A zero-sample block must not consume an edge without flagging it.
TEST(RoutingEnabled, ZeroSampleBlockKeepsTheEdge) {
    FakeBackend backend;
    backend.add("p", 0.2f);
    ModulationMatrix matrix(backend);
    ConstSource src(0.9f);
    src.m_record_change_points = false;
    matrix.add_source("src", &src);
    auto p = matrix.get_smart_handle<float>("p");
    const uint32_t id = matrix.add_routing(routing("src", "p", CombineMode::Additive));
    matrix.prepare(k_sr, k_bs);
    matrix.process(k_bs);
    matrix.process(k_bs);

    matrix.set_routing_enabled(id, false);
    matrix.process(0);
    matrix.process(k_bs);
    ASSERT_NE(p.change_points(), nullptr);
    EXPECT_EQ(*p.change_points(), std::vector<uint32_t>{0});
    EXPECT_FLOAT_EQ(p.load(0), 0.2f);
    matrix.remove_source("src");
}

TEST(RoutingEnabled, SerialisedOnlyWhenDisabledAndRestored) {
    FakeBackend backend;
    backend.add("p", 0.2f);
    ModulationMatrix matrix(backend);
    ConstSource src(0.9f);
    matrix.add_source("src", &src);
    const uint32_t id = matrix.add_routing(routing("src", "p", CombineMode::Replace));
    auto json = matrix.to_json(false);
    ASSERT_EQ(json.size(), 1u);
    EXPECT_FALSE(json[0].contains("enabled"));

    matrix.set_routing_enabled(id, false);
    json = matrix.to_json(false);
    EXPECT_EQ(json[0]["enabled"], false);

    matrix.set_routing_enabled(id, true);
    matrix.from_json(json);  // restores the disabled flag (and rebuilds)
    auto p = matrix.get_smart_handle<float>("p");
    matrix.prepare(k_sr, k_bs);
    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(p.load(0), 0.2f);
    EXPECT_EQ(matrix.to_json(false)[0]["enabled"], false);
    matrix.remove_source("src");
}

// Toggling touches only the live config's atomics: no rebuild (the published
// ProcessingConfig stays the same object), also while the audio thread runs.
TEST(RoutingEnabled, ToggleNeedsNoRebuildConcurrentWithAudio) {
    FakeBackend backend;
    backend.add("p", 0.2f);
    ModulationMatrix matrix(backend);
    ConstSource a(0.9f);
    ConstSource b(0.6f);
    matrix.add_source("a", &a);
    matrix.add_source("b", &b);
    auto p = matrix.get_smart_handle<float>("p");
    const uint32_t ida = matrix.add_routing(routing("a", "p", CombineMode::ReplaceHold, 20));
    const uint32_t idb = matrix.add_routing(routing("b", "p", CombineMode::ReplaceHold, 10));
    matrix.prepare(k_sr, k_bs);
    const ProcessingConfig* before = live_config(matrix);

    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::atomic<int> blocks{0};
    std::atomic<int> saw_a{0};
    std::atomic<int> saw_b{0};
    std::thread audio([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const auto scope = matrix.audio_read_scope();
            matrix.process_with_scope(scope.data(), k_bs);
            for (uint32_t i = 0; i < k_bs; ++i) {
                const float v = p.load(i);
                if (v == 0.9f) {
                    saw_a.fetch_add(1, std::memory_order_relaxed);
                } else if (v == 0.6f) {
                    saw_b.fetch_add(1, std::memory_order_relaxed);
                } else {
                    bad.fetch_add(1);
                }
            }
            blocks.fetch_add(1, std::memory_order_release);
        }
    });
    // Start with only a enabled, then switch between "only a" and "only b" as
    // one batch: no block may see neither (base value) — and none sees both
    // (that would be harmless here thanks to the priorities, but not in general).
    matrix.set_routing_enabled(idb, false);
    for (int k = 0; k < 1000; ++k) {
        const bool a_on = (k % 2) != 0;
        const std::array<RoutingEnabled, 2> batch{RoutingEnabled{ida, a_on},
                                                  RoutingEnabled{idb, !a_on}};
        EXPECT_EQ(matrix.set_routings_enabled(batch), 2u);
        // Let the audio thread run at least one block per state (interleave).
        const int target = blocks.load(std::memory_order_acquire) + 1;
        while (blocks.load(std::memory_order_acquire) < target) { std::this_thread::yield(); }
    }
    stop.store(true, std::memory_order_release);
    audio.join();
    EXPECT_EQ(bad.load(), 0);
    EXPECT_GT(saw_a.load(), 0);
    EXPECT_GT(saw_b.load(), 0);
    EXPECT_EQ(live_config(matrix), before);
    matrix.remove_source("a");
    matrix.remove_source("b");
}

namespace {

float rt_blocks(ModulationMatrix& matrix,
                SmartHandle<float>& p,
                int blocks) TANH_NONBLOCKING_FUNCTION {
    float acc = 0.0f;
    for (int b = 0; b < blocks; ++b) {
        matrix.process(k_bs);
        acc += p.load(0);
    }
    return acc;
}

}  // namespace

// Under -DTANH_WITH_RTSAN=ON the edge handling (hold clear, change point) must
// not allocate or lock on the audio thread.
TEST(RoutingEnabled, AudioPathIsRealtimeSafe) {
    FakeBackend backend;
    backend.add("p", 0.2f);
    ModulationMatrix matrix(backend);
    ConstSource a(0.9f);
    matrix.add_source("a", &a);
    auto p = matrix.get_smart_handle<float>("p");
    const uint32_t id = matrix.add_routing(routing("a", "p", CombineMode::ReplaceHold, 20));
    matrix.prepare(k_sr, k_bs);
    float acc = 0.0f;
    for (int k = 0; k < 50; ++k) {
        matrix.set_routing_enabled(id, (k % 2) == 0);
        a.m_active = (k % 3) != 0;
        acc += rt_blocks(matrix, p, 2);
    }
    EXPECT_GT(acc, 0.0f);
    matrix.remove_source("a");
}
