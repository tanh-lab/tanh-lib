#include "tanh/state/State.h"
#include <benchmark/benchmark.h>
#include <gtest/gtest.h>
#include <tanh/core/Numbers.h>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

using namespace thl;

// =============================================================================
// Setter Benchmarks
// =============================================================================

static void bm_set_double(benchmark::State& bm_state) {
    State state;
    state.set("param", 0.0);
    double v = 0.0;
    for ([[maybe_unused]] auto _ : bm_state) {
        state.set("param", v, NotifyStrategies::None);
        v += 0.001;
    }
}
BENCHMARK(bm_set_double);

static void bm_set_float(benchmark::State& bm_state) {
    State state;
    state.set("param", 0.0f);
    float v = 0.0f;
    for ([[maybe_unused]] auto _ : bm_state) {
        state.set("param", v, NotifyStrategies::None);
        v += 0.001f;
    }
}
BENCHMARK(bm_set_float);

static void bm_set_int(benchmark::State& bm_state) {
    State state;
    state.set("param", 0);
    int v = 0;
    for ([[maybe_unused]] auto _ : bm_state) {
        state.set("param", v, NotifyStrategies::None);
        ++v;
    }
}
BENCHMARK(bm_set_int);

static void bm_set_bool(benchmark::State& bm_state) {
    State state;
    state.set("param", false);
    bool v = false;
    for ([[maybe_unused]] auto _ : bm_state) {
        state.set("param", v, NotifyStrategies::None);
        v = !v;
    }
}
BENCHMARK(bm_set_bool);

static void bm_set_string_short(benchmark::State& bm_state) {
    State state;
    state.set("param", "hello");
    for ([[maybe_unused]] auto _ : bm_state) {
        state.set("param", "world", NotifyStrategies::None);
    }
}
BENCHMARK(bm_set_string_short);

static void bm_set_string_long(benchmark::State& bm_state) {
    State state;
    std::string long_str(1000, 'a');
    state.set("param", long_str);
    for ([[maybe_unused]] auto _ : bm_state) {
        state.set("param", long_str, NotifyStrategies::None);
    }
}
BENCHMARK(bm_set_string_long);

// =============================================================================
// Setter with Notification Benchmarks
// =============================================================================

static void bm_set_double_with_notify(benchmark::State& bm_state) {
    State state;
    state.set("param", 0.0);
    double v = 0.0;
    for ([[maybe_unused]] auto _ : bm_state) {
        state.set("param", v, NotifyStrategies::All);
        v += 0.001;
    }
}
BENCHMARK(bm_set_double_with_notify);

static void bm_set_double_with_listener_notify(benchmark::State& bm_state) {
    State state;
    state.set("param", 0.0);
    int count = 0;
    state.add_callback_listener(
        [&](std::string_view, const Parameter&) { benchmark::DoNotOptimize(++count); });
    double v = 0.0;
    for ([[maybe_unused]] auto _ : bm_state) {
        state.set("param", v, NotifyStrategies::All);
        v += 0.001;
    }
}
BENCHMARK(bm_set_double_with_listener_notify);

// =============================================================================
// Getter Benchmarks
// =============================================================================

static void bm_get_double(benchmark::State& bm_state) {
    State state;
    state.set("param", std::numbers::pi);
    for ([[maybe_unused]] auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get<double>("param"));
    }
}
BENCHMARK(bm_get_double);

static void bm_get_float(benchmark::State& bm_state) {
    State state;
    state.set("param", std::numbers::e_v<float>);
    for ([[maybe_unused]] auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get<float>("param"));
    }
}
BENCHMARK(bm_get_float);

static void bm_get_int(benchmark::State& bm_state) {
    State state;
    state.set("param", 42);
    for ([[maybe_unused]] auto _ : bm_state) { benchmark::DoNotOptimize(state.get<int>("param")); }
}
BENCHMARK(bm_get_int);

static void bm_get_bool(benchmark::State& bm_state) {
    State state;
    state.set("param", true);
    for ([[maybe_unused]] auto _ : bm_state) { benchmark::DoNotOptimize(state.get<bool>("param")); }
}
BENCHMARK(bm_get_bool);

