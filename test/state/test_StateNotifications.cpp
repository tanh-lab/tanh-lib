#include "TestHelpers.h"
#include "tanh/state/State.h"
#include <gtest/gtest.h>

using namespace thl;

// =============================================================================
// Notification and listener tests
// =============================================================================

// Test for parameter notification functionality
TEST(StateTests, ParameterNotification) {
    State state;
    TestParameterListener listener;

    // Register the listener
    state.add_listener(&listener);

    // Create a parameter - this should trigger notification
    state.create("test.param", 42.0);

    // Verify the listener was notified
    EXPECT_EQ(1, listener.m_notification_count);
    EXPECT_EQ("test.param", listener.m_last_path);

    // Update the parameter - set() now always notifies
    state.set("test.param", 84.0);

    // Verify notification occurred (set() always notifies now)
    EXPECT_EQ(2, listener.m_notification_count);

    // Value should be updated
    EXPECT_DOUBLE_EQ(84.0, state.get<double>("test.param"));

    // Test hierarchical notifications
    StateGroup* audio_group = state.create_group("audio");
    TestParameterListener group_listener;
    audio_group->add_listener(&group_listener);

    // Create parameter in audio group
    state.create("audio.volume", 0.75);

    // Root listener should be notified
    EXPECT_EQ(3, listener.m_notification_count);
    // Group listener should be notified
    EXPECT_EQ(1, group_listener.m_notification_count);
    EXPECT_EQ("audio.volume", group_listener.m_last_path);

    // Create another parameter (creation always notifies)
    state.create("audio.bass", 0.5);

    // Both listeners should be notified
    EXPECT_EQ(4, listener.m_notification_count);
    EXPECT_EQ(2, group_listener.m_notification_count);

    // Manually notify
    Parameter param = state.get_parameter("audio.bass");
    param.notify();

    // Verify notifications occurred
    EXPECT_EQ(5, listener.m_notification_count);
    EXPECT_EQ(3, group_listener.m_notification_count);
}

TEST(StateTests, CallbackBasedNotifications) {
    State state;
    int callback_count = 0;
    std::string last_path;
    last_path.reserve(256);
    double last_value = 0.0;

    // Add a callback listener
    CallbackListener cb([&](const Parameter& param) {
        callback_count++;
        last_path = param.key();
        last_value = param.to<double>();
    });
    state.add_listener(&cb);

    // Create parameter
    state.create("test.callback.param", 123.0);

    // Verify callback was invoked
    EXPECT_EQ(1, callback_count);
    EXPECT_EQ("test.callback.param", last_path);
    EXPECT_DOUBLE_EQ(123.0, last_value);

    // Remove callback listener
    state.remove_listener(&cb);

    // Set parameter again
    state.set("test.callback.param", 456.0);

    // Verify callback was not invoked again
    EXPECT_EQ(1, callback_count);

    // But value should be updated
    EXPECT_DOUBLE_EQ(456.0, state.get<double>("test.callback.param"));
}

TEST(StateTests, ManualNotification) {
    State state;
    TestParameterListener listener;

    // Register listener
    state.add_listener(&listener);

    // Create a parameter (creation always notifies now)
    state.create("manual.test", 42.0);

    // Verify notification occurred from create
    EXPECT_EQ(1, listener.m_notification_count);

    // Manually notify about the parameter
    state.notify_parameter_change("manual.test");

    // Verify second notification occurred
    EXPECT_EQ(2, listener.m_notification_count);
    EXPECT_EQ("manual.test", listener.m_last_path);
    EXPECT_DOUBLE_EQ(42.0, listener.m_last_value_double);

    // Test direct Parameter notify method
    Parameter param = state.get_parameter("manual.test");
    param.notify();

    // Verify third notification
    EXPECT_EQ(3, listener.m_notification_count);
}

// =============================================================================
// Gesture notification filtering tests
// =============================================================================

TEST(StateTests, GestureFilterSkipsListenerDuringGesture) {
    State state;

    // Listener that opts out of gesture notifications
    class NoGestureListener : public ParameterListener {
    public:
        NoGestureListener() : ParameterListener(NotifyStrategies::All, false) {}
        void on_parameter_changed(const Parameter&) override { m_count++; }
        int m_count = 0;
    };

    TestParameterListener always_listener;  // default: receives_during_gesture = true
    NoGestureListener filtered_listener;

    state.add_listener(&always_listener);
    state.add_listener(&filtered_listener);

    // Normal set -- both listeners fire
    state.create("test.param", 1.0f);
    EXPECT_EQ(1, always_listener.m_notification_count);
    EXPECT_EQ(1, filtered_listener.m_count);

    // Begin gesture, then set -- only the always-listener fires
    state.set_gesture("test.param", true);
    state.set("test.param", 2.0f);
    EXPECT_EQ(2, always_listener.m_notification_count);
    EXPECT_EQ(1, filtered_listener.m_count);  // still 1

    // End gesture, then set -- both fire again
    state.set_gesture("test.param", false);
    state.set("test.param", 3.0f);
    EXPECT_EQ(3, always_listener.m_notification_count);
    EXPECT_EQ(2, filtered_listener.m_count);
}

