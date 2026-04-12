#include "tanh/modulation/ModulationMatrix.h"

#include <tanh/core/Exports.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stack>
#include <stdexcept>
#include <string_view>
#include <string>
#include <variant>
#include <vector>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

#include "tanh/core/Logger.h"
#include "tanh/state/ParameterDefinitions.h"

#include "tanh/modulation/SmartHandle.h"
#include "tanh/modulation/ResolvedTarget.h"
#include "tanh/modulation/ModulationRouting.h"
#include "tanh/modulation/ResolvedRouting.h"
#include "tanh/state/Parameter.h"
#include "tanh/state/State.h"
#include "tanh/utils/RealtimeSanitizer.h"

using namespace thl::modulation;

// ── ResolvedTarget::read_base_as_float ──────────────────────────────────────

float ResolvedTarget::read_base_as_float() const TANH_NONBLOCKING_FUNCTION {
    if (!m_record) { return 0.0f; }
    switch (m_type) {
        case thl::ParameterType::Float:
            return m_record->m_cache.m_atomic_float.load(std::memory_order_relaxed);
        case thl::ParameterType::Double:
            return static_cast<float>(
                m_record->m_cache.m_atomic_double.load(std::memory_order_relaxed));
        case thl::ParameterType::Int:
            return static_cast<float>(
                m_record->m_cache.m_atomic_int.load(std::memory_order_relaxed));
        case thl::ParameterType::Bool:
            return m_record->m_cache.m_atomic_bool.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        default: return 0.0f;
    }
}

ModulationMatrix::ModulationMatrix(thl::State& state) : m_state(state) {
    m_config.register_reader_thread();
}

ModulationMatrix::~ModulationMatrix() = default;

template <typename T>
SmartHandle<T> ModulationMatrix::get_smart_handle(const std::string_view param_key) {
    std::scoped_lock const lock(m_writer_mutex);

    // Throws StateKeyNotFoundException if parameter doesn't exist.
    // Throws std::invalid_argument if T doesn't match the parameter's type.
    auto handle = m_state.get_handle<T>(param_key);

    // Check modulation flag
    if (!m_state.is_modulatable(param_key)) {
        throw std::invalid_argument("Parameter '" + std::string(param_key) +
                                    "' has modulation disabled");
    }

    auto* target = ensure_target_locked(param_key);
    return {handle, target};
}

// Explicit instantiations for all supported types.
// TANH_API (__declspec(dllexport) on Windows) is required so that MSVC exports
// these template specialisations from the shared library.
template TANH_API SmartHandle<float> ModulationMatrix::get_smart_handle<float>(std::string_view);
template TANH_API SmartHandle<double> ModulationMatrix::get_smart_handle<double>(std::string_view);
template TANH_API SmartHandle<int> ModulationMatrix::get_smart_handle<int>(std::string_view);
template TANH_API SmartHandle<bool> ModulationMatrix::get_smart_handle<bool>(std::string_view);

ResolvedTarget* ModulationMatrix::ensure_target_locked(const std::string_view id) {
    auto it = m_targets.find(id);
    if (it != m_targets.end()) { return &it->second; }
    auto [target_it, inserted] = m_targets.emplace(std::string(id), ResolvedTarget{});
    auto& t = target_it->second;
    t.m_id = id;

    // Populate metadata from State for normalized depth processing
    const thl::Parameter param = m_state.get_parameter(id);
    t.m_range = &param.range();
    t.m_type = param.def().m_type;
    t.m_record = m_state.get_record(id);
    t.m_uses_normalized_buffer = t.m_range && !t.m_range->is_linear();

    return &t;
}

void ModulationMatrix::prepare(double sample_rate, size_t samples_per_block) {
    std::scoped_lock const lock(m_writer_mutex);

    m_sample_rate = sample_rate;
    m_samples_per_block = samples_per_block;

    for (auto& [id, source] : m_sources) { source->prepare(sample_rate, samples_per_block); }

    for (auto& [id, target] : m_targets) { target.resize(samples_per_block); }

    rebuild_schedule_locked();
}

void ModulationMatrix::process(size_t num_samples) TANH_NONBLOCKING_FUNCTION {
    m_config.read([&](const ProcessingConfig& config) {
        // 1. Clear all active targets for this block
        for (auto* target : config.m_active_targets) { target->clear_per_block(); }

        // 2. Execute schedule steps
        for (const auto& step : config.m_schedule) {
            if (auto* bulk = std::get_if<BulkStep>(&step)) {
                process_source_bulk(config, bulk->m_source, num_samples);
            } else if (auto* cyclic = std::get_if<CyclicStep>(&step)) {
                process_cyclic(config, cyclic->m_sources, num_samples);
            }
        }

        // 3. Build final sorted change point lists from flags
        for (auto* target : config.m_active_targets) { target->build_change_points(); }
    });
}

void ModulationMatrix::add_source(const std::string_view id, ModulationSource* source) {
    std::scoped_lock const lock(m_writer_mutex);
    auto it = m_sources.find(id);
    if (it != m_sources.end()) {
        it->second = source;
        rebuild_schedule_locked();
        return;
    }
    m_sources.emplace(std::string(id), source);
    rebuild_schedule_locked();
}

void ModulationMatrix::remove_source(const std::string_view id) {
    std::scoped_lock const lock(m_writer_mutex);
    m_sources.erase(std::string(id));

    // Remove user-facing routings that reference this source.
    std::erase_if(m_user_routings, [&](const ModulationRouting& r) { return r.m_source_id == id; });

    rebuild_schedule_locked();
    // Block until all RT readers have finished with the old config that still
    // referenced the removed source. After this returns the caller may safely
    // delete the ModulationSource object.
    m_config.synchronize();
}

