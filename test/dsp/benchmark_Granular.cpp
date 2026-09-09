// Granular voice hot paths. Worst case for the grain engine is the pool
// near full: Density 1 (100 grains/s) x Size 1 (400 ms) = ~40 active grains
// per voice, every one read + windowed + panned per frame.

#include <benchmark/benchmark.h>
#include <tanh/core/Buffer.h>
#include <tanh/dsp/audio/AudioDataStore.h>
#include <tanh/dsp/granular/GrainEngine.h>
#include <tanh/dsp/granular/GrainVisualizer.h>
#include <tanh/dsp/granular/GranularTypes.h>
#include <tanh/dsp/granular/SamplePlayer.h>
#include <tanh/dsp/granular/SampleReader.h>
#include <tanh/dsp/granular/VoiceParams.h>

#include <array>
#include <cstddef>
#include <vector>

using namespace thl::dsp::granular;

namespace {

constexpr double k_sample_rate = 48000.0;
constexpr size_t k_block = 512;

struct Rig {
    thl::dsp::audio::AudioDataStore m_store;
    SampleReader m_reader{m_store};
    GrainVisualizer m_viz;
    std::array<std::vector<float>, 2> m_data{std::vector<float>(k_block),
                                             std::vector<float>(k_block)};
    AudioBlock m_block;

    explicit Rig(size_t source_channels) {
        auto& banks = m_store.begin_load();
        thl::core::BufferF bank(source_channels, 96000, k_sample_rate);
        for (size_t ch = 0; ch < source_channels; ++ch) {
            float* p = bank.get_write_pointer(ch);
            for (size_t i = 0; i < 96000; ++i) { p[i] = static_cast<float>(i % 100) * 0.01f; }
        }
        banks.push_back(std::move(bank));
        m_store.commit_load(0);
        m_block.m_num_channels = 2;
        m_block.m_num_frames = k_block;
        m_block.m_channels[0] = m_data[0].data();
        m_block.m_channels[1] = m_data[1].data();
    }
};

void BM_GrainEngineRender(benchmark::State& state) {
    auto const mode = static_cast<ChannelMode>(state.range(0));
    Rig rig(2);
    GrainEngine engine(rig.m_reader, rig.m_viz);
    engine.prepare(k_sample_rate, 2);
    engine.seed(1);
    engine.reset_schedule(EngineMode::GranularLoop);

    VoiceParams params;
    params.m_channel_mode = mode;
    params.m_density = 1.0f;
    params.m_size = 1.0f;
    params.m_spread = 0.5f;

    size_t elapsed = 0;
    for (int i = 0; i < 100; ++i) {  // fill the pool
        engine.render(rig.m_block, params, EngineMode::GranularLoop, elapsed);
        elapsed += k_block;
    }
    for (auto _ : state) {
        engine.render(rig.m_block, params, EngineMode::GranularLoop, elapsed);
        elapsed += k_block;
        benchmark::DoNotOptimize(rig.m_data[0][0]);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * k_block);
}
BENCHMARK(BM_GrainEngineRender)
    ->Arg(static_cast<int>(ChannelMode::MonoToStereo))
    ->Arg(static_cast<int>(ChannelMode::TrueStereo))
    ->Arg(static_cast<int>(ChannelMode::TrueMultichannel))
    ->Unit(benchmark::kMicrosecond);

void BM_SamplePlayerRender(benchmark::State& state) {
    // Short loop so roughly a quarter of all frames run a crossfade.
    Rig rig(2);
    SamplePlayer player(rig.m_reader, rig.m_viz);
    player.prepare(k_sample_rate, 2);
    VoiceParams params;
    params.m_engine_mode = EngineMode::Sample;
    params.m_channel_mode = ChannelMode::TrueStereo;
    params.m_sample_end = 2000.0f / 96000.0f;
    params.m_spread = 0.5f;
    player.note_on();
    for (auto _ : state) {
        player.render(rig.m_block, params);
        benchmark::DoNotOptimize(rig.m_data[0][0]);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * k_block);
}
BENCHMARK(BM_SamplePlayerRender)->Unit(benchmark::kMicrosecond);

}  // namespace

BENCHMARK_MAIN();