static void bm_get_string_short(benchmark::State& bm_state) {
    State state;
    state.set("param", "hello world");
    for ([[maybe_unused]] auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get<std::string>("param", true));
    }
}
BENCHMARK(bm_get_string_short);

static void bm_get_string_long(benchmark::State& bm_state) {
    State state;
    state.set("param", std::string(1000, 'a'));
    for ([[maybe_unused]] auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get<std::string>("param", true));
    }
}
BENCHMARK(bm_get_string_long);

// =============================================================================
// Hierarchical Path Benchmarks
// =============================================================================

static void bm_set_hierarchical_depth1(benchmark::State& bm_state) {
    State state;
    state.set("audio.volume", 0.5);
    double v = 0.0;
    for ([[maybe_unused]] auto _ : bm_state) {
        state.set("audio.volume", v, NotifyStrategies::None);
        v += 0.001;
    }
}
BENCHMARK(bm_set_hierarchical_depth1);

static void bm_set_hierarchical_depth3(benchmark::State& bm_state) {
    State state;
    state.set("audio.effects.reverb.mix", 0.5);
    double v = 0.0;
    for ([[maybe_unused]] auto _ : bm_state) {
        state.set("audio.effects.reverb.mix", v, NotifyStrategies::None);
        v += 0.001;
    }
}
BENCHMARK(bm_set_hierarchical_depth3);

static void bm_get_hierarchical_depth1(benchmark::State& bm_state) {
    State state;
    state.set("audio.volume", 0.75);
    for ([[maybe_unused]] auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get<double>("audio.volume"));
    }
}
BENCHMARK(bm_get_hierarchical_depth1);

static void bm_get_hierarchical_depth3(benchmark::State& bm_state) {
    State state;
    state.set("audio.effects.reverb.mix", 0.3);
    for ([[maybe_unused]] auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get<double>("audio.effects.reverb.mix"));
    }
}
BENCHMARK(bm_get_hierarchical_depth3);

// =============================================================================
// Root vs Hierarchical Access Benchmarks
// =============================================================================

static void bm_set_in_root(benchmark::State& bm_state) {
    State state;
    state.set("volume", 0.0);
    double v = 0.0;
    for ([[maybe_unused]] auto _ : bm_state) {
        state.set_in_root("volume", v, NotifyStrategies::None);
        v += 0.001;
    }
}
BENCHMARK(bm_set_in_root);

static void bm_get_from_root(benchmark::State& bm_state) {
    State state;
    state.set("volume", 3.14);
    for ([[maybe_unused]] auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get_from_root<double>("volume"));
    }
}
BENCHMARK(bm_get_from_root);

// =============================================================================
// Parameter Creation Benchmarks
// =============================================================================

static void bm_create_parameter(benchmark::State& bm_state) {
    int i = 0;
    for ([[maybe_unused]] auto _ : bm_state) {
        bm_state.PauseTiming();
        State state;
        std::string key = "param_" + std::to_string(i++);
        bm_state.ResumeTiming();

        state.set(key, 42.0);
    }
}
BENCHMARK(bm_create_parameter);

static void bm_create_hierarchical_parameter(benchmark::State& bm_state) {
    int i = 0;
    for ([[maybe_unused]] auto _ : bm_state) {
        bm_state.PauseTiming();
        State state;
        std::string key = "group.sub.param_" + std::to_string(i++);
        bm_state.ResumeTiming();

        state.set(key, 42.0);
    }
}
BENCHMARK(bm_create_hierarchical_parameter);

// =============================================================================
// Parameter Type Query Benchmarks
// =============================================================================

static void bm_get_parameter_type(benchmark::State& bm_state) {
    State state;
    state.set("param", 42.0);
    for ([[maybe_unused]] auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get_parameter_type("param"));
    }
}
BENCHMARK(bm_get_parameter_type);

static void bm_get_parameter_type_hierarchical(benchmark::State& bm_state) {
    State state;
    state.set("audio.effects.reverb.mix", 0.5);
    for ([[maybe_unused]] auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get_parameter_type("audio.effects.reverb.mix"));
    }
}
BENCHMARK(bm_get_parameter_type_hierarchical);

