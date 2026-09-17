#pragma once

#include <tanh/core/Buffer.h>
#include <tanh/core/Exports.h>

#include <cstddef>
#include <span>
#include <vector>

namespace thl::dsp::pitch {

/**
 * @class PitchBank
 * @brief Builds a bank of pitch-shifted copies of a sample, one slot per
 * semitone, for players that switch pitch by switching source.
 *
 * Slot `index_of(s)` holds the root transposed by `s` semitones over
 * [min_semitones, max_semitones]; the root sits at root_index(). Every copy
 * keeps the root's length and timing (a pitch shift, not a resample), so
 * the slots are equal-length and time-aligned, which is what
 * sampler::SamplePlayer and granular::GrainEngine require to switch between
 * them mid-play.
 *
 * Shifting uses Signalsmith Stretch (cheaper preset, tonality limit
 * `m_tonality_limit_hz`) with short linear fades at both edges, spread
 * across worker threads.
 *
 * @par Real-Time Safety
 * Not real-time safe: allocates, spawns threads and takes seconds for long
 * samples. Build on a loader thread and publish the finished bank.
 */
class TANH_API PitchBank {
public:
    struct Settings {
        int m_min_semitones{-24};
        int m_max_semitones{24};
        /// Worker threads; 0 = half the hardware threads (at least one).
        unsigned m_max_threads{0};
        float m_tonality_limit_hz{8000.0f};
        /// Linear fade at both edges of every shifted copy.
        size_t m_edge_fade_frames{64};
        /// Copy the root into the selected slots instead of shifting (fast,
        /// for debugging and tests).
        bool m_copy_only{false};
    };

    PitchBank() = default;
    explicit PitchBank(const Settings& settings) : m_settings(settings) {}

    [[nodiscard]] const Settings& settings() const { return m_settings; }

    /// Slots in a full bank (max - min + 1).
    [[nodiscard]] size_t num_slots() const {
        return static_cast<size_t>(m_settings.m_max_semitones - m_settings.m_min_semitones) + 1;
    }
    /// Slot of a transposition (caller keeps it in range).
    [[nodiscard]] size_t index_of(int semitones) const {
        return static_cast<size_t>(semitones - m_settings.m_min_semitones);
    }
    /// Transposition of a slot.
    [[nodiscard]] int semitones_of(size_t index) const {
        return static_cast<int>(index) + m_settings.m_min_semitones;
    }
    [[nodiscard]] size_t root_index() const { return index_of(0); }

    /**
     * @brief Fill the selected slots of `bank` from its root.
     *
     * `bank` must hold num_slots() buffers with the root at root_index().
     * Each selected transposition (out-of-range, duplicate and root entries
     * are skipped) gets a buffer of the root's shape; other slots are left
     * as they are.
     *
     * @param sample_rate The root's sample rate (the shifter's analysis
     *        depends on it).
     * @return false, building nothing, when `sample_rate` is not positive.
     */
    bool build(std::vector<core::BufferF>& bank,
               std::span<const int> semitones,
               double sample_rate) const;

    /**
     * @brief Shift `input` by `semitones` into `output` (same shape,
     * pre-allocated).
     * @return false, writing nothing, when `sample_rate` is not positive.
     */
    bool shift(core::BufferF& output,
               const core::BufferF& input,
               float semitones,
               double sample_rate) const;

private:
    Settings m_settings{};
};

}  // namespace thl::dsp::pitch
