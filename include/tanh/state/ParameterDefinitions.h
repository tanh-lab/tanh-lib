#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace thl {

// ── Parameter type enumeration ─────────────────────────────────────────────

enum class ParameterType : uint8_t { Double, Float, Int, Bool, String };

// ── Slider polarity ────────────────────────────────────────────────────────

enum class SliderPolarity : uint8_t { Unipolar, Bipolar };

// ── Parameter flags ────────────────────────────────────────────────────────

namespace ParameterFlags {
constexpr uint32_t k_automatable = 1u << 0;
constexpr uint32_t k_modulatable = 1u << 1;
constexpr uint32_t k_default = k_automatable;
}  // namespace ParameterFlags

// ── Normalization curve ────────────────────────────────────────────────────

/// Function pointer: maps a proportion in [0,1] to another proportion in [0,1].
using NormalizeCurve = float (*)(float proportion);

struct NormalizationCurve {
    enum class Type : uint8_t { Linear, PowerLaw, Custom };

    Type m_type = Type::Linear;
    float m_skew = 1.0f;              // Used only when Type::PowerLaw
    NormalizeCurve m_to = nullptr;    // Custom: normalized -> proportion
    NormalizeCurve m_from = nullptr;  // Custom: proportion -> normalized

    /// Forward direction: normalized [0,1] -> proportion [0,1]
    [[nodiscard]] float apply(float normalized) const {
        switch (m_type) {
            case Type::PowerLaw: return std::pow(normalized, m_skew);
            case Type::Custom: return m_to ? m_to(normalized) : normalized;
            default: return normalized;
        }
    }

    /// Inverse direction: proportion [0,1] -> normalized [0,1]
    [[nodiscard]] float apply_inverse(float proportion) const {
        switch (m_type) {
            case Type::PowerLaw: return m_skew != 0.0f ? std::pow(proportion, 1.0f / m_skew) : 0.0f;
            case Type::Custom: return m_from ? m_from(proportion) : proportion;
            default: return proportion;
        }
    }

    [[nodiscard]] bool is_linear() const {
        return m_type == Type::Linear || (m_type == Type::PowerLaw && m_skew == 1.0f);
    }
};

// ── Range ──────────────────────────────────────────────────────────────────

struct Range {
    float m_min = 0.0f;
    float m_max = 1.0f;
    float m_step = 0.01f;
    bool m_periodic = false;
    NormalizationCurve m_curve;

    // ── Named factories ────────────────────────────────────────────────

    static Range linear(float min, float max, float step = 0.01f) {
        Range r;
        r.m_min = min;
        r.m_max = max;
        r.m_step = step;
        return r;
    }

    static Range power_law(float min, float max, float skew, float step = 0.01f) {
        Range r;
        r.m_min = min;
        r.m_max = max;
        r.m_step = step;
        r.m_curve.m_type = NormalizationCurve::Type::PowerLaw;
        r.m_curve.m_skew = skew;
        return r;
    }

    static Range custom(float min,
                        float max,
                        NormalizeCurve to_fn,
                        NormalizeCurve from_fn,
                        float step = 0.01f) {
        Range r;
        r.m_min = min;
        r.m_max = max;
        r.m_step = step;
        r.m_curve.m_type = NormalizationCurve::Type::Custom;
        r.m_curve.m_to = to_fn;
        r.m_curve.m_from = from_fn;
        return r;
    }

    static Range discrete(int min, int max, int step = 1) {
        Range r;
        r.m_min = static_cast<float>(min);
        r.m_max = static_cast<float>(max);
        r.m_step = static_cast<float>(step);
        return r;
    }

    static Range boolean() {
        Range r;
        r.m_min = 0.0f;
        r.m_max = 1.0f;
        r.m_step = 1.0f;
        return r;
    }

    static Range periodic(float min, float max, float step = 0.01f) {
        Range r;
        r.m_min = min;
        r.m_max = max;
        r.m_step = step;
        r.m_periodic = true;
        return r;
    }

    // ── Normalization ──────────────────────────────────────────────────

    [[nodiscard]] float to_normalized(float plain) const {
        if (m_max == m_min) { return 0.0f; }
        const float proportion = (plain - m_min) / (m_max - m_min);
        return m_curve.apply_inverse(proportion);
    }

    [[nodiscard]] float from_normalized(float normalized) const {
        const float proportion = m_curve.apply(normalized);
        return m_min + (m_max - m_min) * proportion;
    }

    // ── Bounds enforcement ─────────────────────────────────────────────

