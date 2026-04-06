#include "TestHelpers.h"
#include "tanh/state/State.h"
#include "tanh/state/Exceptions.h"
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace thl;

// =============================================================================
// ParameterHandle tests
// =============================================================================

TEST(StateTests, HandleDefaultConstructor) {
    ParameterHandle<double> handle;
    EXPECT_FALSE(handle.is_valid());
}

TEST(StateTests, HandleBasicLoadStore) {
    State state;
    state.create("volume", 0.75);

    auto handle = state.get_handle<double>("volume");
    EXPECT_TRUE(handle.is_valid());
    EXPECT_DOUBLE_EQ(0.75, handle.load());

    handle.store(0.5);
    EXPECT_DOUBLE_EQ(0.5, handle.load());
}

TEST(StateTests, HandleAllNumericTypes) {
    State state;
    state.create("d_param", 3.14);
    state.create("f_param", 2.71f);
    state.create("i_param", 42);
    state.create("b_param", true);

    auto dh = state.get_handle<double>("d_param");
    auto fh = state.get_handle<float>("f_param");
    auto ih = state.get_handle<int>("i_param");
    auto bh = state.get_handle<bool>("b_param");

    EXPECT_DOUBLE_EQ(3.14, dh.load());
    EXPECT_FLOAT_EQ(2.71f, fh.load());
    EXPECT_EQ(42, ih.load());
    EXPECT_TRUE(bh.load());
}

TEST(StateTests, HandleReflectsSetInRoot) {
    State state;
    state.create("gain", 1.0);

    auto handle = state.get_handle<double>("gain");
    EXPECT_DOUBLE_EQ(1.0, handle.load());

    // set_in_root updates both RCU and atomic cache
    state.set("gain", 2.0);
    EXPECT_DOUBLE_EQ(2.0, handle.load());
}

TEST(StateTests, HandleStoreKeepsStateInSync) {
    State state;
    state.create("gain", 1.0);

    auto handle = state.get_handle<double>("gain");
    handle.store(3.0);

    // Handle sees the new value
    EXPECT_DOUBLE_EQ(3.0, handle.load());

    // get_from_root also reads from the same atomic -- state stays in sync
    EXPECT_DOUBLE_EQ(3.0, state.get_from_root<double>("gain"));
}

TEST(StateTests, HandleSetUpdatesNativeAtomicOnly) {
    State state;
    state.create("param", 3.14);

    // Only native-type handle is allowed
    auto dh = state.get_handle<double>("param");
    EXPECT_DOUBLE_EQ(3.14, dh.load());

    // Cross-type handles should throw
    EXPECT_THROW(state.get_handle<float>("param"), ParameterTypeMismatchException);
    EXPECT_THROW(state.get_handle<int>("param"), ParameterTypeMismatchException);
    EXPECT_THROW(state.get_handle<bool>("param"), ParameterTypeMismatchException);

    // set_in_root updates only the native atomic (double)
    state.set("param", 0.0);
    EXPECT_DOUBLE_EQ(0.0, dh.load());

    // Cross-type get still works via on-the-fly conversion
    EXPECT_FLOAT_EQ(0.0f, state.get<float>("param"));
    EXPECT_EQ(0, state.get<int>("param"));
    EXPECT_FALSE(state.get<bool>("param"));
}

TEST(StateTests, HandleSurvivesOtherParameterCreation) {
    State state;
    state.create("param_a", 1.0);

    auto handle = state.get_handle<double>("param_a");
    EXPECT_DOUBLE_EQ(1.0, handle.load());

    // Create many more parameters -- must not invalidate handle
    for (int i = 0; i < 100; ++i) {
        state.create("param_" + std::to_string(i), static_cast<double>(i));
    }

    // Handle still works
    EXPECT_DOUBLE_EQ(1.0, handle.load());

    state.set("param_a", 99.0);
    EXPECT_DOUBLE_EQ(99.0, handle.load());
}

TEST(StateTests, HandleWithHierarchicalPath) {
    State state;
    state.create("audio.effects.reverb.mix", 0.5);

    auto handle = state.get_handle<double>("audio.effects.reverb.mix");
    EXPECT_DOUBLE_EQ(0.5, handle.load());

    state.set("audio.effects.reverb.mix", 0.8);
    EXPECT_DOUBLE_EQ(0.8, handle.load());
}

