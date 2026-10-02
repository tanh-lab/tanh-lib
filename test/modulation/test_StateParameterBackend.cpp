// ModulationMatrix(thl::State&): the v0.4.0 error contract, store_base() and
// the deprecated raw_handle() on State-backed SmartHandles.

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

TEST(StateParameterBackend, StoreBaseWritesStateValue) {
    thl::State state;
    state.create("freq", modulatable_float(440.0f));
    ModulationMatrix matrix(state);

    auto handle = matrix.get_smart_handle<float>("freq");
    handle.store_base(220.0f);
    EXPECT_FLOAT_EQ(handle.load_base(), 220.0f);
    EXPECT_FLOAT_EQ(handle.load(), 220.0f);
    EXPECT_FLOAT_EQ(state.get_handle<float>("freq").load(), 220.0f);
    EXPECT_FLOAT_EQ(state.get<float>("freq"), 220.0f);
}

TEST(StateParameterBackend, RawHandleSeesSameValue) {
    thl::State state;
    state.create("freq", modulatable_float(440.0f));
    ModulationMatrix matrix(state);
    auto handle = matrix.get_smart_handle<float>("freq");

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    auto raw = handle.raw_handle();
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    ASSERT_TRUE(raw.has_value());
    ASSERT_TRUE(raw->is_valid());
    EXPECT_EQ(raw->key(), "freq");
    EXPECT_FLOAT_EQ(raw->load(), 440.0f);

    raw->store(330.0f);
    EXPECT_FLOAT_EQ(handle.load_base(), 330.0f);
    handle.store_base(110.0f);
    EXPECT_FLOAT_EQ(raw->load(), 110.0f);
}
