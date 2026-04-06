#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace thl {

// ── Parameter type enum (single discriminant for all parameters) ─────────────
enum class ParameterType { Double, Float, Int, Bool, String, Unknown };

// ── Slider polarity ──────────────────────────────────────────────────────────
enum class SliderPolarity {
    Unipolar,  // (linear)
    Bipolar    // (centered)
};

// ── Parameter flags (bitfield covering VST3/CLAP flags) ──────────────────────
namespace ParameterFlags {
static constexpr uint32_t k_hidden = 1 << 0;
static constexpr uint32_t k_read_only = 1 << 1;
static constexpr uint32_t k_is_bypass = 1 << 2;
static constexpr uint32_t k_periodic = 1 << 3;
static constexpr uint32_t k_requires_process = 1 << 4;
static constexpr uint32_t k_automatable = 1 << 5;
static constexpr uint32_t k_modulatable = 1 << 6;
static constexpr uint32_t k_per_note_id = 1 << 8;
static constexpr uint32_t k_per_key = 1 << 9;
static constexpr uint32_t k_per_channel = 1 << 10;
static constexpr uint32_t k_per_port = 1 << 11;
}  // namespace ParameterFlags

// ── Normalization curve function pointer ─────────────────────────────────────
// Stateless math transform: value in [min, max] ↔ normalized in [0, 1].
// nullptr = use m_skew power-law as default curve.
using NormalizeCurve = float (*)(float value, float min, float max);

// ── Range ────────────────────────────────────────────────────────────────────
struct Range {
    float m_min;
    float m_max;
    float m_step;
    float m_skew;
    NormalizeCurve m_to_normalized = nullptr;
    NormalizeCurve m_from_normalized = nullptr;

    Range() : m_min(0.0f), m_max(1.0f), m_step(0.01f), m_skew(1.0f) {}

    Range(float min, float max, float step = 0.01f, float skew = 1.0f)
        : m_min(min), m_max(max), m_step(step), m_skew(skew) {}

    Range(int min, int max, int step = 1)
        : m_min(static_cast<float>(min))
        , m_max(static_cast<float>(max))
        , m_step(static_cast<float>(step))
        , m_skew(1.0f) {}

    int min_int() const { return static_cast<int>(m_min); }
    int max_int() const { return static_cast<int>(m_max); }
    int step_int() const { return static_cast<int>(m_step); }

    static Range boolean() { return {0.0f, 1.0f, 1.0f, 1.0f}; }

    // Convert a plain value to normalized [0, 1].
    // If a custom curve is set, use it; otherwise use the skew power-law.
    float to_normalized(float plain) const {
        if (m_to_normalized) { return m_to_normalized(plain, m_min, m_max); }
        if (m_max == m_min) { return 0.0f; }
        float proportion = (plain - m_min) / (m_max - m_min);
        if (m_skew != 1.0f) { proportion = std::pow(proportion, 1.0f / m_skew); }
        return proportion;
    }

    // Convert a normalized [0, 1] value back to plain.
    float from_normalized(float normalized) const {
        if (m_from_normalized) { return m_from_normalized(normalized, m_min, m_max); }
        float proportion = normalized;
        if (m_skew != 1.0f) { proportion = std::pow(proportion, m_skew); }
        return m_min + (m_max - m_min) * proportion;
    }

    // Quantize a plain value to the nearest step.
    float snap(float plain) const {
        if (m_step <= 0.0f) { return plain; }
        float snapped = m_min + std::round((plain - m_min) / m_step) * m_step;
        if (snapped < m_min) { snapped = m_min; }
        if (snapped > m_max) { snapped = m_max; }
        return snapped;
    }
};

// ── ParameterDefinition ──────────────────────────────────────────────────────
// Immutable description of a parameter. Both the input type (constructed by
// plugin authors via subclasses) and the stored type (embedded as const m_def
// in ParameterRecord). Fields are non-const to allow move semantics;
// immutability is enforced by const m_def on ParameterRecord.
struct ParameterDefinition {
    // RT-relevant (read per-block by wrappers, accessed via ParameterHandle)
    ParameterType m_type = ParameterType::Float;
    Range m_range;

    // Setup (read during init, wrapper queries)
    uint32_t m_id = UINT32_MAX;
    float m_default_value = 0.0f;
    uint32_t m_flags = ParameterFlags::k_automatable | ParameterFlags::k_modulatable;
    SliderPolarity m_polarity = SliderPolarity::Unipolar;

    // UI (read for display, serialization)
    std::string m_name;
    std::string m_short_name;
    std::string m_unit;
    size_t m_decimal_places = 2;
    std::vector<std::string> m_choices;
    std::function<std::string(float)> m_value_to_text;
    std::function<float(const std::string&)> m_text_to_value;

    ParameterDefinition() = default;

