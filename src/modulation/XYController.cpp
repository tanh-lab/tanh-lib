#include "tanh/modulation/XYController.h"

#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/ModulationRouting.h>
#include <tanh/modulation/ModulationSource.h>
#include <tanh/modulation/MotionRecorder.h>
#include <tanh/modulation/detail/XYPad.h>
#include <tanh/state/ModulationScope.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <time.h>  // NOLINT(modernize-deprecated-headers): clock_gettime_nsec_np
#else
#include <chrono>
#endif

namespace thl::modulation {

using thl::dsp::transport::TransportInfo;

namespace {

uint64_t pack_position(float x, float y) {
    return (static_cast<uint64_t>(std::bit_cast<uint32_t>(x)) << 32) |
           static_cast<uint64_t>(std::bit_cast<uint32_t>(y));
}

std::pair<float, float> unpack_position(uint64_t packed) {
    return {std::bit_cast<float>(static_cast<uint32_t>(packed >> 32)),
            std::bit_cast<float>(static_cast<uint32_t>(packed & 0xFFFFFFFFu))};
}

int64_t pad_time(std::optional<int64_t> time_ns) {
    return time_ns.value_or(detail::k_xy_pad_untimed);
}

uint64_t hash_combine(uint64_t hash, uint64_t value) {
    hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
    return hash;
}

}  // namespace

// One matrix source of a voice. The first source the matrix reaches in a block
// runs the whole controller; process() only copies the voice's buffers.
class XYController::Output final : public ModulationSource {
public:
    Output(XYController& controller, uint32_t voice, XYPadAxis axis)
        : ModulationSource(k_global_scope, /*fully_active=*/false)
        , m_controller(controller)
        , m_voice(voice)
        , m_axis(axis) {}

    void prepare(double sample_rate, size_t samples_per_block, uint32_t voice_count) override {
        resize_buffers(samples_per_block, voice_count);
        // One source prepares the controller: the controller registers voice 0's
        // x first, before any of its sources is visible to the audio thread.
        if (m_voice == 0 && m_axis == XYPadAxis::X) {
            m_controller.prepare(sample_rate, samples_per_block);
        }
    }

    // The matrix calls every clear_per_block() before the first pre_process_block().
    void clear_per_block() override {
        ModulationSource::clear_per_block();
        m_controller.m_matrix_block_pending = true;
    }

    void pre_process_block() override { m_controller.run_for_matrix(); }

    void process(size_t num_samples, size_t offset) override;

private:
    XYController& m_controller;
    const uint32_t m_voice;
    const XYPadAxis m_axis;
};

struct XYController::Voice {
    Voice(XYController& owner, uint32_t index, const XYControllerConfig& config)
        : m_pad(config.m_max_touches, config.m_mono_priority)
        , m_out_x(owner, index, XYPadAxis::X)
        , m_out_y(owner, index, XYPadAxis::Y)
        , m_out_active(owner, index, XYPadAxis::Active) {}

    detail::XYPad m_pad;
    MotionRecorder m_recorder;

    // Audio thread: block buffers.
    std::vector<uint8_t> m_active;
    std::vector<uint8_t> m_xy_mask;
    std::vector<XYLayer> m_layer;
    std::vector<uint32_t> m_change_points;
    size_t m_num_change_points = 0;
    bool m_has_value = false;
    bool m_latched = false;
    uint32_t m_trail_countdown = 0;

    // Shared: x / y at block end for touch selection, and the enabled flag.
    std::atomic<uint64_t> m_position{0};
    std::atomic<bool> m_enabled{true};

