#include <tanh/modulation/XYController.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace thl::modulation {

using thl::dsp::transport::TransportInfo;

namespace {

uint64_t pack_position(float x, float y) noexcept TANH_NONBLOCKING_FUNCTION {
    return (static_cast<uint64_t>(std::bit_cast<uint32_t>(x)) << 32) |
           static_cast<uint64_t>(std::bit_cast<uint32_t>(y));
}

std::pair<float, float> unpack_position(uint64_t p) {
    return {std::bit_cast<float>(static_cast<uint32_t>(p >> 32)),
            std::bit_cast<float>(static_cast<uint32_t>(p & 0xFFFFFFFFu))};
}

uint64_t mix(uint64_t h, uint64_t v) noexcept TANH_NONBLOCKING_FUNCTION {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

}  // namespace

// ── XYControllerOutput ────────────────────────────────────────────────────────

XYControllerOutput::XYControllerOutput(XYController& controller, uint32_t voice, XYPadAxis axis)
    : ModulationSource(k_global_scope, /*fully_active=*/false)
    , m_controller(controller)
    , m_voice(voice)
    , m_axis(axis) {}

void XYControllerOutput::prepare(double sample_rate,
                                 size_t samples_per_block,
                                 uint32_t voice_count) {
    resize_buffers(samples_per_block, voice_count);
    m_controller.prepare(sample_rate, samples_per_block);
}

void XYControllerOutput::clear_per_block() {
    ModulationSource::clear_per_block();
    // Every clear_per_block() runs before any pre_process_block(): the first
    // output to reach pre_process_block() drives the controller.
    m_controller.m_block_pending = true;
}

void XYControllerOutput::pre_process_block() {
    m_controller.drive_from_matrix();
}

void XYControllerOutput::process(size_t num_samples, size_t offset) {
    const size_t bs = block_size();
    if (bs == 0 || offset >= bs) { return; }
    const size_t end = std::min(bs, offset + num_samples);
    const auto& v = *m_controller.m_voices[m_voice];
    const size_t n = m_controller.m_num_samples;
    float* out = m_output_buffer.data();
    uint8_t* mask = get_output_active().data();

    const float* src = nullptr;
    const uint8_t* src_mask = nullptr;
    switch (m_axis) {
        case XYPadAxis::X:
            src = v.m_recorder.out_x();
            src_mask = v.m_xy_mask.data();
            break;
        case XYPadAxis::Y:
            src = v.m_recorder.out_y();
            src_mask = v.m_xy_mask.data();
            break;
        case XYPadAxis::Active: src_mask = v.m_active.data(); break;
    }

    if (n == 0) {
        std::fill(out + offset, out + end, m_last_output);
        std::fill(mask + offset, mask + end, uint8_t{0});
        return;
    }
    const size_t copy_end = std::min(end, n);
    for (size_t i = offset; i < copy_end; ++i) {
        out[i] = src != nullptr ? src[i] : static_cast<float>(src_mask[i]);
        mask[i] = src_mask[i];
    }
    // A matrix block longer than the driven block (transport length mismatch): hold.
    for (size_t i = std::max(offset, copy_end); i < end; ++i) {
        out[i] = src != nullptr ? src[n - 1] : static_cast<float>(src_mask[n - 1]);
        mask[i] = src_mask[n - 1];
    }
    for (size_t k = 0; k < v.m_num_cps; ++k) {
        const uint32_t cp = v.m_cps[k];
        if (cp >= offset && cp < end) { record_change_point(cp); }
    }
    m_last_output = out[end - 1];
}

// ── XYController: setup ───────────────────────────────────────────────────────

XYController::Voice::Voice(XYController& owner, uint32_t index, const XYPadConfig& pad_config)
    : m_pad(pad_config)
    , m_out_x(owner, index, XYPadAxis::X)
    , m_out_y(owner, index, XYPadAxis::Y)
    , m_out_active(owner, index, XYPadAxis::Active) {}

XYController::XYController(ModulationMatrix& matrix, XYControllerConfig config)
    : m_matrix(matrix), m_config(std::move(config)) {
    m_num_voices = std::clamp<uint32_t>(m_config.m_num_voices, 1, k_xy_controller_max_voices);
    m_config.m_num_voices = m_num_voices;
    m_config.m_max_touches = std::clamp<uint32_t>(m_config.m_max_touches, 1, k_xy_pad_max_touches);
    m_config.m_trail_interval = std::max<uint32_t>(m_config.m_trail_interval, 1);

    XYPadConfig pad;
    pad.m_scope = k_global_scope;
    pad.m_max_touches = m_config.m_max_touches;
    pad.m_mono_priority = m_config.m_mono_priority;
    m_voices.reserve(m_num_voices);
    for (uint32_t v = 0; v < m_num_voices; ++v) {
        m_voices.push_back(std::make_unique<Voice>(*this, v, pad));
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
    if (sample_rate == m_sample_rate && max_block_size == m_capacity) { return; }
    m_sample_rate = sample_rate;
    m_capacity = max_block_size;
    for (auto& vp : m_voices) {
        auto& v = *vp;
        v.m_pad.prepare(max_block_size);
        v.m_recorder.prepare(sample_rate, max_block_size, m_config.m_recorder);
        v.m_active.assign(max_block_size, 0);
        v.m_xy_mask.assign(max_block_size, 0);
        v.m_layer.assign(max_block_size, XYLayer::None);
        v.m_cps.assign(max_block_size + 1, 0);
        v.m_num_cps = 0;
        v.m_trail_countdown = 0;
    }
    const double hz = m_config.m_frame_rate_hz > 0.0 ? m_config.m_frame_rate_hz : 240.0;
    m_frame_interval = static_cast<uint32_t>(std::max(1.0, std::floor(sample_rate / hz)));
    m_num_samples = 0;
    m_sample_time = 0;
    m_since_publish = m_frame_interval;  // publish the first block
}

// ── Routing ───────────────────────────────────────────────────────────────────

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

std::vector<uint32_t> XYController::route_voices(XYPadAxis axis,
                                                 std::span<const std::string> targets,
                                                 const XYRouteOptions& options) {
    std::vector<uint32_t> ids;
    const size_t n = std::min<size_t>(m_num_voices, targets.size());
    ids.reserve(n);
    for (size_t v = 0; v < n; ++v) {
        ids.push_back(route(axis, targets[v], options, static_cast<uint32_t>(v)));
    }
    return ids;
}

bool XYController::unroute(uint32_t routing_id) {
    auto it = std::find(m_routes.begin(), m_routes.end(), routing_id);
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
    if (std::find(m_routes.begin(), m_routes.end(), routing_id) == m_routes.end()) { return false; }
    return m_matrix.set_routing_enabled(routing_id, enabled);
}

bool XYController::set_route_depth(uint32_t routing_id, float depth) {
    if (std::find(m_routes.begin(), m_routes.end(), routing_id) == m_routes.end()) { return false; }
    return m_matrix.update_routing_depth(routing_id, depth);
}

// ── Input (UI thread) ─────────────────────────────────────────────────────────

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

uint32_t XYController::pick_voice(float x, float y) const {
    if (m_num_voices == 1) { return voice_enabled(0) ? 0 : k_no_voice; }
    uint32_t best_free = k_no_voice;
    uint32_t best_any = k_no_voice;
    float d_free = 0.0f;
    float d_any = 0.0f;
    for (uint32_t v = 0; v < m_num_voices; ++v) {
        if (!voice_enabled(v)) { continue; }
        const auto [vx, vy] =
            unpack_position(m_voices[v]->m_position.load(std::memory_order_relaxed));
        const float d = ((vx - x) * (vx - x)) + ((vy - y) * (vy - y));
        bool held = false;
        for (const auto& t : m_ui_touches) { held = held || t.m_voice == v; }
        if (best_any == k_no_voice || d < d_any) {
            best_any = v;
            d_any = d;
        }
        if (!held && (best_free == k_no_voice || d < d_free)) {
            best_free = v;
            d_free = d;
        }
    }
    return best_free != k_no_voice ? best_free : best_any;
}

bool XYController::touch(TouchId id, float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y)) { return false; }
    const uint32_t slot = find_touch(id);
    if (slot != k_no_voice) { return m_voices[m_ui_touches[slot].m_voice]->m_pad.touch(id, x, y); }
    return touch_voice(pick_voice(std::clamp(x, 0.0f, 1.0f), std::clamp(y, 0.0f, 1.0f)), id, x, y);
}

