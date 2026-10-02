#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/ModulationSource.h>
#include <tanh/modulation/XYPad.h>
#include <tanh/modulation/detail/EventSpread.h>
#include <tanh/state/ModulationScope.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace thl::modulation {

// ── XYPadOutput ───────────────────────────────────────────────────────────────

XYPadOutput::XYPadOutput(XYPad& pad, XYPadAxis axis)
    : ModulationSource(pad.config().m_scope, /*fully_active=*/false), m_pad(pad), m_axis(axis) {}

void XYPadOutput::prepare(double /*sample_rate*/, size_t samples_per_block, uint32_t voice_count) {
    resize_buffers(samples_per_block, voice_count);
    m_pad.prepare(samples_per_block);
}

void XYPadOutput::copy_stream(const XYPadStream& s, float* out, uint8_t* mask) const {
    const size_t bs = block_size();
    const size_t n = std::min<size_t>(s.m_num_samples, bs);
    const float* src = nullptr;
    switch (m_axis) {
        case XYPadAxis::X: src = s.m_x; break;
        case XYPadAxis::Y: src = s.m_y; break;
        case XYPadAxis::Active: src = nullptr; break;
    }
    for (size_t i = 0; i < n; ++i) {
        out[i] = src != nullptr ? src[i] : static_cast<float>(s.m_active[i]);
        mask[i] = s.m_active[i];
    }
    // A host block shorter than the pad render: hold the last value.
    if (n > 0 && n < bs) {
        std::fill(out + n, out + bs, out[n - 1]);
        std::fill(mask + n, mask + bs, mask[n - 1]);
    }
}

void XYPadOutput::pre_process_block() {
    const auto bs = static_cast<uint32_t>(block_size());
    if (bs == 0) { return; }
    m_pad.render_for_matrix(bs);

    if (is_global()) {
        const XYPadStream s = m_pad.stream(0);
        copy_stream(s, m_output_buffer.data(), get_output_active().data());
        for (const uint32_t cp : s.m_change_points) { record_change_point(cp); }
        m_last_output = m_output_buffer[bs - 1];
        return;
    }
    for (uint32_t v = 0; v < num_voices(); ++v) {
        if (v < m_pad.num_streams()) {
            const XYPadStream s = m_pad.stream(v);
            copy_stream(s, voice_output(v), voice_output_active(v));
            for (const uint32_t cp : s.m_change_points) { record_voice_change_point(v, cp); }
        } else {
            std::fill_n(voice_output(v), bs, 0.0f);
            std::fill_n(voice_output_active(v), bs, uint8_t{0});
        }
    }
}

// ── XYPad: setup ──────────────────────────────────────────────────────────────

XYPad::XYPad(XYPadConfig config)
    : m_config(config)
    , m_out_x(*this, XYPadAxis::X)
    , m_out_y(*this, XYPadAxis::Y)
    , m_out_active(*this, XYPadAxis::Active) {
    m_config.m_max_touches = std::clamp<uint32_t>(m_config.m_max_touches, 1, k_xy_pad_max_touches);
    m_num_streams = m_config.m_scope == k_global_scope ? 1 : m_config.m_max_touches;
}

XYPad::~XYPad() {
    assert(m_matrix.load() == nullptr && "XYPad destroyed while registered: call remove_from()");
}

void XYPad::add_to(ModulationMatrix& matrix, std::string_view prefix) {
    assert(m_matrix.load() == nullptr && "XYPad is already registered with a matrix");
    m_prefix = std::string(prefix);
    m_matrix.store(&matrix, std::memory_order_release);
    matrix.add_source(source_id(XYPadAxis::X), &m_out_x);
    matrix.add_source(source_id(XYPadAxis::Y), &m_out_y);
    matrix.add_source(source_id(XYPadAxis::Active), &m_out_active);
}

void XYPad::remove_from(ModulationMatrix& matrix) {
    if (m_matrix.load(std::memory_order_acquire) != &matrix) { return; }
    matrix.remove_source(source_id(XYPadAxis::X));
    matrix.remove_source(source_id(XYPadAxis::Y));
    matrix.remove_source(source_id(XYPadAxis::Active));
    m_matrix.store(nullptr, std::memory_order_release);
}

