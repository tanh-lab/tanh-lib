// The host hand-over of ModulationRouting::m_share_with_host: host and
// modulation share a target as a DAW's Touch mode does, with crossfades.

#include <gtest/gtest.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/ModulationRouting.h>
#include <tanh/modulation/ModulationSource.h>
#include <tanh/modulation/ParameterBackend.h>
#include <tanh/modulation/ResolvedTarget.h>
#include <tanh/modulation/SmartHandle.h>
#include <tanh/parameters/ParameterDefinitions.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace thl::modulation;

namespace {

constexpr double k_sr = 48000.0;
constexpr size_t k_bs = 64;

constexpr size_t samples(float ms) {
    return static_cast<size_t>((static_cast<double>(ms) * k_sr / 1000.0) + 0.5);
}

/// A host-style backend: every parameter has a base value, a gesture flag and a
/// host-change stamp, as elastic's ApvtsParameterBackend provides them.
class HostBackend : public ParameterBackend {
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
        Entry& e = *it->second;
        return ParameterBinding{.m_def = e.m_def,
                                .m_base = &e.m_value,
                                .m_in_gesture = &e.m_gesture,
                                .m_host_change_stamp = &e.m_stamp};
    }

    /// The host writes the base value (automation, its UI): value and stamp.
    void host_write(const std::string& key, float value) {
        Entry& e = *m_entries.at(key);
        e.m_value.store(value, std::memory_order_relaxed);
        e.m_stamp.fetch_add(1, std::memory_order_relaxed);
    }
    void set_gesture(const std::string& key, bool on) { m_entries.at(key)->m_gesture.store(on); }

private:
    struct Entry {
        thl::ParameterDefinition m_def;
        std::atomic<float> m_value{0.0f};
        std::atomic<bool> m_gesture{false};
        std::atomic<uint64_t> m_stamp{0};
    };
    std::map<std::string, std::unique_ptr<Entry>, std::less<>> m_entries;
};

/// Global gated source with a constant value: active while m_active is set.
class GateSource : public ModulationSource {
public:
    explicit GateSource(float value) : ModulationSource(k_global_scope, false), m_value(value) {}
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
    }

    std::atomic<float> m_value;
    std::atomic<bool> m_active{false};
};

ModulationRouting shared(std::string_view src,
                         std::string_view dst,
                         CombineMode mode = CombineMode::ReplaceHold,
                         uint32_t priority = 0) {
    ModulationRouting r(src, dst, 1.0f);
    r.m_combine_mode = mode;
    r.m_replace_priority = priority;
    r.m_share_with_host = true;
    return r;
}

/// Matrix with parameter "p" (base 0.2) and two sources: "a" (0.8), "b" (0.6).
struct Rig {
    explicit Rig(CombineMode mode = CombineMode::ReplaceHold) {
        backend.add("p", 0.2f);
        backend.add("q", 0.2f);
        matrix = std::make_unique<ModulationMatrix>(backend);
        matrix->add_source("a", &a);
        matrix->add_source("b", &b);
        p = matrix->get_smart_handle<float>("p");
        routing_a = matrix->add_routing(shared("a", "p", mode));
        matrix->prepare(k_sr, k_bs);
    }
    ~Rig() {
        matrix->remove_source("a");
        matrix->remove_source("b");
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
    Rig(Rig&&) = delete;
    Rig& operator=(Rig&&) = delete;

    /// One block; appends every heard sample of p to m_heard.
    void block() {
        matrix->process(k_bs);
        for (uint32_t i = 0; i < k_bs; ++i) { m_heard.push_back(p.load(i)); }
    }
    void blocks(size_t n) {
        for (size_t k = 0; k < n; ++k) { block(); }
    }
    /// Blocks covering at least @p ms.
    void run_ms(float ms) { blocks((samples(ms) + k_bs - 1) / k_bs); }
    /// Host automation: a new base value every block for @p n blocks.
    void automate(size_t n, float from, float to) {
        for (size_t k = 0; k < n; ++k) {
            backend.host_write(
                "p",
                from + ((to - from) * static_cast<float>(k + 1) / static_cast<float>(n)));
            block();
        }
    }
    [[nodiscard]] float last() const { return m_heard.back(); }
    [[nodiscard]] const HostShareState& state() const {
        return matrix->get_target("p")->m_host_share;
    }

    HostBackend backend;
    std::unique_ptr<ModulationMatrix> matrix;
    GateSource a{0.8f};
    GateSource b{0.6f};
    SmartHandle<float> p;
    uint32_t routing_a = k_invalid_routing_id;
    std::vector<float> m_heard;
};

constexpr float k_eps = 1e-5f;

}  // namespace

