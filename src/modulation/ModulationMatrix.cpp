#include "tanh/modulation/ModulationMatrix.h"

#include <algorithm>
#include <functional>
#include <stack>
#include <stdexcept>

#include "tanh/state/State.h"

using namespace thl::modulation;

ModulationMatrix::ModulationMatrix(thl::State& state) : m_state(state) {
    m_config.register_reader_thread();
}

SmartHandle ModulationMatrix::get_smart_handle(const std::string_view param_key) {
    std::lock_guard<std::mutex> lock(m_writer_mutex);

    // Throws StateKeyNotFoundException if parameter doesn't exist
    auto handle = m_state.get_handle<float>(param_key);

    // Check modulation flag if a definition exists
    auto def = m_state.get_definition_from_root(param_key);
    if (def.has_value() && !def->m_modulation) {
        throw std::invalid_argument("Parameter '" + std::string(param_key) +
                                    "' has modulation disabled");
    }

    auto* target = ensure_target_locked(param_key);
    return SmartHandle(handle, target);
}

ResolvedTarget* ModulationMatrix::ensure_target_locked(const std::string_view id) {
    auto it = m_targets.find(id);
    if (it != m_targets.end()) { return &it->second; }
    ResolvedTarget target;
    auto [target_it, inserted] = m_targets.emplace(std::string(id), target);
    target_it->second.m_id = id;
    target_it->second.resize(m_samples_per_block);
    return &target_it->second;
}

void ModulationMatrix::prepare(double sample_rate, size_t samples_per_block) {
    std::lock_guard<std::mutex> lock(m_writer_mutex);

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
    std::lock_guard<std::mutex> lock(m_writer_mutex);
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
    std::lock_guard<std::mutex> lock(m_writer_mutex);
    m_sources.erase(std::string(id));

    // Remove user-facing routings that reference this source.
    m_user_routings.erase(
        std::remove_if(m_user_routings.begin(),
                       m_user_routings.end(),
                       [&](const ModulationRouting& r) { return r.m_source_id == id; }),
        m_user_routings.end());

    rebuild_schedule_locked();
    // Block until all RT readers have finished with the old config that still
    // referenced the removed source. After this returns the caller may safely
    // delete the ModulationSource object.
    m_config.synchronize();
}

void ModulationMatrix::add_routing(const ModulationRouting& routing) {
    std::lock_guard<std::mutex> lock(m_writer_mutex);
    m_user_routings.push_back(routing);
    rebuild_schedule_locked();
}

void ModulationMatrix::remove_routing(const std::string_view source_id,
                                      const std::string_view target_id) {
    std::lock_guard<std::mutex> lock(m_writer_mutex);
    m_user_routings.erase(std::remove_if(m_user_routings.begin(),
                                         m_user_routings.end(),
                                         [&](const ModulationRouting& r) {
                                             return r.m_source_id == source_id &&
                                                    r.m_target_id == target_id;
                                         }),
                          m_user_routings.end());
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
    std::lock_guard<std::mutex> lock(m_writer_mutex);
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
        r.m_max_decimation = routing.m_max_decimation;
        r.m_samples_until_update = 0;
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

    std::function<void(const std::string)> strongconnect = [&](const std::string v) {
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
        bool self_loop = self_it != has_self_edge.end() && self_it->second;

        if (scc.size() == 1 && !self_loop) {
            auto src_it = m_sources.find(scc[0]);
            if (src_it != m_sources.end()) { out_schedule.push_back(BulkStep{src_it->second}); }
        } else {
            CyclicStep step;
            step.m_sources.reserve(scc.size());
            for (auto& id : scc) {
                auto src_it = m_sources.find(id);
                if (src_it != m_sources.end()) { step.m_sources.push_back(src_it->second); }
            }
            out_schedule.push_back(std::move(step));
        }
    }
}

// ── Per-source bulk processing ────────────────────────────────────────────────

void ModulationMatrix::process_source_bulk(const ProcessingConfig& config,
                                           ModulationSource* source,
                                           size_t num_samples) {
    source->process(num_samples);

    auto it = config.m_routings_by_source.find(source);
    if (it == config.m_routings_by_source.end()) { return; }

    for (auto* routing : it->second) {
        const auto& src_output = source->get_output_buffer();
        for (size_t i = 0; i < num_samples; ++i) {
            routing->m_target->m_modulation_buffer[i] += src_output[i] * routing->m_depth;
        }

        for (uint32_t cp : source->get_change_points()) {
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
            float sample;
            source->process_single(&sample, static_cast<uint32_t>(i));

            // Apply routing immediately so subsequent sources see updated
            // target buffers within the same sample iteration.
            auto it = config.m_routings_by_source.find(source);
            if (it != config.m_routings_by_source.end()) {
                for (auto* routing : it->second) {
                    routing->m_target->m_modulation_buffer[i] += sample * routing->m_depth;
                }
            }
        }
    }

    // Propagate change points from sources to target flags
    for (auto* source : sources) {
        auto it = config.m_routings_by_source.find(source);
        if (it == config.m_routings_by_source.end()) { continue; }

        for (auto* routing : it->second) {
            for (uint32_t cp : source->get_change_points()) {
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