void XYPad::prepare(size_t max_block_size) {
    if (max_block_size <= m_capacity) { return; }
    m_capacity = max_block_size;
    const size_t total = m_capacity * m_num_streams;
    m_x_storage.assign(total, 0.0f);
    m_y_storage.assign(total, 0.0f);
    m_active_storage.assign(total, 0);
    m_cp_storage.assign(total, 0);
    m_cp_count.fill(0);
    m_rendered_frames = 0;
}

ModulationSource& XYPad::source(XYPadAxis axis) {
    switch (axis) {
        case XYPadAxis::X: return m_out_x;
        case XYPadAxis::Y: return m_out_y;
        case XYPadAxis::Active: break;
    }
    return m_out_active;
}

std::string XYPad::source_id(XYPadAxis axis) const {
    if (m_prefix.empty()) { return {}; }
    switch (axis) {
        case XYPadAxis::X: return m_prefix + ".x";
        case XYPadAxis::Y: return m_prefix + ".y";
        case XYPadAxis::Active: break;
    }
    return m_prefix + ".active";
}

// ── XYPad: UI thread ──────────────────────────────────────────────────────────

uint32_t XYPad::find_slot(TouchId id) const {
    for (uint32_t s = 0; s < m_config.m_max_touches; ++s) {
        if (m_ui_slots[s].m_active && m_ui_slots[s].m_id == id) { return s; }
    }
    return k_no_slot;
}

uint32_t XYPad::pick_driver() const {
    uint32_t best = k_no_slot;
    for (uint32_t s = 0; s < m_config.m_max_touches; ++s) {
        if (!m_ui_slots[s].m_active) { continue; }
        if (best == k_no_slot) {
            best = s;
            continue;
        }
        const bool newer = m_ui_press_seq[s] > m_ui_press_seq[best];
        if (m_config.m_mono_priority == MonoPriority::Last ? newer : !newer) { best = s; }
    }
    return best;
}

bool XYPad::flush_stream(uint32_t stream) {
    Pending& p = m_pending[stream];
    const auto s8 = static_cast<uint8_t>(stream);
    if (p.m_pre) {
        const PadEvent e{s8, p.m_pre_down ? Kind::Down : Kind::Move, p.m_pre_x, p.m_pre_y};
        if (!m_queue.try_push(e)) { return false; }
        p.m_pre = false;
        p.m_pre_down = false;
    }
    if (p.m_up) {
        const PadEvent e{s8, Kind::Up, p.m_up_x, p.m_up_y};
        if (!m_queue.try_push(e)) { return false; }
        p.m_up = false;
    }
    if (p.m_post) {
        const PadEvent e{s8, Kind::Down, p.m_post_x, p.m_post_y};
        if (!m_queue.try_push(e)) { return false; }
        p.m_post = false;
    }
    return true;
}

bool XYPad::flush() {
    bool clean = true;
    for (uint32_t s = 0; s < m_num_streams; ++s) {
        if (m_pending[s].any()) { clean = flush_stream(s) && clean; }
    }
    return clean;
}

bool XYPad::emit_pos(uint32_t stream, float x, float y, bool down) {
    Pending& p = m_pending[stream];
    if (p.any() && !flush_stream(stream)) {
        // Still blocked: coalesce behind what is pending (latest position wins).
        if (p.m_up) {
            p.m_post = true;
            p.m_post_x = x;
            p.m_post_y = y;
        } else {
            p.m_pre = true;
            p.m_pre_down = p.m_pre_down || down;
            p.m_pre_x = x;
            p.m_pre_y = y;
        }
        return true;
    }
    const PadEvent e{static_cast<uint8_t>(stream), down ? Kind::Down : Kind::Move, x, y};
    if (m_queue.try_push(e)) { return true; }
    if (down) { return false; }  // a new touch that cannot be sent claims nothing
    p.m_pre = true;
    p.m_pre_x = x;
    p.m_pre_y = y;
    return true;
}