uint32_t ModulationMatrix::add_routing(const ModulationRouting& routing) {
    std::scoped_lock const lock(m_writer_mutex);

    // Reject duplicate (source, target) pair.
    for (const auto& existing : m_user_routings) {
        if (existing.m_source_id == routing.m_source_id &&
            existing.m_target_id == routing.m_target_id) {
            thl::Logger::logf(thl::Logger::LogLevel::Warning,
                              "modulation",
                              "Routing rejected: duplicate source '%s' -> target '%s' "
                              "(existing routing id %u).",
                              routing.m_source_id.c_str(),
                              routing.m_target_id.c_str(),
                              existing.m_id);
            return k_invalid_routing_id;
        }
    }

    // Reject if another Replace/ReplaceHold routing already targets the same parameter.
    const bool is_replace = routing.m_combine_mode == CombineMode::Replace ||
                            routing.m_combine_mode == CombineMode::ReplaceHold;
    if (is_replace) {
        for (const auto& existing : m_user_routings) {
            const bool existing_is_replace = existing.m_combine_mode == CombineMode::Replace ||
                                             existing.m_combine_mode == CombineMode::ReplaceHold;
            if (existing_is_replace && existing.m_target_id == routing.m_target_id) {
                thl::Logger::logf(
                    thl::Logger::LogLevel::Warning,
                    "modulation",
                    "Replace routing rejected: source '%s' -> target '%s'. "
                    "A Replace routing from source '%s' already targets this parameter.",
                    routing.m_source_id.c_str(),
                    routing.m_target_id.c_str(),
                    existing.m_source_id.c_str());
                return k_invalid_routing_id;
            }
        }
    }

    // Reject routing if source is not registered
    if (m_sources.find(routing.m_source_id) == m_sources.end()) {
        thl::Logger::logf(thl::Logger::LogLevel::Warning,
                          "modulation",
                          "Routing rejected: source '%s' is not registered.",
                          routing.m_source_id.c_str());
        return k_invalid_routing_id;
    }

    // Reject routing if target parameter is not modulatable in State
    if (!m_state.is_modulatable(routing.m_target_id)) {
        thl::Logger::logf(thl::Logger::LogLevel::Warning,
                          "modulation",
                          "Routing rejected: target '%s' is not a modulatable parameter.",
                          routing.m_target_id.c_str());
        return k_invalid_routing_id;
    }

    // Resolve target if not yet in m_targets
    ensure_target_locked(routing.m_target_id);

    const uint32_t id = m_next_routing_id++;
    m_user_routings.push_back(routing);
    m_user_routings.back().m_id = id;
    rebuild_schedule_locked();
    return id;
}

void ModulationMatrix::remove_routing(std::string_view source_id, std::string_view target_id) {
    std::scoped_lock const lock(m_writer_mutex);
    std::erase_if(m_user_routings, [&](const ModulationRouting& r) {
        return r.m_source_id == source_id && r.m_target_id == target_id;
    });
    rebuild_schedule_locked();
}

void ModulationMatrix::remove_routing(uint32_t routing_id) {
    std::scoped_lock const lock(m_writer_mutex);
    std::erase_if(m_user_routings,
                  [&](const ModulationRouting& r) { return r.m_id == routing_id; });
    rebuild_schedule_locked();
}

// ── Private routing helpers ──────────────────────────────────────────────────

ModulationRouting* ModulationMatrix::find_user_routing_locked(std::string_view source_id,
                                                              std::string_view target_id) {
    for (auto& r : m_user_routings) {
        if (r.m_source_id == source_id && r.m_target_id == target_id) { return &r; }
    }
    return nullptr;
}

ModulationRouting* ModulationMatrix::find_user_routing_locked(uint32_t routing_id) {
    for (auto& r : m_user_routings) {
        if (r.m_id == routing_id) { return &r; }
    }
    return nullptr;
}

float ModulationMatrix::compute_depth_precomputed(const ModulationRouting& routing,
                                                  const ResolvedTarget& target) {
    const auto* range = target.m_range;
    if (!range) { return routing.m_depth; }

    const float span = range->m_max - range->m_min;
    const bool is_replace = routing.m_combine_mode == CombineMode::Replace ||
                            routing.m_combine_mode == CombineMode::ReplaceHold;
    if (target.m_uses_normalized_buffer && !is_replace) {
        return routing.m_depth_mode == DepthMode::Absolute ? routing.m_depth / span
                                                           : routing.m_depth;
    }
    return routing.m_depth_mode == DepthMode::Normalized ? routing.m_depth * span : routing.m_depth;
}

bool ModulationMatrix::update_routing_depth_locked(ModulationRouting& user_routing,
                                                   float new_depth) {
    user_routing.m_depth = new_depth;

    auto tgt_it = m_targets.find(user_routing.m_target_id);
    if (tgt_it == m_targets.end()) { return false; }

    const float precomputed = compute_depth_precomputed(user_routing, tgt_it->second);
    const uint32_t id = user_routing.m_id;
    m_config.read([&](const ProcessingConfig& config) {
        for (const auto& r : config.m_routings) {
            if (r.m_id == id) {
                r.m_depth.store(new_depth, std::memory_order_relaxed);
                r.m_depth_abs_precomputed.store(precomputed, std::memory_order_relaxed);
                break;
            }
        }
    });
    return true;
}

bool ModulationMatrix::update_routing_replace_range_locked(ModulationRouting& user_routing,
                                                           float range_min,
                                                           float range_max) {
    user_routing.m_replace_range_min = range_min;
    user_routing.m_replace_range_max = range_max;
    user_routing.m_has_replace_range = true;

    const uint32_t id = user_routing.m_id;
    m_config.read([&](const ProcessingConfig& config) {
        for (const auto& r : config.m_routings) {
            if (r.m_id == id) {
                r.m_replace_range_min.store(range_min, std::memory_order_relaxed);
                r.m_replace_range_max.store(range_max, std::memory_order_relaxed);
                r.m_has_replace_range.store(true, std::memory_order_relaxed);
                break;
            }
        }
    });
    return true;
}

bool ModulationMatrix::clear_routing_replace_range_locked(ModulationRouting& user_routing) {
    user_routing.m_has_replace_range = false;

    const uint32_t id = user_routing.m_id;
    m_config.read([&](const ProcessingConfig& config) {
        for (const auto& r : config.m_routings) {
            if (r.m_id == id) {
                r.m_has_replace_range.store(false, std::memory_order_relaxed);
                break;
            }
        }
    });
    return true;
}

