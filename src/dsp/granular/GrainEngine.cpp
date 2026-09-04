#include <tanh/dsp/granular/ChannelMixer.h>
#include <tanh/dsp/granular/GrainEngine.h>
#include <tanh/dsp/granular/GrainVisualizer.h>
#include <tanh/dsp/granular/GranularTypes.h>
#include <tanh/dsp/granular/HeadPolicy.h>
#include <tanh/dsp/granular/SampleReader.h>
#include <tanh/dsp/granular/SampleRegion.h>
#include <tanh/dsp/granular/VoiceParams.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace thl::dsp::granular {

GrainEngine::GrainEngine(const SampleReader& reader, GrainVisualizer& viz)
    : m_reader(reader)
    , m_viz(viz)
    , m_random_generator(std::random_device{}())
    , m_uni_dist(0.0f, 1.0f) {
    m_grains.resize(k_max_grains);
    for (auto& grain : m_grains) { grain.m_active = false; }
}

void GrainEngine::prepare(double sample_rate, size_t num_channels) {
    m_sample_rate = sample_rate;
    m_channels = std::min(num_channels, k_max_channel_support);
    for (auto& grain : m_grains) {
        grain.m_envelope.set_sample_rate(static_cast<float>(m_sample_rate));
    }
    m_next_grain_time = 0;
}

HeadPolicy& GrainEngine::head_for(EngineMode mode) {
    return mode == EngineMode::GranularPosition ? static_cast<HeadPolicy&>(m_position_head)
                                                : static_cast<HeadPolicy&>(m_loop_head);
}

void GrainEngine::reset_schedule(EngineMode mode) {
    m_next_grain_time = 0;
    head_for(mode).reset();
}

void GrainEngine::deactivate_all() {
    for (size_t gi = 0; gi < m_grains.size(); ++gi) {
        if (!m_grains[gi].m_active) { continue; }
        m_grains[gi].m_active = false;
        m_viz.grain_finished(static_cast<int>(gi));
    }
}

void GrainEngine::render(const AudioBlock& block,
                         const VoiceParams& params,
                         EngineMode mode,
                         size_t playback_elapsed_samples) {
    update_trigger_rate(params.m_density);
    const Bank bank = select_bank(params.m_sample_index);
    HeadPolicy& head = head_for(mode);
    size_t const write_channels = std::min(m_channels, block.m_num_channels);

    for (size_t i = 0; i < block.m_num_frames; ++i) {
        trigger_due_grain(bank, params, head, playback_elapsed_samples + i);

        channel_mixer::Frame frame{};
        mix_grains_frame(bank, params, frame);

        for (size_t ch = 0; ch < write_channels; ++ch) { block.m_channels[ch][i] = frame[ch]; }
    }

    report_visualization(block.m_num_frames, bank);
}

void GrainEngine::update_trigger_rate(float density) {
    // Density maps exponentially in rate so equal knob travel gives equal
    // rate ratios. (Clamped in the snapshot: an LFO trough can't collapse
    // the trigger rate and silence a held voice.)
    float const rate = k_min_grain_rate * std::pow(k_max_grain_rate / k_min_grain_rate, density);
    m_min_grain_interval = static_cast<size_t>(m_sample_rate / rate);
}

GrainEngine::Bank GrainEngine::select_bank(int raw_sample_index) {
    Bank bank;
    bank.m_index = m_reader.num_banks() > 0 ? m_reader.clamp_bank(raw_sample_index) : 0;
    bank.m_frames = m_reader.num_frames(bank.m_index);
    bank.m_channels = std::max<size_t>(1, m_reader.num_channels(bank.m_index));
    if (m_current_bank != bank.m_index) {
        m_current_bank = bank.m_index;
        m_next_grain_time = 0;  // a pitch-bank switch retriggers at once
    }
    return bank;
}

void GrainEngine::trigger_due_grain(const Bank& bank,
                                    const VoiceParams& params,
                                    HeadPolicy& head,
                                    size_t playback_elapsed_samples) {
    if (m_next_grain_time > 0) {
        m_next_grain_time--;
        return;
    }
    trigger_grain(bank, params, head, playback_elapsed_samples);
    m_next_grain_time = m_min_grain_interval - 1;
}

