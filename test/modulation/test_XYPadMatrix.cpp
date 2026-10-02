// XYPad registered as matrix sources (x / y / active) over a host-style
// ParameterBackend — no thl::State involved.

#include <gtest/gtest.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/ModulationRouting.h>
#include <tanh/modulation/ParameterBackend.h>
#include <tanh/modulation/SmartHandle.h>
#include <tanh/modulation/XYPad.h>
#include <tanh/state/ParameterDefinitions.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace thl::modulation;

namespace {

constexpr double k_sr = 48000.0;
constexpr size_t k_bs = 512;

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

ModulationRouting replace_routing(std::string_view src, std::string_view dst, CombineMode mode) {
    ModulationRouting r(src, dst, 1.0f);
    r.m_combine_mode = mode;
    r.m_replace_priority = 10;
    return r;
}

}  // namespace

TEST(XYPadMatrix, MatrixIntegration_ReplaceHoldAndReplace) {
    FakeBackend backend;
    backend.add("slot_a", 0.2f);
    backend.add("gate", 0.3f);
    ModulationMatrix matrix(backend);
    XYPad pad;
    pad.add_to(matrix, "xy.pad1");
    EXPECT_EQ(pad.source_id(XYPadAxis::Active), "xy.pad1.active");
    auto a = matrix.get_smart_handle<float>("slot_a");
    auto gate = matrix.get_smart_handle<float>("gate");
    matrix.add_routing(replace_routing("xy.pad1.x", "slot_a", CombineMode::ReplaceHold));
    matrix.add_routing(replace_routing("xy.pad1.active", "gate", CombineMode::Replace));
    matrix.prepare(k_sr, k_bs);

    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(a.load(k_bs - 1), 0.2f);  // untouched → base value
    EXPECT_FLOAT_EQ(gate.load(k_bs - 1), 0.3f);

    pad.touch(1, 0.7f, 0.1f);
    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(a.load(0), 0.7f);
    EXPECT_FLOAT_EQ(gate.load(0), 1.0f);

    pad.release(1);
    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(a.load(k_bs - 1), 0.7f);     // ReplaceHold keeps the last touch
    EXPECT_FLOAT_EQ(gate.load(k_bs - 1), 0.3f);  // Replace falls back to base

    pad.remove_from(matrix);
}

// The three outputs must see one drain per block whatever order the matrix
// runs them in (it runs every clear_per_block() before any pre_process_block()).
TEST(XYPadMatrix, DrainsExactlyOnce_AnyOutputOrder) {
    FakeBackend backend;
    ModulationMatrix matrix(backend);
    XYPad pad;
    pad.add_to(matrix, "p");
    matrix.prepare(k_sr, k_bs);
    matrix.process(k_bs);

    std::array<ModulationSource*, 3> outputs{&pad.source(XYPadAxis::X),
                                             &pad.source(XYPadAxis::Y),
                                             &pad.source(XYPadAxis::Active)};
    std::array<int, 3> order{0, 1, 2};
    int perm = 0;
    do {
        const float x = 0.1f * static_cast<float>(perm + 1);
        pad.touch(1, x, 1.0f - x);
        pad.touch(1, x + 0.05f, 0.95f - x);
        for (auto* o : outputs) { o->clear_per_block(); }
        for (const int i : order) { outputs[static_cast<size_t>(i)]->pre_process_block(); }
        for (auto* o : outputs) {
            ASSERT_EQ(o->get_change_points().size(), 2u) << "perm " << perm;
            EXPECT_EQ(o->get_change_points()[1], 256u);
        }
        EXPECT_FLOAT_EQ(outputs[0]->get_output_at(0), x);
        EXPECT_FLOAT_EQ(outputs[0]->get_output_at(k_bs - 1), x + 0.05f);
        EXPECT_FLOAT_EQ(outputs[1]->get_output_at(k_bs - 1), 0.95f - x);
        EXPECT_FLOAT_EQ(outputs[2]->get_output_at(0), 1.0f);
        pad.touch(1, 0.99f, 0.99f);  // must not be drained by the same block's matrix pass
        matrix.process(k_bs);        // same block → reuses the render, advances the counter
        EXPECT_FLOAT_EQ(outputs[0]->get_output_at(k_bs - 1), x + 0.05f);
        matrix.process(k_bs);  // next block picks the move up
        EXPECT_FLOAT_EQ(outputs[0]->get_output_at(0), 0.99f);
        ++perm;
    } while (std::next_permutation(order.begin(), order.end()));
    EXPECT_EQ(perm, 6);
    pad.remove_from(matrix);
}