// ── Public routing update methods ───────────────────────────────────────────

bool ModulationMatrix::update_routing_depth(std::string_view source_id,
                                            std::string_view target_id,
                                            float new_depth) {
    std::scoped_lock const lock(m_writer_mutex);
    auto* routing = find_user_routing_locked(source_id, target_id);
    if (!routing) { return false; }
    return update_routing_depth_locked(*routing, new_depth);
}

bool ModulationMatrix::update_routing_depth(uint32_t routing_id, float new_depth) {
    std::scoped_lock const lock(m_writer_mutex);
    auto* routing = find_user_routing_locked(routing_id);
    if (!routing) { return false; }
    return update_routing_depth_locked(*routing, new_depth);
}

bool ModulationMatrix::update_routing_replace_range(std::string_view source_id,
                                                    std::string_view target_id,
                                                    float range_min,
                                                    float range_max) {
    std::scoped_lock const lock(m_writer_mutex);
    auto* routing = find_user_routing_locked(source_id, target_id);
    if (!routing) { return false; }
    return update_routing_replace_range_locked(*routing, range_min, range_max);
}

bool ModulationMatrix::update_routing_replace_range(uint32_t routing_id,
                                                    float range_min,
                                                    float range_max) {
    std::scoped_lock const lock(m_writer_mutex);
    auto* routing = find_user_routing_locked(routing_id);
    if (!routing) { return false; }
    return update_routing_replace_range_locked(*routing, range_min, range_max);
}

bool ModulationMatrix::clear_routing_replace_range(std::string_view source_id,
                                                   std::string_view target_id) {
    std::scoped_lock const lock(m_writer_mutex);
    auto* routing = find_user_routing_locked(source_id, target_id);
    if (!routing) { return false; }
    return clear_routing_replace_range_locked(*routing);
}

bool ModulationMatrix::clear_routing_replace_range(uint32_t routing_id) {
    std::scoped_lock const lock(m_writer_mutex);
    auto* routing = find_user_routing_locked(routing_id);
    if (!routing) { return false; }
    return clear_routing_replace_range_locked(*routing);
}

const ResolvedTarget* ModulationMatrix::get_target(const std::string_view id) const {
    auto it = m_targets.find(id);
    return it != m_targets.end() ? &it->second : nullptr;
}

ResolvedTarget* ModulationMatrix::get_target(const std::string_view id) {
    auto it = m_targets.find(id);
    return it != m_targets.end() ? &it->second : nullptr;
}

// ── Schedule rebuild (Tarjan SCC + topological sort) ──────────────────────────

void ModulationMatrix::rebuild_schedule() {
    std::scoped_lock const lock(m_writer_mutex);
    rebuild_schedule_locked();
}

std::vector<ScheduleStep> ModulationMatrix::get_schedule() const {
    return m_config.read([](const ProcessingConfig& config) { return config.m_schedule; });
}