void XYPad::emit_up(uint32_t stream, float x, float y) {
    Pending& p = m_pending[stream];
    if (p.any() && !flush_stream(stream)) {
        // A pending down followed by this up is a tap that never reached the
        // audio thread: drop it, but let the pending up carry its position.
        p.m_post = false;
        p.m_up = true;
        p.m_up_x = x;
        p.m_up_y = y;
        return;
    }
    const PadEvent e{static_cast<uint8_t>(stream), Kind::Up, x, y};
    if (!m_queue.try_push(e)) {
        p.m_up = true;
        p.m_up_x = x;
        p.m_up_y = y;
    }
}

bool XYPad::touch(TouchId id, float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y)) { return false; }
    x = std::clamp(x, 0.0f, 1.0f);
    y = std::clamp(y, 0.0f, 1.0f);
    flush();

    const bool global = m_num_streams == 1 && m_config.m_scope == k_global_scope;
    uint32_t slot = find_slot(id);
    if (slot != k_no_slot) {
        m_ui_slots[slot].m_x = x;
        m_ui_slots[slot].m_y = y;
        if (!global) { return emit_pos(slot, x, y, false); }
        if (slot == m_driver) { return emit_pos(0, x, y, false); }
        return true;
    }

    for (slot = 0; slot < m_config.m_max_touches; ++slot) {
        if (!m_ui_slots[slot].m_active) { break; }
    }
    if (slot == m_config.m_max_touches) { return false; }

    if (!global) {
        if (!emit_pos(slot, x, y, true)) { return false; }
    } else {
        const bool idle = m_driver == k_no_slot;
        const bool takes_over = idle || m_config.m_mono_priority == MonoPriority::Last;
        if (takes_over) {
            if (!emit_pos(0, x, y, idle)) { return false; }
            m_driver = slot;
        }
    }
    m_ui_slots[slot] = TouchView{.m_id = id, .m_x = x, .m_y = y, .m_active = true};
    m_ui_press_seq[slot] = ++m_press_counter;
    return true;
}

bool XYPad::release(TouchId id) {
    flush();
    const uint32_t slot = find_slot(id);
    if (slot == k_no_slot) { return false; }
    TouchView& t = m_ui_slots[slot];
    t.m_active = false;

    const bool global = m_num_streams == 1 && m_config.m_scope == k_global_scope;
    if (!global) {
        emit_up(slot, t.m_x, t.m_y);
        return true;
    }
    if (slot != m_driver) { return true; }
    m_driver = pick_driver();
    if (m_driver != k_no_slot) {
        const TouchView& fb = m_ui_slots[m_driver];
        emit_pos(0, fb.m_x, fb.m_y, false);
    } else {
        emit_up(0, t.m_x, t.m_y);
    }
    return true;
}

void XYPad::release_all() {
    flush();
    const bool global = m_num_streams == 1 && m_config.m_scope == k_global_scope;
    for (uint32_t s = 0; s < m_config.m_max_touches; ++s) {
        TouchView& t = m_ui_slots[s];
        if (!t.m_active) { continue; }
        t.m_active = false;
        if (!global) { emit_up(s, t.m_x, t.m_y); }
    }
    if (global && m_driver != k_no_slot) {
        const TouchView& t = m_ui_slots[m_driver];
        m_driver = k_no_slot;
        emit_up(0, t.m_x, t.m_y);
    }
}

std::span<const XYPad::TouchView> XYPad::touches() const {
    return {m_ui_slots.data(), m_config.m_max_touches};
}

// ── XYPad: audio thread ───────────────────────────────────────────────────────

void XYPad::apply(const PadEvent& e,
                  uint32_t offset,
                  uint32_t n) noexcept TANH_NONBLOCKING_FUNCTION {
    const uint32_t s = e.m_stream;
    const size_t base = static_cast<size_t>(s) * m_capacity;
    float* x = m_x_storage.data() + base;
    float* y = m_y_storage.data() + base;
    uint8_t* active = m_active_storage.data() + base;

    std::fill(x + offset, x + n, e.m_x);
    std::fill(y + offset, y + n, e.m_y);
    if (e.m_kind == Kind::Up) {
        std::fill(active + offset, active + n, uint8_t{0});
    } else {
        if (e.m_kind == Kind::Down || active[offset] == 0) {  // gate rises here
            m_down_seq[s] = ++m_audio_down_counter;
        }
        std::fill(active + offset, active + n, uint8_t{1});
    }

    uint32_t* cps = m_cp_storage.data() + base;
    uint32_t& count = m_cp_count[s];
    if (count == 0 || cps[count - 1] != offset) { cps[count++] = offset; }
}