    [[nodiscard]] float clamp(float v) const { return std::clamp(v, m_min, m_max); }

    [[nodiscard]] float wrap(float v) const {
        const float range = m_max - m_min;
        if (range <= 0.0f) { return m_min; }
        const float r = std::fmod(v - m_min, range);
        return r < 0.0f ? r + range + m_min : r + m_min;
    }

    [[nodiscard]] float constrain(float v) const { return m_periodic ? wrap(v) : clamp(v); }

    // ── Quantization ───────────────────────────────────────────────────

    [[nodiscard]] float snap(float plain) const {
        if (m_step <= 0.0f) { return plain; }
        return std::round((plain - m_min) / m_step) * m_step + m_min;
    }

    // ── Query ──────────────────────────────────────────────────────────

    [[nodiscard]] bool is_linear() const { return m_curve.is_linear(); }
};

// ── ParameterDefinition ────────────────────────────────────────────────────

struct ParameterDefinition {
    // ── Core fields ────────────────────────────────────────────────────

    std::string m_name;
    ParameterType m_type = ParameterType::Float;
    Range m_range;
    float m_default_value = 0.0f;
    size_t m_decimal_places = 2;
    uint32_t m_flags = ParameterFlags::k_default;
    SliderPolarity m_polarity = SliderPolarity::Unipolar;
    uint32_t m_id = UINT32_MAX;
    std::string m_unit;
    std::string m_short_name;
    std::vector<std::string> m_choices;

    // ── Display formatters ─────────────────────────────────────────────

    std::function<std::string(float)> m_value_to_text;
    std::function<float(const std::string&)> m_text_to_value;

    // ── Convenience accessors ──────────────────────────────────────────

    [[nodiscard]] float as_float() const { return m_default_value; }
    [[nodiscard]] int as_int() const { return static_cast<int>(m_default_value); }
    [[nodiscard]] bool as_bool() const { return m_default_value != 0.0f; }

    [[nodiscard]] bool is_automatable() const {
        return (m_flags & ParameterFlags::k_automatable) != 0;
    }
    [[nodiscard]] bool is_modulatable() const {
        return (m_flags & ParameterFlags::k_modulatable) != 0;
    }

    // ── Chainable setters ──────────────────────────────────────────────

    ParameterDefinition& unit(std::string u) {
        m_unit = std::move(u);
        return *this;
    }
    ParameterDefinition& short_name(std::string s) {
        m_short_name = std::move(s);
        return *this;
    }
    ParameterDefinition& decimal_places(size_t dp) {
        m_decimal_places = dp;
        return *this;
    }
    ParameterDefinition& polarity(SliderPolarity p) {
        m_polarity = p;
        return *this;
    }
    ParameterDefinition& flags(uint32_t f) {
        m_flags = f;
        return *this;
    }
    ParameterDefinition& automatable(bool v) {
        if (v) {
            m_flags |= ParameterFlags::k_automatable;
        } else {
            m_flags &= ~ParameterFlags::k_automatable;
        }
        return *this;
    }
    ParameterDefinition& modulatable(bool v) {
        if (v) {
            m_flags |= ParameterFlags::k_modulatable;
        } else {
            m_flags &= ~ParameterFlags::k_modulatable;
        }
        return *this;
    }
    ParameterDefinition& param_id(uint32_t i) {
        m_id = i;
        return *this;
    }
    ParameterDefinition& choices(std::vector<std::string> c) {
        m_choices = std::move(c);
        return *this;
    }
    ParameterDefinition& value_to_text_fn(std::function<std::string(float)> fn) {
        m_value_to_text = std::move(fn);
        return *this;
    }
    ParameterDefinition& text_to_value_fn(std::function<float(const std::string&)> fn) {
        m_text_to_value = std::move(fn);
        return *this;
    }

    // ── Static factories ───────────────────────────────────────────────

    static ParameterDefinition make_float(std::string name,
                                          Range range,
                                          float default_val,
                                          size_t dp = 2) {
        ParameterDefinition def;
        def.m_name = std::move(name);
        def.m_type = ParameterType::Float;
        def.m_range = range;
        def.m_default_value = default_val;
        def.m_decimal_places = dp;
        def.m_value_to_text = make_float_formatter(dp);
        def.m_text_to_value = make_float_parser();
        return def;
    }

    static ParameterDefinition make_int(std::string name, Range range, int default_val) {
        ParameterDefinition def;
        def.m_name = std::move(name);
        def.m_type = ParameterType::Int;
        def.m_range = range;
        def.m_default_value = static_cast<float>(default_val);
        def.m_decimal_places = 0;
        def.m_value_to_text = make_int_formatter();
        def.m_text_to_value = make_int_parser();
        return def;
    }