void ModulationMatrix::rebuild_schedule_locked() {
    // ── Pass 1: Determine per-target buffer requirements ────────────────
    struct TargetInfo {
        uint32_t m_max_voices = 0;
        bool m_mono_additive = false;
        bool m_mono_replace = false;
        bool m_voice_additive = false;
        bool m_voice_replace = false;
    };
    std::unordered_map<std::string, TargetInfo> target_info;

    // 1a: Determine which targets are polyphonic (any voice-capable source)
    for (const auto& routing : m_user_routings) {
        auto src_it = m_sources.find(routing.m_source_id);
        if (src_it == m_sources.end()) { continue; }

        auto& info = target_info[routing.m_target_id];
        if (src_it->second->has_voice_output()) {
            info.m_max_voices = std::max(info.m_max_voices, src_it->second->num_voices());
        }
    }

    // 1b: Determine buffer needs based on actual routing modes.
    // A target is either mono-only or voice-only — never both.
    for (const auto& routing : m_user_routings) {
        auto src_it = m_sources.find(routing.m_source_id);
        if (src_it == m_sources.end()) { continue; }

        auto& info = target_info[routing.m_target_id];
        const bool tgt_poly = info.m_max_voices > 0;
        const bool is_replace = routing.m_combine_mode == CombineMode::Replace ||
                                routing.m_combine_mode == CombineMode::ReplaceHold;

        if (tgt_poly) {
            if (is_replace) {
                info.m_voice_replace = true;
            } else {
                info.m_voice_additive = true;
            }
        } else if (src_it->second->has_mono_output()) {
            if (is_replace) {
                info.m_mono_replace = true;
            } else {
                info.m_mono_additive = true;
            }
        }
    }

    // ── Pass 1c: Allocate/free buffers on targets ───────────────────────
    for (auto& [id, target] : m_targets) {
        auto it = target_info.find(id);

        target.m_has_mono_additive = it != target_info.end() && it->second.m_mono_additive;
        target.m_has_mono_replace = it != target_info.end() && it->second.m_mono_replace;

        if (target.m_has_mono_additive) {
            target.m_additive_buffer.assign(m_samples_per_block, 0.0f);
        } else {
            target.m_additive_buffer.clear();
        }

        if (target.m_has_mono_replace) {
            target.m_replace_buffer.assign(m_samples_per_block, 0.0f);
            target.m_replace_active.assign(m_samples_per_block, 0);
        } else {
            target.m_replace_buffer.clear();
            target.m_replace_active.clear();
        }

        const bool has_any_routing = it != target_info.end();
        if (has_any_routing) {
            target.m_change_point_flags.assign(m_samples_per_block, false);
            target.m_change_points.clear();
            target.m_change_points.reserve(m_samples_per_block);
        } else {
            target.m_change_point_flags.clear();
            target.m_change_points.clear();
        }

        const bool has_poly = has_any_routing && it->second.m_max_voices > 0;
        if (has_poly) {
            const uint32_t nv = it->second.m_max_voices;
            const bool va = it->second.m_voice_additive;
            const bool vr = it->second.m_voice_replace;
            if (!target.m_voice || target.m_voice->m_num_voices != nv ||
                target.m_voice->m_block_size != m_samples_per_block ||
                target.m_voice->m_has_additive != va || target.m_voice->m_has_replace != vr) {
                target.m_voice = std::make_unique<VoiceBuffers>();
                target.m_voice->resize(nv, m_samples_per_block, va, vr);
            }
        } else {
            target.m_voice.reset();
        }
    }

    // ── Pass 2: Resolve routings ────────────────────────────────────────
    std::vector<ResolvedRouting> new_routings;
    for (auto& routing : m_user_routings) {
        auto src_it = m_sources.find(routing.m_source_id);
        auto tgt_it = m_targets.find(routing.m_target_id);

        if (src_it == m_sources.end() || tgt_it == m_targets.end()) { continue; }

        // Determine routing mode from source capability + target type
        const bool src_mono = src_it->second->has_mono_output();
        const bool src_voice = src_it->second->has_voice_output();
        const bool tgt_poly = tgt_it->second.m_voice != nullptr;

        RoutingMode routing_mode;
        if (tgt_poly && src_voice) {
            routing_mode = RoutingMode::PolyToPoly;
        } else if (tgt_poly && !src_voice) {
            routing_mode = RoutingMode::MonoToPoly;
        } else if (!tgt_poly && src_mono) {
            routing_mode = RoutingMode::MonoToMono;
        } else {
            routing_mode = RoutingMode::PolyToMono;
        }

        // Reject PolyToMono
        if (routing_mode == RoutingMode::PolyToMono) {
            thl::Logger::logf(thl::Logger::LogLevel::Warning,
                              "modulation",
                              "Routing rejected: poly source '%s' -> mono target '%s'. "
                              "Use a mono source or make the target polyphonic.",
                              routing.m_source_id.c_str(),
                              routing.m_target_id.c_str());
            continue;
        }

        // Voice mismatch warning for PolyToPoly
        if (routing_mode == RoutingMode::PolyToPoly) {
            const uint32_t src_v = src_it->second->num_voices();
            const uint32_t tgt_v = tgt_it->second.m_voice->m_num_voices;
            if (src_v != tgt_v) {
                thl::Logger::logf(
                    thl::Logger::LogLevel::Warning,
                    "modulation",
                    "Voice count mismatch: source '%s' has %u voices, target '%s' has %u "
                    "— only %u voices will be modulated",
                    routing.m_source_id.c_str(),
                    src_v,
                    routing.m_target_id.c_str(),
                    tgt_v,
                    std::min(src_v, tgt_v));
            }
        }

        ResolvedRouting r;
        r.m_id = routing.m_id;
        r.m_source = src_it->second;
        r.m_target = &tgt_it->second;
        r.m_depth.store(routing.m_depth, std::memory_order_relaxed);
        r.m_depth_mode = routing.m_depth_mode;
        r.m_combine_mode = routing.m_combine_mode;
        r.m_routing_mode = routing_mode;
        r.m_max_decimation = routing.m_max_decimation;
        r.m_skip_during_gesture = routing.m_skip_during_gesture;
        r.m_samples_until_update = 0;

        // Replace range — copy from user routing.
        r.m_replace_range_min.store(routing.m_replace_range_min, std::memory_order_relaxed);
        r.m_replace_range_max.store(routing.m_replace_range_max, std::memory_order_relaxed);
        r.m_has_replace_range.store(routing.m_has_replace_range, std::memory_order_relaxed);

        // Size per-voice held values for polyphonic ReplaceHold routings.
        if (r.m_combine_mode == CombineMode::ReplaceHold && tgt_poly) {
            r.m_held_voice_values.assign(tgt_it->second.m_voice->m_num_voices, 0.0f);
        }

        // Pre-compute effective depth for the RT hot path.
        r.m_depth_abs_precomputed.store(compute_depth_precomputed(routing, tgt_it->second),
                                        std::memory_order_relaxed);
        new_routings.push_back(r);
    }

    // Build target → owning source map (for dependency graph).
    std::unordered_map<std::string, std::string> target_owner;
    for (auto& [source_id, source] : m_sources) {
        for (auto& key : source->parameter_keys()) { target_owner[key] = source_id; }
    }

    // Build dependency graph
    std::vector<std::string> source_ids;
    source_ids.reserve(m_sources.size());
    for (auto& [id, _] : m_sources) { source_ids.push_back(id); }

    std::unordered_map<std::string, std::vector<std::string>> adj;
    std::unordered_map<std::string, bool> has_self_edge;
    for (auto& id : source_ids) {
        adj[id] = {};
        has_self_edge[id] = false;
    }

    for (auto& routing : m_user_routings) {
        auto owner_it = target_owner.find(routing.m_target_id);
        if (owner_it == target_owner.end()) { continue; }

        const std::string_view owner_id = owner_it->second;
        if (owner_id == routing.m_source_id) {
            has_self_edge[std::string(owner_id)] = true;
        } else {
            adj[std::string(owner_id)].push_back(routing.m_source_id);
        }
    }

    // Build the schedule via Tarjan SCC
    std::vector<ScheduleStep> new_schedule;
    build_schedule_from_graph(source_ids, adj, has_self_edge, new_schedule);

    // Collect active target pointers — only targets with resolved routings
    std::vector<ResolvedTarget*> new_active_targets;
    new_active_targets.reserve(target_info.size());
    for (auto& [id, target] : m_targets) {
        if (target_info.contains(id)) { new_active_targets.push_back(&target); }
    }

    // Publish everything atomically via RCU
    m_config.update([&](ProcessingConfig& config) {
        config.m_routings = std::move(new_routings);
        config.m_schedule = std::move(new_schedule);
        config.m_active_targets = std::move(new_active_targets);
        config.m_routings_by_source.clear();
        for (const auto& r : config.m_routings) {
            config.m_routings_by_source[r.m_source].push_back(&r);
        }
    });
}