    Output m_out_x;
    Output m_out_y;
    Output m_out_active;
};

void XYController::Output::process(size_t num_samples, size_t offset) {
    const size_t block = block_size();
    if (block == 0 || offset >= block) { return; }
    const size_t end = std::min(block, offset + num_samples);
    const Voice& v = *m_controller.m_voices[m_voice];
    const size_t n = m_controller.m_num_samples;
    float* out = m_output_buffer.data();
    uint8_t* mask = get_output_active().data();

    if (n == 0) {
        std::fill(out + offset, out + end, m_last_output);
        std::fill(mask + offset, mask + end, uint8_t{0});
        return;
    }

    // x and y are live once the voice has a value, so a routing enabled later
    // picks the dot up at once; active is the gate.
    const float* values = nullptr;
    const uint8_t* gate = v.m_xy_mask.data();
    switch (m_axis) {
        case XYPadAxis::X: values = v.m_recorder.out_x(); break;
        case XYPadAxis::Y: values = v.m_recorder.out_y(); break;
        case XYPadAxis::Active: gate = v.m_active.data(); break;
    }
    auto value_at = [&](size_t i) {
        return values != nullptr ? values[i] : static_cast<float>(gate[i]);
    };
    // A matrix block longer than the driven block (a transport length mismatch) holds.
    for (size_t i = offset; i < end; ++i) {
        const size_t source = std::min(i, n - 1);
        out[i] = value_at(source);
        mask[i] = gate[source];
    }
    for (size_t k = 0; k < v.m_num_change_points; ++k) {
        const uint32_t cp = v.m_change_points[k];
        if (cp >= offset && cp < end) { record_change_point(cp); }
    }
    m_last_output = out[end - 1];
}

XYController::XYController(ModulationMatrix& matrix, XYControllerConfig config)
    : m_matrix(matrix), m_config(std::move(config)) {
    m_num_voices = std::clamp<uint32_t>(m_config.m_num_voices, 1, k_xy_controller_max_voices);
    m_config.m_num_voices = m_num_voices;
    m_config.m_max_touches =
        std::clamp<uint32_t>(m_config.m_max_touches, 1, k_xy_controller_max_touches);
    m_config.m_trail_interval = std::max<uint32_t>(m_config.m_trail_interval, 1);

    m_voices.reserve(m_num_voices);
    for (uint32_t v = 0; v < m_num_voices; ++v) {
        m_voices.push_back(std::make_unique<Voice>(*this, v, m_config));
    }
    for (uint32_t v = 0; v < m_num_voices; ++v) {
        auto& voice = *m_voices[v];
        m_matrix.add_source(source_id(XYPadAxis::X, v), &voice.m_out_x);
        m_matrix.add_source(source_id(XYPadAxis::Y, v), &voice.m_out_y);
        m_matrix.add_source(source_id(XYPadAxis::Active, v), &voice.m_out_active);
    }
}

XYController::~XYController() {
    unroute_all();
    for (uint32_t v = 0; v < m_num_voices; ++v) {
        m_matrix.remove_source(source_id(XYPadAxis::X, v));
        m_matrix.remove_source(source_id(XYPadAxis::Y, v));
        m_matrix.remove_source(source_id(XYPadAxis::Active, v));
    }
}

std::string XYController::source_id(XYPadAxis axis, uint32_t voice) const {
    std::string id = m_config.m_id;
    if (m_num_voices > 1) {
        id += '.';
        id += std::to_string(voice);
    }
    switch (axis) {
        case XYPadAxis::X: id += ".x"; break;
        case XYPadAxis::Y: id += ".y"; break;
        case XYPadAxis::Active: id += ".active"; break;
    }
    return id;
}

ModulationSource& XYController::source(XYPadAxis axis, uint32_t voice) {
    auto& v = *m_voices.at(voice);
    switch (axis) {
        case XYPadAxis::X: return v.m_out_x;
        case XYPadAxis::Y: return v.m_out_y;
        case XYPadAxis::Active: break;
    }
    return v.m_out_active;
}

void XYController::prepare(double sample_rate, size_t max_block_size) {
    if (sample_rate <= 0.0 || max_block_size == 0) { return; }
    m_capacity = max_block_size;
    m_sample_rate = sample_rate;
    // Default delay: a touch stamped just before a callback still lands inside
    // the block that callback renders, with 5 ms left for UI delivery.
    const double delay_ms = m_config.m_input_delay_ms.value_or(
        5.0 + (1000.0 * static_cast<double>(max_block_size) / sample_rate));
    const detail::XYPadTiming timing{
        .m_sample_rate = sample_rate,
        .m_delay_ns = static_cast<int64_t>(std::max(delay_ms, 0.0) * 1e6),
        .m_ramp_interval = std::max<uint32_t>(m_config.m_recorder.m_render_interval, 1),
    };
    for (auto& voice : m_voices) {
        auto& v = *voice;
        v.m_pad.prepare(max_block_size, timing);
        v.m_recorder.prepare(sample_rate, max_block_size, m_config.m_recorder);
        v.m_active.assign(max_block_size, 0);
        v.m_xy_mask.assign(max_block_size, 0);
        v.m_layer.assign(max_block_size, XYLayer::None);
        v.m_change_points.assign(max_block_size + 1, 0);
        v.m_num_change_points = 0;
        v.m_trail_countdown = 0;
    }
    const double hz = m_config.m_frame_rate_hz > 0.0 ? m_config.m_frame_rate_hz : 240.0;
    m_frame_interval = static_cast<uint32_t>(std::max(1.0, std::floor(sample_rate / hz)));
    m_num_samples = 0;
    m_sample_time = 0;
    m_rendered_ahead = false;
    m_since_publish = m_frame_interval;  // publish the first block
}

uint32_t XYController::route(XYPadAxis axis,
                             std::string_view target_id,
                             const XYRouteOptions& options,
                             uint32_t voice) {
    if (voice >= m_num_voices) { return k_invalid_routing_id; }
    ModulationRouting r(source_id(axis, voice), target_id, options.m_depth);
    r.m_combine_mode = options.m_combine.value_or(
        axis == XYPadAxis::Active ? CombineMode::Replace : CombineMode::ReplaceHold);
    r.m_replace_priority = options.m_priority;
    r.m_replace_hold_priority = options.m_hold_priority;
    r.m_enabled = options.m_enabled;
    r.m_max_decimation = options.m_max_decimation;
    const uint32_t id = m_matrix.add_routing(r);
    if (id == k_invalid_routing_id) { return id; }
    if (options.m_range_normalized) {
        m_matrix.update_routing_replace_range_normalized(r.m_source_id,
                                                         r.m_target_id,
                                                         options.m_range_normalized->first,
                                                         options.m_range_normalized->second);
    }
    m_routes.push_back(id);
    return id;
}

bool XYController::unroute(uint32_t routing_id) {
    auto it = std::ranges::find(m_routes, routing_id);
    if (it == m_routes.end()) { return false; }
    m_routes.erase(it);
    m_matrix.remove_routing(routing_id);
    return true;
}

void XYController::unroute_all() {
    for (const uint32_t id : m_routes) { m_matrix.remove_routing(id); }
    m_routes.clear();
}

bool XYController::set_route_enabled(uint32_t routing_id, bool enabled) {
    if (std::ranges::find(m_routes, routing_id) == m_routes.end()) { return false; }
    return m_matrix.set_routing_enabled(routing_id, enabled);
}

bool XYController::set_route_depth(uint32_t routing_id, float depth) {
    if (std::ranges::find(m_routes, routing_id) == m_routes.end()) { return false; }
    return m_matrix.update_routing_depth(routing_id, depth);
}

uint32_t XYController::find_touch(TouchId id) const {
    for (uint32_t i = 0; i < m_ui_touches.size(); ++i) {
        if (m_ui_touches[i].m_voice != k_no_voice && m_ui_touches[i].m_id == id) { return i; }
    }
    return k_no_voice;
}

uint32_t XYController::voice_of(TouchId id) const {
    const uint32_t slot = find_touch(id);
    return slot == k_no_voice ? k_no_voice : m_ui_touches[slot].m_voice;
}

// The nearest enabled voice that no finger holds, else the nearest enabled one,
// measured against the dot positions of the last block.
uint32_t XYController::pick_voice(float x, float y) const {
    if (m_num_voices == 1) { return voice_enabled(0) ? 0 : k_no_voice; }
    uint32_t best_free = k_no_voice;
    uint32_t best_any = k_no_voice;
    float distance_free = 0.0f;
    float distance_any = 0.0f;
    for (uint32_t v = 0; v < m_num_voices; ++v) {
        if (!voice_enabled(v)) { continue; }
        const auto [vx, vy] =
            unpack_position(m_voices[v]->m_position.load(std::memory_order_relaxed));
        const float d = ((vx - x) * (vx - x)) + ((vy - y) * (vy - y));
        const bool held =
            std::ranges::any_of(m_ui_touches, [v](const UiTouch& t) { return t.m_voice == v; });
        if (best_any == k_no_voice || d < distance_any) {
            best_any = v;
            distance_any = d;
        }
        if (!held && (best_free == k_no_voice || d < distance_free)) {
            best_free = v;
            distance_free = d;
        }
    }
    return best_free != k_no_voice ? best_free : best_any;
}

bool XYController::touch(TouchId id, float x, float y, std::optional<int64_t> time_ns) {
    if (!std::isfinite(x) || !std::isfinite(y)) { return false; }
    const uint32_t slot = find_touch(id);
    if (slot != k_no_voice) {
        return m_voices[m_ui_touches[slot].m_voice]->m_pad.touch(id, x, y, pad_time(time_ns));
    }
    return touch_voice(pick_voice(std::clamp(x, 0.0f, 1.0f), std::clamp(y, 0.0f, 1.0f)),
                       id,
                       x,
                       y,
                       time_ns);
}

bool XYController::touch_voice(uint32_t voice,
                               TouchId id,
                               float x,
                               float y,
                               std::optional<int64_t> time_ns) {
    if (voice >= m_num_voices || !std::isfinite(x) || !std::isfinite(y)) { return false; }
    const uint32_t slot = find_touch(id);
    if (slot != k_no_voice) {
        // A held touch keeps its voice.
        return m_voices[m_ui_touches[slot].m_voice]->m_pad.touch(id, x, y, pad_time(time_ns));
    }
    const auto free_slot = std::ranges::find_if(m_ui_touches, [](const UiTouch& t) {
        return t.m_voice == k_no_voice;
    });
    if (free_slot == m_ui_touches.end()) { return false; }
    if (!m_voices[voice]->m_pad.touch(id, x, y, pad_time(time_ns))) { return false; }
    *free_slot = UiTouch{.m_id = id, .m_voice = voice};
    return true;
}

bool XYController::release(TouchId id, std::optional<int64_t> time_ns) {
    const uint32_t slot = find_touch(id);
    if (slot == k_no_voice) { return false; }
    const uint32_t voice = m_ui_touches[slot].m_voice;
    m_ui_touches[slot] = UiTouch{};
    return m_voices[voice]->m_pad.release(id, pad_time(time_ns));
}

void XYController::release_all() {
    for (auto& v : m_voices) { v->m_pad.release_all(); }
    m_ui_touches.fill(UiTouch{});
}

bool XYController::flush() {
    bool done = true;
    for (auto& v : m_voices) { done = v->m_pad.flush() && done; }
    return done;
}

void XYController::set_voice_enabled(uint32_t voice, bool enabled) {
    if (voice < m_num_voices) {
        m_voices[voice]->m_enabled.store(enabled, std::memory_order_relaxed);
    }
}

bool XYController::voice_enabled(uint32_t voice) const {
    return voice < m_num_voices && m_voices[voice]->m_enabled.load(std::memory_order_relaxed);
}

MotionRecorder& XYController::recorder(uint32_t voice) {
    return m_voices.at(voice)->m_recorder;
}

const MotionRecorder& XYController::voice_recorder(uint32_t voice) const {
    return m_voices.at(voice)->m_recorder;
}

bool XYController::service() {
    bool any = false;
    for (auto& v : m_voices) { any = v->m_recorder.service() || any; }
    return any;
}

void XYController::set_smoothing(float amount) {
    for (auto& v : m_voices) { v->m_recorder.set_smoothing(amount); }
}

void XYController::set_loop_end(LoopEnd mode) {
    for (auto& v : m_voices) { v->m_recorder.set_loop_end(mode); }
}

bool XYController::read_frame(XYFrame& out) {
    return m_frames.read(out);
}

size_t XYController::drain_trail(std::span<XYPathPoint> out) {
    size_t n = 0;
    while (n < out.size() && m_trail.try_pop(out[n])) { ++n; }
    return n;
}

const float* XYController::out_x(uint32_t voice) const TANH_NONBLOCKING_FUNCTION {
    return m_voices[voice]->m_recorder.out_x();
}

const float* XYController::out_y(uint32_t voice) const TANH_NONBLOCKING_FUNCTION {
    return m_voices[voice]->m_recorder.out_y();
}

const uint8_t* XYController::out_active(uint32_t voice) const TANH_NONBLOCKING_FUNCTION {
    return m_voices[voice]->m_active.data();
}

const XYLayer* XYController::out_layer(uint32_t voice) const TANH_NONBLOCKING_FUNCTION {
    return m_voices[voice]->m_layer.data();
}

std::span<const uint32_t> XYController::change_points(uint32_t voice) const
    TANH_NONBLOCKING_FUNCTION {
    const auto& v = *m_voices[voice];
    return {v.m_change_points.data(), v.m_num_change_points};
}

int64_t XYController::clock_now_ns() TANH_NONBLOCKING_FUNCTION {
#if defined(__APPLE__)
    // UIKit / AppKit event timestamps and AudioTimeStamp host times count
    // uptime; libc++'s steady_clock counts sleep too (CLOCK_MONOTONIC_RAW).
    return static_cast<int64_t>(
        clock_gettime_nsec_np(CLOCK_UPTIME_RAW));  // NOLINT(misc-include-cleaner)
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
#endif
}

void XYController::set_block_time(int64_t now_ns) TANH_NONBLOCKING_FUNCTION {
    m_block_time_ns = now_ns;
    m_block_time_fresh = true;
}

// Run-once per block: set_transport() opens a block; process_block() renders it
// ahead of the matrix; the matrix pass then runs the controller only if nothing
// rendered it. A new set_transport() discards a render the matrix never consumed.
void XYController::set_transport(const TransportInfo& transport) TANH_NONBLOCKING_FUNCTION {
    m_transport = transport;
    m_transport_fresh = true;
    m_rendered_ahead = false;
}

void XYController::process_block(const TransportInfo& transport) TANH_NONBLOCKING_FUNCTION {
    m_transport = transport;
    m_transport_fresh = false;
    run(m_transport);
    m_rendered_ahead = true;
}

void XYController::reset() TANH_NONBLOCKING_FUNCTION {
    m_reset_pending = true;
    for (auto& v : m_voices) { v->m_latched = false; }
}

void XYController::run_for_matrix() {
    if (!m_matrix_block_pending) { return; }
    m_matrix_block_pending = false;
    if (m_rendered_ahead) {
        m_rendered_ahead = false;
        return;
    }
    if (!m_transport_fresh) {
        // Engine bug: no set_transport() for this block. Continue the last one.
        assert(false && "XYController: set_transport() missing before matrix.process()");
        ++m_stale_transport_blocks;
        m_transport.m_beat_position = m_transport.beat_end();
        m_transport.m_flags &= ~TransportInfo::k_discontinuity_mask;
        if (m_transport.m_num_samples == 0) {
            m_transport.m_num_samples = static_cast<uint32_t>(m_capacity);
        }
    }
    m_transport_fresh = false;
    run(m_transport);
}

void XYController::run(const TransportInfo& transport) {
    const auto capacity = static_cast<uint32_t>(m_capacity);
    const uint32_t total = capacity == 0 ? 0 : transport.m_num_samples;
    // The block time places timestamped touches in this block only.
    const bool timed = m_block_time_fresh;
    m_block_time_fresh = false;
    if (total == 0) {
        m_num_samples = 0;
        for (auto& v : m_voices) { v->m_num_change_points = 0; }
        return;
    }
    ++m_blocks_driven;

    TransportInfo t = transport;
    if (m_reset_pending) {
        t.m_flags |= TransportInfo::k_timeline_reset;
        m_reset_pending = false;
    }
    // A block larger than the prepared size runs as prepared-size chunks, exactly
    // like the same audio in prepared-size blocks. The outputs keep the last chunk.
    for (uint32_t offset = 0; offset < total; offset += capacity) {
        m_chunk_time_ns.reset();
        if (timed) {
            m_chunk_time_ns = m_block_time_ns +
                              static_cast<int64_t>(
                                  std::llround(static_cast<double>(offset) * 1e9 / m_sample_rate));
        }
        run_chunk(t.sub_block(offset, std::min(capacity, total - offset)));
    }
}

void XYController::run_chunk(const TransportInfo& transport) {
    const uint32_t n = transport.m_num_samples;
    m_num_samples = n;

    const bool latch = m_latch.load(std::memory_order_relaxed);
    const bool latch_changed = latch != m_last_latch;
    m_last_latch = latch;
    const bool ui = m_ui_attached.load(std::memory_order_relaxed);

    for (uint32_t i = 0; i < m_num_voices; ++i) {
        run_voice(*m_voices[i], i, transport, latch_changed, ui);
    }
    m_sample_time += n;
    if (ui) { publish_frame(n); }
}

void XYController::run_voice(Voice& v,
                             uint32_t index,
                             const TransportInfo& transport,
                             bool latch_changed,
                             bool ui) {
    const uint32_t n = transport.m_num_samples;
    const bool latch = m_last_latch;

    // The recorder composes touch over playback, including the glide back after
    // a release, so the controller only adds the latch and the layer.
    v.m_pad.process_block(n, m_chunk_time_ns);
    v.m_recorder.process(transport, v.m_pad.output(), n);

    const uint8_t* gate = v.m_recorder.out_gate();
    const uint8_t* live = v.m_recorder.out_live();
    if (!latch) { v.m_latched = false; }
    for (uint32_t i = 0; i < n; ++i) {
        const bool g = gate[i] != 0;
        const bool l = live[i] != 0;
        if (latch && l) { v.m_latched = true; }
        v.m_has_value = v.m_has_value || g;
        v.m_active[i] = (g || v.m_latched) ? 1 : 0;
        v.m_xy_mask[i] = v.m_has_value ? 1 : 0;
        v.m_layer[i] = l ? XYLayer::Touch : (g ? XYLayer::Motion : XYLayer::None);
    }

    // The recorder's change points, plus offset 0 when the latch flag changed.
    const auto cps = v.m_recorder.change_points();
    size_t k = 0;
    if (latch_changed && (cps.empty() || cps.front() != 0)) { v.m_change_points[k++] = 0; }
    for (const uint32_t cp : cps) {
        if (k < v.m_change_points.size()) { v.m_change_points[k++] = cp; }
    }
    v.m_num_change_points = k;

    const float* x = v.m_recorder.out_x();
    const float* y = v.m_recorder.out_y();
    v.m_position.store(pack_position(x[n - 1], y[n - 1]), std::memory_order_relaxed);

    if (!ui) { return; }

    const MotionSnapshot motion = v.m_recorder.snapshot();

    // Live trail while recording; drop-oldest when the UI does not drain.
    if (motion.m_state == MotionState::Recording) {
        const uint32_t interval = m_config.m_trail_interval;
        uint32_t i = std::min(v.m_trail_countdown, n);
        for (; i < n; i += interval) {
            const XYPathPoint p{.m_x = x[i],
                                .m_y = y[i],
                                .m_progress = motion.m_progress,
                                .m_voice = static_cast<uint8_t>(index),
                                .m_active = gate[i]};
            m_trail_dropped += static_cast<uint32_t>(m_trail.push_overwrite(p));
        }
        v.m_trail_countdown = i - n;
    } else {
        v.m_trail_countdown = 0;
    }

    XYVoiceFrame& f = m_frames.write_buffer().m_voices[index];
    f.m_x = x[n - 1];
    f.m_y = y[n - 1];
    f.m_active = v.m_active[n - 1];
    f.m_has_value = v.m_has_value;
    f.m_touched = live[n - 1] != 0;
    f.m_layer = v.m_layer[n - 1];
    f.m_path_version = v.m_recorder.lane_version();
    f.m_motion = motion;
}

// Publishes at once when anything discrete changed, else at the frame rate.
void XYController::publish_frame(uint32_t num_samples) {
    XYFrame& frame = m_frames.write_buffer();
    frame.m_sample_time = m_sample_time;
    frame.m_num_voices = m_num_voices;
    frame.m_trail_dropped = m_trail_dropped;
    frame.m_latch = m_last_latch;
    for (uint32_t v = m_num_voices; v < XYFrame::k_max_voices; ++v) {
        frame.m_voices[v] = XYVoiceFrame{};
    }

    uint64_t signature = m_last_latch ? 1u : 0u;
    for (uint32_t v = 0; v < m_num_voices; ++v) {
        const auto& f = frame.m_voices[v];
        signature = hash_combine(signature, static_cast<uint64_t>(f.m_motion.m_state));
        signature = hash_combine(signature, f.m_motion.m_take_id);
        signature = hash_combine(signature, f.m_path_version);
        signature = hash_combine(signature, static_cast<uint64_t>(f.m_layer));
        signature = hash_combine(signature,
                                 (f.m_active != 0 ? 1u : 0u) | (f.m_touched ? 2u : 0u) |
                                     (f.m_motion.m_playing ? 4u : 0u) |
                                     (f.m_motion.m_reverse ? 8u : 0u) | (f.m_has_value ? 16u : 0u));
    }
    m_since_publish += num_samples;
    if (signature == m_last_signature && m_since_publish < m_frame_interval &&
        m_frame_sequence > 0) {
        return;
    }
    m_last_signature = signature;
    m_since_publish = 0;
    frame.m_sequence = ++m_frame_sequence;
    m_frames.publish();
}

}  // namespace thl::modulation
