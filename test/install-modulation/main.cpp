// Builds a ModulationMatrix over a host parameter store from an installed tanh
// without State, modulates one parameter and reads it back.
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/ModulationSource.h>
#include <tanh/modulation/ParameterBackend.h>
#include <tanh/parameters/ParameterExceptions.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string_view>

namespace {

// One float parameter "gain", 0..1.
class HostParameters final : public thl::modulation::ParameterBackend {
public:
    [[nodiscard]] std::optional<thl::modulation::ParameterBinding> find(
        std::string_view key) const override {
        if (key != "gain") { return std::nullopt; }
        return thl::modulation::ParameterBinding{
            .m_def =
                thl::ParameterDefinition::make_float("Gain", thl::Range::linear(0.0f, 1.0f), 0.0f)
                    .modulatable(true),
            .m_base = &m_gain,
        };
    }

private:
    mutable std::atomic<float> m_gain{0.25f};
};

// A constant 0.5 on every sample.
class Half final : public thl::modulation::ModulationSource {
public:
    Half() : ModulationSource(thl::modulation::k_global_scope) {}
    void prepare(double /*sample_rate*/, size_t samples_per_block, uint32_t voice_count) override {
        resize_buffers(samples_per_block, voice_count);
    }
    void process(size_t num_samples, size_t offset) override {
        for (size_t i = offset; i < offset + num_samples; ++i) { m_output_buffer[i] = 0.5f; }
    }
};

}  // namespace

int main() {
    HostParameters parameters;
    thl::modulation::ModulationMatrix matrix(parameters);
    Half half;
    matrix.add_source("half", &half);
    matrix.prepare(48000.0, 64);
    auto gain = matrix.get_smart_handle<float>("gain");
    matrix.add_routing({"half", "gain"});
    matrix.process(64);
    const float value = gain.load(63);

    bool threw = false;
    try {
        (void)matrix.get_smart_handle<float>("missing");
    } catch (const thl::ParameterNotFoundException&) { threw = true; }

    std::printf("gain=%g unknown-key-throws=%d\n", static_cast<double>(value), threw ? 1 : 0);
    return (value > 0.25f && threw) ? 0 : 1;
}