void GrainEngine::mix_grains_frame(const Bank& bank,
                                   const VoiceParams& params,
                                   channel_mixer::Frame& frame) {
    for (size_t gi = 0; gi < m_grains.size(); ++gi) {
        auto& grain = m_grains[gi];
        if (!grain.m_active) { continue; }

        // Window
        float const normalized_position =
            static_cast<float>(grain.m_current_position) / static_cast<float>(grain.m_grain_size);
        float const envelope = grain.m_envelope.process_at_position(normalized_position);
        if (!grain.m_envelope.is_active() || normalized_position >= 1.0f) {
            grain.m_active = false;
            m_viz.grain_finished(static_cast<int>(gi));
            continue;
        }

        // Read, pan, accumulate
        float const source_pos = static_cast<float>(grain.m_start_position) +
                                 (static_cast<float>(grain.m_current_position) * grain.m_velocity);
        float const pan = 0.5f + (grain.m_position_spread - 0.5f) * params.m_spread;
        channel_mixer::accumulate_grain(m_reader,
                                        source_pos,
                                        grain.m_sample_index,
                                        bank.m_channels,
                                        params.m_channel_mode,
                                        pan,
                                        envelope,
                                        m_channels,
                                        frame);

        // Advance
        grain.m_current_position++;
        if (grain.m_current_position >= grain.m_grain_size) {
            grain.m_active = false;
            m_viz.grain_finished(static_cast<int>(gi));
        }
    }
}

void GrainEngine::report_visualization(size_t num_frames, const Bank& bank) {
    if (bank.m_frames == 0 || !m_viz.grain_frame_due(num_frames)) { return; }
    auto const total_f = static_cast<float>(bank.m_frames);
    m_viz.report_master_level();
    for (size_t gi = 0; gi < m_grains.size(); ++gi) {
        auto& grain = m_grains[gi];
        if (!grain.m_active) { continue; }
        float const current_pos =
            (static_cast<float>(grain.m_start_position) +
             static_cast<float>(grain.m_current_position) * grain.m_velocity) /
            total_f;
        float const normalized_position =
            static_cast<float>(grain.m_current_position) / static_cast<float>(grain.m_grain_size);
        float const envelope = grain.m_envelope.process_at_position(normalized_position);
        m_viz.grain_updated(static_cast<int>(gi), current_pos, envelope);
    }
}

void GrainEngine::trigger_grain(const Bank& bank,
                                const VoiceParams& params,
                                HeadPolicy& head,
                                size_t playback_elapsed_samples) {
    Grain* grain = find_free_grain();
    if (grain == nullptr || !m_reader.bank_valid(bank.m_index)) { return; }

    // Draw order matters for the RNG stream: size, velocity, head, pan.
    size_t grain_size = calculate_grain_size(params.m_size, params.m_temperature_size);
    // Modulation can drive velocity to zero or negative, which would
    // produce empty grains (silence) via the size maths below.
    float const velocity =
        std::max(calculate_velocity(params.m_velocity, params.m_temperature_velocity), 0.01f);

    auto const region = head.region(bank.m_frames, params);
    if (region.size() == 0) { return; }
    float const temperature =
        apply_temperature_ramp(params.m_temperature_position, playback_elapsed_samples);
    long const start =
        head.pick_start(region, temperature, m_min_grain_interval, params, m_random_generator);

    size_t const covered = fit_to_region(start, region, velocity, grain_size);
    if (covered == 0) { return; }

    start_grain(*grain, start, grain_size, velocity, bank.m_index);

    auto const total = static_cast<float>(bank.m_frames);
    m_viz.grain_triggered(
        static_cast<int>(grain - m_grains.data()),
        static_cast<float>(start) / total,
        static_cast<float>(covered) / total,
        velocity,
        static_cast<float>(grain_size) / static_cast<float>(m_sample_rate) * 1000.0f);
}

Grain* GrainEngine::find_free_grain() {
    for (auto& grain : m_grains) {
        if (!grain.m_active) { return &grain; }
    }
    return nullptr;
}

