#include "tanh/state/State.h"
#include <benchmark/benchmark.h>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

using namespace thl;

// =============================================================================
// Setter Benchmarks
// =============================================================================

static void BM_SetDouble(benchmark::State& bm_state) {
    State state;
    state.set("param", 0.0);
    double v = 0.0;
    for (auto _ : bm_state) {
        state.set("param", v, NotifyStrategies::None);
        v += 0.001;
    }
}
BENCHMARK(BM_SetDouble);

static void BM_SetFloat(benchmark::State& bm_state) {
    State state;
    state.set("param", 0.0f);
    float v = 0.0f;
    for (auto _ : bm_state) {
        state.set("param", v, NotifyStrategies::None);
        v += 0.001f;
    }
}
BENCHMARK(BM_SetFloat);

static void BM_SetInt(benchmark::State& bm_state) {
    State state;
    state.set("param", 0);
    int v = 0;
    for (auto _ : bm_state) {
        state.set("param", v, NotifyStrategies::None);
        ++v;
    }
}
BENCHMARK(BM_SetInt);

static void BM_SetBool(benchmark::State& bm_state) {
    State state;
    state.set("param", false);
    bool v = false;
    for (auto _ : bm_state) {
        state.set("param", v, NotifyStrategies::None);
        v = !v;
    }
}
BENCHMARK(BM_SetBool);

static void BM_SetStringShort(benchmark::State& bm_state) {
    State state;
    state.set("param", "hello");
    for (auto _ : bm_state) {
        state.set("param", "world", NotifyStrategies::None);
    }
}
BENCHMARK(BM_SetStringShort);

static void BM_SetStringLong(benchmark::State& bm_state) {
    State state;
    std::string long_str(1000, 'a');
    state.set("param", long_str);
    for (auto _ : bm_state) {
        state.set("param", long_str, NotifyStrategies::None);
    }
}
BENCHMARK(BM_SetStringLong);

// =============================================================================
// Setter with Notification Benchmarks
// =============================================================================

static void BM_SetDoubleWithNotify(benchmark::State& bm_state) {
    State state;
    state.set("param", 0.0);
    double v = 0.0;
    for (auto _ : bm_state) {
        state.set("param", v, NotifyStrategies::All);
        v += 0.001;
    }
}
BENCHMARK(BM_SetDoubleWithNotify);

static void BM_SetDoubleWithListenerNotify(benchmark::State& bm_state) {
    State state;
    state.set("param", 0.0);
    int count = 0;
    state.add_callback_listener([&](std::string_view, const Parameter&) {
        benchmark::DoNotOptimize(++count);
    });
    double v = 0.0;
    for (auto _ : bm_state) {
        state.set("param", v, NotifyStrategies::All);
        v += 0.001;
    }
}
BENCHMARK(BM_SetDoubleWithListenerNotify);

// =============================================================================
// Getter Benchmarks
// =============================================================================

static void BM_GetDouble(benchmark::State& bm_state) {
    State state;
    state.set("param", 3.14159);
    for (auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get<double>("param"));
    }
}
BENCHMARK(BM_GetDouble);

static void BM_GetFloat(benchmark::State& bm_state) {
    State state;
    state.set("param", 2.71828f);
    for (auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get<float>("param"));
    }
}
BENCHMARK(BM_GetFloat);

static void BM_GetInt(benchmark::State& bm_state) {
    State state;
    state.set("param", 42);
    for (auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get<int>("param"));
    }
}
BENCHMARK(BM_GetInt);

static void BM_GetBool(benchmark::State& bm_state) {
    State state;
    state.set("param", true);
    for (auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get<bool>("param"));
    }
}
BENCHMARK(BM_GetBool);

static void BM_GetStringShort(benchmark::State& bm_state) {
    State state;
    state.set("param", "hello world");
    for (auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get<std::string>("param", true));
    }
}
BENCHMARK(BM_GetStringShort);

static void BM_GetStringLong(benchmark::State& bm_state) {
    State state;
    state.set("param", std::string(1000, 'a'));
    for (auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get<std::string>("param", true));
    }
}
BENCHMARK(BM_GetStringLong);

