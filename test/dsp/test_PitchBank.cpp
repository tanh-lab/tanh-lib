// pitch::PitchBank: slot mapping, selective fills and the pitch of the
// shifted copies.

#include <gtest/gtest.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/Numbers.h>
#include <tanh/dsp/pitch/PitchBank.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

using thl::core::BufferF;
using thl::dsp::pitch::PitchBank;

namespace {

BufferF sine(double frequency, double sample_rate, size_t frames) {
    BufferF buffer(1, frames, sample_rate);
    float* data = buffer.get_write_pointer(0);
    for (size_t i = 0; i < frames; ++i) {
        data[i] = static_cast<float>(
            std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(i) / sample_rate));
    }
    return buffer;
}

// Frequency from the zero-crossing rate over [begin, end).
double estimate_frequency(const float* data, size_t begin, size_t end, double sample_rate) {
    size_t crossings = 0;
    for (size_t i = begin + 1; i < end; ++i) {
        if ((data[i - 1] >= 0.0f) != (data[i] >= 0.0f)) { ++crossings; }
    }
    double const seconds = static_cast<double>(end - begin) / sample_rate;
    return static_cast<double>(crossings) / (2.0 * seconds);
}

}  // namespace

TEST(PitchBank, SlotsMapSemitonesBothWays) {
    PitchBank const bank;
    EXPECT_EQ(bank.num_slots(), 49u);
    EXPECT_EQ(bank.root_index(), 24u);
    EXPECT_EQ(bank.index_of(-24), 0u);
    EXPECT_EQ(bank.index_of(24), 48u);
    for (int s = -24; s <= 24; ++s) { EXPECT_EQ(bank.semitones_of(bank.index_of(s)), s); }

    PitchBank const octave({.m_min_semitones = -12, .m_max_semitones = 12});
    EXPECT_EQ(octave.num_slots(), 25u);
    EXPECT_EQ(octave.root_index(), 12u);
}

TEST(PitchBank, BuildFillsOnlyTheSelectedSlots) {
    PitchBank const bank;
    std::vector<BufferF> slots(bank.num_slots());
    slots[bank.root_index()] = sine(220.0, 16000.0, 1600);
    // Duplicates, the root and out-of-range entries are skipped.
    const std::vector<int> selected = {-24, -12, 0, 12, 24, 12, 99, -30};
    bank.build(slots, selected);
    for (int s = -24; s <= 24; ++s) {
        bool const expected = s == -24 || s == -12 || s == 0 || s == 12 || s == 24;
        EXPECT_EQ(!slots[bank.index_of(s)].empty(), expected) << "semitone " << s;
    }
    for (int const s : {-24, -12, 12, 24}) {
        const auto& shifted = slots[bank.index_of(s)];
        EXPECT_EQ(shifted.get_num_samples(), 1600u);
        EXPECT_EQ(shifted.get_num_channels(), 1u);
    }
    // No root: nothing to do.
    std::vector<BufferF> empty(bank.num_slots());
    bank.build(empty, selected);
    for (const auto& slot : empty) { EXPECT_TRUE(slot.empty()); }
}

TEST(PitchBank, ShiftedCopiesKeepLengthAndHitTheirPitch) {
    constexpr double k_rate = 48000.0;
    constexpr size_t k_frames = 48000;
    PitchBank const bank;
    std::vector<BufferF> slots(bank.num_slots());
    slots[bank.root_index()] = sine(440.0, k_rate, k_frames);
    const std::vector<int> selected = {-12, -7, 7, 12};
    bank.build(slots, selected);
    for (int const s : selected) {
        const auto& shifted = slots[bank.index_of(s)];
        ASSERT_EQ(shifted.get_num_samples(), k_frames);
        double const expected = 440.0 * std::pow(2.0, s / 12.0);
        // The middle half, clear of the edge fades.
        double const measured =
            estimate_frequency(shifted.get_read_pointer(0), k_frames / 4, k_frames * 3 / 4, k_rate);
        EXPECT_NEAR(measured, expected, expected * 0.05) << "semitone " << s;
    }
}

TEST(PitchBank, CopyOnlyDuplicatesTheRoot) {
    PitchBank const bank({.m_copy_only = true});
    std::vector<BufferF> slots(bank.num_slots());
    slots[bank.root_index()] = sine(220.0, 16000.0, 800);
    const std::vector<int> selected = {5};
    bank.build(slots, selected);
    const auto& copy = slots[bank.index_of(5)];
    ASSERT_EQ(copy.get_num_samples(), 800u);
    EXPECT_TRUE(std::equal(copy.get_read_pointer(0),
                           copy.get_read_pointer(0) + 800,
                           slots[bank.root_index()].get_read_pointer(0)));
}