size_t GrainEngine::fit_to_region(long start,
                                  const SampleRegion& region,
                                  float velocity,
                                  size_t& grain_size) {
    auto covered = static_cast<size_t>(std::ceil(static_cast<float>(grain_size) * velocity));
    long const end_frame = static_cast<long>(region.m_end);
    long const grain_end = start + static_cast<long>(covered);
    if (grain_end <= end_frame) { return covered; }
    covered = static_cast<size_t>(std::max(0L, end_frame - start));
    if (covered == 0) { return 0; }
    grain_size = std::max(static_cast<size_t>(1),
                          static_cast<size_t>(static_cast<float>(covered) / velocity));
    return covered;
}

void GrainEngine::start_grain(Grain& grain,
                              long start,
                              size_t grain_size,
                              float velocity,
                              size_t bank) {
    grain.m_start_position = static_cast<size_t>(start);
    grain.m_current_position = 0;
    grain.m_grain_size = grain_size;
    grain.m_velocity = velocity;
    grain.m_active = true;
    grain.m_sample_index = bank;
    grain.m_position_spread = m_uni_dist(m_random_generator);

    float const duration_ms =
        (static_cast<float>(grain_size) / static_cast<float>(m_sample_rate)) * 1000.0f;
    grain.m_envelope.set_sample_rate(static_cast<float>(m_sample_rate));
    grain.m_envelope.set_duration(duration_ms);
    grain.m_envelope.start();
}

size_t GrainEngine::calculate_grain_size(float grain_size_param, float temperature) {
    auto min_size = static_cast<size_t>(k_min_grain_size * m_sample_rate);
    auto max_size = static_cast<size_t>(k_max_grain_size * m_sample_rate);
    size_t const range = max_size - min_size;
    size_t grain_size =
        min_size + static_cast<size_t>(grain_size_param * static_cast<float>(range));

    // Randomize grain size based on temperature
    float rand_value = m_uni_dist(m_random_generator);  // [0, 1)
    rand_value = (rand_value * 2.f - 1.f) / 2.f;        // [-0.5, 0.5)
    rand_value *= std::pow(temperature, 3.f);
    auto lower_interval = static_cast<float>(grain_size - min_size);
    auto upper_interval = static_cast<float>(max_size - grain_size);
    if (rand_value < 0.f) {
        grain_size =
            static_cast<size_t>(static_cast<float>(grain_size) + lower_interval * rand_value);
    } else {
        // Do not make the grain size much larger for small values
        grain_size = static_cast<size_t>(static_cast<float>(grain_size) +
                                         upper_interval * rand_value * 0.3f +
                                         static_cast<float>(grain_size) * rand_value);
    }
    return std::clamp(grain_size, min_size, max_size);
}

float GrainEngine::calculate_velocity(float velocity, float temperature) {
    float velocity_factor = m_uni_dist(m_random_generator);  // [0, 1)
    velocity_factor = (velocity_factor * 2.f - 1.f);         // [-1, 1)
    velocity_factor *= std::pow(temperature, 15.f);
    float const semitone_factor = std::pow(2.f, 7.f / 12.f);
    if (velocity_factor < 0.f) {
        velocity /= (1.f - velocity_factor * (semitone_factor - 1.f));
    } else {
        velocity *= (1.f + velocity_factor * (semitone_factor - 1.f));
    }
    return velocity;
}

float GrainEngine::apply_temperature_ramp(float temperature,
                                          size_t playback_elapsed_samples) const {
    auto ramp_samples = static_cast<float>(k_temperature_ramp_duration * m_sample_rate);
    float ramp_factor = 1.0f;
    if (static_cast<float>(playback_elapsed_samples) < ramp_samples) {
        ramp_factor = std::sin((static_cast<float>(playback_elapsed_samples) / ramp_samples) *
                               std::numbers::pi_v<float> / 2.0f);
    }
    // Blend between ramped and unramped: low temperature -> ramp active,
    // high -> ramp bypassed
    float const ramped = temperature * ramp_factor;
    return ramped + (temperature - ramped) * temperature;
}

}  // namespace thl::dsp::granular