// =============================================================================
// Hierarchical Path Benchmarks
// =============================================================================

static void BM_SetHierarchical_Depth1(benchmark::State& bm_state) {
    State state;
    state.set("audio.volume", 0.5);
    double v = 0.0;
    for (auto _ : bm_state) {
        state.set("audio.volume", v, NotifyStrategies::None);
        v += 0.001;
    }
}
BENCHMARK(BM_SetHierarchical_Depth1);

static void BM_SetHierarchical_Depth3(benchmark::State& bm_state) {
    State state;
    state.set("audio.effects.reverb.mix", 0.5);
    double v = 0.0;
    for (auto _ : bm_state) {
        state.set("audio.effects.reverb.mix", v, NotifyStrategies::None);
        v += 0.001;
    }
}
BENCHMARK(BM_SetHierarchical_Depth3);

static void BM_GetHierarchical_Depth1(benchmark::State& bm_state) {
    State state;
    state.set("audio.volume", 0.75);
    for (auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get<double>("audio.volume"));
    }
}
BENCHMARK(BM_GetHierarchical_Depth1);

static void BM_GetHierarchical_Depth3(benchmark::State& bm_state) {
    State state;
    state.set("audio.effects.reverb.mix", 0.3);
    for (auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get<double>("audio.effects.reverb.mix"));
    }
}
BENCHMARK(BM_GetHierarchical_Depth3);

// =============================================================================
// Root vs Hierarchical Access Benchmarks
// =============================================================================

static void BM_SetInRoot(benchmark::State& bm_state) {
    State state;
    state.set("volume", 0.0);
    double v = 0.0;
    for (auto _ : bm_state) {
        state.set_in_root("volume", v, NotifyStrategies::None);
        v += 0.001;
    }
}
BENCHMARK(BM_SetInRoot);

static void BM_GetFromRoot(benchmark::State& bm_state) {
    State state;
    state.set("volume", 3.14);
    for (auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get_from_root<double>("volume"));
    }
}
BENCHMARK(BM_GetFromRoot);

// =============================================================================
// Parameter Creation Benchmarks
// =============================================================================

static void BM_CreateParameter(benchmark::State& bm_state) {
    int i = 0;
    for (auto _ : bm_state) {
        bm_state.PauseTiming();
        State state;
        std::string key = "param_" + std::to_string(i++);
        bm_state.ResumeTiming();

        state.set(key, 42.0);
    }
}
BENCHMARK(BM_CreateParameter);

static void BM_CreateHierarchicalParameter(benchmark::State& bm_state) {
    int i = 0;
    for (auto _ : bm_state) {
        bm_state.PauseTiming();
        State state;
        std::string key = "group.sub.param_" + std::to_string(i++);
        bm_state.ResumeTiming();

        state.set(key, 42.0);
    }
}
BENCHMARK(BM_CreateHierarchicalParameter);

// =============================================================================
// Parameter Type Query Benchmarks
// =============================================================================

static void BM_GetParameterType(benchmark::State& bm_state) {
    State state;
    state.set("param", 42.0);
    for (auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get_parameter_type("param"));
    }
}
BENCHMARK(BM_GetParameterType);

static void BM_GetParameterTypeHierarchical(benchmark::State& bm_state) {
    State state;
    state.set("audio.effects.reverb.mix", 0.5);
    for (auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get_parameter_type("audio.effects.reverb.mix"));
    }
}
BENCHMARK(BM_GetParameterTypeHierarchical);

// =============================================================================
// Many Parameters Scaling Benchmarks
// =============================================================================

static void BM_GetWithManyParameters(benchmark::State& bm_state) {
    State state;
    const int num_params = bm_state.range(0);
    for (int i = 0; i < num_params; ++i) {
        state.set("param_" + std::to_string(i), static_cast<double>(i));
    }
    // Benchmark getting the last parameter
    std::string target = "param_" + std::to_string(num_params - 1);
    for (auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get<double>(target));
    }
}
BENCHMARK(BM_GetWithManyParameters)->Range(1, 1024);