TEST(StateTests, GestureFilterWithGroupListeners) {
    State state;
    StateGroup* group = state.create_group("synth");

    class NoGestureListener : public ParameterListener {
    public:
        NoGestureListener() : ParameterListener(NotifyStrategies::All, false) {}
        void on_parameter_changed(const Parameter&) override { m_count++; }
        int m_count = 0;
    };

    TestParameterListener root_listener;
    NoGestureListener group_filtered;

    state.add_listener(&root_listener);
    group->add_listener(&group_filtered);

    state.create("synth.cutoff", 0.5f);
    EXPECT_EQ(1, root_listener.m_notification_count);
    EXPECT_EQ(1, group_filtered.m_count);

    // Gesture active on this parameter
    state.set_gesture("synth.cutoff", true);
    state.set("synth.cutoff", 0.8f);
    EXPECT_EQ(2, root_listener.m_notification_count);
    EXPECT_EQ(1, group_filtered.m_count);  // skipped

    state.set_gesture("synth.cutoff", false);
    state.set("synth.cutoff", 0.3f);
    EXPECT_EQ(3, root_listener.m_notification_count);
    EXPECT_EQ(2, group_filtered.m_count);  // fires again
}

TEST(StateTests, GestureFilterIsPerParameter) {
    State state;

    class NoGestureListener : public ParameterListener {
    public:
        NoGestureListener() : ParameterListener(NotifyStrategies::All, false) {}
        void on_parameter_changed(const Parameter&) override { m_count++; }
        int m_count = 0;
    };

    NoGestureListener filtered;
    state.add_listener(&filtered);

    state.create("param.a", 1.0f);
    state.create("param.b", 1.0f);
    EXPECT_EQ(2, filtered.m_count);

    // Only param.a is in gesture
    state.set_gesture("param.a", true);

    state.set("param.a", 2.0f);
    EXPECT_EQ(2, filtered.m_count);  // skipped

    state.set("param.b", 2.0f);
    EXPECT_EQ(3, filtered.m_count);  // not in gesture -- fires

    state.set_gesture("param.a", false);
    state.set("param.a", 3.0f);
    EXPECT_EQ(4, filtered.m_count);  // fires again
}

TEST(StateTests, GestureValueStillUpdatedWhenListenerSkipped) {
    State state;

    class NoGestureListener : public ParameterListener {
    public:
        NoGestureListener() : ParameterListener(NotifyStrategies::All, false) {}
        void on_parameter_changed(const Parameter&) override { m_count++; }
        int m_count = 0;
    };

    NoGestureListener filtered;
    state.add_listener(&filtered);

    state.create("vol", 0.5f);
    EXPECT_EQ(1, filtered.m_count);

    state.set_gesture("vol", true);
    state.set("vol", 0.9f);
    EXPECT_EQ(1, filtered.m_count);  // notification skipped

    // But the value IS updated
    EXPECT_FLOAT_EQ(0.9f, state.get<float>("vol"));
}

// =============================================================================
// StringView invalidation tests
// =============================================================================