// An owner (XY controller) drives the pad itself before the matrix: the matrix
// outputs must reuse that render (real block size) instead of draining again.
TEST(XYPadMatrix, ExternalDriverRenderIsReused) {
    FakeBackend backend;
    backend.add("slot_a", 0.0f);
    ModulationMatrix matrix(backend);
    XYPad pad;
    pad.add_to(matrix, "p");
    auto a = matrix.get_smart_handle<float>("slot_a");
    matrix.add_routing(replace_routing("p.x", "slot_a", CombineMode::ReplaceHold));
    matrix.prepare(k_sr, k_bs);

    pad.touch(1, 0.25f, 0.5f);
    pad.touch(1, 0.75f, 0.5f);
    pad.process_block(100);  // host block of 100 frames
    const XYPadStream primary = pad.primary();
    EXPECT_EQ(primary.m_num_samples, 100u);
    EXPECT_EQ(primary.m_change_points[1], 50u);  // spread over the real block

    matrix.process(100);
    EXPECT_FLOAT_EQ(a.load(49), 0.25f);
    EXPECT_FLOAT_EQ(a.load(50), 0.75f);
    EXPECT_EQ(pad.source(XYPadAxis::X).get_change_points().size(), 2u);
    pad.remove_from(matrix);
}

TEST(XYPadMatrix, VoiceScopeRoutesPerVoice) {
    FakeBackend backend;
    ModulationMatrix matrix(backend);
    const ModulationScope voices = matrix.register_scope("voice", 4);
    XYPad pad(XYPadConfig{.m_scope = voices, .m_max_touches = 2});
    pad.add_to(matrix, "p");
    matrix.prepare(k_sr, k_bs);
    pad.touch(1, 0.1f, 0.2f);
    pad.touch(2, 0.3f, 0.4f);
    matrix.process(k_bs);
    auto& x = pad.source(XYPadAxis::X);
    EXPECT_EQ(x.num_voices(), 4u);
    EXPECT_FLOAT_EQ(x.voice_output(0)[0], 0.1f);
    EXPECT_FLOAT_EQ(x.voice_output(1)[0], 0.3f);
    EXPECT_EQ(x.voice_output_active(1)[0], 1);
    EXPECT_EQ(x.voice_output_active(2)[0], 0);  // beyond max_touches: silent
    EXPECT_EQ(x.get_voice_change_points(1).size(), 1u);
    pad.remove_from(matrix);
}

namespace {

void rt_audio_block(ModulationMatrix& matrix, size_t n) TANH_NONBLOCKING_FUNCTION {
    const auto scope = matrix.audio_read_scope();
    matrix.process_with_scope(scope.data(), n);
}

}  // namespace

// Under -DTANH_WITH_RTSAN=ON any allocation, lock or syscall in the pad's
// audio path (drain, render, output copy) aborts the test.
TEST(XYPadMatrix, AudioPathIsRealtimeSafe) {
    FakeBackend backend;
    ModulationMatrix matrix(backend);
    std::vector<std::unique_ptr<XYPad>> pads;
    for (int p = 0; p < 8; ++p) {
        const std::string key = "slot" + std::to_string(p);
        backend.add(key, 0.0f);
        pads.push_back(std::make_unique<XYPad>());
        pads.back()->add_to(matrix, "xy.pad" + std::to_string(p));
        matrix.add_routing(
            replace_routing("xy.pad" + std::to_string(p) + ".x", key, CombineMode::ReplaceHold));
    }
    matrix.prepare(k_sr, k_bs);
    for (int b = 0; b < 50; ++b) {
        for (int p = 0; p < 8; ++p) {
            auto& pad = *pads[static_cast<size_t>(p)];
            if (b % 7 == 3) {
                pad.release(1);
            } else {
                pad.touch(1, static_cast<float>(b % 10) / 10.0f, 0.5f);
            }
        }
        rt_audio_block(matrix, b % 2 == 0 ? k_bs : 128);
    }
    for (auto& pad : pads) { pad->remove_from(matrix); }
}
