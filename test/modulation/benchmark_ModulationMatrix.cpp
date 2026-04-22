#include <benchmark/benchmark.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <tanh/modulation/InputEventQueue.h>
#include <tanh/modulation/LFOSource.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/SmartHandle.h>
#include <tanh/state/State.h>

using namespace thl;
using namespace thl::modulation;

// =============================================================================
// Helpers
// =============================================================================

static constexpr double k_sample_rate = 48000.0;
static constexpr size_t k_block_size = 512;

// Concrete LFO — returns fixed values directly (no State dependency).
class BenchLFO : public LFOSourceImpl {
public:
    float m_frequency = 10.0f;
    LFOWaveform m_waveform = LFOWaveform::Sine;
    int m_decimation = 1;

private:
    float get_parameter_float(Parameter p, uint32_t) override {
        switch (p) {
            case Frequency: return m_frequency;
            default: return 0.0f;
        }
    }
    int get_parameter_int(Parameter p, uint32_t) override {
        switch (p) {
            case Waveform: return static_cast<int>(m_waveform);
            case Decimation: return m_decimation;
            default: return 0;
        }
    }
};

static ParameterDefinition modulatable_float(float default_value = 0.0f) {
    return ParameterDefinition::make_float("", Range::linear(0.0f, 1.0f), default_value)
        .automatable(false)
        .modulatable(true);
}

// =============================================================================
// Baseline: ParameterHandle load (no modulation overhead)
// =============================================================================

static void bm_handle_load_float(benchmark::State& bm_state) {
    State state;
    state.create("param", modulatable_float(0.5f));
    auto handle = state.get_handle<float>("param");
    for ([[maybe_unused]] auto _ : bm_state) { benchmark::DoNotOptimize(handle.load()); }
}
BENCHMARK(bm_handle_load_float);

// =============================================================================
// SmartHandle load — unmodulated (should match ParameterHandle)
// =============================================================================

static void bm_smart_handle_load_unmodulated(benchmark::State& bm_state) {
    State state;
    state.create("param", modulatable_float(0.5f));
    ModulationMatrix matrix(state);
    auto handle = matrix.get_smart_handle<float>("param");

    matrix.prepare(k_sample_rate, k_block_size);

    for ([[maybe_unused]] auto _ : bm_state) { benchmark::DoNotOptimize(handle.load(0)); }
}
BENCHMARK(bm_smart_handle_load_unmodulated);

// =============================================================================
// SmartHandle load — modulated (base + buffer read)
// =============================================================================