TEST(StateTests, HandleNativeTypeEnforced) {
    State state;
    state.create("dval", 10.0);
    state.create("fval", 2.5f);
    state.create("ival", 42);
    state.create("bval", true);

    // Native-type handles succeed
    EXPECT_NO_THROW(state.get_handle<double>("dval"));
    EXPECT_NO_THROW(state.get_handle<float>("fval"));
    EXPECT_NO_THROW(state.get_handle<int>("ival"));
    EXPECT_NO_THROW(state.get_handle<bool>("bval"));

    // Cross-type handles throw
    EXPECT_THROW(state.get_handle<float>("dval"), ParameterTypeMismatchException);
    EXPECT_THROW(state.get_handle<double>("fval"), ParameterTypeMismatchException);
    EXPECT_THROW(state.get_handle<bool>("ival"), ParameterTypeMismatchException);
    EXPECT_THROW(state.get_handle<int>("bval"), ParameterTypeMismatchException);
}

TEST(StateTests, HandleCopySemantics) {
    State state;
    state.create("param", 42.0);

    auto h1 = state.get_handle<double>("param");
    auto h2 = h1;  // Copy

    EXPECT_TRUE(h2.is_valid());
    EXPECT_DOUBLE_EQ(42.0, h2.load());

    h1.store(99.0);
    // Both handles point to the same AtomicCacheEntry
    EXPECT_DOUBLE_EQ(99.0, h2.load());
}

TEST(StateTests, HandleConcurrentLoadStore) {
    State state;
    state.create("param", 0.0);

    auto handle = state.get_handle<double>("param");

    const int num_threads = 4;
    const int ops_per_thread = 10000;
    std::atomic<bool> go{false};

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    // Writer threads use handle.store
    for (int t = 0; t < num_threads / 2; ++t) {
        threads.emplace_back([&, t] {
            while (!go.load(std::memory_order_acquire)) {}
            for (int i = 0; i < ops_per_thread; ++i) {
                handle.store(static_cast<double>(t * ops_per_thread + i));
            }
        });
    }

    // Reader threads use handle.load
    for (int t = 0; t < num_threads / 2; ++t) {
        threads.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) {}
            for (int i = 0; i < ops_per_thread; ++i) {
                volatile double val = handle.load();
                (void)val;
            }
        });
    }

    go.store(true, std::memory_order_release);
    for (auto& t : threads) { t.join(); }

    // Just verify handle is still usable
    double final_val = handle.load();
    EXPECT_GE(final_val, 0.0);
}

// =============================================================================
// ID-based access tests
// =============================================================================

// Helper: create a ParameterFloat with a specific m_id.
static ParameterFloat make_float_with_id(const std::string& name,
                                         Range range,
                                         float default_val,
                                         uint32_t id) {
    ParameterFloat def(name, range, default_val);
    def.m_id = id;
    return def;
}

TEST(StateTests, GetHandleByIdBasic) {
    State state;

    state.create("synth.freq", make_float_with_id("Frequency", Range(20.0f, 20000.0f), 440.0f, 1));
    state.create("synth.gain", make_float_with_id("Gain", Range(0.0f, 1.0f), 0.8f, 2));

    auto freq_handle = state.get_handle_by_id<float>(1);
    auto gain_handle = state.get_handle_by_id<float>(2);

    EXPECT_TRUE(freq_handle.is_valid());
    EXPECT_TRUE(gain_handle.is_valid());
    EXPECT_FLOAT_EQ(440.0f, freq_handle.load());
    EXPECT_FLOAT_EQ(0.8f, gain_handle.load());

    // Metadata accessible via handle
    EXPECT_EQ("Frequency", freq_handle.def().m_name);
    EXPECT_EQ(1u, freq_handle.id());
    EXPECT_EQ("synth.freq", freq_handle.key());
    EXPECT_EQ(ParameterType::Float, freq_handle.type());
}