void ModulationMatrix::build_schedule_from_graph(
    const std::vector<std::string>& source_ids,
    const std::unordered_map<std::string, std::vector<std::string>>& adj,
    const std::unordered_map<std::string, bool>& has_self_edge,
    std::vector<ScheduleStep>& out_schedule) {
    // Tarjan's SCC algorithm
    int index_counter = 0;
    std::unordered_map<std::string, int> index;
    std::unordered_map<std::string, int> lowlink;
    std::unordered_map<std::string, bool> on_stack;
    std::stack<std::string> stack;
    std::vector<std::vector<std::string>> sccs;

    std::function<void(const std::string&)> strongconnect = [&](const std::string& v) {
        index[v] = index_counter;
        lowlink[v] = index_counter;
        ++index_counter;
        stack.push(v);
        on_stack[v] = true;

        auto adj_it = adj.find(v);
        if (adj_it != adj.end()) {
            for (auto& w : adj_it->second) {
                if (index.find(w) == index.end()) {
                    strongconnect(w);
                    lowlink[v] = std::min(lowlink[v], lowlink[w]);
                } else if (on_stack[w]) {
                    lowlink[v] = std::min(lowlink[v], index[w]);
                }
            }
        }

        if (lowlink[v] == index[v]) {
            std::vector<std::string> scc;
            std::string w;
            do {
                w = stack.top();
                stack.pop();
                on_stack[w] = false;
                scc.push_back(w);
            } while (w != v);
            sccs.push_back(std::move(scc));
        }
    };

    for (auto& id : source_ids) {
        if (index.find(id) == index.end()) { strongconnect(id); }
    }

    for (auto& scc : sccs) {
        auto self_it = has_self_edge.find(scc[0]);
        bool const self_loop = self_it != has_self_edge.end() && self_it->second;

        if (scc.size() == 1 && !self_loop) {
            auto src_it = m_sources.find(scc[0]);
            if (src_it != m_sources.end()) { out_schedule.emplace_back(BulkStep{src_it->second}); }
        } else {
            CyclicStep step;
            step.m_sources.reserve(scc.size());
            for (auto& id : scc) {
                auto src_it = m_sources.find(id);
                if (src_it != m_sources.end()) { step.m_sources.push_back(src_it->second); }
            }
            out_schedule.emplace_back(std::move(step));
        }
    }
}

// ── Routing application helpers ─────────────────────────────────────────────

