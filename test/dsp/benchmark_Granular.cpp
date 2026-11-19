// Granular voice hot paths. Worst case for the grain engine is the pool
// near full: Density 1 (100 grains/s) x Size 1 (400 ms) = ~40 active grains
// per voice, every one read + windowed + panned per frame.

#include <benchmark/benchmark.h>
#include <tanh/core/Buffer.h>
#include <tanh/dsp/audio/AudioDataStore.h>
#include <tanh/dsp/granular/ChannelMixer.h>
#include <tanh/dsp/granular/GrainEngine.h>
#include <tanh/dsp/granular/GrainVisualizer.h>
#include <tanh/dsp/granular/GranularTypes.h>
#include <tanh/dsp/sampler/SamplePlayer.h>
#include <tanh/dsp/sampler/SampleView.h>

#include <array>
#include <cstddef>
#include <vector>

using namespace thl::dsp::granular;

namespace {

constexpr double k_sample_rate = 48000.0;
constexpr size_t k_block = 512;

struct Rig {
    thl::dsp::audio::AudioDataStore m_store;
    std::vector<thl::dsp::sampler::SampleView> m_views;
    std::array<std::vector<float>, 2> m_data{std::vector<float>(k_block),
                                             std::vector<float>(k_block)};
    std::array<std::vector<float>, 2> m_head{std::vector<float>(k_block),
                                             std::vector<float>(k_block)};
    std::array<float*, 2> m_out{};
    std::array<float*, 2> m_head_out{};

    explicit Rig(size_t source_channels) {
        auto& banks = m_store.begin_load();
        thl::core::BufferF bank(source_channels, 96000, k_sample_rate);
        for (size_t ch = 0; ch < source_channels; ++ch) {
            float* p = bank.get_write_pointer(ch);
            for (size_t i = 0; i < 96000; ++i) { p[i] = static_cast<float>(i % 100) * 0.01f; }
        }
        banks.push_back(std::move(bank));
        m_store.commit_load(0);
        for (const auto& b : m_store.get_buffer()) {
            m_views.push_back(thl::dsp::sampler::SampleView::of(b));
        }
        m_out = {m_data[0].data(), m_data[1].data()};
        m_head_out = {m_head[0].data(), m_head[1].data()};
    }
};

void BM_GrainEngineRender(benchmark::State& state) {
    auto const mode = static_cast<ChannelMode>(state.range(0));
    Rig rig(2);
    GrainEngine engine;
    engine.prepare(k_sample_rate, 2);
    engine.seed(1);
    engine.reset_schedule(HeadMode::Scan);

    GrainParams params;
    params.m_channel_mode = mode;
    params.m_density = 1.0f;
    params.m_size = 1.0f;
    params.m_spread = 0.5f;

    size_t elapsed = 0;
    for (int i = 0; i < 100; ++i) {  // fill the pool
        engine
            .render(rig.m_views, 0, params, HeadMode::Scan, rig.m_out.data(), 2, k_block, elapsed);
        elapsed += k_block;
    }
    for ([[maybe_unused]] auto _ : state) {
        engine
            .render(rig.m_views, 0, params, HeadMode::Scan, rig.m_out.data(), 2, k_block, elapsed);
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
    // Short loop so roughly a quarter of all frames run a crossfade; the head
    // then goes through the voice's TrueStereo mix.
    Rig rig(2);
    thl::dsp::sampler::SamplePlayer player;
    player.prepare(k_sample_rate);
    player.set_markers(0.0f, 2000.0f / 96000.0f, 0.0f);
    player.note_on();
    for ([[maybe_unused]] auto _ : state) {
        player.render(rig.m_views, 0, rig.m_head_out.data(), 2, k_block);
        channel_mixer::mix_head(rig.m_head_out.data(),
                                2,
                                ChannelMode::TrueStereo,
                                0.5f,
                                rig.m_out.data(),
                                2,
                                k_block);
        benchmark::DoNotOptimize(rig.m_data[0][0]);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * k_block);
}
BENCHMARK(BM_SamplePlayerRender)->Unit(benchmark::kMicrosecond);

}  // namespace

BENCHMARK_MAIN();
