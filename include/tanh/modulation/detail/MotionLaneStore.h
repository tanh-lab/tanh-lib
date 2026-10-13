#pragma once

#include <tanh/core/Exports.h>
#include <tanh/core/threading/RCU.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionLaneProcessing.h>
#include <tanh/modulation/detail/MotionHistory.h>
#include <tanh/modulation/detail/MotionTakeRecorder.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace thl::modulation::detail {

/// The RCU payload: the published lane and what plays of it.
struct MotionPlayedLane {
    MotionLane m_lane;       ///< raw, as published
    std::vector<float> m_x;  ///< what plays: seam closed and smoothed
    std::vector<float> m_y;
    /// The lane with its seam closed but unsmoothed, when m_x / m_y differ from it: what an
    /// overwriting take starts from, so overdubs are not smoothed again. Empty: m_x / m_y.
    std::vector<float> m_seed_x;
    std::vector<float> m_seed_y;
    bool m_smoothed = false;  ///< m_x / m_y differ from the raw lane
    uint32_t m_version = 0;   ///< per publication, also for a new curve of the same lane
};

/// What the audio thread plays in one block: the published lane or a submitted take.
struct MotionLaneSource {
    MotionLoopView m_view;   ///< what plays
    MotionLoopView m_seed;   ///< the same, unsmoothed
    uint32_t m_id = 0;       ///< take id; 0: nothing
    uint32_t m_version = 0;  ///< a new curve of the same id (smoothing): glide in place
    /// What plays differs from the take as it was handed over (smoothing, LoopEnd::Jump).
    bool m_smoothed = false;
    [[nodiscard]] bool empty() const { return m_view.empty(); }
};

/**
 * @brief Hands takes from the audio thread to the message thread and publishes lanes.
 *
 * Two preallocated slots move through Free, Ready (submit(), audio thread), Published
 * (service(), message thread) and back to Free (select(), audio thread, once the published
 * lane replaces them). A slot plays from the block it is submitted in, its seam closed in
 * place for LoopEnd::Smooth (the raw values kept, so the published lane is raw). Take ids come
 * from one counter shared with load(), clear(), undo() and redo(): the newest id plays.
 *
 * Every publication can be undone once, except the revisions of one take: only its first
 * keeps the lane it replaced (one take, one undo step).
 *
 * Threads: prepare: message thread, audio stopped. submit, select, read_scope: audio thread.
 * service, load, clear, undo, redo, clear_history, set_smoothing, set_loop_end: one message
 * thread (allocates). lane, played_lane, read_lane, version: message or UI thread.
 */
class TANH_API MotionLaneStore {
public:
    MotionLaneStore();

    void prepare(size_t max_points);

    /// Audio thread: hand @p take over (copied), the revision of take @p serial; false when
    /// both slots wait for service().
    [[nodiscard]] bool submit(const MotionLoopView& take,
                              uint32_t serial) TANH_NONBLOCKING_FUNCTION;
    /// Audio thread: the newest of @p published and the submitted takes; frees the slots
    /// @p published replaced.
    [[nodiscard]] MotionLaneSource select(const MotionPlayedLane& published)
        TANH_NONBLOCKING_FUNCTION;
    /// Audio thread: the published lane for this block.
    [[nodiscard]] RCU<MotionPlayedLane>::ReadScope read_scope() const TANH_NONBLOCKING_FUNCTION {
        return m_lanes.read_scope(m_audio_reader);
    }

    /// Publish submitted takes; true if a lane was published.
    bool service();
    /// Publish @p lane (validated, clamped, resampled down to @p max_points); its id, or 0 if it
    /// is malformed.
    uint32_t load(MotionLane lane, size_t max_points);
    /// Publish an empty lane; its id.
    uint32_t clear();
    /// Any thread: forget what take @p serial and every later take hand over until
    /// stop_dropping_after() (takes cleared while they run): it neither plays nor is published.
    void drop_takes_from(uint32_t serial) {
        m_dropped.store((uint64_t{serial} << 32) | 0xFFFFFFFFu, std::memory_order_release);
    }
    /// Takes after @p serial play again (the audio thread ended the cleared take).
    void stop_dropping_after(uint32_t serial) {
        uint64_t range = m_dropped.load(std::memory_order_acquire);
        while (!m_dropped.compare_exchange_weak(range,
                                                ((range >> 32) << 32) | serial,
                                                std::memory_order_acq_rel)) {}
    }
    bool undo();
    bool redo();
    [[nodiscard]] bool can_undo() const { return m_history.can_undo(); }
    [[nodiscard]] bool can_redo() const { return m_history.can_redo(); }
    void clear_history() { m_history.clear(); }

    void set_smoothing(float amount);
    [[nodiscard]] float smoothing() const { return m_smoothing.load(std::memory_order_relaxed); }
    void set_loop_end(LoopEnd mode);
    [[nodiscard]] LoopEnd loop_end() const { return m_loop_end.load(std::memory_order_relaxed); }

    [[nodiscard]] MotionLane lane() const;
    [[nodiscard]] MotionLane played_lane() const;
    template <typename F>
    void read_lane(F&& f) const {
        m_lanes.read([&](const MotionPlayedLane& l) { f(l.m_lane); });
    }
    /// Bumped by every publication, also of a new curve.
    [[nodiscard]] uint32_t version() const { return m_version.load(std::memory_order_acquire); }

private:
    enum class SlotState : uint8_t { Free, Ready, Published };

    struct Slot {
        std::atomic<SlotState> m_state{SlotState::Free};
        std::vector<float> m_x;
        std::vector<float> m_y;
        std::vector<uint8_t> m_gate;
        std::vector<float> m_raw_tail_x;  // the raw values of the points the seam closing moved
        std::vector<float> m_raw_tail_y;
        // Written by the audio thread before Ready (release), read after (acquire).
        MotionLoopView m_view;
        size_t m_seam_window = 0;
        uint32_t m_id = 0;
        uint32_t m_serial = 0;
    };

    void publish(MotionLane lane, bool record_history);
    void publish_played(MotionLane lane);
    [[nodiscard]] MotionLaneSource slot_source(const Slot& slot) const TANH_NONBLOCKING_FUNCTION;

    RCU<MotionPlayedLane> m_lanes;
    RCU<MotionPlayedLane>::Reader m_audio_reader;
    std::array<Slot, 2> m_slots;
    MotionHistory m_history;
    std::atomic<uint32_t> m_next_id{1};
    std::atomic<uint64_t> m_dropped{0};  // serials [from, to] as (from << 32) | to

    [[nodiscard]] bool dropped(uint32_t serial) const {
        const uint64_t range = m_dropped.load(std::memory_order_acquire);
        return serial != 0 && serial >= (range >> 32) && serial <= (range & 0xFFFFFFFFu);
    }
    std::atomic<uint32_t> m_version{0};
    std::atomic<float> m_smoothing{0.0f};
    std::atomic<LoopEnd> m_loop_end{LoopEnd::Smooth};
    uint32_t m_published_id = 0;    // message thread
    uint32_t m_played_version = 0;  // message thread
    uint32_t m_history_serial = 0;  // message thread: the take whose first revision was kept
};

}  // namespace thl::modulation::detail