namespace {

// Helper: apply a single replace sample with ReplaceHold support.
inline void apply_replace_sample(const ResolvedRouting& routing,
                                 float* replace_buf,
                                 uint8_t* active_buf,
                                 size_t i,
                                 float value,
                                 bool src_active) TANH_NONBLOCKING_FUNCTION {
    if (src_active) {
        replace_buf[i] = value;
        active_buf[i] = 1;
        routing.m_held_value = value;
    } else if (routing.m_combine_mode == CombineMode::ReplaceHold) {
        replace_buf[i] = routing.m_held_value;
        active_buf[i] = 1;
    }
}

// Helper: apply a single replace sample for poly with per-voice held values.
inline void apply_replace_sample_voice(const ResolvedRouting& routing,
                                       float* replace_buf,
                                       uint8_t* active_buf,
                                       size_t i,
                                       float value,
                                       bool src_active,
                                       uint32_t voice) TANH_NONBLOCKING_FUNCTION {
    if (src_active) {
        replace_buf[i] = value;
        active_buf[i] = 1;
        if (voice < routing.m_held_voice_values.size()) {
            routing.m_held_voice_values[voice] = value;
        }
    } else if (routing.m_combine_mode == CombineMode::ReplaceHold &&
               voice < routing.m_held_voice_values.size()) {
        replace_buf[i] = routing.m_held_voice_values[voice];
        active_buf[i] = 1;
    }
}

// Compute the replace value for a single sample, applying range mapping if active.
inline float compute_replace_value(const ResolvedRouting& routing,
                                   float src_sample) TANH_NONBLOCKING_FUNCTION {
    if (routing.m_has_replace_range.load(std::memory_order_relaxed)) {
        const float raw_depth = routing.m_depth.load(std::memory_order_relaxed);
        const float rmin = routing.m_replace_range_min.load(std::memory_order_relaxed);
        const float rmax = routing.m_replace_range_max.load(std::memory_order_relaxed);
        return rmin + src_sample * raw_depth * (rmax - rmin);
    }
    return src_sample * routing.m_depth_abs_precomputed.load(std::memory_order_relaxed);
}

// MonoToMono: write to mono additive or mono replace buffer.
void apply_routing_mono_to_mono(const ResolvedRouting& routing,
                                const ModulationSource* source,
                                size_t num_samples) TANH_NONBLOCKING_FUNCTION {
    const auto& src_output = source->get_output_buffer();
    if (routing.m_combine_mode == CombineMode::Additive) {
        const float depth = routing.m_depth_abs_precomputed.load(std::memory_order_relaxed);
        if (source->is_fully_active()) {
            for (size_t i = 0; i < num_samples; ++i) {
                routing.m_target->m_additive_buffer[i] += src_output[i] * depth;
            }
        } else {
            for (size_t i = 0; i < num_samples; ++i) {
                if (source->get_output_active_at(static_cast<uint32_t>(i))) {
                    routing.m_target->m_additive_buffer[i] += src_output[i] * depth;
                }
            }
        }
    } else {
        if (source->is_fully_active()) {
            for (size_t i = 0; i < num_samples; ++i) {
                apply_replace_sample(routing,
                                     routing.m_target->m_replace_buffer.data(),
                                     routing.m_target->m_replace_active.data(),
                                     i,
                                     compute_replace_value(routing, src_output[i]),
                                     true);
            }
        } else {
            for (size_t i = 0; i < num_samples; ++i) {
                const bool active = source->get_output_active_at(static_cast<uint32_t>(i));
                apply_replace_sample(routing,
                                     routing.m_target->m_replace_buffer.data(),
                                     routing.m_target->m_replace_active.data(),
                                     i,
                                     compute_replace_value(routing, src_output[i]),
                                     active);
            }
        }
    }
}

// PolyToPoly: write to per-voice additive or replace buffers.
void apply_routing_poly_to_poly(const ResolvedRouting& routing,
                                const ModulationSource* source,
                                size_t num_samples) TANH_NONBLOCKING_FUNCTION {
    auto* vb = routing.m_target->m_voice.get();
    const uint32_t nv = std::min(source->num_voices(), vb->m_num_voices);
    if (routing.m_combine_mode == CombineMode::Additive) {
        const float depth = routing.m_depth_abs_precomputed.load(std::memory_order_relaxed);
        if (source->is_fully_active()) {
            for (uint32_t v = 0; v < nv; ++v) {
                const float* in = source->voice_output(v);
                float* out = vb->additive_voice(v);
                for (size_t i = 0; i < num_samples; ++i) { out[i] += in[i] * depth; }
            }
        } else {
            for (uint32_t v = 0; v < nv; ++v) {
                const float* in = source->voice_output(v);
                const uint8_t* src_active = source->voice_output_active(v);
                float* out = vb->additive_voice(v);
                for (size_t i = 0; i < num_samples; ++i) {
                    if (src_active[i]) { out[i] += in[i] * depth; }
                }
            }
        }
    } else {
        if (source->is_fully_active()) {
            for (uint32_t v = 0; v < nv; ++v) {
                const float* in = source->voice_output(v);
                float* out = vb->replace_voice(v);
                uint8_t* active = vb->replace_active_voice(v);
                for (size_t i = 0; i < num_samples; ++i) {
                    apply_replace_sample_voice(routing,
                                               out,
                                               active,
                                               i,
                                               compute_replace_value(routing, in[i]),
                                               true,
                                               v);
                }
            }
        } else {
            for (uint32_t v = 0; v < nv; ++v) {
                const float* in = source->voice_output(v);
                const uint8_t* src_active = source->voice_output_active(v);
                float* out = vb->replace_voice(v);
                uint8_t* active = vb->replace_active_voice(v);
                for (size_t i = 0; i < num_samples; ++i) {
                    apply_replace_sample_voice(routing,
                                               out,
                                               active,
                                               i,
                                               compute_replace_value(routing, in[i]),
                                               src_active[i] != 0,
                                               v);
                }
            }
        }
    }
}

// MonoToPoly: broadcast mono source to all voice buffers.
void apply_routing_mono_to_poly(const ResolvedRouting& routing,
                                const ModulationSource* source,
                                size_t num_samples) TANH_NONBLOCKING_FUNCTION {
    const auto& src_output = source->get_output_buffer();
    auto* vb = routing.m_target->m_voice.get();
    if (routing.m_combine_mode == CombineMode::Additive) {
        const float depth = routing.m_depth_abs_precomputed.load(std::memory_order_relaxed);
        if (source->is_fully_active()) {
            for (uint32_t v = 0; v < vb->m_num_voices; ++v) {
                float* out = vb->additive_voice(v);
                for (size_t i = 0; i < num_samples; ++i) { out[i] += src_output[i] * depth; }
            }
        } else {
            for (uint32_t v = 0; v < vb->m_num_voices; ++v) {
                float* out = vb->additive_voice(v);
                for (size_t i = 0; i < num_samples; ++i) {
                    if (source->get_output_active_at(static_cast<uint32_t>(i))) {
                        out[i] += src_output[i] * depth;
                    }
                }
            }
        }
    } else {
        if (source->is_fully_active()) {
            for (uint32_t v = 0; v < vb->m_num_voices; ++v) {
                float* out = vb->replace_voice(v);
                uint8_t* active = vb->replace_active_voice(v);
                for (size_t i = 0; i < num_samples; ++i) {
                    apply_replace_sample(routing,
                                         out,
                                         active,
                                         i,
                                         compute_replace_value(routing, src_output[i]),
                                         true);
                }
            }
        } else {
            for (uint32_t v = 0; v < vb->m_num_voices; ++v) {
                float* out = vb->replace_voice(v);
                uint8_t* active = vb->replace_active_voice(v);
                for (size_t i = 0; i < num_samples; ++i) {
                    const bool src_act = source->get_output_active_at(static_cast<uint32_t>(i));
                    apply_replace_sample(routing,
                                         out,
                                         active,
                                         i,
                                         compute_replace_value(routing, src_output[i]),
                                         src_act);
                }
            }
        }
    }
}

// Apply a single source sample for cyclic (per-sample) processing.
void apply_modulation_sample_mono(const ResolvedRouting& routing,
                                  float src_sample,
                                  bool src_active,
                                  size_t i) TANH_NONBLOCKING_FUNCTION {
    if (routing.m_combine_mode == CombineMode::Additive) {
        const float depth = routing.m_depth_abs_precomputed.load(std::memory_order_relaxed);
        if (src_active) { routing.m_target->m_additive_buffer[i] += src_sample * depth; }
    } else {
        apply_replace_sample(routing,
                             routing.m_target->m_replace_buffer.data(),
                             routing.m_target->m_replace_active.data(),
                             i,
                             compute_replace_value(routing, src_sample),
                             src_active);
    }
}

}  // namespace

// ── Per-source bulk processing ────────────────────────────────────────────────