// Test for string_view invalidation when listener calls set() in callback
TEST(StateTests, StringViewInvalidationInNestedSetCalls) {
    State state;

    // Create a listener that captures the path via param.key() and then calls set() on
    // another parameter
    class NestedSetListener : public ParameterListener {
    public:
        // Each invocation records a (before, after) pair in m_matched_pairs
        struct PathPair {
            std::string m_before;
            std::string m_after;
        };

        void on_parameter_changed(const Parameter& param) override {
            std::string_view path = param.key();
            // Store the path immediately - this creates a copy
            std::string path_copy_immediate(path);

            // Also store pointer and size of the string_view to check later
            const char* view_data = path.data();
            size_t view_size = path.size();

            // Now trigger a nested set() call which might invalidate the string_view
            if (path == "trigger.param1") {
                m_state_ref->set("response.param1", 100.0);
                m_state_ref->set("response.param2", 200.0);
                m_state_ref->set("response.param3", 300.0);
            } else if (path == "trigger.deeply.nested.param") {
                m_state_ref->set("response.deep.level1.param", 1.0);
                m_state_ref->set("response.deep.level2.param", 2.0);
                m_state_ref->set("response.deep.level3.param", 3.0);
            }

            // Now try to access the original string_view - this is where it might be
            // corrupted
            std::string path_copy_after(path);

            // Store matched pair for this invocation
            m_matched_pairs.push_back({path_copy_immediate, path_copy_after});

            // Verify the string_view still points to valid data
            EXPECT_EQ(path_copy_immediate, path_copy_after)
                << "string_view was corrupted during nested set() call!";

            // Verify the string_view hasn't changed pointer or size
            EXPECT_EQ(view_data, path.data())
                << "string_view data pointer changed during nested set() call!";
            EXPECT_EQ(view_size, path.size())
                << "string_view size changed during nested set() call!";
        }

        State* m_state_ref = nullptr;
        std::vector<PathPair> m_matched_pairs;
    };

    NestedSetListener listener;
    listener.m_state_ref = &state;
    state.add_listener(&listener);

    // Pre-create the response parameters so nested set() calls don't need create()
    state.create("response.param1", 0.0);
    state.create("response.param2", 0.0);
    state.create("response.param3", 0.0);
    state.create("response.deep.level1.param", 0.0);
    state.create("response.deep.level2.param", 0.0);
    state.create("response.deep.level3.param", 0.0);

    // Clear notification count from creates
    listener.m_matched_pairs.clear();

    // Test 1: Simple nested set
    state.create("trigger.param1", 42.0);

    // trigger.param1 + 3 response params = 4 notifications
    ASSERT_EQ(4, listener.m_matched_pairs.size());

    // Verify every invocation's before/after match (string_view not corrupted)
    for (size_t i = 0; i < listener.m_matched_pairs.size(); ++i) {
        EXPECT_EQ(listener.m_matched_pairs[i].m_before, listener.m_matched_pairs[i].m_after)
            << "Path mismatch at invocation " << i;
    }

    // Test 2: Deeply nested path
    state.create("trigger.deeply.nested.param", 123.0);

    // Previous 4 + trigger.deeply.nested.param + 3 response.deep params = 8 total
    ASSERT_EQ(8, listener.m_matched_pairs.size());

    // Test 3: Multiple consecutive sets that trigger nested calls
    state.set("trigger.param1", 99.0);

    // Verify all invocations' before/after pairs match
    for (size_t i = 0; i < listener.m_matched_pairs.size(); ++i) {
        EXPECT_EQ(listener.m_matched_pairs[i].m_before, listener.m_matched_pairs[i].m_after)
            << "Path mismatch at invocation " << i;
    }

    // Verify all response parameters were actually set
    EXPECT_DOUBLE_EQ(100.0, state.get<double>("response.param1"));
    EXPECT_DOUBLE_EQ(200.0, state.get<double>("response.param2"));
    EXPECT_DOUBLE_EQ(300.0, state.get<double>("response.param3"));
    EXPECT_DOUBLE_EQ(1.0, state.get<double>("response.deep.level1.param"));
    EXPECT_DOUBLE_EQ(2.0, state.get<double>("response.deep.level2.param"));
    EXPECT_DOUBLE_EQ(3.0, state.get<double>("response.deep.level3.param"));
}

// Test string_view invalidation with callback-based listeners
TEST(StateTests, StringViewInvalidationInCallbackListeners) {
    State state;

    struct PathPair {
        std::string m_before;
        std::string m_after;
    };
    std::vector<PathPair> matched_pairs;

    // Pre-create target parameters
    state.create("target.b", 0.0);
    state.create("target.c", 0.0);

    // Add a callback listener that performs nested set() calls
    CallbackListener cb([&](const Parameter& param) {
        std::string_view path = param.key();
        // Capture path immediately
        std::string immediate_copy(path);

        const char* view_ptr = path.data();

        // Perform nested set that might invalidate the string_view
        if (path == "source.a") {
            state.set("target.b", 20.0);
            state.set("target.c", 30.0);
        }

        // Try to use the string_view again
        std::string after_copy(path);
        matched_pairs.push_back({immediate_copy, after_copy});

        // Verify they match
        EXPECT_EQ(immediate_copy, after_copy) << "string_view corrupted in callback listener!";

        // Verify pointer hasn't changed
        EXPECT_EQ(view_ptr, path.data()) << "string_view pointer changed in callback listener!";
    });
    state.add_listener(&cb);

    // Clear matched_pairs from pre-create notifications
    matched_pairs.clear();

    // Trigger the callback
    state.create("source.a", 10.0);

    // Verify we got notifications for all three parameters
    ASSERT_EQ(3, matched_pairs.size());

    // Verify all invocations' before/after pairs match
    for (size_t i = 0; i < matched_pairs.size(); ++i) {
        EXPECT_EQ(matched_pairs[i].m_before, matched_pairs[i].m_after)
            << "Path mismatch at invocation " << i;
    }
}

