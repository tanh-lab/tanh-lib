#include <gtest/gtest.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/SmartHandle.h>
#include <tanh/state/State.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "TestHelpers.h"

using namespace thl::modulation;
using namespace std::chrono_literals;

namespace {
// Keeps the compiler from folding away SmartHandle::load() calls in the
// audio-thread loop — without an observable side effect the whole read loop
// could be elided and the race would never trigger.
std::atomic<float> g_sink{0.0f};
inline void sink(float v) {
    g_sink.store(v, std::memory_order_relaxed);
}
}  // namespace

// Concurrent stress: exercises the rebuild/publish race that the atomic-pointer
// refactor fixes. An "audio" thread loops opening a ReadScope and calling
// process_with_scope + SmartHandle::load while a "writer" thread concurrently
// mutates routings and re-prepares the matrix with varying block sizes. Under
// the old design, the audio thread could dereference m_voice/m_mono mid-rebuild
// and crash; under the new design those pointers are only swapped after a
// synchronize() barrier, so readers always see either the old or the new
// buffer — never a torn or reclaimed one.
TEST(ConcurrentRebuild, NoNullDerefUnderConcurrentRoutingChurn) {
    thl::State state;
    const int num_params = 8;
    std::vector<std::string> param_keys;
    param_keys.reserve(num_params);
    for (int i = 0; i < num_params; ++i) {
        std::string key = "p" + std::to_string(i);
        state.create(key, modulatable_float(0.5f));
        param_keys.push_back(std::move(key));
    }

    ModulationMatrix matrix(state);

    TestLFOSource lfo_a;
    lfo_a.m_frequency = 3.0f;
    TestLFOSource lfo_b;
    lfo_b.m_frequency = 7.5f;
    matrix.add_source("lfo_a", &lfo_a);
    matrix.add_source("lfo_b", &lfo_b);

    std::vector<SmartHandle<float>> handles;
    handles.reserve(num_params);
    for (const auto& k : param_keys) { handles.push_back(matrix.get_smart_handle<float>(k)); }

    matrix.prepare(k_sample_rate, k_block_size);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> audio_iterations{0};
    std::atomic<uint64_t> writer_iterations{0};

    std::thread audio_thread([&]() {
        matrix.ensure_thread_registered();
        while (!stop.load(std::memory_order_relaxed)) {
            auto scope = matrix.read_scope();
            matrix.process_with_scope(scope.data(), k_block_size);

            // Reads the atomic buffer pointers inside the scope — the retired
            // buffers from any in-flight rebuild are guaranteed live until
            // this scope closes and the writer's synchronize() completes.
            float acc = 0.0f;
            for (const auto& h : handles) { acc += h.load(0); }
            sink(acc);
            audio_iterations.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread writer_thread([&]() {
        uint32_t seed = 1;
        std::vector<uint32_t> live_routing_ids;
        live_routing_ids.reserve(32);
        // add_routing() checks State::is_modulatable(), a nonblocking read.
        state.ensure_thread_registered();

        while (!stop.load(std::memory_order_relaxed)) {
            seed = seed * 1103515245U + 12345U;
            const int op = static_cast<int>((seed >> 16) % 3U);
            const int param_idx = static_cast<int>((seed >> 8) % num_params);
            const char* src = ((seed >> 4) & 1U) ? "lfo_a" : "lfo_b";

            switch (op) {
                case 0: {
                    ModulationRouting r(src, param_keys[param_idx], 0.25f);
                    const uint32_t id = matrix.add_routing(r);
                    if (id != k_invalid_routing_id) { live_routing_ids.push_back(id); }
                    break;
                }
                case 1: {
                    if (!live_routing_ids.empty()) {
                        const size_t which = (seed >> 1) % live_routing_ids.size();
                        matrix.remove_routing(live_routing_ids[which]);
                        live_routing_ids[which] = live_routing_ids.back();
                        live_routing_ids.pop_back();
                    }
                    break;
                }
                case 2: {
                    if (!live_routing_ids.empty()) {
                        const size_t which = (seed >> 2) % live_routing_ids.size();
                        matrix.update_routing_depth(live_routing_ids[which], 0.5f);
                    }
                    break;
                }
                default: break;
            }
            writer_iterations.fetch_add(1, std::memory_order_relaxed);
#ifndef _WIN32
            // Skipped on Windows: scheduler granularity (~1–15ms) makes this
            // sleep far longer than intended and starves the writer loop.
            std::this_thread::sleep_for(50us);
#endif
        }
    });

    std::this_thread::sleep_for(1500ms);
    stop.store(true, std::memory_order_relaxed);
    audio_thread.join();
    writer_thread.join();

    // Sanity: both threads made meaningful progress. Without this the test
    // could spuriously pass by never actually racing.
    EXPECT_GT(audio_iterations.load(), 100U);
    EXPECT_GT(writer_iterations.load(), 100U);
}

// Additional scenario: a single routing is repeatedly removed and re-added on
// the same (source, target) pair. Each add/remove cycle allocates and retires
// the target's MonoBuffers. Verifies that SmartHandle reads never observe a
// half-swapped pointer and that reclamation via synchronize() is actually
// happening (no unbounded heap growth would be a separate OOM detection).
TEST(ConcurrentRebuild, RepeatedAddRemoveSingleRouting) {
    thl::State state;
    state.create("freq", modulatable_float(440.0f));

    ModulationMatrix matrix(state);

    TestLFOSource lfo;
    lfo.m_frequency = 5.0f;
    matrix.add_source("lfo", &lfo);

    auto handle = matrix.get_smart_handle<float>("freq");
    matrix.prepare(k_sample_rate, k_block_size);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> audio_iterations{0};

    std::thread audio_thread([&]() {
        matrix.ensure_thread_registered();
        while (!stop.load(std::memory_order_relaxed)) {
            auto scope = matrix.read_scope();
            matrix.process_with_scope(scope.data(), k_block_size);
            const float v = handle.load(0);
            sink(v);
            audio_iterations.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread writer_thread([&]() {
        state.ensure_thread_registered();
        while (!stop.load(std::memory_order_relaxed)) {
            const uint32_t id = matrix.add_routing({"lfo", "freq", 50.0f});
            if (id != k_invalid_routing_id) { matrix.remove_routing(id); }
        }
    });

    std::this_thread::sleep_for(1000ms);
    stop.store(true, std::memory_order_relaxed);
    audio_thread.join();
    writer_thread.join();

    EXPECT_GT(audio_iterations.load(), 100U);
}

// Regression: a second Replace routing landing on a polyphonic target flips the
// target's m_has_replace_priority flag false → true, and the fresh VoiceBuffers
// carrying that flag is published one step *before* the new ProcessingConfig. An
// audio block still running the old config then loads the new buffer, sees a
// non-null priority_buf, and takes the multi-Replace branch inside
// apply_replace_sample_voice — indexing per-voice freshness vectors that its own
// rebuild only sized when the target was already contended.
//
// That was a null deref on the audio thread (SIGSEGV at 0x0, seen on device via
// mapping randomize / preset load, which churn many routings per call). The
// vectors are now sized for every poly Replace routing, so the branch is safe
// whichever buffer the block happens to load.
TEST(ConcurrentRebuild, PolyReplaceContentionChurnDoesNotCrash) {
    thl::State state;
    ModulationMatrix matrix(state);
    const auto voice_scope = matrix.register_scope("voice", 4);
    state.create("freq", modulatable_float(0.5f, voice_scope));

    PolyTestSource poly_a(voice_scope);
    poly_a.m_voice_values = {0.9f, 0.8f, 0.7f, 0.6f};
    PolyTestSource poly_b(voice_scope);
    poly_b.m_voice_values = {0.1f, 0.2f, 0.3f, 0.4f};
    matrix.add_source("poly_a", &poly_a);
    matrix.add_source("poly_b", &poly_b);

    auto handle = matrix.get_smart_handle<float>("freq");

    // The resident Replace routing. It is resolved while it is the *only*
    // Replace writer on the target, so it is the one whose freshness vectors
    // used to go unsized.
    ModulationRouting resident{"poly_a",
                               "freq",
                               1.0f,
                               0,
                               DepthMode::Absolute,
                               CombineMode::Replace};
    resident.m_replace_priority = 1;
    ASSERT_NE(matrix.add_routing(resident), k_invalid_routing_id);

    matrix.prepare(k_sample_rate, k_block_size);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> audio_iterations{0};
    std::atomic<uint64_t> writer_iterations{0};

    std::thread audio_thread([&]() {
        matrix.ensure_thread_registered();
        while (!stop.load(std::memory_order_relaxed)) {
            auto scope = matrix.read_scope();
            matrix.process_with_scope(scope.data(), k_block_size);
            float acc = 0.0f;
            for (uint32_t v = 0; v < 4; ++v) { acc += handle.load(0, v); }
            sink(acc);
            audio_iterations.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Adds and removes a *second* Replace routing on the same target, so the
    // target oscillates between single-Replace (fast path, priority_buf null)
    // and multi-Replace (priority_buf allocated) on every writer iteration.
    std::thread writer_thread([&]() {
        state.ensure_thread_registered();
        ModulationRouting contender{"poly_b",
                                    "freq",
                                    1.0f,
                                    0,
                                    DepthMode::Absolute,
                                    CombineMode::Replace};
        contender.m_replace_priority = 2;
        while (!stop.load(std::memory_order_relaxed)) {
            const uint32_t id = matrix.add_routing(contender);
            if (id != k_invalid_routing_id) { matrix.remove_routing(id); }
            writer_iterations.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::this_thread::sleep_for(1500ms);
    stop.store(true, std::memory_order_relaxed);
    audio_thread.join();
    writer_thread.join();

    EXPECT_GT(audio_iterations.load(), 100U);
    EXPECT_GT(writer_iterations.load(), 100U);
}