TEST(StateTests, GetByIdReturnsParameter) {
    State state;

    state.create("vol", make_float_with_id("Volume", Range(0.0f, 1.0f), 0.5f, 10));

    Parameter param = state.get_parameter_by_id(10);
    EXPECT_FLOAT_EQ(0.5f, param.to<float>());
    EXPECT_EQ("Volume", param.def().m_name);
    EXPECT_EQ("vol", param.key());
}

TEST(StateTests, GetHandleByIdThrowsOnUnknownId) {
    State state;
    state.create("param", make_float_with_id("P", Range(), 0.0f, 1));

    EXPECT_THROW(state.get_handle_by_id<float>(999), StateKeyNotFoundException);
    EXPECT_THROW(state.get_parameter_by_id(999), StateKeyNotFoundException);
}

TEST(StateTests, GetHandleByIdThrowsOnTypeMismatch) {
    State state;
    state.create("param", make_float_with_id("P", Range(), 0.0f, 1));

    // Parameter is float, requesting double handle should throw
    EXPECT_THROW(state.get_handle_by_id<double>(1), ParameterTypeMismatchException);
    EXPECT_THROW(state.get_handle_by_id<int>(1), ParameterTypeMismatchException);
}

TEST(StateTests, DuplicateIdThrows) {
    State state;

    state.create("param1", make_float_with_id("P1", Range(), 0.0f, 5));

    // Same ID 5 on a different parameter should throw
    EXPECT_THROW(state.create("param2", make_float_with_id("P2", Range(), 0.0f, 5)),
                 DuplicateParameterIdException);
}

TEST(StateTests, AutoIdAssignment) {
    State state;

    // Value-created params get auto-assigned IDs starting from 0
    state.create("plain", 42.0);
    state.create("another", 7.0);

    // Auto-assigned IDs are sequential starting from 0
    EXPECT_DOUBLE_EQ(42.0, state.get_handle_by_id<double>(0).load());
    EXPECT_DOUBLE_EQ(7.0, state.get_handle_by_id<double>(1).load());

    // UINT32_MAX should not be findable (no longer used as a real ID)
    EXPECT_THROW(state.get_handle_by_id<double>(UINT32_MAX), StateKeyNotFoundException);
}

TEST(StateTests, ClearRemovesIdIndex) {
    State state;

    state.create("param", make_float_with_id("P", Range(), 0.0f, 1));

    EXPECT_FLOAT_EQ(0.0f, state.get_handle_by_id<float>(1).load());

    state.clear();

    EXPECT_THROW(state.get_handle_by_id<float>(1), StateKeyNotFoundException);
}

TEST(StateTests, HandleByIdAllNumericTypes) {
    State state;

    state.create("f", make_float_with_id("F", Range(), 3.14f, 1));

    ParameterInt int_def("I", Range(0, 100), 42);
    int_def.m_id = 2;
    state.create("i", int_def);

    ParameterBool bool_def("B", true);
    bool_def.m_id = 3;
    state.create("b", bool_def);

    auto fh = state.get_handle_by_id<float>(1);
    auto ih = state.get_handle_by_id<int>(2);
    auto bh = state.get_handle_by_id<bool>(3);

    EXPECT_FLOAT_EQ(3.14f, fh.load());
    EXPECT_EQ(42, ih.load());
    EXPECT_TRUE(bh.load());
}

// =============================================================================
// get_by_id tests
// =============================================================================

TEST(StateTests, GetByIdFloat) {
    State state;
    state.create("freq", make_float_with_id("Frequency", Range(20.0f, 20000.0f), 440.0f, 1));

    EXPECT_FLOAT_EQ(440.0f, state.get_by_id<float>(1));

    state.set("freq", 880.0f);
    EXPECT_FLOAT_EQ(880.0f, state.get_by_id<float>(1));
}

TEST(StateTests, GetByIdInt) {
    State state;
    ParameterInt int_def("Voices", Range(1, 16), 4);
    int_def.m_id = 10;
    state.create("voices", int_def);

    EXPECT_EQ(4, state.get_by_id<int>(10));

    state.set("voices", 8);
    EXPECT_EQ(8, state.get_by_id<int>(10));
}