    static ParameterDefinition make_bool(std::string name, bool default_val = false) {
        ParameterDefinition def;
        def.m_name = std::move(name);
        def.m_type = ParameterType::Bool;
        def.m_range = Range::boolean();
        def.m_default_value = default_val ? 1.0f : 0.0f;
        def.m_decimal_places = 0;
        def.m_value_to_text = make_bool_formatter();
        def.m_text_to_value = make_bool_parser();
        return def;
    }

    static ParameterDefinition make_choice(std::string name,
                                           std::vector<std::string> choice_labels,
                                           int default_val = 0) {
        ParameterDefinition def;
        def.m_name = std::move(name);
        def.m_type = ParameterType::Int;
        def.m_range = Range::discrete(0, static_cast<int>(choice_labels.size()) - 1);
        def.m_default_value = static_cast<float>(default_val);
        def.m_decimal_places = 0;
        def.m_value_to_text = make_choice_formatter(choice_labels);
        def.m_text_to_value = make_choice_parser(choice_labels);
        def.m_choices = std::move(choice_labels);
        return def;
    }

    // ── Default formatter generators (also used by State::create_in_root) ──

    static std::function<std::string(float)> make_float_formatter(size_t dp) {
        return [dp](float value) -> std::string {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(static_cast<int>(dp)) << value;
            return oss.str();
        };
    }

    static std::function<float(const std::string&)> make_float_parser() {
        return [](const std::string& text) -> float {
            try {
                return std::stof(text);
            } catch (...) { return 0.0f; }
        };
    }

    static std::function<std::string(float)> make_int_formatter() {
        return [](float value) -> std::string { return std::to_string(static_cast<int>(value)); };
    }

    static std::function<float(const std::string&)> make_int_parser() {
        return [](const std::string& text) -> float {
            try {
                return static_cast<float>(std::stoi(text));
            } catch (...) { return 0.0f; }
        };
    }

    static std::function<std::string(float)> make_bool_formatter() {
        return [](float value) -> std::string { return value >= 0.5f ? "On" : "Off"; };
    }

    static std::function<float(const std::string&)> make_bool_parser() {
        return [](const std::string& text) -> float {
            if (text == "On" || text == "on" || text == "true" || text == "1" || text == "yes") {
                return 1.0f;
            }
            return 0.0f;
        };
    }

    static std::function<std::string(float)> make_choice_formatter(
        const std::vector<std::string>& labels) {
        return [labels](float value) -> std::string {
            auto idx = static_cast<size_t>(value);
            if (idx < labels.size()) { return labels[idx]; }
            return std::to_string(static_cast<int>(value));
        };
    }

    static std::function<float(const std::string&)> make_choice_parser(
        const std::vector<std::string>& labels) {
        return [labels](const std::string& text) -> float {
            for (size_t i = 0; i < labels.size(); ++i) {
                if (labels[i] == text) { return static_cast<float>(i); }
            }
            try {
                return static_cast<float>(std::stoi(text));
            } catch (...) { return 0.0f; }
        };
    }

    /// Generate default formatters based on m_type if not already set.
    /// Called by State::create_in_root() to ensure every parameter has formatters.
    void ensure_formatters() {
        if (!m_value_to_text) {
            // NOLINTNEXTLINE(bugprone-branch-clone) — Float/Double/Int/Bool branches are distinct.
            switch (m_type) {
                case ParameterType::Float:
                case ParameterType::Double:
                    m_value_to_text = make_float_formatter(m_decimal_places);
                    break;
                case ParameterType::Int:
                    m_value_to_text =
                        m_choices.empty() ? make_int_formatter() : make_choice_formatter(m_choices);
                    break;
                case ParameterType::Bool: m_value_to_text = make_bool_formatter(); break;
                default: break;
            }
        }
        if (!m_text_to_value) {
            // NOLINTNEXTLINE(bugprone-branch-clone) — Float/Double/Int/Bool branches are distinct.
            switch (m_type) {
                case ParameterType::Float:
                case ParameterType::Double: m_text_to_value = make_float_parser(); break;
                case ParameterType::Int:
                    m_text_to_value =
                        m_choices.empty() ? make_int_parser() : make_choice_parser(m_choices);
                    break;
                case ParameterType::Bool: m_text_to_value = make_bool_parser(); break;
                default: break;
            }
        }
    }
};

}  // namespace thl