// ── Last active wins ─────────────────────────────────────────────────────────

TEST(HostShare, HostFirstThenModulationTheModulationWins) {
    Rig r;
    r.automate(4, 0.2f, 0.4f);  // the host is active
    EXPECT_NEAR(r.last(), 0.4f, k_eps);
    EXPECT_EQ(r.state().m_owner, HostShareOwner::Host);

    r.a.m_active = true;  // the pad becomes active later: it wins
    for (int k = 0; k < 20; ++k) {
        r.backend.host_write("p", 0.4f + (0.001f * static_cast<float>(k)));  // host keeps going
        r.block();
    }
    EXPECT_EQ(r.state().m_owner, HostShareOwner::Modulation);
    EXPECT_NEAR(r.last(), 0.8f, k_eps);
}

TEST(HostShare, ModulationFirstThenHostTheHostWins) {
    Rig r;
    r.a.m_active = true;
    r.run_ms(HostShareTimes::k_take_ms + 5.0f);
    EXPECT_NEAR(r.last(), 0.8f, k_eps);

    r.automate(1, 0.3f, 0.3f);  // the host changes while the pad is still active
    EXPECT_EQ(r.state().m_owner, HostShareOwner::Host);
    r.run_ms(HostShareTimes::k_return_ms + 5.0f);
    EXPECT_NEAR(r.last(), 0.3f, k_eps);  // the host's value, the pad still held
}

TEST(HostShare, BothActiveTheLaterWinsEachTime) {
    Rig r;
    r.a.m_active = true;
    r.run_ms(30.0f);
    EXPECT_EQ(r.state().m_owner, HostShareOwner::Modulation);

    // Host becomes active (both now): host wins.
    r.automate(45, 0.3f, 0.35f);  // longer than the return ramp
    EXPECT_EQ(r.state().m_owner, HostShareOwner::Host);
    EXPECT_NEAR(r.last(), 0.35f, 1e-4f);

    // The pad re-touches while the host keeps automating: the pad wins.
    r.a.m_active = false;
    r.block();
    r.a.m_active = true;
    r.automate(20, 0.35f, 0.4f);
    EXPECT_EQ(r.state().m_owner, HostShareOwner::Modulation);
    EXPECT_NEAR(r.last(), 0.8f, k_eps);
}

TEST(HostShare, OwnerStopsTheActiveOtherContinues) {
    // The pad owns, the host keeps automating; the pad stops: the host takes over.
    {
        Rig r;
        r.automate(2, 0.2f, 0.3f);
        r.a.m_active = true;
        r.automate(20, 0.3f, 0.32f);
        ASSERT_EQ(r.state().m_owner, HostShareOwner::Modulation);
        r.a.m_active = false;
        r.automate(1, 0.33f, 0.33f);
        EXPECT_EQ(r.state().m_owner, HostShareOwner::Host);
        r.automate(60, 0.33f, 0.33f);
        EXPECT_NEAR(r.last(), 0.33f, k_eps);
    }
    // The host owns, the pad is held down; the host stops (its hold ends): the pad continues.
    {
        Rig r;
        r.a.m_active = true;
        r.run_ms(30.0f);
        r.automate(1, 0.4f, 0.4f);
        ASSERT_EQ(r.state().m_owner, HostShareOwner::Host);
        r.run_ms(HostShareTimes::k_hold_ms + 10.0f);
        EXPECT_EQ(r.state().m_owner, HostShareOwner::Modulation);
        r.run_ms(HostShareTimes::k_take_ms + 5.0f);
        EXPECT_NEAR(r.last(), 0.8f, k_eps);
    }
}

