#include <gtest/gtest.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/SmartHandle.h>
#include <tanh/modulation/StateParameterBackend.h>
#include <tanh/state/Exceptions.h>
#include <tanh/state/State.h>

#include <stdexcept>

#include "TestHelpers.h"

using namespace thl::modulation;

namespace {

/// Global source with a constant, always active value.
class ConstSource : public ModulationSource {
public:
    explicit ConstSource(float value) : ModulationSource(k_global_scope, true), m_value(value) {}
    void prepare(double /*sr*/, size_t spb, uint32_t voice_count) override {
        resize_buffers(spb, voice_count);
    }
    void process(size_t num_samples, size_t offset = 0) override {
        for (size_t i = offset; i < offset + num_samples; ++i) { m_output_buffer[i] = m_value; }
    }

private:
    float m_value;
};

}  // namespace

TEST(StateParameterBackend, LookupErrorsMatchState) {
    thl::State state;
    state.create("freq", modulatable_float(440.0f));
    state.create("osc.level", modulatable_float(0.5f));
    state.create("fixed",
                 thl::ParameterDefinition::make_float("Fixed", thl::Range::linear(0.0f, 1.0f), 0.0f)
                     .modulatable(false));
    ModulationMatrix matrix(state);

    // Each case throws exactly what State::get_handle<T>() throws.
    const auto expect_same = [&](auto requested, std::string_view key) {
        using T = decltype(requested);
        std::string state_what;
        try {
            (void)state.get_handle<T>(key);
        } catch (const std::exception& e) { state_what = e.what(); }
        ASSERT_FALSE(state_what.empty());
        std::string matrix_what;
        try {
            (void)matrix.get_smart_handle<T>(key);
        } catch (const std::exception& e) { matrix_what = e.what(); }
        EXPECT_EQ(matrix_what, state_what) << key;
    };
    expect_same(0.0f, "missing");
    expect_same(0.0f, "osc.missing");
    expect_same(0.0f, "nogroup.missing");
    expect_same(0, "freq");

    EXPECT_THROW(matrix.get_smart_handle<float>("missing"), thl::StateKeyNotFoundException);
    EXPECT_THROW(matrix.get_smart_handle<float>("osc.missing"), thl::StateKeyNotFoundException);
    EXPECT_THROW(matrix.get_smart_handle<float>("nogroup.missing"),
                 thl::StateGroupNotFoundException);
    EXPECT_THROW(matrix.get_smart_handle<int>("freq"), thl::ParameterTypeMismatchException);
    EXPECT_THROW(matrix.get_smart_handle<int>("fixed"), thl::ParameterTypeMismatchException);
    EXPECT_THROW(matrix.get_smart_handle<float>("fixed"), std::invalid_argument);
    EXPECT_NO_THROW((void)matrix.get_smart_handle<float>("osc.level"));
}

TEST(StateParameterBackend, HandleReadsStateValue) {
    thl::State state;
    state.create("freq", modulatable_float(440.0f));
    ModulationMatrix matrix(state);

    auto handle = matrix.get_smart_handle<float>("freq");
    EXPECT_FLOAT_EQ(handle.load(), 440.0f);
    state.set("freq", 220.0f);
    EXPECT_FLOAT_EQ(handle.load(), 220.0f);
    EXPECT_EQ(handle.key(), "freq");
    EXPECT_EQ(&matrix.state(), &state);
}

// Every write through State is host activity: the binding carries the record's
// change stamp, so automation takes a shared target back from the modulation.
TEST(StateParameterBackend, StateWritesAreHostChanges) {
    thl::State state;
    state.create("freq", modulatable_float(0.2f));
    StateParameterBackend backend(state);
    const auto binding = backend.find("freq");
    ASSERT_TRUE(binding.has_value());
    ASSERT_NE(binding->m_host_change_stamp, nullptr);
    const uint64_t before = binding->m_host_change_stamp->load();
    state.set("freq", 0.4f);
    EXPECT_NE(binding->m_host_change_stamp->load(), before);

    ModulationMatrix matrix(state);
    ConstSource src(0.8f);
    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<float>("freq");
    ModulationRouting routing("src", "freq", 1.0f);
    routing.m_combine_mode = CombineMode::ReplaceHold;
    routing.m_share_with_host = true;
    ASSERT_NE(matrix.add_routing(routing), k_invalid_routing_id);
    matrix.set_host_share_times({.m_take_ms = 0.0f, .m_return_ms = 0.0f, .m_hold_ms = 0.0f});
    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);
    EXPECT_NEAR(handle.load(k_block_size - 1), 0.8f, 1e-5f);

    state.set("freq", 0.3f);  // automation
    matrix.process(k_block_size);
    EXPECT_NEAR(handle.load(k_block_size - 1), 0.3f, 1e-5f);
    matrix.remove_source("src");
}
