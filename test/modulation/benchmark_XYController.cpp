// XYController cost: 8 PerEffect controllers (motion playing on 6, 2 touched),
// alone and inside the matrix with the ElasticFX wiring (plus a Single
// controller with 8 voices and Single/PerEffect routings on 16 targets).

#include <benchmark/benchmark.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/XYController.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "XYControllerRig.h"

using namespace thl::modulation;

namespace {

MotionLane bench_lane(double beats) {
    MotionLane lane;
    lane.m_timebase = MotionTimebase::Beats;
    lane.m_length = beats;
    const int n = static_cast<int>(beats * 96);
    for (int i = 0; i < n; ++i) {
        const double ph = 2.0 * 3.14159265358979 * i / n;
        lane.m_x.push_back(static_cast<float>(0.5 + (0.4 * std::sin(ph))));
        lane.m_y.push_back(static_cast<float>(0.5 + (0.4 * std::cos(ph))));
        lane.m_gate.push_back(1);
    }
    return lane;
}

struct Bench {
    explicit Bench(uint32_t block) : router(*init_matrix()) {
        XYControllerConfig cfg;
        cfg.m_recorder.m_max_points = 8192;
        cfg.m_id = "single";
        cfg.m_num_voices = 8;
        single = std::make_unique<XYController>(*matrix, cfg);
        cfg.m_num_voices = 1;
        for (uint32_t n = 0; n < 8; ++n) {
            cfg.m_id = "pad" + std::to_string(n + 1);
            pads[n] = std::make_unique<XYController>(*matrix, cfg);
            const std::string s = "s" + std::to_string(n + 1);
            router.add_target(s + ".a", XYPadAxis::X, *pads[n], 0, *single, n);
            router.add_target(s + ".b", XYPadAxis::Y, *pads[n], 0, *single, n);
            pads[n]->route(XYPadAxis::Active, s + ".wet");
            if (n < 6) { pads[n]->recorder().load_lane(bench_lane(2.0 + static_cast<double>(n))); }
        }
        matrix->prepare(xy_test::k_sr, block);
        pads[6]->touch(1, 0.3f, 0.7f);
        pads[7]->touch(1, 0.8f, 0.2f);
        for (int b = 0; b < 8; ++b) { matrix_block(block); }  // publish lanes, settle
    }

    ModulationMatrix* init_matrix() {
        for (int n = 1; n <= 8; ++n) {
            const std::string s = "s" + std::to_string(n);
            backend.add(s + ".a", 0.0f);
            backend.add(s + ".b", 0.0f);
            backend.add(s + ".wet", 0.0f);
        }
        matrix = std::make_unique<ModulationMatrix>(backend);
        return matrix.get();
    }

    void matrix_block(uint32_t n) {
        const auto t = transport.next(n);
        single->set_transport(t);
        for (auto& p : pads) { p->set_transport(t); }
        matrix->process(n);
    }

    void controllers_block(uint32_t n) {
        const auto t = transport.next(n);
        for (auto& p : pads) { p->process_block(t); }
    }

    xy_test::FakeBackend backend;
    std::unique_ptr<ModulationMatrix> matrix;
    XYModeRouter router;
    std::unique_ptr<XYController> single;
    std::array<std::unique_ptr<XYController>, 8> pads;
    xy_test::SimTransport transport;
};

}  // namespace

// The 8 PerEffect controllers' driver steps alone (pad → recorder → buffers,
// frame publication), without the matrix.
static void bm_xy_8_controllers(benchmark::State& state) {
    const auto n = static_cast<uint32_t>(state.range(0));
    Bench b(n);
    for (auto _ : state) {
        b.controllers_block(n);
        benchmark::DoNotOptimize(b.pads[0]->out_x(0));
    }
}
BENCHMARK(bm_xy_8_controllers)->Arg(32)->Arg(128)->Arg(256)->Arg(512);

// Full matrix: 9 controllers (8 PerEffect + Single with 8 voices), 24 targets,
// 40 routings (16 targets with two Replace writers).
static void bm_xy_matrix_elasticfx(benchmark::State& state) {
    const auto n = static_cast<uint32_t>(state.range(0));
    Bench b(n);
    for (auto _ : state) {
        b.matrix_block(n);
        benchmark::DoNotOptimize(b.pads[0]->out_x(0));
    }
}
BENCHMARK(bm_xy_matrix_elasticfx)->Arg(32)->Arg(128)->Arg(256)->Arg(512);