void ModulationMatrix::process_source_bulk(const ProcessingConfig& config,
                                           ModulationSource* source,
                                           size_t num_samples) {
    auto it = config.m_routings_by_source.find(source);
    if (it == config.m_routings_by_source.end()) { return; }

    // Determine which process methods to call based on routing modes
    bool needs_mono = false;
    bool needs_voice = false;
    for (const auto* routing : it->second) {
        if (routing->m_routing_mode == RoutingMode::MonoToMono) {
            needs_mono = true;
        } else {
            needs_voice = true;
        }
    }

    // Clear per-block state before processing
    source->clear_change_points();
    if (!source->is_fully_active()) { source->clear_output_active(); }
    if (source->has_voice_output()) {
        source->clear_voice_change_points();
        if (!source->is_fully_active()) { source->clear_voice_output_active(); }
    }

    if (needs_mono && source->has_mono_output()) { source->process(num_samples); }
    if (needs_voice && source->has_voice_output()) {
        for (uint32_t v = 0; v < source->num_voices(); ++v) {
            source->process_voice(v, num_samples);
        }
    }
    // MonoToPoly uses mono output buffer — ensure it's processed
    if (needs_voice && !source->has_voice_output() && source->has_mono_output()) {
        if (!needs_mono) { source->process(num_samples); }
    }

    for (const auto* routing : it->second) {
        if (routing->m_skip_during_gesture &&
            routing->m_target->m_record->m_in_gesture.load(std::memory_order_relaxed)) {
            continue;
        }

        switch (routing->m_routing_mode) {
            case RoutingMode::MonoToMono:
                apply_routing_mono_to_mono(*routing, source, num_samples);
                break;
            case RoutingMode::PolyToPoly:
                apply_routing_poly_to_poly(*routing, source, num_samples);
                break;
            case RoutingMode::MonoToPoly:
                apply_routing_mono_to_poly(*routing, source, num_samples);
                break;
            case RoutingMode::PolyToMono: break;  // rejected at schedule-build time
        }

        // Propagate change points to target flags
        if (routing->m_routing_mode == RoutingMode::MonoToMono) {
            for (const uint32_t cp : source->get_change_points()) {
                if (cp < num_samples) { routing->m_target->m_change_point_flags[cp] = true; }
            }
        } else if (routing->m_routing_mode == RoutingMode::PolyToPoly &&
                   routing->m_target->m_voice) {
            const uint32_t nv =
                std::min(source->num_voices(), routing->m_target->m_voice->m_num_voices);
            for (uint32_t v = 0; v < nv; ++v) {
                const auto& vcp = source->get_voice_change_points(v);
                const size_t base =
                    static_cast<size_t>(v) * routing->m_target->m_voice->m_block_size;
                for (const uint32_t cp : vcp) {
                    if (cp < num_samples) {
                        routing->m_target->m_voice->m_change_point_flags_storage[base + cp] = 1;
                    }
                }
            }
        } else if (routing->m_routing_mode == RoutingMode::MonoToPoly &&
                   routing->m_target->m_voice) {
            // Broadcast mono change points to all voices
            for (const uint32_t cp : source->get_change_points()) {
                if (cp >= num_samples) { continue; }
                for (uint32_t v = 0; v < routing->m_target->m_voice->m_num_voices; ++v) {
                    const size_t base =
                        static_cast<size_t>(v) * routing->m_target->m_voice->m_block_size;
                    routing->m_target->m_voice->m_change_point_flags_storage[base + cp] = 1;
                }
            }
        }

        apply_routing_change_points(*routing, num_samples);
    }
}

// ── Cyclic group processing (per-sample with z⁻¹) ────────────────────────────

