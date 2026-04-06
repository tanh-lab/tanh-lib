#include "tanh/modulation/ModulationMatrix.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stack>
#include <stdexcept>
#include <string_view>
#include <string>
#include <variant>
#include <vector>
#include <unordered_map>
#include <utility>

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

// Explicit instantiations for all supported types
template SmartHandle<float> ModulationMatrix::get_smart_handle<float>(std::string_view);
template SmartHandle<double> ModulationMatrix::get_smart_handle<double>(std::string_view);
template SmartHandle<int> ModulationMatrix::get_smart_handle<int>(std::string_view);
template SmartHandle<bool> ModulationMatrix::get_smart_handle<bool>(std::string_view);

ResolvedTarget* ModulationMatrix::ensure_target_locked(const std::string_view id) {
    auto it = m_targets.find(id);
    if (it != m_targets.end()) { return &it->second; }
    ResolvedTarget target;
    auto [target_it, inserted] = m_targets.emplace(std::string(id), target);
    auto& t = target_it->second;
    t.m_id = id;
    t.resize(m_samples_per_block);

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
    auto [source_it, inserted] = m_sources.emplace(std::string(id), source);
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

void ModulationMatrix::add_routing(const ModulationRouting& routing) {
    std::scoped_lock const lock(m_writer_mutex);
    m_user_routings.push_back(routing);
    rebuild_schedule_locked();
}

void ModulationMatrix::remove_routing(const std::string_view source_id,
                                      const std::string_view target_id) {
    std::scoped_lock const lock(m_writer_mutex);
    std::erase_if(m_user_routings, [&](const ModulationRouting& r) {
        return r.m_source_id == source_id && r.m_target_id == target_id;
    });
    rebuild_schedule_locked();
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
    // Resolve routings from user-facing strings to raw pointers
    std::vector<ResolvedRouting> new_routings;
    for (auto& routing : m_user_routings) {
        auto src_it = m_sources.find(routing.m_source_id);
        auto tgt_it = m_targets.find(routing.m_target_id);

        if (src_it == m_sources.end() || tgt_it == m_targets.end()) {
            continue;  // Skip unresolved routings
        }

        ResolvedRouting r;
        r.m_source = src_it->second;
        r.m_target = &tgt_it->second;
        r.m_depth = routing.m_depth;
        r.m_depth_mode = routing.m_depth_mode;
        r.m_max_decimation = routing.m_max_decimation;
        r.m_samples_until_update = 0;

        // Pre-compute effective depth for the RT hot path.
        // Non-linear targets accumulate in normalized [0,1] space; linear targets
        // accumulate in plain parameter units. m_depth_abs_precomputed holds the
        // per-sample multiplier so the inner loop is branch-free.
        const auto* range = tgt_it->second.m_range;
        if (range) {
            const float span = range->m_max - range->m_min;
            if (r.m_target->m_uses_normalized_buffer) {
                // Non-linear: buffer is in normalized space
                r.m_depth_abs_precomputed =
                    r.m_depth_mode == DepthMode::Absolute ? r.m_depth / span : r.m_depth;
            } else {
                // Linear: buffer is in plain parameter units
                r.m_depth_abs_precomputed =
                    r.m_depth_mode == DepthMode::Normalized ? r.m_depth * span : r.m_depth;
            }
        }
        new_routings.push_back(r);
    }

    // Build target → owning source map (for dependency graph).
    // A target is "owned" by a source if its id matches one of the source's
    // parameter_keys(). This creates edges in the dependency graph.
    std::unordered_map<std::string, std::string> target_owner;
    for (auto& [source_id, source] : m_sources) {
        for (auto& key : source->parameter_keys()) { target_owner[key] = source_id; }
    }

    // Build dependency graph: adjacency list + self-edge detection.
    // Edge from source A to source B means: A depends on B's output
    // (there exists a routing from B to one of A's parameter targets).
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
            // owner depends on routing.source_id
            adj[std::string(owner_id)].push_back(routing.m_source_id);
        }
    }

    // Build the schedule via Tarjan SCC
    std::vector<ScheduleStep> new_schedule;
    build_schedule_from_graph(source_ids, adj, has_self_edge, new_schedule);

    // Collect active target pointers (pointer-stable in unordered_map)
    std::vector<ResolvedTarget*> new_active_targets;
    new_active_targets.reserve(m_targets.size());
    for (auto& [id, target] : m_targets) { new_active_targets.push_back(&target); }

    // Publish everything atomically via RCU
    m_config.update([&](ProcessingConfig& config) {
        config.m_routings = std::move(new_routings);
        config.m_schedule = std::move(new_schedule);
        config.m_active_targets = std::move(new_active_targets);
        // Rebuild per-source routing lookup — pointers into config.routings
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

    // Tarjan outputs SCCs in topological order (dependencies first).
    // Each SCC with 1 node and no self-edge → BulkStep.
    // Each SCC with multiple nodes or a self-edge → CyclicStep.
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

// ── Depth-mode-aware modulation helpers ──────────────────────────────────────

namespace {

// Apply a single source sample through a routing to the target buffer at index i.
// All depth modes and range types are handled uniformly via m_depth_abs_precomputed
// (set at schedule build time for all four depth-mode × range-type combinations).
void apply_modulation_sample(const ResolvedRouting& routing,
                             float src_sample,
                             size_t i) TANH_NONBLOCKING_FUNCTION {
    routing.m_target->m_modulation_buffer[i] += src_sample * routing.m_depth_abs_precomputed;
}

// Fill the modulation buffer for a bulk routing (whole block at once).
void apply_routing_bulk(const ResolvedRouting& routing,
                        const std::vector<float>& src_output,
                        size_t num_samples) TANH_NONBLOCKING_FUNCTION {
    const float depth = routing.m_depth_abs_precomputed;
    for (size_t i = 0; i < num_samples; ++i) {
        routing.m_target->m_modulation_buffer[i] += src_output[i] * depth;
    }
}

}  // namespace

// ── Per-source bulk processing ────────────────────────────────────────────────

void ModulationMatrix::process_source_bulk(const ProcessingConfig& config,
                                           ModulationSource* source,
                                           size_t num_samples) {
    source->process(num_samples);

    auto it = config.m_routings_by_source.find(source);
    if (it == config.m_routings_by_source.end()) { return; }

    for (auto* routing : it->second) {
        const auto& src_output = source->get_output_buffer();
        apply_routing_bulk(*routing, src_output, num_samples);

        for (uint32_t const cp : source->get_change_points()) {
            if (cp < num_samples) { routing->m_target->m_change_point_flags[cp] = true; }
        }

        apply_routing_change_points(*routing, num_samples);
    }
}

// ── Cyclic group processing (per-sample with z⁻¹) ────────────────────────────

void ModulationMatrix::process_cyclic(const ProcessingConfig& config,
                                      const std::vector<ModulationSource*>& sources,
                                      size_t num_samples) {
    // Clear change points for all cyclic sources
    for (auto* source : sources) { source->clear_change_points(); }

    // Per-sample processing: interleave sources, apply routings immediately
    for (size_t i = 0; i < num_samples; ++i) {
        for (auto* source : sources) {
            float sample = 0.0f;
            source->process_single(&sample, static_cast<uint32_t>(i));

            // Apply routing immediately so subsequent sources see updated
            // target buffers within the same sample iteration.
            auto it = config.m_routings_by_source.find(source);
            if (it != config.m_routings_by_source.end()) {
                for (auto* routing : it->second) { apply_modulation_sample(*routing, sample, i); }
            }
        }
    }

    // Propagate change points from sources to target flags
    for (auto* source : sources) {
        auto it = config.m_routings_by_source.find(source);
        if (it == config.m_routings_by_source.end()) { continue; }

        for (auto* routing : it->second) {
            for (uint32_t const cp : source->get_change_points()) {
                if (cp < num_samples) { routing->m_target->m_change_point_flags[cp] = true; }
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
