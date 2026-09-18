// signalsmith-linear's fft.h calls std::memcpy without including <cstring>
// (libstdc++ doesn't pull it in transitively): include it first.
// clang-format off
#include <cstring>
#include <signalsmith-stretch/signalsmith-stretch.h>
// clang-format on

#include <tanh/core/Buffer.h>
#include <tanh/dsp/pitch/PitchBank.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <set>
#include <span>
#include <thread>
#include <vector>

namespace thl::dsp::pitch {

namespace {

// Planar float storage addressed as buffer[channel][frame] with a read
// offset, the shape Signalsmith Stretch reads and writes.
struct PlanarBuffer {
    float* m_data;
    size_t m_stride;
    int m_offset = 0;

    struct Channel {
        float* m_base;
        int m_offset;
        float operator[](int i) const { return m_base[m_offset + i]; }
        float& operator[](int i) { return m_base[m_offset + i]; }
    };

    Channel operator[](int c) const {
        return Channel{.m_base = m_data + static_cast<std::ptrdiff_t>(c) *
                                              static_cast<std::ptrdiff_t>(m_stride),
                       .m_offset = m_offset};
    }
};

void shift_into(core::BufferF& output,
                const core::BufferF& input,
                float semitones,
                double sample_rate,
                const PitchBank::Settings& settings,
                std::vector<float>& padded_input,
                std::vector<float>& out_flat) {
    size_t const num_frames = input.get_num_samples();
    size_t const num_channels = input.get_num_channels();

    if (std::abs(semitones) < 0.001f || settings.m_copy_only) {
        for (size_t ch = 0; ch < num_channels; ++ch) {
            std::memcpy(output.get_write_pointer(ch),
                        input.get_read_pointer(ch),
                        num_frames * sizeof(float));
        }
        return;
    }

    signalsmith::stretch::SignalsmithStretch<float> stretch;
    stretch.presetCheaper(static_cast<int>(num_channels), static_cast<float>(sample_rate));
    stretch.setTransposeSemitones(semitones,
                                  settings.m_tonality_limit_hz / static_cast<float>(sample_rate));

    int const seek_length = static_cast<int>(stretch.outputSeekLength(1.0));
    int const output_pos = static_cast<int>(num_frames) + stretch.outputLatency();
    int const input_index = output_pos + stretch.inputLatency();
    auto const padded_length = static_cast<size_t>(input_index);

    // Scratch only grows across calls on one thread.
    size_t const padded_total = padded_length * num_channels;
    if (padded_input.size() < padded_total) { padded_input.resize(padded_total); }
    std::fill(padded_input.begin(),
              padded_input.begin() + static_cast<std::ptrdiff_t>(padded_total),
              0.0f);
    size_t const out_total = num_frames * num_channels;
    if (out_flat.size() < out_total) { out_flat.resize(out_total); }
    std::fill(out_flat.begin(), out_flat.begin() + static_cast<std::ptrdiff_t>(out_total), 0.0f);

    for (size_t ch = 0; ch < num_channels; ++ch) {
        std::memcpy(padded_input.data() + ch * padded_length,
                    input.get_read_pointer(ch),
                    num_frames * sizeof(float));
    }

    PlanarBuffer in_buf{.m_data = padded_input.data(), .m_stride = padded_length, .m_offset = 0};
    PlanarBuffer const out_buf{.m_data = out_flat.data(), .m_stride = num_frames, .m_offset = 0};
    stretch.outputSeek(in_buf, seek_length);
    in_buf.m_offset = seek_length;
    stretch.process(in_buf, input_index - seek_length, out_buf, static_cast<int>(num_frames));

    size_t const fade_length = std::min(settings.m_edge_fade_frames, num_frames / 2);
    for (size_t ch = 0; ch < num_channels; ++ch) {
        float* data = out_flat.data() + ch * num_frames;
        for (size_t s = 0; s < fade_length; ++s) {
            float const g = static_cast<float>(s) / static_cast<float>(fade_length);
            data[s] *= g;
            data[num_frames - 1 - s] *= g;
        }
    }

    for (size_t ch = 0; ch < num_channels; ++ch) {
        std::memcpy(output.get_write_pointer(ch),
                    out_flat.data() + ch * num_frames,
                    num_frames * sizeof(float));
    }
}

}  // namespace

bool PitchBank::shift(core::BufferF& output,
                      const core::BufferF& input,
                      float semitones,
                      double sample_rate) const {
    if (!(sample_rate > 0.0)) { return false; }
    std::vector<float> padded_input;
    std::vector<float> out_flat;
    shift_into(output, input, semitones, sample_rate, m_settings, padded_input, out_flat);
    return true;
}

bool PitchBank::build(std::vector<core::BufferF>& bank,
                      std::span<const int> semitones,
                      double sample_rate) const {
    if (!(sample_rate > 0.0)) { return false; }
    size_t const root = root_index();
    if (root >= bank.size()) { return true; }
    const core::BufferF& input = bank[root];
    if (input.empty()) { return true; }

    size_t const num_frames = input.get_num_samples();
    size_t const num_channels = input.get_num_channels();

    std::vector<size_t> selected;
    selected.reserve(semitones.size());
    std::set<int> seen;
    for (int const s : semitones) {
        if (s < m_settings.m_min_semitones || s > m_settings.m_max_semitones) { continue; }
        if (!seen.insert(s).second) { continue; }
        size_t const index = index_of(s);
        if (index == root || index >= bank.size()) { continue; }
        selected.push_back(index);
    }
    if (selected.empty()) { return true; }

    // Allocate every output before the workers start: they only write.
    for (size_t const index : selected) {
        bank[index] = core::BufferF(num_channels, num_frames, sample_rate);
    }

    unsigned threads = m_settings.m_max_threads;
    if (threads == 0) { threads = std::max(1u, std::thread::hardware_concurrency() / 2); }
    threads = std::min<unsigned>(threads, static_cast<unsigned>(selected.size()));

    auto const work = [&](size_t begin, size_t end) {
        std::vector<float> padded_input;
        std::vector<float> out_flat(num_frames * num_channels);
        for (size_t i = begin; i < end; ++i) {
            size_t const index = selected[i];
            shift_into(bank[index],
                       input,
                       static_cast<float>(semitones_of(index)),
                       sample_rate,
                       m_settings,
                       padded_input,
                       out_flat);
        }
    };

    std::vector<std::thread> workers;
    size_t const per_thread = selected.size() / threads;
    size_t const remainder = selected.size() % threads;
    size_t begin = 0;
    for (unsigned t = 0; t < threads; ++t) {
        size_t const end = begin + per_thread + (t < remainder ? 1 : 0);
        workers.emplace_back(work, begin, end);
        begin = end;
    }
    for (auto& worker : workers) { worker.join(); }
    return true;
}

}  // namespace thl::dsp::pitch
