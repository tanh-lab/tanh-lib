// Scope registry primitives — Suite A of the modulation-scope refactor plan.
//
// Covers the name↔id registry on ModulationMatrix: idempotent re-registration,
// voice-count updates, find_scope lookup, reserved Global slot, and
// scope_name reverse lookup. These are pre-requisites for every other scope
// behavior (allocation, routing validation, source dispatch), so regressions
// here cascade.

#include <gtest/gtest.h>

#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/state/ModulationScope.h>
#include <tanh/state/State.h>

using namespace thl::modulation;

TEST(ModulationScope, GlobalPreRegistered) {
    thl::State state;
    ModulationMatrix matrix(state);

    EXPECT_EQ(matrix.voice_count(thl::modulation::k_global_scope), 1u);
    EXPECT_EQ(matrix.scope_name(thl::modulation::k_global_scope),
              std::string_view{thl::modulation::k_global_scope_name});
}

TEST(ModulationScope, RegisterScopeReturnsDistinctHandle) {
    thl::State state;
    ModulationMatrix matrix(state);

    const auto s = matrix.register_scope("voice", 8);
    EXPECT_NE(s.m_id, thl::modulation::k_global_scope.m_id);
    EXPECT_EQ(matrix.voice_count(s), 8u);
    EXPECT_EQ(matrix.scope_name(s), std::string_view{"voice"});
}

TEST(ModulationScope, RegisterScopeIsIdempotent) {
    thl::State state;
    ModulationMatrix matrix(state);

    const auto a = matrix.register_scope("voice", 4);
    const auto b = matrix.register_scope("voice", 4);
    EXPECT_EQ(a.m_id, b.m_id);
    // Name pointer is stable across idempotent re-registration — both handles
    // reference the same storage node in m_scope_names.
    EXPECT_EQ(a.m_name, b.m_name);
}

TEST(ModulationScope, ReRegisterWithDifferentCountUpdatesCount) {
    thl::State state;
    ModulationMatrix matrix(state);

    const auto a = matrix.register_scope("voice", 4);
    EXPECT_EQ(matrix.voice_count(a), 4u);
    const auto b = matrix.register_scope("voice", 8);
    EXPECT_EQ(a.m_id, b.m_id);
    EXPECT_EQ(matrix.voice_count(b), 8u);
    EXPECT_EQ(matrix.voice_count(a), 8u);  // same id → sees updated count
}

TEST(ModulationScope, FindScopeReturnsHandleForKnownName) {
    thl::State state;
    ModulationMatrix matrix(state);

    const auto registered = matrix.register_scope("voice", 2);
    const auto found = matrix.find_scope("voice");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->m_id, registered.m_id);
    EXPECT_EQ(std::string_view{found->m_name}, std::string_view{"voice"});
}

TEST(ModulationScope, FindScopeReturnsNulloptForUnknownName) {
    thl::State state;
    ModulationMatrix matrix(state);
    matrix.register_scope("voice", 2);

    EXPECT_FALSE(matrix.find_scope("nonexistent").has_value());
}

TEST(ModulationScope, MultipleScopesCoexistWithDistinctIds) {
    thl::State state;
    ModulationMatrix matrix(state);

    const auto a = matrix.register_scope("voice", 2);
    const auto b = matrix.register_scope("lane", 4);
    const auto c = matrix.register_scope("rail", 8);

    EXPECT_NE(a.m_id, b.m_id);
    EXPECT_NE(b.m_id, c.m_id);
    EXPECT_NE(a.m_id, c.m_id);

    EXPECT_EQ(matrix.voice_count(a), 2u);
    EXPECT_EQ(matrix.voice_count(b), 4u);
    EXPECT_EQ(matrix.voice_count(c), 8u);
}

TEST(ModulationScope, SetVoiceCountUpdatesRegistry) {
    thl::State state;
    ModulationMatrix matrix(state);

    const auto s = matrix.register_scope("voice", 2);
    matrix.set_voice_count(s, 6);
    EXPECT_EQ(matrix.voice_count(s), 6u);
}
