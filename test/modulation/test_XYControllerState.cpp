#include <gtest/gtest.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/SmartHandle.h>
#include <tanh/modulation/XYController.h>
#include <tanh/state/State.h>

#include <nlohmann/json.hpp>

#include "XYControllerTestHelpers.h"

using namespace thl::modulation;

namespace {

constexpr uint32_t k_block_size = 256;

thl::ParameterDefinition modulatable_float(float default_value) {
    return thl::ParameterDefinition::make_float("", thl::Range::linear(0.0f, 1.0f), default_value)
        .modulatable(true);
}

}  // namespace

TEST(XYControllerState, TouchDrivesStateParameters) {
    thl::State state;
    state.create("slot1.a", modulatable_float(0.1f));
    state.create("slot1.b", modulatable_float(0.1f));
    state.create("slot1.wet", modulatable_float(0.25f));
    ModulationMatrix matrix(state);
    XYControllerConfig cfg;
    cfg.m_id = "pad1";
    XYController c(matrix, cfg);
    EXPECT_NE(c.route(XYPadAxis::X, "slot1.a"), k_invalid_routing_id);
    EXPECT_NE(c.route(XYPadAxis::Y, "slot1.b"), k_invalid_routing_id);
    EXPECT_NE(c.route(XYPadAxis::Active, "slot1.wet"), k_invalid_routing_id);
    auto a = matrix.get_smart_handle<float>("slot1.a");
    auto b = matrix.get_smart_handle<float>("slot1.b");
    auto wet = matrix.get_smart_handle<float>("slot1.wet");
    matrix.prepare(xy_test::k_sr, k_block_size);
    xy_test::SimTransport t;
    auto block = [&] {
        c.set_transport(t.next(k_block_size));
        matrix.process(k_block_size);
    };

    block();
    EXPECT_FLOAT_EQ(a.load(0), 0.1f);
    c.touch(1, 0.8f, 0.3f);
    block();
    EXPECT_FLOAT_EQ(a.load(k_block_size - 1), 0.8f);
    EXPECT_FLOAT_EQ(b.load(k_block_size - 1), 0.3f);
    EXPECT_FLOAT_EQ(wet.load(k_block_size - 1), 1.0f);
    c.release(1);
    block();
    EXPECT_FLOAT_EQ(a.load(k_block_size - 1), 0.8f);
    EXPECT_FLOAT_EQ(wet.load(k_block_size - 1), 0.25f);

    // Routings serialise with the State's parameters as usual.
    const auto json = matrix.to_json();
    EXPECT_EQ(json["modulation_routings"].size(), 3u);
}
