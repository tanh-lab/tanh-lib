#pragma once

#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/ParameterBackend.h>
#include <tanh/state/ParameterDefinitions.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace xy_test {

constexpr double k_sr = 48000.0;

/// A host-style ParameterBackend over plain atomics, all parameters in [0, 1].
class FakeBackend : public thl::modulation::ParameterBackend {
public:
    void add(const std::string& key, float default_value) {
        auto e = std::make_unique<Entry>();
        e->m_def =
            thl::ParameterDefinition::make_float(key, thl::Range::linear(0.0f, 1.0f), default_value)
                .modulatable(true);
        e->m_value.store(default_value);
        m_entries[key] = std::move(e);
    }
    std::optional<thl::modulation::ParameterBinding> find(std::string_view key) const override {
        auto it = m_entries.find(key);
        if (it == m_entries.end()) { return std::nullopt; }
        return thl::modulation::ParameterBinding{.m_def = it->second->m_def,
                                                 .m_base = &it->second->m_value};
    }

private:
    struct Entry {
        thl::ParameterDefinition m_def;
        std::atomic<float> m_value{0.0f};
    };
    std::map<std::string, std::unique_ptr<Entry>, std::less<>> m_entries;
};

/// A playing transport at a fixed tempo; next(n) returns the block's info and advances.
struct SimTransport {
    double m_beat = 0.0;
    double m_bpm = 120.0;
    bool m_playing = true;

    thl::dsp::transport::TransportInfo next(uint32_t n) {
        using thl::dsp::transport::TransportInfo;
        TransportInfo t;
        t.m_flags = TransportInfo::k_has_tempo | TransportInfo::k_has_beat_position;
        if (m_playing) { t.m_flags |= TransportInfo::k_is_playing; }
        t.m_bpm = m_bpm;
        t.m_beat_position = m_beat;
        t.m_beats_per_sample = m_playing ? m_bpm / (60.0 * k_sr) : 0.0;
        t.m_num_samples = n;
        m_beat = t.beat_end();
        return t;
    }
};

}  // namespace xy_test