void XYPad::process_block(uint32_t num_samples) noexcept TANH_NONBLOCKING_FUNCTION {
    const ModulationMatrix* matrix = m_matrix.load(std::memory_order_acquire);
    if (matrix != nullptr) {
        m_render_stamp = matrix->get_num_processed_samples();
        m_rendered_for_matrix = true;
    }

    const auto n = static_cast<uint32_t>(std::min<size_t>(num_samples, m_capacity));
    m_rendered_frames = n;
    if (n == 0) { return; }  // keep events for a real block

    // Seed every stream with the previous block's final state.
    for (uint32_t s = 0; s < m_num_streams; ++s) {
        const size_t base = static_cast<size_t>(s) * m_capacity;
        std::fill_n(m_x_storage.data() + base, n, m_last_x[s]);
        std::fill_n(m_y_storage.data() + base, n, m_last_y[s]);
        std::fill_n(m_active_storage.data() + base, n, m_last_active[s]);
        m_cp_count[s] = 0;
    }

    // Snapshot the queue, then spread each stream's events over the block.
    size_t count = 0;
    while (count < m_scratch.size() && m_queue.try_pop(m_scratch[count])) { ++count; }

    std::array<uint32_t, k_xy_pad_max_touches> total{};
    std::array<uint32_t, k_xy_pad_max_touches> index{};
    for (size_t i = 0; i < count; ++i) {
        if (m_scratch[i].m_stream < m_num_streams) { ++total[m_scratch[i].m_stream]; }
    }
    for (size_t i = 0; i < count; ++i) {
        const PadEvent& e = m_scratch[i];
        if (e.m_stream >= m_num_streams) { continue; }
        const uint32_t offset = detail::spread_offset(index[e.m_stream]++, total[e.m_stream], n);
        apply(e, offset, n);
    }

    // Carry the final state and pick the primary stream (oldest held touch).
    uint32_t primary = k_no_slot;
    for (uint32_t s = 0; s < m_num_streams; ++s) {
        const size_t last = (static_cast<size_t>(s) * m_capacity) + n - 1;
        const bool released = m_last_active[s] != 0 && m_active_storage[last] == 0;
        m_last_x[s] = m_x_storage[last];
        m_last_y[s] = m_y_storage[last];
        m_last_active[s] = m_active_storage[last];
        if (released && primary == k_no_slot) { m_primary = s; }
        if (m_last_active[s] != 0 &&
            (primary == k_no_slot || m_down_seq[s] < m_down_seq[primary])) {
            primary = s;
        }
    }
    if (primary != k_no_slot) { m_primary = primary; }
}

void XYPad::render_for_matrix(uint32_t num_samples) noexcept TANH_NONBLOCKING_FUNCTION {
    const ModulationMatrix* matrix = m_matrix.load(std::memory_order_acquire);
    if (matrix != nullptr && m_rendered_for_matrix &&
        matrix->get_num_processed_samples() == m_render_stamp) {
        return;  // already rendered this block (an owner or a sibling output)
    }
    process_block(num_samples);
}

XYPadStream XYPad::stream(uint32_t index) const noexcept TANH_NONBLOCKING_FUNCTION {
    if (index >= m_num_streams || m_capacity == 0) { return {}; }
    const size_t base = static_cast<size_t>(index) * m_capacity;
    return XYPadStream{
        .m_x = m_x_storage.data() + base,
        .m_y = m_y_storage.data() + base,
        .m_active = m_active_storage.data() + base,
        .m_change_points = std::span<const uint32_t>(m_cp_storage.data() + base, m_cp_count[index]),
        .m_num_samples = m_rendered_frames,
    };
}

XYPadStream XYPad::primary() const noexcept TANH_NONBLOCKING_FUNCTION {
    return stream(m_primary);
}

}  // namespace thl::modulation