TEST(StateTests, GetByIdBool) {
    State state;
    ParameterBool bool_def("Bypass", false);
    bool_def.m_id = 20;
    state.create("bypass", bool_def);

    EXPECT_FALSE(state.get_by_id<bool>(20));

    state.set("bypass", true);
    EXPECT_TRUE(state.get_by_id<bool>(20));
}

TEST(StateTests, GetByIdString) {
    State state;
    ParameterDefinition str_def;
    str_def.m_type = ParameterType::String;
    str_def.m_name = "Preset";
    str_def.m_id = 30;
    state.create("preset", str_def);
    state.set("preset", std::string("Init"));

    EXPECT_EQ("Init", state.get_by_id<std::string>(30, true));
}

TEST(StateTests, GetByIdCrossTypeConversion) {
    State state;
    state.create("freq", make_float_with_id("Frequency", Range(20.0f, 20000.0f), 440.0f, 1));

    // Reading float param as double/int should convert
    EXPECT_FLOAT_EQ(440.0f, static_cast<float>(state.get_by_id<double>(1)));
    EXPECT_EQ(440, state.get_by_id<int>(1));
}

TEST(StateTests, GetByIdThrowsOnUnknownId) {
    State state;
    state.create("param", make_float_with_id("P", Range(), 0.0f, 1));

    EXPECT_THROW(state.get_by_id<float>(999), StateKeyNotFoundException);
}

TEST(StateTests, GetByIdStringWithoutAllowBlockingThrows) {
    State state;
    ParameterDefinition str_def;
    str_def.m_type = ParameterType::String;
    str_def.m_name = "Preset";
    str_def.m_id = 30;
    state.create("preset", str_def);

    EXPECT_THROW(state.get_by_id<std::string>(30), BlockingException);
    EXPECT_NO_THROW(state.get_by_id<std::string>(30, true));
}

// =============================================================================
// set_by_id tests
// =============================================================================

TEST(StateTests, SetByIdFloat) {
    State state;
    state.create("freq", make_float_with_id("Frequency", Range(20.0f, 20000.0f), 440.0f, 1));

    state.set_by_id<float>(1, 880.0f);
    EXPECT_FLOAT_EQ(880.0f, state.get<float>("freq"));
}

TEST(StateTests, SetByIdInt) {
    State state;
    ParameterDefinition int_def;
    int_def.m_type = ParameterType::Int;
    int_def.m_name = "Note";
    int_def.m_id = 10;
    state.create("note", int_def);

    state.set_by_id<int>(10, 72);
    EXPECT_EQ(72, state.get<int>("note"));
}

TEST(StateTests, SetByIdBool) {
    State state;
    ParameterDefinition bool_def;
    bool_def.m_type = ParameterType::Bool;
    bool_def.m_name = "Active";
    bool_def.m_id = 20;
    state.create("active", bool_def);

    state.set_by_id<bool>(20, true);
    EXPECT_TRUE(state.get<bool>("active"));

    state.set_by_id<bool>(20, false);
    EXPECT_FALSE(state.get<bool>("active"));
}

TEST(StateTests, SetByIdDouble) {
    State state;
    ParameterDefinition dbl_def;
    dbl_def.m_type = ParameterType::Double;
    dbl_def.m_name = "Gain";
    dbl_def.m_id = 5;
    state.create("gain", dbl_def);

    state.set_by_id<double>(5, 0.75);
    EXPECT_DOUBLE_EQ(0.75, state.get<double>("gain"));
}

TEST(StateTests, SetByIdString) {
    State state;
    ParameterDefinition str_def;
    str_def.m_type = ParameterType::String;
    str_def.m_name = "Preset";
    str_def.m_id = 30;
    state.create("preset", str_def);

    state.set_by_id<std::string>(30, "Warm Pad");
    EXPECT_EQ("Warm Pad", state.get<std::string>("preset", true));
}

TEST(StateTests, SetByIdThrowsOnUnknownId) {
    State state;
    state.create("param", make_float_with_id("P", Range(), 0.0f, 1));

    EXPECT_THROW(state.set_by_id<float>(999, 1.0f), StateKeyNotFoundException);
}