TEST(HostShare, NeitherActiveTheValueStays) {
    // The pad owns and is released, the host is idle: the pad's value stays.
    Rig r;
    r.a.m_active = true;
    r.run_ms(30.0f);
    r.a.m_active = false;
    r.run_ms(1000.0f);
    EXPECT_EQ(r.state().m_owner, HostShareOwner::Modulation);
    for (size_t i = r.m_heard.size() - samples(1000.0f); i < r.m_heard.size(); ++i) {
        ASSERT_NEAR(r.m_heard[i], 0.8f, k_eps);
    }

    // The host takes over and stops, the pad is idle: the host's value stays.
    r.automate(1, 0.5f, 0.5f);
    r.run_ms(1000.0f);
    EXPECT_EQ(r.state().m_owner, HostShareOwner::Host);
    EXPECT_NEAR(r.last(), 0.5f, k_eps);
}

TEST(HostShare, ReplaceWithoutHoldReturnsToTheHostWhenItsSourceStops) {
    Rig r(CombineMode::Replace);
    r.a.m_active = true;
    r.run_ms(30.0f);
    EXPECT_NEAR(r.last(), 0.8f, k_eps);
    r.a.m_active = false;  // nothing written any more: back to the base, as a crossfade
    r.block();
    EXPECT_GT(r.last(), 0.2f + 0.1f);
    r.run_ms(HostShareTimes::k_return_ms + 5.0f);
    EXPECT_EQ(r.state().m_owner, HostShareOwner::Host);
    EXPECT_NEAR(r.last(), 0.2f, k_eps);
}

TEST(HostShare, DisablingTheRoutingHandsBackToTheHost) {
    Rig r;
    r.a.m_active = true;
    r.run_ms(30.0f);
    ASSERT_TRUE(r.matrix->set_routing_enabled(r.routing_a, false));
    r.run_ms(HostShareTimes::k_return_ms + 5.0f);
    EXPECT_NEAR(r.last(), 0.2f, k_eps);
}

// ── Hold ─────────────────────────────────────────────────────────────────────

TEST(HostShare, HostStaysOwnerForTheHoldAfterItsLastChange) {
    Rig r;
    r.a.m_active = true;  // the pad keeps playing throughout
    r.run_ms(30.0f);
    r.automate(1, 0.4f, 0.4f);
    const size_t change = r.m_heard.size() - k_bs;  // the block that saw the change

    r.run_ms(HostShareTimes::k_hold_ms + 60.0f);
    // The weight leaves 0 again one block (the change's) plus the hold later.
    const size_t hand_back = change + k_bs + samples(HostShareTimes::k_hold_ms);
    for (size_t i = change + samples(HostShareTimes::k_return_ms) + 1; i < hand_back; ++i) {
        ASSERT_NEAR(r.m_heard[i], 0.4f, k_eps) << "sample " << i - change;
    }
    EXPECT_GT(r.m_heard[hand_back + 1], 0.4f);
    EXPECT_NEAR(r.last(), 0.8f, k_eps);
}

TEST(HostShare, ConfiguredTimesApply) {
    Rig r;
    r.matrix->set_host_share_times({.m_take_ms = 2.0f, .m_return_ms = 4.0f, .m_hold_ms = 10.0f});
    EXPECT_FLOAT_EQ(r.matrix->host_share_times().m_hold_ms, 10.0f);
    r.a.m_active = true;
    r.block();
    r.block();  // 128 samples > 96 (2 ms)
    EXPECT_NEAR(r.last(), 0.8f, k_eps);
    r.automate(1, 0.4f, 0.4f);
    r.blocks(3);  // 4 ms = 192 samples
    EXPECT_NEAR(r.last(), 0.4f, k_eps);
    r.blocks(10);  // 10 ms hold = 480 samples, then 2 ms back
    EXPECT_NEAR(r.last(), 0.8f, k_eps);
}

// ── Ramps ────────────────────────────────────────────────────────────────────

