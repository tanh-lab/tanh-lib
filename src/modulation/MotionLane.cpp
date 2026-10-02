#include <tanh/modulation/MotionLane.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <vector>

namespace thl::modulation {

namespace {

constexpr int k_json_version = 1;
constexpr double k_quant = 65535.0;

uint16_t quantise(float v) {
    const double c = std::clamp(static_cast<double>(v), 0.0, 1.0);
    return static_cast<uint16_t>(std::lround(c * k_quant));
}

std::optional<double> finite_number(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) { return std::nullopt; }
    const double v = it->get<double>();
    if (!std::isfinite(v)) { return std::nullopt; }
    return v;
}

bool read_channel(const nlohmann::json& j, const char* key, std::vector<float>& out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array()) { return false; }
    out.clear();
    out.reserve(it->size());
    for (const auto& e : *it) {
        if (!e.is_number_integer() && !e.is_number_unsigned()) { return false; }
        const int64_t q = e.get<int64_t>();
        if (q < 0 || q > 65535) { return false; }
        out.push_back(static_cast<float>(static_cast<double>(q) / k_quant));
    }
    return true;
}

}  // namespace

namespace detail {

float catmull_rom_wrap(const float* points,
                       size_t n,
                       double index) noexcept TANH_NONBLOCKING_FUNCTION {
    if (n == 0) { return 0.0f; }
    const double idx = wrap_phase(index, static_cast<double>(n));
    auto i1 = static_cast<size_t>(idx);
    if (i1 >= n) { i1 = n - 1; }
    const double u = idx - static_cast<double>(i1);
    const size_t i0 = i1 == 0 ? n - 1 : i1 - 1;
    const size_t i2 = i1 + 1 >= n ? i1 + 1 - n : i1 + 1;
    const size_t i3 = i2 + 1 >= n ? i2 + 1 - n : i2 + 1;
    const double p0 = points[i0];
    const double p1 = points[i1];
    const double p2 = points[i2];
    const double p3 = points[i3];
    const double v =
        p1 +
        (0.5 * u *
         (p2 - p0 +
          u * ((2.0 * p0) - (5.0 * p1) + (4.0 * p2) - p3 + (u * ((3.0 * (p1 - p2)) + p3 - p0)))));
    return static_cast<float>(std::clamp(v, 0.0, 1.0));
}

}  // namespace detail

MotionPoint MotionLane::sample(double phase) const {
    const size_t n = num_points();
    if (n == 0 || !(m_length > 0.0)) { return {}; }
    const double idx = detail::wrap_phase(phase, m_length) * static_cast<double>(n) / m_length;
    auto gi = static_cast<size_t>(idx);
    if (gi >= n) { gi = n - 1; }
    return MotionPoint{
        .m_x = detail::catmull_rom_wrap(m_x.data(), n, idx),
        .m_y = detail::catmull_rom_wrap(m_y.data(), n, idx),
        .m_gate = gi < m_gate.size() ? m_gate[gi] : uint8_t{0},
    };
}

nlohmann::json MotionLane::to_json() const {
    nlohmann::json j;
    j["version"] = k_json_version;
    j["take_id"] = m_take_id;
    j["timebase"] = m_timebase == MotionTimebase::Beats ? "beats" : "seconds";
    j["rate"] = m_rate;
    j["length"] = m_length;
    j["anchor"] = m_anchor;
    auto xs = nlohmann::json::array();
    auto ys = nlohmann::json::array();
    auto gate = nlohmann::json::array();
    const size_t n = num_points();
    uint8_t prev = 0;
    for (size_t i = 0; i < n; ++i) {
        xs.push_back(quantise(m_x[i]));
        ys.push_back(quantise(i < m_y.size() ? m_y[i] : 0.0f));
        const uint8_t g = (i < m_gate.size() && m_gate[i] != 0) ? 1 : 0;
        if (i == 0 || g != prev) { gate.push_back(nlohmann::json::array({i, g})); }
        prev = g;
    }
    j["x"] = std::move(xs);
    j["y"] = std::move(ys);
    j["gate"] = std::move(gate);
    return j;
}