static void BM_SetWithManyParameters(benchmark::State& bm_state) {
    State state;
    const int num_params = bm_state.range(0);
    for (int i = 0; i < num_params; ++i) {
        state.set("param_" + std::to_string(i), static_cast<double>(i));
    }
    std::string target = "param_" + std::to_string(num_params - 1);
    double v = 0.0;
    for (auto _ : bm_state) {
        state.set(target, v, NotifyStrategies::None);
        v += 0.001;
    }
}
BENCHMARK(BM_SetWithManyParameters)->Range(1, 1024);

// =============================================================================
// JSON Update Benchmarks
// =============================================================================

static void BM_UpdateFromJsonSimple(benchmark::State& bm_state) {
    State state;
    state.set("volume", 0.5);
    state.set("muted", false);
    nlohmann::json update = {{"volume", 0.8}, {"muted", true}};
    for (auto _ : bm_state) {
        state.update_from_json(update, NotifyStrategies::None);
    }
}
BENCHMARK(BM_UpdateFromJsonSimple);

static void BM_UpdateFromJsonNested(benchmark::State& bm_state) {
    State state;
    state.set("audio.effects.reverb.wet", 0.3);
    state.set("audio.effects.reverb.dry", 0.7);
    state.set("audio.effects.reverb.room_size", 0.5);
    nlohmann::json update = {
        {"audio", {{"effects", {{"reverb", {{"wet", 0.4}, {"dry", 0.6}, {"room_size", 0.8}}}}}}}};
    for (auto _ : bm_state) {
        state.update_from_json(update, NotifyStrategies::None);
    }
}
BENCHMARK(BM_UpdateFromJsonNested);

// =============================================================================
// Group Operations Benchmarks
// =============================================================================

static void BM_HasGroup(benchmark::State& bm_state) {
    State state;
    state.create_group("audio");
    for (auto _ : bm_state) {
        benchmark::DoNotOptimize(state.has_group("audio"));
    }
}
BENCHMARK(BM_HasGroup);

static void BM_GetGroup(benchmark::State& bm_state) {
    State state;
    state.create_group("audio");
    for (auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get_group("audio"));
    }
}
BENCHMARK(BM_GetGroup);

static void BM_IsEmpty(benchmark::State& bm_state) {
    State state;
    state.set("param", 42.0);
    for (auto _ : bm_state) {
        benchmark::DoNotOptimize(state.is_empty());
    }
}
BENCHMARK(BM_IsEmpty);

// =============================================================================
// Multi-threaded Read Benchmarks
// NOTE: Uses manual threading to avoid deadlock in RCU thread-local cleanup
// that occurs with benchmark::State::Threads(). Google Benchmark's worker
// threads trigger RCU::ThreadRCUState destruction on exit, which can spin
// indefinitely waiting for read_generation to reach 0.
// =============================================================================

static void BM_ConcurrentReads(benchmark::State& bm_state) {
    const int num_threads = bm_state.range(0);
    State state;
    state.set("param", 3.14159);

    for (auto _ : bm_state) {
        std::atomic<bool> go{false};
        std::atomic<int> ops{0};
        const int reads_per_thread = 10000;

        std::vector<std::thread> threads;
        threads.reserve(num_threads);
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&] {
                state.ensure_thread_registered();
                while (!go.load(std::memory_order_acquire)) {}
                for (int i = 0; i < reads_per_thread; ++i) {
                    benchmark::DoNotOptimize(state.get<double>("param"));
                }
                ops.fetch_add(reads_per_thread, std::memory_order_relaxed);
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& t : threads) { t.join(); }
        bm_state.SetItemsProcessed(ops.load(std::memory_order_relaxed));
    }
}
BENCHMARK(BM_ConcurrentReads)->Arg(1)->Arg(2)->Arg(4)->Arg(8);