TEST(HostShare, RampsAreBoundedAndEndOnTime) {
    Rig r;
    r.block();
    const size_t take_start = r.m_heard.size();
    r.a.m_active = true;
    r.run_ms(HostShareTimes::k_take_ms + 10.0f);
    r.automate(1, 0.2f, 0.2f);  // the host takes back (same base value)
    const size_t return_start = r.m_heard.size() - k_bs;
    r.run_ms(HostShareTimes::k_return_ms + 10.0f);

    const float span = 0.8f - 0.2f;
    const float take_slope = span / static_cast<float>(samples(HostShareTimes::k_take_ms));
    const float return_slope = span / static_cast<float>(samples(HostShareTimes::k_return_ms));
    for (size_t i = 1; i < r.m_heard.size(); ++i) {
        const float slope = i < return_start ? take_slope : return_slope;
        ASSERT_LE(std::abs(r.m_heard[i] - r.m_heard[i - 1]), slope + 1e-5f) << "sample " << i;
    }

    // The take reaches the pad after exactly k_take_ms, the return the host after k_return_ms.
    const size_t take_n = samples(HostShareTimes::k_take_ms);
    EXPECT_LT(r.m_heard[take_start + take_n - 2], 0.8f - k_eps);
    EXPECT_NEAR(r.m_heard[take_start + take_n - 1], 0.8f, k_eps);
    const size_t return_n = samples(HostShareTimes::k_return_ms);
    EXPECT_GT(r.m_heard[return_start + return_n - 2], 0.2f + k_eps);
    EXPECT_NEAR(r.m_heard[return_start + return_n - 1], 0.2f, k_eps);
}

TEST(HostShare, RampsFollowAMovingBaseWithoutSteps) {
    // The host automates through the hand-over: the heard value never jumps more
    // than the ramp's slope plus the host's own motion.
    Rig r;
    r.a.m_active = true;
    r.run_ms(30.0f);
    const float per_block = 0.01f;
    float base = 0.2f;
    for (int k = 0; k < 60; ++k) {
        base += per_block;
        r.backend.host_write("p", base);
        r.block();
    }
    const float slope = (0.8f - 0.2f) / static_cast<float>(samples(HostShareTimes::k_return_ms));
    for (size_t i = 1; i < r.m_heard.size(); ++i) {
        ASSERT_LE(std::abs(r.m_heard[i] - r.m_heard[i - 1]), slope + per_block + 1e-5f)
            << "sample " << i;
    }
    EXPECT_NEAR(r.last(), base, k_eps);
}

// ── Gesture ──────────────────────────────────────────────────────────────────

TEST(HostShare, GestureIsHostActivityWithoutValueChanges) {
    Rig r;
    r.a.m_active = true;
    r.run_ms(30.0f);
    r.backend.set_gesture("p", true);  // a knob held still
    r.run_ms(500.0f);
    EXPECT_EQ(r.state().m_owner, HostShareOwner::Host);
    EXPECT_NEAR(r.last(), 0.2f, k_eps);

    r.backend.set_gesture("p", false);  // released: the hold, then the pad again
    r.run_ms(HostShareTimes::k_hold_ms - 20.0f);
    EXPECT_NEAR(r.last(), 0.2f, k_eps);
    r.run_ms(60.0f);
    EXPECT_NEAR(r.last(), 0.8f, k_eps);
}

// ── Change points ────────────────────────────────────────────────────────────

TEST(HostShare, ChangePointsWhileTheWeightMoves) {
    Rig r;
    r.block();
    r.a.m_active = true;  // GateSource records no change points of its own
    const size_t take_n = samples(HostShareTimes::k_take_ms);
    std::vector<uint32_t> flagged;  // absolute sample of every change point
    size_t t = 0;
    while (t < take_n + k_bs) {
        r.block();
        const auto* cps = r.p.change_points();
        ASSERT_NE(cps, nullptr);
        for (const uint32_t cp : *cps) { flagged.push_back(static_cast<uint32_t>(t + cp)); }
        t += k_bs;
    }
    ASSERT_FALSE(flagged.empty());
    EXPECT_EQ(flagged.front(), 0u);                                    // the ramp starts
    EXPECT_NE(std::ranges::find(flagged, take_n - 1), flagged.end());  // and ends
    for (size_t k = 1; k < flagged.size(); ++k) {
        EXPECT_LE(flagged[k] - flagged[k - 1], HostShareTimes::k_change_point_interval);
    }
    EXPECT_LE(flagged.back(), take_n - 1);  // nothing once it rests
}

// ── Two routings on one target, routings without the opt-in ──────────────────