    ParameterDefinition(std::string name,
                        ParameterType type,
                        Range range,
                        float default_value,
                        size_t decimal_places,
                        bool automation,
                        bool modulation,
                        std::vector<std::string> choices,
                        SliderPolarity polarity)
        : m_type(type)
        , m_range(range)
        , m_default_value(default_value)
        , m_flags((automation ? ParameterFlags::k_automatable : 0u) |
                  (modulation ? ParameterFlags::k_modulatable : 0u))
        , m_polarity(polarity)
        , m_name(std::move(name))
        , m_decimal_places(decimal_places)
        , m_choices(std::move(choices)) {}

    float as_float() const { return m_default_value; }
    int as_int() const { return static_cast<int>(m_default_value); }
    bool as_bool() const { return m_default_value != 0.0f; }

    bool is_automatable() const { return (m_flags & ParameterFlags::k_automatable) != 0; }
    bool is_modulatable() const { return (m_flags & ParameterFlags::k_modulatable) != 0; }
};

// ── Ergonomic subclass constructors ──────────────────────────────────────────

struct ParameterFloat : public ParameterDefinition {
    ParameterFloat(std::string name,
                   Range range,
                   float default_val,
                   size_t decimal_places = 2,
                   bool automation = true,
                   bool modulation = true,
                   SliderPolarity polarity = SliderPolarity::Unipolar,
                   std::vector<std::string> choices = {})
        : ParameterDefinition(std::move(name),
                              ParameterType::Float,
                              range,
                              default_val,
                              decimal_places,
                              automation,
                              modulation,
                              std::move(choices),
                              polarity) {
        m_value_to_text = [dp = decimal_places](float v) -> std::string {
            std::ostringstream oss;
            oss << std::fixed;
            oss.precision(static_cast<std::streamsize>(dp));
            oss << v;
            return oss.str();
        };
        m_text_to_value = [](const std::string& s) -> float {
            float result = 0.0f;
            auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
            return (ec == std::errc{}) ? result : 0.0f;
        };
    }
};

struct ParameterInt : public ParameterDefinition {
    ParameterInt(std::string name,
                 Range range,
                 int default_val,
                 bool automation = true,
                 bool modulation = true,
                 SliderPolarity polarity = SliderPolarity::Unipolar,
                 std::vector<std::string> choices = {})
        : ParameterDefinition(std::move(name),
                              ParameterType::Int,
                              range,
                              static_cast<float>(default_val),
                              0,
                              automation,
                              modulation,
                              std::move(choices),
                              polarity) {
        m_value_to_text = [](float v) -> std::string {
            return std::to_string(static_cast<int>(v));
        };
        m_text_to_value = [](const std::string& s) -> float {
            int result = 0;
            auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
            return (ec == std::errc{}) ? static_cast<float>(result) : 0.0f;
        };
    }
};

struct ParameterBool : public ParameterDefinition {
    ParameterBool(std::string name,
                  bool default_val = false,
                  bool automation = true,
                  bool modulation = true,
                  SliderPolarity polarity = SliderPolarity::Unipolar,
                  std::vector<std::string> choices = {})
        : ParameterDefinition(std::move(name),
                              ParameterType::Bool,
                              Range::boolean(),
                              default_val ? 1.0f : 0.0f,
                              0,
                              automation,
                              modulation,
                              std::move(choices),
                              polarity) {
        m_value_to_text = [](float v) -> std::string { return v != 0.0f ? "On" : "Off"; };
        m_text_to_value = [](const std::string& s) -> float {
            return (s == "On" || s == "on" || s == "true" || s == "1") ? 1.0f : 0.0f;
        };
    }
};

struct ParameterChoice : public ParameterDefinition {
    ParameterChoice(std::string name,
                    std::vector<std::string> choices,
                    int default_val = 0,
                    bool automation = true,
                    bool modulation = true,
                    SliderPolarity polarity = SliderPolarity::Unipolar)
        : ParameterDefinition{std::move(name),
                              ParameterType::Int,
                              make_range(choices),
                              static_cast<float>(default_val),
                              0,
                              automation,
                              modulation,
                              std::move(choices),
                              polarity} {
        m_value_to_text = [c = m_choices](float v) -> std::string {
            auto idx = static_cast<size_t>(v);
            return idx < c.size() ? c[idx] : std::to_string(static_cast<int>(v));
        };
        m_text_to_value = [c = m_choices](const std::string& s) -> float {
            for (size_t i = 0; i < c.size(); ++i) {
                if (c[i] == s) { return static_cast<float>(i); }
            }
            // Fall back to parsing as int
            int result = 0;
            auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
            return (ec == std::errc{}) ? static_cast<float>(result) : 0.0f;
        };
    }

private:
    static Range make_range(const std::vector<std::string>& choices) {
        return {0, static_cast<int>(choices.size()) - 1, 1};
    }
};

}  // namespace thl
