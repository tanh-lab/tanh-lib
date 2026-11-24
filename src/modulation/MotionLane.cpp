#include "tanh/modulation/MotionLane.h"

#include <tanh/core/AbiNamespace.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "MotionInterpolation.h"

namespace thl::inline THL_ABI_NAMESPACE::modulation {

namespace {

constexpr int k_json_version = 2;
constexpr int k_json_version_xy = 1;  // x and y as two channels
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

bool read_channel(const nlohmann::json& values, std::vector<float>& out) {
    if (!values.is_array()) { return false; }
    out.clear();
    out.reserve(values.size());
    for (const auto& e : values) {
        if (!e.is_number_integer() && !e.is_number_unsigned()) { return false; }
        const int64_t q = e.get<int64_t>();
        if (q < 0 || q > 65535) { return false; }
        out.push_back(static_cast<float>(static_cast<double>(q) / k_quant));
    }
    return true;
}

}  // namespace

namespace detail {

float catmull_rom_wrap(const float* points, size_t n, double index) {
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
    MotionPoint point{.m_gate = gi < m_gate.size() ? m_gate[gi] : uint8_t{0}};
    const size_t channels = std::min(num_channels(), k_motion_max_channels);
    for (size_t c = 0; c < channels; ++c) {
        point.m_values[c] = m_channels[c].size() == n
                                ? detail::catmull_rom_wrap(m_channels[c].data(), n, idx)
                                : 0.0f;
    }
    return point;
}

nlohmann::json MotionLane::to_json() const {
    nlohmann::json j;
    j["version"] = k_json_version;
    j["take_id"] = m_take_id;
    j["timebase"] = m_timebase == MotionTimebase::Beats ? "beats" : "seconds";
    j["rate"] = m_rate;
    j["length"] = m_length;
    j["anchor"] = m_anchor;
    if (m_seam != 0) { j["seam"] = m_seam; }
    const size_t n = num_points();
    auto channels = nlohmann::json::array();
    for (const auto& channel : m_channels) {
        auto values = nlohmann::json::array();
        for (size_t i = 0; i < n; ++i) {
            values.push_back(quantise(i < channel.size() ? channel[i] : 0.0f));
        }
        channels.push_back(std::move(values));
    }
    auto gate = nlohmann::json::array();
    uint8_t prev = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t g = (i < m_gate.size() && m_gate[i] != 0) ? 1 : 0;
        if (i == 0 || g != prev) { gate.push_back(nlohmann::json::array({i, g})); }
        prev = g;
    }
    j["channels"] = std::move(channels);
    j["gate"] = std::move(gate);
    return j;
}

std::optional<MotionLane> MotionLane::from_json(const nlohmann::json& json, size_t max_points) {
    try {
        if (!json.is_object()) { return std::nullopt; }
        const auto version = json.find("version");
        if (version == json.end() || !version->is_number_integer()) { return std::nullopt; }
        const int64_t v = version->get<int64_t>();
        if (v != k_json_version && v != k_json_version_xy) { return std::nullopt; }

        MotionLane lane;
        if (v == k_json_version_xy) {
            const auto x = json.find("x");
            const auto y = json.find("y");
            if (x == json.end() || y == json.end()) { return std::nullopt; }
            lane.m_channels.resize(2);
            if (!read_channel(*x, lane.m_channels[0]) || !read_channel(*y, lane.m_channels[1])) {
                return std::nullopt;
            }
        } else {
            const auto channels = json.find("channels");
            if (channels == json.end() || !channels->is_array() ||
                channels->size() > k_motion_max_channels) {
                return std::nullopt;
            }
            lane.m_channels.resize(channels->size());
            for (size_t c = 0; c < channels->size(); ++c) {
                if (!read_channel((*channels)[c], lane.m_channels[c])) { return std::nullopt; }
            }
        }
        const size_t n = lane.num_points();
        for (const auto& channel : lane.m_channels) {
            if (channel.size() != n) { return std::nullopt; }
        }

        const auto take_id = json.find("take_id");
        if (take_id != json.end()) {
            if (!take_id->is_number_unsigned() && !take_id->is_number_integer()) {
                return std::nullopt;
            }
            const int64_t id = take_id->get<int64_t>();
            if (id < 0 || std::cmp_greater(id, UINT32_MAX)) { return std::nullopt; }
            lane.m_take_id = static_cast<uint32_t>(id);
        }
        if (n == 0) {
            MotionLane empty{.m_take_id = lane.m_take_id};
            empty.m_channels.resize(lane.num_channels());
            return empty;
        }

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
            const int64_t g = edge[1].get<int64_t>();
            if (index <= prev_index || std::cmp_greater_equal(index, n) || (g != 0 && g != 1)) {
                return std::nullopt;
            }
            // Fill the previous run up to this edge.
            if (prev_index >= 0) {
                std::fill(lane.m_gate.begin() + prev_index, lane.m_gate.begin() + index, value);
            }
            prev_index = index;
            value = static_cast<uint8_t>(g);
        }
        if (prev_index >= 0) {
            std::fill(lane.m_gate.begin() + prev_index, lane.m_gate.end(), value);
        }

        const auto seam = json.find("seam");
        if (seam != json.end()) {
            if (!seam->is_number_integer()) { return std::nullopt; }
            const int64_t s = seam->get<int64_t>();
            if (s < 0 || std::cmp_greater_equal(s, n)) { return std::nullopt; }
            lane.m_seam = static_cast<size_t>(s);
        }

        if (max_points > 0 && n > max_points) { lane.resample(max_points); }
        return lane;
    } catch (...) { return std::nullopt; }
}

void MotionLane::resample(size_t num_points) {
    const size_t n = this->num_points();
    if (n == 0 || num_points == 0 || num_points == n) { return; }
    std::vector<std::vector<float>> channels(num_channels(), std::vector<float>(num_points));
    std::vector<uint8_t> gate(num_points);
    const double step = static_cast<double>(n) / static_cast<double>(num_points);
    for (size_t k = 0; k < num_points; ++k) {
        const double idx = static_cast<double>(k) * step;
        for (size_t c = 0; c < channels.size(); ++c) {
            channels[c][k] = m_channels[c].size() == n
                                 ? detail::catmull_rom_wrap(m_channels[c].data(), n, idx)
                                 : 0.0f;
        }
        auto gi = static_cast<size_t>(idx);
        if (gi >= n) { gi = n - 1; }
        gate[k] = gi < m_gate.size() ? m_gate[gi] : uint8_t{0};
    }
    m_rate *= static_cast<double>(num_points) / static_cast<double>(n);
    const auto seam = static_cast<size_t>(std::llround(static_cast<double>(m_seam) / step));
    m_seam = seam % num_points;
    m_channels = std::move(channels);
    m_gate = std::move(gate);
}

}  // namespace thl::inline THL_ABI_NAMESPACE::modulation