std::optional<MotionLane> MotionLane::from_json(const nlohmann::json& json, size_t max_points) {
    try {
        if (!json.is_object()) { return std::nullopt; }
        const auto version = json.find("version");
        if (version == json.end() || !version->is_number_integer() ||
            version->get<int64_t>() != k_json_version) {
            return std::nullopt;
        }

        MotionLane lane;
        if (!read_channel(json, "x", lane.m_x) || !read_channel(json, "y", lane.m_y)) {
            return std::nullopt;
        }
        if (lane.m_x.size() != lane.m_y.size()) { return std::nullopt; }
        const size_t n = lane.m_x.size();

        const auto take_id = json.find("take_id");
        if (take_id != json.end()) {
            if (!take_id->is_number_unsigned() && !take_id->is_number_integer()) {
                return std::nullopt;
            }
            const int64_t id = take_id->get<int64_t>();
            if (id < 0 || id > static_cast<int64_t>(UINT32_MAX)) { return std::nullopt; }
            lane.m_take_id = static_cast<uint32_t>(id);
        }
        if (n == 0) { return MotionLane{.m_take_id = lane.m_take_id}; }

        const auto timebase = json.find("timebase");
        if (timebase == json.end() || !timebase->is_string()) { return std::nullopt; }
        const auto tb = timebase->get<std::string>();
        if (tb == "beats") {
            lane.m_timebase = MotionTimebase::Beats;
        } else if (tb == "seconds") {
            lane.m_timebase = MotionTimebase::Seconds;
        } else {
            return std::nullopt;
        }

        const auto rate = finite_number(json, "rate");
        const auto length = finite_number(json, "length");
        const auto anchor = finite_number(json, "anchor");
        if (!rate || !length || !anchor || !(*rate > 0.0) || !(*length > 0.0)) {
            return std::nullopt;
        }
        lane.m_rate = *rate;
        lane.m_length = *length;
        lane.m_anchor = detail::wrap_phase(*anchor, *length);

        const auto gate = json.find("gate");
        if (gate == json.end() || !gate->is_array()) { return std::nullopt; }
        lane.m_gate.assign(n, 0);
        int64_t prev_index = -1;
        uint8_t value = 0;
        for (const auto& edge : *gate) {
            if (!edge.is_array() || edge.size() != 2 || !edge[0].is_number_integer() ||
                !edge[1].is_number_integer()) {
                return std::nullopt;
            }
            const int64_t index = edge[0].get<int64_t>();
            const int64_t v = edge[1].get<int64_t>();
            if (index <= prev_index || index >= static_cast<int64_t>(n) || (v != 0 && v != 1)) {
                return std::nullopt;
            }
            // Fill the previous run up to this edge.
            if (prev_index >= 0) {
                std::fill(lane.m_gate.begin() + prev_index, lane.m_gate.begin() + index, value);
            }
            prev_index = index;
            value = static_cast<uint8_t>(v);
        }
        if (prev_index >= 0) {
            std::fill(lane.m_gate.begin() + prev_index, lane.m_gate.end(), value);
        }

        if (max_points > 0 && n > max_points) { lane.resample(max_points); }
        return lane;
    } catch (...) { return std::nullopt; }
}

void MotionLane::resample(size_t num_points) {
    const size_t n = this->num_points();
    if (n == 0 || num_points == 0 || num_points == n) { return; }
    std::vector<float> x(num_points);
    std::vector<float> y(num_points);
    std::vector<uint8_t> gate(num_points);
    const double step = static_cast<double>(n) / static_cast<double>(num_points);
    for (size_t k = 0; k < num_points; ++k) {
        const double idx = static_cast<double>(k) * step;
        x[k] = detail::catmull_rom_wrap(m_x.data(), n, idx);
        y[k] = detail::catmull_rom_wrap(m_y.data(), n, idx);
        auto gi = static_cast<size_t>(idx);
        if (gi >= n) { gi = n - 1; }
        gate[k] = gi < m_gate.size() ? m_gate[gi] : uint8_t{0};
    }
    m_rate *= static_cast<double>(num_points) / static_cast<double>(n);
    m_x = std::move(x);
    m_y = std::move(y);
    m_gate = std::move(gate);
}

}  // namespace thl::modulation