bool XYController::touch_voice(uint32_t voice, TouchId id, float x, float y) {
    if (voice >= m_num_voices || !std::isfinite(x) || !std::isfinite(y)) { return false; }
    const uint32_t slot = find_touch(id);
    if (slot != k_no_voice) {
        // A held touch keeps its voice.
        return m_voices[m_ui_touches[slot].m_voice]->m_pad.touch(id, x, y);
    }
    uint32_t free_slot = k_no_voice;
    for (uint32_t i = 0; i < m_ui_touches.size(); ++i) {
        if (m_ui_touches[i].m_voice == k_no_voice) {
            free_slot = i;
            break;
        }
    }
    if (free_slot == k_no_voice) { return false; }
    if (!m_voices[voice]->m_pad.touch(id, x, y)) { return false; }
    m_ui_touches[free_slot] = UiTouch{.m_id = id, .m_voice = voice};
    return true;
}

bool XYController::release(TouchId id) {
    const uint32_t slot = find_touch(id);
    if (slot == k_no_voice) { return false; }
    const uint32_t voice = m_ui_touches[slot].m_voice;
    m_ui_touches[slot] = UiTouch{};
    return m_voices[voice]->m_pad.release(id);
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

bool XYController::service() {
    bool any = false;
    for (auto& v : m_voices) { any = v->m_recorder.service() || any; }
    return any;
}

// ── UI snapshot ───────────────────────────────────────────────────────────────

bool XYController::read_frame(XYFrame& out) {
    return m_frames.read(out);
}

size_t XYController::drain_trail(std::span<XYPathPoint> out) {
    size_t n = 0;
    while (n < out.size() && m_trail.try_pop(out[n])) { ++n; }
    return n;
}

// ── Audio thread ──────────────────────────────────────────────────────────────

const float* XYController::out_x(uint32_t voice) const noexcept TANH_NONBLOCKING_FUNCTION {
    return m_voices[voice]->m_recorder.out_x();
}

const float* XYController::out_y(uint32_t voice) const noexcept TANH_NONBLOCKING_FUNCTION {
    return m_voices[voice]->m_recorder.out_y();
}

const uint8_t* XYController::out_active(uint32_t voice) const noexcept TANH_NONBLOCKING_FUNCTION {
    return m_voices[voice]->m_active.data();
}

const XYLayer* XYController::out_layer(uint32_t voice) const noexcept TANH_NONBLOCKING_FUNCTION {
    return m_voices[voice]->m_layer.data();
}

std::span<const uint32_t> XYController::change_points(uint32_t voice) const noexcept
    TANH_NONBLOCKING_FUNCTION {
    const auto& v = *m_voices[voice];
    return {v.m_cps.data(), v.m_num_cps};
}

void XYController::process_block(const TransportInfo& transport) noexcept
    TANH_NONBLOCKING_FUNCTION {
    set_transport(transport);
    m_transport_fresh = false;
    run(m_transport);
    m_driven_standalone = true;
}

void XYController::reset() noexcept TANH_NONBLOCKING_FUNCTION {
    m_reset_pending = true;
    for (auto& v : m_voices) { v->m_latched = false; }
}

void XYController::drive_from_matrix() noexcept TANH_NONBLOCKING_FUNCTION {
    if (!m_block_pending) { return; }
    m_block_pending = false;
    if (m_driven_standalone) {
        // process_block() already rendered this block before matrix.process().
        m_driven_standalone = false;
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

void XYController::run(const TransportInfo& transport) noexcept TANH_NONBLOCKING_FUNCTION {
    const auto n = static_cast<uint32_t>(std::min<size_t>(transport.m_num_samples, m_capacity));
    m_num_samples = n;
    if (n == 0) {
        for (auto& v : m_voices) { v->m_num_cps = 0; }
        return;
    }
    ++m_blocks_driven;

    TransportInfo t = transport;
    t.m_num_samples = n;
    if (m_reset_pending) {
        t.m_flags |= TransportInfo::k_timeline_reset;
        m_reset_pending = false;
    }

    const bool latch = m_latch.load(std::memory_order_relaxed);
    const bool latch_changed = latch != m_last_latch;
    m_last_latch = latch;
    const bool ui = m_ui_attached.load(std::memory_order_relaxed);

    for (uint32_t i = 0; i < m_num_voices; ++i) {
        run_voice(*m_voices[i], i, t, n, latch, latch_changed, ui);
    }
    m_sample_time += n;
    if (ui) { publish_frame(n); }
}

void XYController::run_voice(Voice& v,
                             uint32_t index,
                             const TransportInfo& t,
                             uint32_t n,
                             bool latch,
                             bool latch_changed,
                             bool ui) noexcept TANH_NONBLOCKING_FUNCTION {
    // 1. Touch layer: drain the pad.
    v.m_pad.process_block(n);
    // 2. Motion layer: record / play back; composes touch > playback (with its
    //    own glide back after a release — no second glide here).
    v.m_recorder.process(t, v.m_pad.primary(), n);
    // 3. [Extension point] A sequencer layer would advance here and fill the
    //    samples where neither touch nor motion is active.

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

    // Change points: the recorder's (render ticks, gate and touch edges, glide
    // starts) plus offset 0 when the latch flag changed.
    const auto cps = v.m_recorder.change_points();
    size_t k = 0;
    if (latch_changed && (cps.empty() || cps.front() != 0)) { v.m_cps[k++] = 0; }
    for (const uint32_t cp : cps) {
        if (k < v.m_cps.size()) { v.m_cps[k++] = cp; }
    }
    v.m_num_cps = k;

    const float* x = v.m_recorder.out_x();
    const float* y = v.m_recorder.out_y();
    v.m_position.store(pack_position(x[n - 1], y[n - 1]), std::memory_order_relaxed);

    if (!ui) { return; }

    const MotionSnapshot snap = v.m_recorder.ui_snapshot();  // this block (same thread)

    // Live trail while recording (drop-oldest when the UI does not drain).
    if (snap.m_state == MotionState::Recording) {
        const uint32_t interval = m_config.m_trail_interval;
        uint32_t i = std::min(v.m_trail_countdown, n);
        for (; i < n; i += interval) {
            const XYPathPoint p{.m_x = x[i],
                                .m_y = y[i],
                                .m_progress = snap.m_progress,
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
    f.m_motion_playing = snap.m_playing;
    f.m_reverse = snap.m_reverse;
    f.m_beats = snap.m_beats;
    f.m_layer = v.m_layer[n - 1];
    f.m_motion_state = snap.m_state;
    f.m_motion_phase = snap.m_phase;
    f.m_record_progress = snap.m_progress;
    f.m_take_id = snap.m_take_id;
    f.m_path_version = v.m_recorder.lane_version();
    f.m_loop_length = snap.m_length;
}

void XYController::publish_frame(uint32_t n) noexcept TANH_NONBLOCKING_FUNCTION {
    XYFrame& frame = m_frames.write_buffer();
    frame.m_sample_time = m_sample_time;
    frame.m_num_voices = m_num_voices;
    frame.m_trail_dropped = m_trail_dropped;
    frame.m_latch = m_last_latch;
    for (uint32_t v = m_num_voices; v < XYFrame::k_max_voices; ++v) {
        frame.m_voices[v] = XYVoiceFrame{};
    }

    // Publish at once when anything discrete changed, else at the frame rate.
    uint64_t sig = m_last_latch ? 1u : 0u;
    for (uint32_t v = 0; v < m_num_voices; ++v) {
        const auto& f = frame.m_voices[v];
        sig = mix(sig, static_cast<uint64_t>(f.m_motion_state));
        sig = mix(sig, f.m_take_id);
        sig = mix(sig, f.m_path_version);
        sig = mix(sig, static_cast<uint64_t>(f.m_layer));
        sig = mix(sig,
                  (f.m_active != 0 ? 1u : 0u) | (f.m_touched ? 2u : 0u) |
                      (f.m_motion_playing ? 4u : 0u) | (f.m_reverse ? 8u : 0u) |
                      (f.m_has_value ? 16u : 0u));
    }
    m_since_publish += n;
    if (sig == m_last_signature && m_since_publish < m_frame_interval && m_frame_sequence > 0) {
        return;
    }
    m_last_signature = sig;
    m_since_publish = 0;
    frame.m_sequence = ++m_frame_sequence;
    m_frames.publish();
}

// ── XYModeRouter ──────────────────────────────────────────────────────────────

XYModeRouter::XYModeRouter(ModulationMatrix& matrix, XYPadMode mode)
    : m_matrix(matrix), m_mode(mode) {}

bool XYModeRouter::add_target(std::string_view target_id,
                              XYPadAxis axis,
                              XYController& per_effect,
                              uint32_t per_effect_voice,
                              XYController& single,
                              uint32_t single_voice) {
    XYRouteOptions pe;
    pe.m_priority = 10;
    XYRouteOptions si;
    si.m_priority = 20;
    return add_target(target_id, axis, per_effect, per_effect_voice, single, single_voice, pe, si);
}

bool XYModeRouter::add_target(std::string_view target_id,
                              XYPadAxis axis,
                              XYController& per_effect,
                              uint32_t per_effect_voice,
                              XYController& single,
                              uint32_t single_voice,
                              XYRouteOptions per_effect_options,
                              XYRouteOptions single_options) {
    per_effect_options.m_enabled = m_mode == XYPadMode::PerEffect;
    single_options.m_enabled = m_mode == XYPadMode::Single;
    const uint32_t pe = per_effect.route(axis, target_id, per_effect_options, per_effect_voice);
    if (pe == k_invalid_routing_id) { return false; }
    const uint32_t si = single.route(axis, target_id, single_options, single_voice);
    if (si == k_invalid_routing_id) {
        per_effect.unroute(pe);
        return false;
    }
    m_per_effect.push_back(pe);
    m_single.push_back(si);
    m_batch.reserve(m_per_effect.size() * 2);
    return true;
}

void XYModeRouter::set_mode(XYPadMode mode) {
    m_mode = mode;
    m_batch.clear();
    for (const uint32_t id : m_per_effect) {
        m_batch.push_back(
            RoutingEnabled{.m_routing_id = id, .m_enabled = mode == XYPadMode::PerEffect});
    }
    for (const uint32_t id : m_single) {
        m_batch.push_back(
            RoutingEnabled{.m_routing_id = id, .m_enabled = mode == XYPadMode::Single});
    }
    m_matrix.set_routings_enabled(m_batch);
}

}  // namespace thl::modulation
