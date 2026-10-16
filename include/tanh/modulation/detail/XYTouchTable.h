#pragma once

#include <tanh/modulation/XYTypes.h>

#include <array>
#include <cstdint>

namespace thl::modulation::detail {

/// The fingers an XYController tracks and the voice each one drives (UI thread).
class XYTouchTable {
public:
    /// The voice @p id drives, or k_xy_no_voice.
    [[nodiscard]] uint32_t voice_of(TouchId id) const {
        for (const auto& t : m_touches) {
            if (t.m_voice != k_xy_no_voice && t.m_id == id) { return t.m_voice; }
        }
        return k_xy_no_voice;
    }
    /// True if a finger drives @p voice.
    [[nodiscard]] bool held(uint32_t voice) const {
        for (const auto& t : m_touches) {
            if (t.m_voice == voice) { return true; }
        }
        return false;
    }
    /// Records that @p id drives @p voice. False when every slot is taken.
    bool add(TouchId id, uint32_t voice) {
        for (auto& t : m_touches) {
            if (t.m_voice == k_xy_no_voice) {
                t = Touch{.m_id = id, .m_voice = voice};
                return true;
            }
        }
        return false;
    }
    /// Forgets @p id; returns the voice it drove, or k_xy_no_voice.
    uint32_t remove(TouchId id) {
        for (auto& t : m_touches) {
            if (t.m_voice != k_xy_no_voice && t.m_id == id) {
                const uint32_t voice = t.m_voice;
                t = Touch{};
                return voice;
            }
        }
        return k_xy_no_voice;
    }
    void clear() { m_touches.fill(Touch{}); }

private:
    struct Touch {
        TouchId m_id = 0;
        uint32_t m_voice = k_xy_no_voice;
    };
    std::array<Touch, k_xy_controller_max_touches> m_touches{};
};

}  // namespace thl::modulation::detail
