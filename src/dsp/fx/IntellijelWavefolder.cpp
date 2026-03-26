#include <tanh/dsp/fx/IntellijelWavefolder.h>

#include <algorithm>
#include <cmath>

namespace thl::dsp::fx {

IntellijelWavefolderImpl::IntellijelWavefolderImpl() = default;
IntellijelWavefolderImpl::~IntellijelWavefolderImpl() = default;

void IntellijelWavefolderImpl::prepare(const double& /*sample_rate*/,
                                       const size_t& /*samples_per_block*/,
                                       const size_t& /*num_channels*/) {
    // Stateless — nothing to initialise.
}

void IntellijelWavefolderImpl::process(thl::dsp::audio::AudioBufferView buffer,
                                       uint32_t modulation_offset) {
    const float drive     = std::clamp(get_parameter<float>(Drive, modulation_offset),    0.1f, 20.0f);
    const float folds     = std::clamp(get_parameter<float>(Folds, modulation_offset),    0.0f, 10.0f);
    const float symmetry  = get_parameter<float>(Symmetry, modulation_offset);
    const float jfet_tone = std::clamp(get_parameter<float>(JfetTone, modulation_offset), 0.0f,  1.0f);

    const size_t num_channels = buffer.get_num_channels();
    const size_t num_frames   = buffer.get_num_frames();

    for (size_t ch = 0; ch < num_channels; ++ch) {
        float* data = buffer.get_write_pointer(ch);
        for (size_t i = 0; i < num_frames; ++i)
            data[i] = process_sample(data[i], drive, folds, symmetry, jfet_tone);
    }
}

float IntellijelWavefolderImpl::process_sample(float x, float drive, float folds,
                                               float symmetry, float jfet_tone) {
    float s = (x + symmetry) * drive;
    s = std::sin(s * (1.0f + folds));

    if (jfet_tone > 0.0f) {
        const float saturated = jfet_saturate(s);
        s = s * (1.0f - jfet_tone) + saturated * jfet_tone;
    }
    return s;
}

float IntellijelWavefolderImpl::jfet_saturate(float x) {
    const float sym = std::tanh(x * 1.5f);
    return x > 0.0f ? sym * 0.95f : sym;
}

}  // namespace thl::dsp::fx