static void BM_ConcurrentHierarchicalReads(benchmark::State& bm_state) {
    const int num_threads = bm_state.range(0);
    State state;
    state.set("audio.effects.reverb.mix", 0.5);

    for (auto _ : bm_state) {
        std::atomic<bool> go{false};
        std::atomic<int> ops{0};
        const int reads_per_thread = 10000;

        std::vector<std::thread> threads;
        threads.reserve(num_threads);
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&] {
                state.ensure_thread_registered();
                while (!go.load(std::memory_order_acquire)) {}
                for (int i = 0; i < reads_per_thread; ++i) {
                    benchmark::DoNotOptimize(state.get<double>("audio.effects.reverb.mix"));
                }
                ops.fetch_add(reads_per_thread, std::memory_order_relaxed);
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& t : threads) { t.join(); }
        bm_state.SetItemsProcessed(ops.load(std::memory_order_relaxed));
    }
}
BENCHMARK(BM_ConcurrentHierarchicalReads)->Arg(1)->Arg(2)->Arg(4)->Arg(8);

// =============================================================================
// ParameterHandle Benchmarks
// =============================================================================

static void BM_HandleLoadDouble(benchmark::State& bm_state) {
    State state;
    state.set("param", 3.14159);
    auto handle = state.get_handle<double>("param");
    for (auto _ : bm_state) {
        benchmark::DoNotOptimize(handle.load());
    }
}
BENCHMARK(BM_HandleLoadDouble);

static void BM_HandleLoadFloat(benchmark::State& bm_state) {
    State state;
    state.set("param", 2.71828f);
    auto handle = state.get_handle<float>("param");
    for (auto _ : bm_state) {
        benchmark::DoNotOptimize(handle.load());
    }
}
BENCHMARK(BM_HandleLoadFloat);

static void BM_HandleLoadInt(benchmark::State& bm_state) {
    State state;
    state.set("param", 42);
    auto handle = state.get_handle<int>("param");
    for (auto _ : bm_state) {
        benchmark::DoNotOptimize(handle.load());
    }
}
BENCHMARK(BM_HandleLoadInt);

static void BM_HandleLoadBool(benchmark::State& bm_state) {
    State state;
    state.set("param", true);
    auto handle = state.get_handle<bool>("param");
    for (auto _ : bm_state) {
        benchmark::DoNotOptimize(handle.load());
    }
}
BENCHMARK(BM_HandleLoadBool);

static void BM_HandleStoreDouble(benchmark::State& bm_state) {
    State state;
    state.set("param", 0.0);
    auto handle = state.get_handle<double>("param");
    double v = 0.0;
    for (auto _ : bm_state) {
        handle.store(v);
        v += 0.001;
    }
}
BENCHMARK(BM_HandleStoreDouble);

static void BM_HandleVsGetFromRoot(benchmark::State& bm_state) {
    State state;
    state.set("param", 3.14159);
    auto handle = state.get_handle<double>("param");
    for (auto _ : bm_state) {
        benchmark::DoNotOptimize(handle.load());
    }
}
BENCHMARK(BM_HandleVsGetFromRoot);

static void BM_HandleConcurrentLoads(benchmark::State& bm_state) {
    const int num_threads = bm_state.range(0);
    State state;
    state.set("param", 3.14159);
    auto handle = state.get_handle<double>("param");

    for (auto _ : bm_state) {
        std::atomic<bool> go{false};
        std::atomic<int> ops{0};
        const int reads_per_thread = 10000;

        std::vector<std::thread> threads;
        threads.reserve(num_threads);
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&] {
                while (!go.load(std::memory_order_acquire)) {}
                for (int i = 0; i < reads_per_thread; ++i) {
                    benchmark::DoNotOptimize(handle.load());
                }
                ops.fetch_add(reads_per_thread, std::memory_order_relaxed);
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& t : threads) { t.join(); }
        bm_state.SetItemsProcessed(ops.load(std::memory_order_relaxed));
    }
}
BENCHMARK(BM_HandleConcurrentLoads)->Arg(1)->Arg(2)->Arg(4)->Arg(8);

// =============================================================================
// State Dump Benchmark
// =============================================================================

static void BM_GetStateDump(benchmark::State& bm_state) {
    State state;
    state.set("volume", 0.8);
    state.set("muted", false);
    state.set("audio.effects.reverb.mix", 0.5);
    state.set("audio.effects.delay.time", 500);
    state.set("visual.brightness", 75);
    for (auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get_state_dump());
    }
}
BENCHMARK(BM_GetStateDump);

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