// =============================================================================
// Many Parameters Scaling Benchmarks
// =============================================================================

static void bm_get_with_many_parameters(benchmark::State& bm_state) {
    State state;
    const auto num_params = bm_state.range(0);
    for (int64_t i = 0; i < num_params; ++i) {
        state.set("param_" + std::to_string(i), static_cast<double>(i));
    }
    // Benchmark getting the last parameter
    std::string target = "param_" + std::to_string(num_params - 1);
    for ([[maybe_unused]] auto _ : bm_state) {
        benchmark::DoNotOptimize(state.get<double>(target));
    }
}
BENCHMARK(bm_get_with_many_parameters)->Range(1, 1024);

static void bm_set_with_many_parameters(benchmark::State& bm_state) {
    State state;
    const auto num_params = bm_state.range(0);
    for (int64_t i = 0; i < num_params; ++i) {
        state.set("param_" + std::to_string(i), static_cast<double>(i));
    }
    std::string target = "param_" + std::to_string(num_params - 1);
    double v = 0.0;
    for ([[maybe_unused]] auto _ : bm_state) {
        state.set(target, v, NotifyStrategies::None);
        v += 0.001;
    }
}
BENCHMARK(bm_set_with_many_parameters)->Range(1, 1024);

// =============================================================================
// JSON Update Benchmarks
// =============================================================================

static void bm_update_from_json_simple(benchmark::State& bm_state) {
    State state;
    state.set("volume", 0.5);
    state.set("muted", false);
    nlohmann::json update = {{"volume", 0.8}, {"muted", true}};
    for ([[maybe_unused]] auto _ : bm_state) {
        state.update_from_json(update, NotifyStrategies::None);
    }
}
BENCHMARK(bm_update_from_json_simple);

static void bm_update_from_json_nested(benchmark::State& bm_state) {
    State state;
    state.set("audio.effects.reverb.wet", 0.3);
    state.set("audio.effects.reverb.dry", 0.7);
    state.set("audio.effects.reverb.room_size", 0.5);
    nlohmann::json update = {
        {"audio", {{"effects", {{"reverb", {{"wet", 0.4}, {"dry", 0.6}, {"room_size", 0.8}}}}}}}};
    for ([[maybe_unused]] auto _ : bm_state) {
        state.update_from_json(update, NotifyStrategies::None);
    }
}
BENCHMARK(bm_update_from_json_nested);

// =============================================================================
// Group Operations Benchmarks
// =============================================================================

static void bm_has_group(benchmark::State& bm_state) {
    State state;
    state.create_group("audio");
    for ([[maybe_unused]] auto _ : bm_state) { benchmark::DoNotOptimize(state.has_group("audio")); }
}
BENCHMARK(bm_has_group);

static void bm_get_group(benchmark::State& bm_state) {
    State state;
    state.create_group("audio");
    for ([[maybe_unused]] auto _ : bm_state) { benchmark::DoNotOptimize(state.get_group("audio")); }
}
BENCHMARK(bm_get_group);

static void bm_is_empty(benchmark::State& bm_state) {
    State state;
    state.set("param", 42.0);
    for ([[maybe_unused]] auto _ : bm_state) { benchmark::DoNotOptimize(state.is_empty()); }
}
BENCHMARK(bm_is_empty);

// =============================================================================
// Multi-threaded Read Benchmarks
// NOTE: Uses manual threading to avoid deadlock in RCU thread-local cleanup
// that occurs with benchmark::State::Threads(). Google Benchmark's worker
// threads trigger RCU::ThreadRCUState destruction on exit, which can spin
// indefinitely waiting for read_generation to reach 0.
// =============================================================================