static void bm_smart_handle_load_modulated(benchmark::State& bm_state) {
    State state;
    state.create("param", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    BenchLFO lfo;
    matrix.add_source("lfo", &lfo);
    auto handle = matrix.get_smart_handle<float>("param");
    matrix.add_routing({"lfo", "param", 0.5f});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    uint32_t offset = 0;
    for ([[maybe_unused]] auto _ : bm_state) {
        benchmark::DoNotOptimize(handle.load(offset));
        offset = (offset + 1) % static_cast<uint32_t>(k_block_size);
    }
}
BENCHMARK(bm_smart_handle_load_modulated);

// =============================================================================
// SmartHandle load_normalized — modulated
// =============================================================================

static void bm_smart_handle_load_normalized(benchmark::State& bm_state) {
    State state;
    state.create("param", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    BenchLFO lfo;
    matrix.add_source("lfo", &lfo);
    auto handle = matrix.get_smart_handle<float>("param");
    matrix.add_routing({"lfo", "param", 0.5f});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    uint32_t offset = 0;
    for ([[maybe_unused]] auto _ : bm_state) {
        benchmark::DoNotOptimize(handle.load_normalized(offset));
        offset = (offset + 1) % static_cast<uint32_t>(k_block_size);
    }
}
BENCHMARK(bm_smart_handle_load_normalized);

// =============================================================================
// SmartHandle<int> load — modulated
// =============================================================================

static void bm_smart_handle_load_int(benchmark::State& bm_state) {
    State state;
    state.create(
        "choice",
        ParameterDefinition::make_int("Choice", Range::discrete(0, 10), 5).modulatable(true));
    ModulationMatrix matrix(state);

    BenchLFO lfo;
    matrix.add_source("lfo", &lfo);
    auto handle = matrix.get_smart_handle<int>("choice");
    matrix.add_routing({"lfo", "choice", 3.0f, 0, DepthMode::Absolute});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    uint32_t offset = 0;
    for ([[maybe_unused]] auto _ : bm_state) {
        benchmark::DoNotOptimize(handle.load(offset));
        offset = (offset + 1) % static_cast<uint32_t>(k_block_size);
    }
}
BENCHMARK(bm_smart_handle_load_int);

// =============================================================================
// ModulationMatrix::process — single source, single target
// =============================================================================

static void bm_process_single_routing(benchmark::State& bm_state) {
    State state;
    state.create("param", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    BenchLFO lfo;
    matrix.add_source("lfo", &lfo);
    matrix.get_smart_handle<float>("param");
    matrix.add_routing({"lfo", "param", 0.5f});

    matrix.prepare(k_sample_rate, k_block_size);

    for ([[maybe_unused]] auto _ : bm_state) { matrix.process(k_block_size); }
    bm_state.SetItemsProcessed(static_cast<int64_t>(bm_state.iterations()) *
                               static_cast<int64_t>(k_block_size));
}
BENCHMARK(bm_process_single_routing);

// =============================================================================
// ModulationMatrix::process — scaling: N sources to N targets
// =============================================================================

static void bm_process_n_routings(benchmark::State& bm_state) {
    const auto num_routings = bm_state.range(0);
    State state;
    std::vector<BenchLFO> lfos(static_cast<size_t>(num_routings));
    ModulationMatrix matrix(state);

    for (int64_t i = 0; i < num_routings; ++i) {
        std::string src_id = "lfo_" + std::to_string(i);
        std::string tgt_key = "param_" + std::to_string(i);
        state.create(tgt_key, modulatable_float(0.5f));
        lfos[static_cast<size_t>(i)].m_frequency = 10.0f + static_cast<float>(i);
        matrix.add_source(src_id, &lfos[static_cast<size_t>(i)]);
        matrix.get_smart_handle<float>(tgt_key);
        matrix.add_routing({src_id, tgt_key, 0.5f});
    }

    matrix.prepare(k_sample_rate, k_block_size);

    for ([[maybe_unused]] auto _ : bm_state) { matrix.process(k_block_size); }
    bm_state.SetItemsProcessed(static_cast<int64_t>(bm_state.iterations()) *
                               static_cast<int64_t>(k_block_size) * num_routings);
}
BENCHMARK(bm_process_n_routings)->Arg(1)->Arg(4)->Arg(16)->Arg(64);

// =============================================================================
// ModulationMatrix::process — multiple sources to same target (fan-in)
// =============================================================================

static void bm_process_fan_in(benchmark::State& bm_state) {
    const auto num_sources = bm_state.range(0);
    State state;
    state.create("param", modulatable_float(0.5f));
    std::vector<BenchLFO> lfos(static_cast<size_t>(num_sources));
    ModulationMatrix matrix(state);

    matrix.get_smart_handle<float>("param");
    for (int64_t i = 0; i < num_sources; ++i) {
        std::string src_id = "lfo_" + std::to_string(i);
        lfos[static_cast<size_t>(i)].m_frequency = 10.0f + static_cast<float>(i);
        matrix.add_source(src_id, &lfos[static_cast<size_t>(i)]);
        matrix.add_routing({src_id, "param", 0.1f});
    }

    matrix.prepare(k_sample_rate, k_block_size);

    for ([[maybe_unused]] auto _ : bm_state) { matrix.process(k_block_size); }
    bm_state.SetItemsProcessed(static_cast<int64_t>(bm_state.iterations()) *
                               static_cast<int64_t>(k_block_size));
}
BENCHMARK(bm_process_fan_in)->Arg(1)->Arg(4)->Arg(16);

// =============================================================================
// Depth Mode: Absolute vs Normalized (linear) vs Normalized (skewed)
// =============================================================================

static void bm_process_depth_absolute(benchmark::State& bm_state) {
    State state;
    state.create(
        "param",
        ParameterDefinition::make_float("P", Range::linear(0.0f, 100.0f), 50.0f).modulatable(true));
    ModulationMatrix matrix(state);

    BenchLFO lfo;
    matrix.add_source("lfo", &lfo);
    matrix.get_smart_handle<float>("param");
    matrix.add_routing({"lfo", "param", 10.0f, 0, DepthMode::Absolute});

    matrix.prepare(k_sample_rate, k_block_size);

    for ([[maybe_unused]] auto _ : bm_state) { matrix.process(k_block_size); }
    bm_state.SetItemsProcessed(static_cast<int64_t>(bm_state.iterations()) *
                               static_cast<int64_t>(k_block_size));
}
BENCHMARK(bm_process_depth_absolute);

static void bm_process_depth_normalized_linear(benchmark::State& bm_state) {
    State state;
    state.create(
        "param",
        ParameterDefinition::make_float("P", Range::linear(0.0f, 100.0f), 50.0f).modulatable(true));
    ModulationMatrix matrix(state);

    BenchLFO lfo;
    matrix.add_source("lfo", &lfo);
    matrix.get_smart_handle<float>("param");
    matrix.add_routing({"lfo", "param", 0.5f, 0, DepthMode::Normalized});

    matrix.prepare(k_sample_rate, k_block_size);

    for ([[maybe_unused]] auto _ : bm_state) { matrix.process(k_block_size); }
    bm_state.SetItemsProcessed(static_cast<int64_t>(bm_state.iterations()) *
                               static_cast<int64_t>(k_block_size));
}
BENCHMARK(bm_process_depth_normalized_linear);

static void bm_process_depth_normalized_skewed(benchmark::State& bm_state) {
    State state;
    state.create(
        "freq",
        ParameterDefinition::make_float("Freq", Range::power_law(20.0f, 20000.0f, 3.0f), 440.0f)
            .modulatable(true));
    ModulationMatrix matrix(state);

    BenchLFO lfo;
    matrix.add_source("lfo", &lfo);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"lfo", "freq", 0.3f, 0, DepthMode::Normalized});

    matrix.prepare(k_sample_rate, k_block_size);

    for ([[maybe_unused]] auto _ : bm_state) { matrix.process(k_block_size); }
    bm_state.SetItemsProcessed(static_cast<int64_t>(bm_state.iterations()) *
                               static_cast<int64_t>(k_block_size));
}
BENCHMARK(bm_process_depth_normalized_skewed);

// =============================================================================
// Block Size Scaling
// =============================================================================

static void bm_process_block_size(benchmark::State& bm_state) {
    const auto block_size = static_cast<size_t>(bm_state.range(0));
    State state;
    state.create("param", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    BenchLFO lfo;
    matrix.add_source("lfo", &lfo);
    matrix.get_smart_handle<float>("param");
    matrix.add_routing({"lfo", "param", 0.5f});

    matrix.prepare(k_sample_rate, block_size);

    for ([[maybe_unused]] auto _ : bm_state) { matrix.process(block_size); }
    bm_state.SetItemsProcessed(static_cast<int64_t>(bm_state.iterations()) *
                               static_cast<int64_t>(block_size));
}
BENCHMARK(bm_process_block_size)->Arg(32)->Arg(64)->Arg(128)->Arg(256)->Arg(512)->Arg(1024);

// =============================================================================
// Decimation: high-resolution vs decimated LFO
// =============================================================================

static void bm_process_decimation(benchmark::State& bm_state) {
    const auto decimation = static_cast<int>(bm_state.range(0));
    State state;
    state.create("param", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    BenchLFO lfo;
    lfo.m_decimation = decimation;
    matrix.add_source("lfo", &lfo);
    matrix.get_smart_handle<float>("param");
    matrix.add_routing({"lfo", "param", 0.5f});

    matrix.prepare(k_sample_rate, k_block_size);

    for ([[maybe_unused]] auto _ : bm_state) { matrix.process(k_block_size); }
    bm_state.SetItemsProcessed(static_cast<int64_t>(bm_state.iterations()) *
                               static_cast<int64_t>(k_block_size));
}
BENCHMARK(bm_process_decimation)->Arg(1)->Arg(8)->Arg(32)->Arg(128);

// =============================================================================
// collect_change_points from SmartHandle span
// =============================================================================

static void bm_collect_change_points(benchmark::State& bm_state) {
    const auto num_handles = static_cast<size_t>(bm_state.range(0));
    State state;
    ModulationMatrix matrix(state);

    BenchLFO lfo;
    lfo.m_decimation = 1;
    matrix.add_source("lfo", &lfo);

    std::vector<SmartHandle<float>> handles;
    handles.reserve(num_handles);
    for (size_t i = 0; i < num_handles; ++i) {
        std::string key = "param_" + std::to_string(i);
        state.create(key, modulatable_float(0.5f));
        handles.push_back(matrix.get_smart_handle<float>(key));
        matrix.add_routing({"lfo", key, 0.1f});
    }

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    std::vector<uint32_t> merged;
    merged.reserve(k_block_size);

    for ([[maybe_unused]] auto _ : bm_state) {
        collect_change_points(std::span<const SmartHandle<float>>(handles), merged);
        benchmark::DoNotOptimize(merged.data());
    }
}
BENCHMARK(bm_collect_change_points)->Arg(1)->Arg(4)->Arg(16);

// =============================================================================
// Full DSP-like loop: process + read all samples via SmartHandle
// =============================================================================

static void bm_full_modulated_read_loop(benchmark::State& bm_state) {
    State state;
    state.create("freq",
                 ParameterDefinition::make_float("Freq", Range::linear(20.0f, 20000.0f), 440.0f)
                     .modulatable(true));
    state.create(
        "gain",
        ParameterDefinition::make_float("Gain", Range::linear(0.0f, 1.0f), 0.8f).modulatable(true));
    ModulationMatrix matrix(state);

    BenchLFO lfo;
    lfo.m_frequency = 5.0f;
    lfo.m_decimation = 16;
    matrix.add_source("lfo", &lfo);

    auto freq = matrix.get_smart_handle<float>("freq");
    auto gain = matrix.get_smart_handle<float>("gain");
    matrix.add_routing({"lfo", "freq", 0.2f});
    matrix.add_routing({"lfo", "gain", 0.1f});

    matrix.prepare(k_sample_rate, k_block_size);

    std::vector<uint32_t> change_pts;
    change_pts.reserve(k_block_size);
    std::array<SmartHandle<float>, 2> handles = {freq, gain};

    float accumulator = 0.0f;
    for ([[maybe_unused]] auto _ : bm_state) {
        matrix.process(k_block_size);
        collect_change_points(std::span<const SmartHandle<float>>(handles), change_pts);

        // Simulate DSP read loop with change-point sub-blocking
        size_t cp_idx = 0;
        for (size_t i = 0; i < k_block_size; ++i) {
            // Advance to next change point region
            while (cp_idx < change_pts.size() && change_pts[cp_idx] <= i) { ++cp_idx; }
            accumulator +=
                freq.load(static_cast<uint32_t>(i)) * gain.load(static_cast<uint32_t>(i));
        }
        benchmark::DoNotOptimize(accumulator);
    }
    bm_state.SetItemsProcessed(static_cast<int64_t>(bm_state.iterations()) *
                               static_cast<int64_t>(k_block_size));
}
BENCHMARK(bm_full_modulated_read_loop);

// =============================================================================
// InputEventQueue — drain_spread throughput (N events × M voices per block)
// =============================================================================

using namespace thl::modulation;

static void bm_input_event_queue_drain_spread(benchmark::State& bm_state) {
    const auto events_per_voice = static_cast<uint32_t>(bm_state.range(0));
    const auto num_voices = static_cast<uint32_t>(bm_state.range(1));
    const uint32_t block_size = static_cast<uint32_t>(k_block_size);
    const size_t capacity = static_cast<size_t>(events_per_voice) * num_voices;

    // A non-global scope handle so the queue allocates voice buckets. Not
    // bound to any matrix — InputEventQueue only inspects has_mono().
    static constexpr thl::modulation::ModulationScope k_bench_voice_scope{.m_id = 1, .m_name = "bench_voice"};
    InputEventQueue queue(k_bench_voice_scope, capacity);
    queue.prepare(num_voices);

    uint32_t callback_count = 0;
    const auto cb = [&](const InputEventQueue::Event&, uint32_t) { ++callback_count; };

    for ([[maybe_unused]] auto _ : bm_state) {
        bm_state.PauseTiming();
        for (uint32_t v = 0; v < num_voices; ++v) {
            for (uint32_t i = 0; i < events_per_voice; ++i) {
                queue.push_voice_value(v, static_cast<float>(i) / events_per_voice);
            }
        }
        callback_count = 0;
        bm_state.ResumeTiming();

        size_t n = queue.drain_spread(block_size, cb);
        benchmark::DoNotOptimize(n);
        benchmark::DoNotOptimize(callback_count);
    }

    bm_state.SetItemsProcessed(static_cast<int64_t>(bm_state.iterations()) *
                               static_cast<int64_t>(events_per_voice) *
                               static_cast<int64_t>(num_voices));
}

// Typical touch-and-drag densities: 1-8 events per voice, 1 / 4 / 10 voices.
BENCHMARK(bm_input_event_queue_drain_spread)
    ->Args({1, 1})    // single tap, mono-touch.
    ->Args({4, 1})    // rapid move, mono-touch.
    ->Args({1, 10})   // 10-voice simultaneous trigger (strum-fix case).
    ->Args({4, 4})    // moderate multi-touch drag.
    ->Args({8, 10});  // worst-case: dense drag across all voices.

// =============================================================================
// Main
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::benchmark::Initialize(&argc, argv);
    ::benchmark::RunSpecifiedBenchmarks();
    ::benchmark::Shutdown();
    return 0;
}