// Test string_view invalidation with very long parameter paths
TEST(StateTests, StringViewInvalidationWithLongPaths) {
    State state;

    class LongPathListener : public ParameterListener {
    public:
        void on_parameter_changed(const Parameter& param) override {
            std::string_view path = param.key();
            std::string path_before(path);
            const char* ptr_before = path.data();
            size_t size_before = path.size();

            // Only trigger nested sets for the original trigger path (exact prefix match)
            // to avoid infinite recursion from response paths also matching
            if (path.starts_with("very.")) {
                m_state_ref->set("another.very.long.hierarchical.path.level1.level2.level3.param1",
                                 1.0);
                m_state_ref->set("another.very.long.hierarchical.path.level1.level2.level3.param2",
                                 2.0);
                m_state_ref->set("another.very.long.hierarchical.path.level1.level2.level3.param3",
                                 3.0);
                m_state_ref->set("yet.another.extremely.long.path.with.many.nested.groups.param",
                                 4.0);
            }

            // Verify string_view is still valid
            std::string path_after(path);
            EXPECT_EQ(path_before, path_after) << "Long path string_view was corrupted!";
            EXPECT_EQ(ptr_before, path.data()) << "Long path string_view pointer changed!";
            EXPECT_EQ(size_before, path.size()) << "Long path string_view size changed!";

            m_paths.push_back(path_before);
        }

        State* m_state_ref = nullptr;
        std::vector<std::string> m_paths;
    };

    LongPathListener listener;
    listener.m_state_ref = &state;

    // Pre-create response parameters
    state.create("another.very.long.hierarchical.path.level1.level2.level3.param1", 0.0);
    state.create("another.very.long.hierarchical.path.level1.level2.level3.param2", 0.0);
    state.create("another.very.long.hierarchical.path.level1.level2.level3.param3", 0.0);
    state.create("yet.another.extremely.long.path.with.many.nested.groups.param", 0.0);

    state.add_listener(&listener);

    // Create a very long parameter path
    state.create("very.long.hierarchical.path.with.multiple.levels.and.groups.final.param", 99.0);

    // Verify the listener received notifications with valid paths
    // (m_paths vector is in DFS post-order due to recursion, so check by content)
    ASSERT_GE(listener.m_paths.size(), 1);
    bool found_trigger = false;
    for (const auto& p : listener.m_paths) {
        if (p == "very.long.hierarchical.path.with.multiple.levels.and.groups.final.param") {
            found_trigger = true;
        }
    }
    EXPECT_TRUE(found_trigger) << "Trigger path not found in listener notifications";

    // Verify all the nested parameters were set
    EXPECT_DOUBLE_EQ(
        1.0,
        state.get<double>("another.very.long.hierarchical.path.level1.level2.level3.param1"));
    EXPECT_DOUBLE_EQ(
        4.0,
        state.get<double>("yet.another.extremely.long.path.with.many.nested.groups.param"));
}

