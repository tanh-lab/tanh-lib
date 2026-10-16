#include <gtest/gtest.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/SmartHandle.h>
#include <tanh/state/Exceptions.h>
#include <tanh/state/State.h>

#include <stdexcept>

#include "TestHelpers.h"

using namespace thl::modulation;

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