TEST(StateTests, SetByIdNotifiesListeners) {
    State state;
    state.create("vol", make_float_with_id("Volume", Range(0.0f, 1.0f), 0.5f, 1));

    TestParameterListener listener;
    state.add_listener(&listener);

    state.set_by_id<float>(1, 0.9f);
    EXPECT_EQ(1, listener.m_notification_count);
    EXPECT_FLOAT_EQ(0.9f, state.get<float>("vol"));
}

TEST(StateTests, SetByIdCrossTypeConversion) {
    State state;
    state.create("freq", make_float_with_id("Frequency", Range(20.0f, 20000.0f), 440.0f, 1));

    // Set float parameter using int value
    state.set_by_id<int>(1, 880);
    EXPECT_FLOAT_EQ(880.0f, state.get<float>("freq"));
}

TEST(StateTests, SetByIdRoundTrip) {
    State state;
    state.create("param", make_float_with_id("P", Range(), 0.0f, 1));

    state.set_by_id<float>(1, 3.14f);
    EXPECT_FLOAT_EQ(3.14f, state.get_by_id<float>(1));
}

// =============================================================================
// Subgroup get_handle tests
// =============================================================================

TEST(StateTests, GetHandleFromSubgroup) {
    State state;
    StateGroup* synth = state.create_group("synth");
    synth->create("freq", 440.0f);

    // Get handle via subgroup (relative path)
    auto handle = synth->get_handle<float>("freq");
    EXPECT_FLOAT_EQ(440.0f, handle.load());

    // Modify via root state, verify handle sees the change
    state.set("synth.freq", 880.0f);
    EXPECT_FLOAT_EQ(880.0f, handle.load());
}

TEST(StateTests, GetHandleFromNestedSubgroup) {
    State state;
    StateGroup* audio = state.create_group("audio");
    StateGroup* effects = audio->create_group("effects");
    StateGroup* reverb = effects->create_group("reverb");
    reverb->create("mix", 0.3f);

    // Get handle from deeply nested group
    auto handle = reverb->get_handle<float>("mix");
    EXPECT_FLOAT_EQ(0.3f, handle.load());

    // Get handle via intermediate group (relative path)
    auto handle2 = effects->get_handle<float>("reverb.mix");
    EXPECT_FLOAT_EQ(0.3f, handle2.load());

    // Get handle via root (full path)
    auto handle3 = state.get_handle<float>("audio.effects.reverb.mix");
    EXPECT_FLOAT_EQ(0.3f, handle3.load());

    // All handles point to the same underlying record
    state.set("audio.effects.reverb.mix", 0.9f);
    EXPECT_FLOAT_EQ(0.9f, handle.load());
    EXPECT_FLOAT_EQ(0.9f, handle2.load());
    EXPECT_FLOAT_EQ(0.9f, handle3.load());
}

TEST(StateTests, GetHandleFromSubgroupAllTypes) {
    State state;
    StateGroup* group = state.create_group("params");
    group->create("d", 1.0);
    group->create("f", 2.5f);
    group->create("i", 42);
    group->create("b", true);

    auto dh = group->get_handle<double>("d");
    auto fh = group->get_handle<float>("f");
    auto ih = group->get_handle<int>("i");
    auto bh = group->get_handle<bool>("b");

    EXPECT_DOUBLE_EQ(1.0, dh.load());
    EXPECT_FLOAT_EQ(2.5f, fh.load());
    EXPECT_EQ(42, ih.load());
    EXPECT_TRUE(bh.load());
}

TEST(StateTests, GetHandleFromSubgroupThrowsTypeMismatch) {
    State state;
    StateGroup* group = state.create_group("params");
    group->create("val", 1.0f);

    EXPECT_THROW(group->get_handle<double>("val"), ParameterTypeMismatchException);
    EXPECT_THROW(group->get_handle<int>("val"), ParameterTypeMismatchException);
    EXPECT_THROW(group->get_handle<bool>("val"), ParameterTypeMismatchException);
}

TEST(StateTests, GetHandleFromSubgroupThrowsOnMissing) {
    State state;
    StateGroup* group = state.create_group("params");
    group->create("val", 1.0f);

    EXPECT_THROW(group->get_handle<float>("nonexistent"), StateKeyNotFoundException);
}