// Test chain of listener notifications where each triggers more sets
TEST(StateTests, StringViewInvalidationWithListenerChain) {
    State state;

    class ChainListener : public ParameterListener {
    public:
        explicit ChainListener(int id) : m_listener_id(id) {}

        void on_parameter_changed(const Parameter& param) override {
            std::string_view path = param.key();
            std::string path_copy(path);
            const char* ptr = path.data();

            // Each listener in the chain triggers the next
            if (path == "chain.step1" && m_listener_id == 1) {
                m_state_ref->set("chain.step2", 2.0);
            } else if (path == "chain.step2" && m_listener_id == 2) {
                m_state_ref->set("chain.step3", 3.0);
            } else if (path == "chain.step3" && m_listener_id == 3) {
                m_state_ref->set("chain.step4", 4.0);
            }

            // Verify string_view is still valid after potential nested calls
            std::string path_after(path);
            EXPECT_EQ(path_copy, path_after)
                << "string_view corrupted in listener chain at listener " << m_listener_id;
            EXPECT_EQ(ptr, path.data())
                << "string_view pointer changed in listener chain at listener " << m_listener_id;

            m_notifications.push_back(path_copy);
        }

        State* m_state_ref{nullptr};
        int m_listener_id;
        std::vector<std::string> m_notifications;
    };

    // Pre-create the chain parameters
    state.create("chain.step1", 0.0);
    state.create("chain.step2", 0.0);
    state.create("chain.step3", 0.0);
    state.create("chain.step4", 0.0);

    ChainListener listener1(1);
    ChainListener listener2(2);
    ChainListener listener3(3);

    listener1.m_state_ref = &state;
    listener2.m_state_ref = &state;
    listener3.m_state_ref = &state;

    state.add_listener(&listener1);
    state.add_listener(&listener2);
    state.add_listener(&listener3);

    // Start the chain
    state.set("chain.step1", 1.0);

    // All listeners should have received notifications
    EXPECT_GE(listener1.m_notifications.size(), 1);
    EXPECT_GE(listener2.m_notifications.size(), 1);
    EXPECT_GE(listener3.m_notifications.size(), 1);

    // Verify the chain completed
    EXPECT_DOUBLE_EQ(1.0, state.get<double>("chain.step1"));
    EXPECT_DOUBLE_EQ(2.0, state.get<double>("chain.step2"));
    EXPECT_DOUBLE_EQ(3.0, state.get<double>("chain.step3"));
    EXPECT_DOUBLE_EQ(4.0, state.get<double>("chain.step4"));
}

// =============================================================================
// Subgroup set_gesture tests
// =============================================================================

TEST(StateTests, SetGestureFromSubgroup) {
    State state;

    class NoGestureListener : public ParameterListener {
    public:
        NoGestureListener() : ParameterListener(NotifyStrategies::All, false) {}
        void on_parameter_changed(const Parameter&) override { m_count++; }
        int m_count = 0;
    };

    StateGroup* synth = state.create_group("synth");
    synth->create("cutoff", 0.5f);

    TestParameterListener always_listener;
    NoGestureListener filtered_listener;
    state.add_listener(&always_listener);
    state.add_listener(&filtered_listener);

    // set via subgroup, then gesture via subgroup
    synth->set("cutoff", 0.6f);
    EXPECT_EQ(1, always_listener.m_notification_count);
    EXPECT_EQ(1, filtered_listener.m_count);

    synth->set_gesture("cutoff", true);
    synth->set("cutoff", 0.8f);
    EXPECT_EQ(2, always_listener.m_notification_count);
    EXPECT_EQ(1, filtered_listener.m_count);  // skipped

    synth->set_gesture("cutoff", false);
    synth->set("cutoff", 0.3f);
    EXPECT_EQ(3, always_listener.m_notification_count);
    EXPECT_EQ(2, filtered_listener.m_count);  // fires again
}

TEST(StateTests, SetGestureFromNestedSubgroup) {
    State state;

    class NoGestureListener : public ParameterListener {
    public:
        NoGestureListener() : ParameterListener(NotifyStrategies::All, false) {}
        void on_parameter_changed(const Parameter&) override { m_count++; }
        int m_count = 0;
    };

    StateGroup* audio = state.create_group("audio");
    StateGroup* effects = audio->create_group("effects");
    effects->create("mix", 0.5f);

    NoGestureListener filtered;
    state.add_listener(&filtered);

    // Gesture set via intermediate group
    audio->set_gesture("effects.mix", true);
    state.set("audio.effects.mix", 0.9f);
    EXPECT_EQ(0, filtered.m_count);  // skipped (create notified before listener added)

    audio->set_gesture("effects.mix", false);
    state.set("audio.effects.mix", 0.1f);
    EXPECT_EQ(1, filtered.m_count);  // fires
}

TEST(StateTests, SetGestureFromSubgroupMatchesRootGesture) {
    // Gesture set via subgroup should be visible via root and vice versa
    State state;

    class NoGestureListener : public ParameterListener {
    public:
        NoGestureListener() : ParameterListener(NotifyStrategies::All, false) {}
        void on_parameter_changed(const Parameter&) override { m_count++; }
        int m_count = 0;
    };

    StateGroup* synth = state.create_group("synth");
    synth->create("vol", 0.5f);

    NoGestureListener filtered;
    state.add_listener(&filtered);

    // Set gesture via subgroup, verify root state recognizes it
    synth->set_gesture("vol", true);
    state.set("synth.vol", 0.7f);
    EXPECT_EQ(0, filtered.m_count);  // skipped

    // End gesture via root state
    state.set_gesture("synth.vol", false);
    state.set("synth.vol", 0.4f);
    EXPECT_EQ(1, filtered.m_count);  // fires again
}