static void bm_concurrent_reads(benchmark::State& bm_state) {
    const auto num_threads = bm_state.range(0);
    State state;
    state.set("param", std::numbers::pi);

    for ([[maybe_unused]] auto _ : bm_state) {
        std::atomic<bool> go{false};
        std::atomic<int> ops{0};
        const int reads_per_thread = 10000;

        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(num_threads));
        for (int64_t t = 0; t < num_threads; ++t) {
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
BENCHMARK(bm_concurrent_reads)->Arg(1)->Arg(2)->Arg(4)->Arg(8);

static void bm_concurrent_hierarchical_reads(benchmark::State& bm_state) {
    const auto num_threads = bm_state.range(0);
    State state;
    state.set("audio.effects.reverb.mix", 0.5);

    for ([[maybe_unused]] auto _ : bm_state) {
        std::atomic<bool> go{false};
        std::atomic<int> ops{0};
        const int reads_per_thread = 10000;

        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(num_threads));
        for (int64_t t = 0; t < num_threads; ++t) {
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
BENCHMARK(bm_concurrent_hierarchical_reads)->Arg(1)->Arg(2)->Arg(4)->Arg(8);

// =============================================================================
// ParameterHandle Benchmarks
// =============================================================================

static void bm_handle_load_double(benchmark::State& bm_state) {
    State state;
    state.set("param", std::numbers::pi);
    auto handle = state.get_handle<double>("param");
    for ([[maybe_unused]] auto _ : bm_state) { benchmark::DoNotOptimize(handle.load()); }
}
BENCHMARK(bm_handle_load_double);

static void bm_handle_load_float(benchmark::State& bm_state) {
    State state;
    state.set("param", std::numbers::e_v<float>);
    auto handle = state.get_handle<float>("param");
    for ([[maybe_unused]] auto _ : bm_state) { benchmark::DoNotOptimize(handle.load()); }
}
BENCHMARK(bm_handle_load_float);

static void bm_handle_load_int(benchmark::State& bm_state) {
    State state;
    state.set("param", 42);
    auto handle = state.get_handle<int>("param");
    for ([[maybe_unused]] auto _ : bm_state) { benchmark::DoNotOptimize(handle.load()); }
}
BENCHMARK(bm_handle_load_int);

static void bm_handle_load_bool(benchmark::State& bm_state) {
    State state;
    state.set("param", true);
    auto handle = state.get_handle<bool>("param");
    for ([[maybe_unused]] auto _ : bm_state) { benchmark::DoNotOptimize(handle.load()); }
}
BENCHMARK(bm_handle_load_bool);

static void bm_handle_store_double(benchmark::State& bm_state) {
    State state;
    state.set("param", 0.0);
    auto handle = state.get_handle<double>("param");
    double v = 0.0;
    for ([[maybe_unused]] auto _ : bm_state) {
        handle.store(v);
        v += 0.001;
    }
}
BENCHMARK(bm_handle_store_double);

static void bm_handle_vs_get_from_root(benchmark::State& bm_state) {
    State state;
    state.set("param", std::numbers::pi);
    auto handle = state.get_handle<double>("param");
    for ([[maybe_unused]] auto _ : bm_state) { benchmark::DoNotOptimize(handle.load()); }
}
BENCHMARK(bm_handle_vs_get_from_root);

static void bm_handle_concurrent_loads(benchmark::State& bm_state) {
    const auto num_threads = bm_state.range(0);
    State state;
    state.set("param", std::numbers::pi);
    auto handle = state.get_handle<double>("param");

    for ([[maybe_unused]] auto _ : bm_state) {
        std::atomic<bool> go{false};
        std::atomic<int> ops{0};
        const int reads_per_thread = 10000;

        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(num_threads));
        for (int64_t t = 0; t < num_threads; ++t) {
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
BENCHMARK(bm_handle_concurrent_loads)->Arg(1)->Arg(2)->Arg(4)->Arg(8);

// =============================================================================
// State Dump Benchmark
// =============================================================================

static void bm_get_state_dump(benchmark::State& bm_state) {
    State state;
    state.set("volume", 0.8);
    state.set("muted", false);
    state.set("audio.effects.reverb.mix", 0.5);
    state.set("audio.effects.delay.time", 500);
    state.set("visual.brightness", 75);
    for ([[maybe_unused]] auto _ : bm_state) { benchmark::DoNotOptimize(state.get_state_dump()); }
}
BENCHMARK(bm_get_state_dump);

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
