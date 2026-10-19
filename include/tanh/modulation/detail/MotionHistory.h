#pragma once

#include <tanh/modulation/MotionLane.h>

#include <optional>
#include <utility>

namespace thl::modulation::detail {

/**
 * @brief One level of undo and redo over published lanes (message thread).
 *
 * record() keeps the lane a publication replaced; undo() and redo() swap the
 * current lane with the kept one. A new record() drops the redo lane.
 */
class MotionHistory {
public:
    /// A publication replaced @p previous (an empty lane replacing an empty lane is no change).
    void record(MotionLane previous, bool next_empty) {
        if (previous.empty() && next_empty) { return; }
        m_undo = std::move(previous);
        m_redo.reset();
    }

    /// The lane to restore instead of @p current, which becomes redoable; nullopt if none.
    [[nodiscard]] std::optional<MotionLane> undo(MotionLane current) {
        return swap(m_undo, m_redo, std::move(current));
    }
    /// The lane the last undo() replaced, @p current becomes undoable; nullopt if none.
    [[nodiscard]] std::optional<MotionLane> redo(MotionLane current) {
        return swap(m_redo, m_undo, std::move(current));
    }

    [[nodiscard]] bool can_undo() const { return m_undo.has_value(); }
    [[nodiscard]] bool can_redo() const { return m_redo.has_value(); }

    void clear() {
        m_undo.reset();
        m_redo.reset();
    }

private:
    static std::optional<MotionLane> swap(std::optional<MotionLane>& from,
                                          std::optional<MotionLane>& to,
                                          MotionLane current) {
        if (!from) { return std::nullopt; }
        std::optional<MotionLane> restored = std::move(from);
        from.reset();
        to = std::move(current);
        return restored;
    }

    std::optional<MotionLane> m_undo;
    std::optional<MotionLane> m_redo;
};

}  // namespace thl::modulation::detail