// Sharing is a property of the target: every Replace routing on it is one
// writer (their composed output, priorities as usual); the host hands over with
// that writer as a whole.
TEST(HostShare, TwoRoutingsOnOneTargetShareAsOneWriter) {
    Rig r;
    ModulationRouting rb(std::string_view("b"), std::string_view("p"), 1.0f);
    rb.m_combine_mode = CombineMode::Replace;  // no opt-in of its own
    rb.m_replace_priority = 1;
    ASSERT_NE(r.matrix->add_routing(rb), k_invalid_routing_id);

    r.a.m_active = true;
    r.b.m_active = true;
    r.run_ms(30.0f);
    EXPECT_NEAR(r.last(), 0.6f, k_eps);  // b's higher priority wins the composition

    r.automate(1, 0.3f, 0.3f);  // the host takes over from both
    r.run_ms(HostShareTimes::k_return_ms + 5.0f);
    EXPECT_NEAR(r.last(), 0.3f, k_eps);

    r.b.m_active = false;  // a is still live: after the host's hold, a's value
    r.run_ms(HostShareTimes::k_hold_ms + HostShareTimes::k_take_ms + 10.0f);
    EXPECT_NEAR(r.last(), 0.8f, k_eps);
}

TEST(HostShare, RoutingsWithoutTheOptInAreUnchanged) {
    Rig r;
    auto q = r.matrix->get_smart_handle<float>("q");
    ModulationRouting rq(std::string_view("b"), std::string_view("q"), 1.0f);
    rq.m_combine_mode = CombineMode::Replace;
    rq.m_skip_during_gesture = true;
    ASSERT_NE(r.matrix->add_routing(rq), k_invalid_routing_id);

    r.b.m_active = true;
    r.block();
    EXPECT_FLOAT_EQ(q.load(0), 0.6f);  // a step, no ramp
    r.backend.host_write("q", 0.5f);
    r.block();
    EXPECT_FLOAT_EQ(q.load(0), 0.6f);  // host writes do not matter
    r.backend.set_gesture("q", true);
    r.block();
    EXPECT_FLOAT_EQ(q.load(0), 0.5f);  // skip_during_gesture still skips
    r.backend.set_gesture("q", false);
    r.b.m_active = false;
    r.block();
    EXPECT_FLOAT_EQ(q.load(0), 0.5f);  // plain Replace falls back at once
    EXPECT_EQ(r.matrix->get_target("q")->m_mono.load()->m_has_host_share, false);
}

TEST(HostShare, SkipDuringGestureIsIgnoredOnASharedTarget) {
    Rig r;
    ModulationRouting ra = shared("a", "p");
    ra.m_skip_during_gesture = true;
    r.matrix->remove_routing(r.routing_a);
    ASSERT_NE(r.matrix->add_routing(ra), k_invalid_routing_id);
    r.a.m_active = true;
    r.run_ms(30.0f);
    r.backend.set_gesture("p", true);
    r.block();
    // Crossfades out instead of a step to the base.
    EXPECT_GT(r.last(), 0.7f);
}

TEST(HostShare, SerialisesTheOptIn) {
    Rig r;
    const auto json = r.matrix->to_json(false);
    ASSERT_EQ(json.size(), 1u);
    EXPECT_TRUE(json[0]["share_with_host"].get<bool>());
    r.matrix->from_json(json);
    const auto again = r.matrix->to_json(false);
    EXPECT_TRUE(again[0]["share_with_host"].get<bool>());

    r.matrix->remove_routing(again[0]["id"].get<uint32_t>());
    ModulationRouting plain(std::string_view("a"), std::string_view("p"), 1.0f);
    plain.m_combine_mode = CombineMode::Replace;
    r.matrix->add_routing(plain);
    EXPECT_FALSE(r.matrix->to_json(false)[0].contains("share_with_host"));
}

// ── Real-time safety ─────────────────────────────────────────────────────────

namespace {

float rt_blocks(ModulationMatrix& matrix,
                const SmartHandle<float>& p,
                int blocks) TANH_NONBLOCKING_FUNCTION {
    float acc = 0.0f;
    for (int b = 0; b < blocks; ++b) {
        matrix.process(k_bs);
        acc += p.load(0);
    }
    return acc;
}

}  // namespace

// Under -DTANH_WITH_RTSAN=ON the hand-over pass must not allocate or lock.
TEST(HostShare, AudioPathIsRealtimeSafe) {
    Rig r;
    float acc = 0.0f;
    for (int k = 0; k < 80; ++k) {
        r.a.m_active = (k % 7) < 4;
        if (k % 5 == 0) { r.backend.host_write("p", 0.1f * static_cast<float>(k % 9)); }
        r.backend.set_gesture("p", (k % 11) == 3);
        acc += rt_blocks(*r.matrix, r.p, 3);
    }
    EXPECT_GT(acc, 0.0f);
}