void ModulationMatrix::process_cyclic(const ProcessingConfig& config,
                                      const std::vector<ModulationSource*>& sources,
                                      size_t num_samples) {
    // Clear per-block state for all sources
    for (auto* source : sources) {
        source->clear_change_points();
        if (!source->is_fully_active()) { source->clear_output_active(); }
        if (source->has_voice_output()) {
            source->clear_voice_change_points();
            if (!source->is_fully_active()) { source->clear_voice_output_active(); }
        }
    }

    // Per-sample processing: interleave sources, apply routings immediately
    for (size_t i = 0; i < num_samples; ++i) {
        for (auto* source : sources) {
            // Mono path
            if (source->has_mono_output()) {
                source->process(1, i);
                const float sample = source->get_output_at(static_cast<uint32_t>(i));
                const bool active = source->is_fully_active() ||
                                    source->get_output_active_at(static_cast<uint32_t>(i)) != 0;

                auto it = config.m_routings_by_source.find(source);
                if (it != config.m_routings_by_source.end()) {
                    for (const auto* routing : it->second) {
                        if (routing->m_routing_mode == RoutingMode::MonoToMono) {
                            if (routing->m_skip_during_gesture &&
                                routing->m_target->m_record->m_in_gesture.load(
                                    std::memory_order_relaxed)) {
                                continue;
                            }
                            apply_modulation_sample_mono(*routing, sample, active, i);
                        }
                    }
                }
            }

            // Voice path
            if (source->has_voice_output()) {
                auto it = config.m_routings_by_source.find(source);
                for (uint32_t v = 0; v < source->num_voices(); ++v) {
                    source->process_voice(v, 1, i);
                    const float sample = source->voice_output(v)[i];
                    const bool active =
                        source->is_fully_active() || source->voice_output_active(v)[i] != 0;

                    if (it != config.m_routings_by_source.end()) {
                        for (const auto* routing : it->second) {
                            if (routing->m_routing_mode == RoutingMode::PolyToPoly &&
                                routing->m_target->m_voice &&
                                v < routing->m_target->m_voice->m_num_voices) {
                                if (routing->m_skip_during_gesture &&
                                    routing->m_target->m_record->m_in_gesture.load(
                                        std::memory_order_relaxed)) {
                                    continue;
                                }
                                if (routing->m_combine_mode == CombineMode::Additive) {
                                    if (active) {
                                        const float depth = routing->m_depth_abs_precomputed.load(
                                            std::memory_order_relaxed);
                                        routing->m_target->m_voice->additive_voice(v)[i] +=
                                            sample * depth;
                                    }
                                } else {
                                    apply_replace_sample_voice(
                                        *routing,
                                        routing->m_target->m_voice->replace_voice(v),
                                        routing->m_target->m_voice->replace_active_voice(v),
                                        i,
                                        compute_replace_value(*routing, sample),
                                        active,
                                        v);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Propagate change points from sources to target flags
    for (auto* source : sources) {
        auto it = config.m_routings_by_source.find(source);
        if (it == config.m_routings_by_source.end()) { continue; }

        for (const auto* routing : it->second) {
            if (routing->m_skip_during_gesture &&
                routing->m_target->m_record->m_in_gesture.load(std::memory_order_relaxed)) {
                continue;
            }

            if (routing->m_routing_mode == RoutingMode::MonoToMono) {
                for (const uint32_t cp : source->get_change_points()) {
                    if (cp < num_samples) { routing->m_target->m_change_point_flags[cp] = true; }
                }
            } else if (routing->m_routing_mode == RoutingMode::PolyToPoly &&
                       routing->m_target->m_voice) {
                const uint32_t nv =
                    std::min(source->num_voices(), routing->m_target->m_voice->m_num_voices);
                for (uint32_t v = 0; v < nv; ++v) {
                    const auto& vcp = source->get_voice_change_points(v);
                    const size_t base =
                        static_cast<size_t>(v) * routing->m_target->m_voice->m_block_size;
                    for (const uint32_t cp : vcp) {
                        if (cp < num_samples) {
                            routing->m_target->m_voice->m_change_point_flags_storage[base + cp] = 1;
                        }
                    }
                }
            }
            apply_routing_change_points(*routing, num_samples);
        }
    }
}

// ── Routing change point helper ───────────────────────────────────────────────

void ModulationMatrix::apply_routing_change_points(const ResolvedRouting& routing,
                                                   size_t num_samples) {
    if (routing.m_max_decimation == 0) { return; }

    for (size_t i = 0; i < num_samples; ++i) {
        if (routing.m_samples_until_update == 0) {
            routing.m_target->m_change_point_flags[i] = true;
            routing.m_samples_until_update = routing.m_max_decimation;
        }
        --routing.m_samples_until_update;
    }
}

// ── Serialization ───────────────────────────────────────────────────────────

namespace {

const char* depth_mode_to_string(DepthMode mode) {
    switch (mode) {
        case DepthMode::Absolute: return "absolute";
        case DepthMode::Normalized: return "normalized";
    }
    return "normalized";
}

DepthMode depth_mode_from_string(const std::string& str) {
    if (str == "absolute") { return DepthMode::Absolute; }
    return DepthMode::Normalized;
}

const char* combine_mode_to_string(CombineMode mode) {
    switch (mode) {
        case CombineMode::Additive: return "additive";
        case CombineMode::Replace: return "replace";
        case CombineMode::ReplaceHold: return "replace_hold";
    }
    return "additive";
}

CombineMode combine_mode_from_string(const std::string& str) {
    if (str == "replace") { return CombineMode::Replace; }
    if (str == "replace_hold") { return CombineMode::ReplaceHold; }
    return CombineMode::Additive;
}

}  // namespace

nlohmann::json  // NOLINT(misc-include-cleaner)
    ModulationMatrix::to_json(bool include_state) {
    std::scoped_lock const lock(m_writer_mutex);

    nlohmann::json routings_array = nlohmann::json::array();
    for (const auto& r : m_user_routings) {
        nlohmann::json obj;
        obj["id"] = r.m_id;
        obj["source_id"] = r.m_source_id;
        obj["target_id"] = r.m_target_id;
        obj["depth"] = r.m_depth;
        obj["depth_mode"] = depth_mode_to_string(r.m_depth_mode);
        obj["combine_mode"] = combine_mode_to_string(r.m_combine_mode);
        obj["max_decimation"] = r.m_max_decimation;
        if (r.m_has_replace_range) {
            obj["replace_range_min"] = r.m_replace_range_min;
            obj["replace_range_max"] = r.m_replace_range_max;
        }
        if (r.m_skip_during_gesture) { obj["skip_during_gesture"] = true; }
        routings_array.push_back(std::move(obj));
    }

    if (!include_state) { return routings_array; }

    nlohmann::json root;
    root["parameters"] = m_state.to_json();
    root["modulation_routings"] = std::move(routings_array);
    return root;
}

void ModulationMatrix::from_json(const nlohmann::json& json) {
    std::scoped_lock const lock(m_writer_mutex);

    // Helper: parse a single routing JSON object into a ModulationRouting.
    auto parse_routing = [](const nlohmann::json& obj) -> ModulationRouting {
        ModulationRouting r;
        r.m_id = obj.value("id", k_invalid_routing_id);
        r.m_source_id = obj.value("source_id", "");
        r.m_target_id = obj.value("target_id", "");
        r.m_depth = obj.value("depth", 1.0f);
        r.m_depth_mode = depth_mode_from_string(obj.value("depth_mode", "normalized"));
        r.m_combine_mode = combine_mode_from_string(obj.value("combine_mode", "additive"));
        r.m_max_decimation = obj.value("max_decimation", uint32_t{0});
        r.m_skip_during_gesture = obj.value("skip_during_gesture", false);
        if (obj.contains("replace_range_min") && obj.contains("replace_range_max")) {
            r.m_replace_range_min = obj.value("replace_range_min", 0.0f);
            r.m_replace_range_max = obj.value("replace_range_max", 1.0f);
            r.m_has_replace_range = true;
        }
        return r;
    };

    // Helper: after loading routings, assign IDs to any that are missing and
    // advance m_next_routing_id past the highest loaded ID.
    auto finalize_ids = [this]() {
        uint32_t max_id = 0;
        for (const auto& r : m_user_routings) {
            if (r.m_id != k_invalid_routing_id && r.m_id > max_id) { max_id = r.m_id; }
        }
        m_next_routing_id = max_id + 1;
        // Assign IDs to routings that didn't have one in the JSON.
        for (auto& r : m_user_routings) {
            if (r.m_id == k_invalid_routing_id) { r.m_id = m_next_routing_id++; }
        }
    };

    // Restore routings
    if (json.contains("modulation_routings") && json["modulation_routings"].is_array()) {
        m_user_routings.clear();
        for (const auto& obj : json["modulation_routings"]) {
            m_user_routings.push_back(parse_routing(obj));
        }
        finalize_ids();
        rebuild_schedule_locked();
    } else if (json.is_array()) {
        // Bare routings array (from to_json(false))
        m_user_routings.clear();
        for (const auto& obj : json) { m_user_routings.push_back(parse_routing(obj)); }
        finalize_ids();
        rebuild_schedule_locked();
    }

    // Forward parameters to State
    if (json.contains("parameters") && json["parameters"].is_array()) {
        m_state.from_json(json["parameters"]);
    }
}
