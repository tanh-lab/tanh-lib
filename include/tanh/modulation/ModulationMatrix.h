#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <tanh/core/threading/RCU.h>
#include <tanh/modulation/ModulationRouting.h>
#include <tanh/modulation/ModulationSource.h>
#include <tanh/modulation/ResolvedRouting.h>
#include <tanh/modulation/ResolvedTarget.h>
#include <tanh/modulation/SmartHandle.h>

namespace thl {
class State;
}

namespace thl::modulation {

// Schedule step types for the processing schedule built by Tarjan SCC.
struct BulkStep {
    ModulationSource* source;
};

struct CyclicStep {
    std::vector<ModulationSource*> sources;
};

using ScheduleStep = std::variant<BulkStep, CyclicStep>;

// All state read by the RT thread — bundled into a single RCU instance for
// atomic publication. Pointers in routings_by_source reference elements in
// the routings vector of the same ProcessingConfig instance; they stay valid
// for the duration of an RCU read section.
struct ProcessingConfig {
    std::vector<ResolvedRouting> routings;
    std::vector<ScheduleStep> schedule;
    std::unordered_map<ModulationSource*, std::vector<const ResolvedRouting*>>
        routings_by_source;
    std::vector<ResolvedTarget*> active_targets;
};

class ModulationMatrix {
public:
    explicit ModulationMatrix(thl::State& state);
    ~ModulationMatrix() = default;

    ModulationMatrix(const ModulationMatrix&) = delete;
    ModulationMatrix& operator=(const ModulationMatrix&) = delete;

    void prepare(double sample_rate, size_t samples_per_block);

    // Process all sources and fill modulation buffers for all targets.
    void process(size_t num_samples);

    // Source management
    void add_source(const std::string& id, ModulationSource* source);

    // Remove source and block until all RT readers have finished with the old
    // config. After this returns the caller may safely delete the source.
    void remove_source(const std::string& id);

    // Get a SmartHandle for a State parameter. Lazily creates a ResolvedTarget
    // the first time a parameter key is requested. The returned SmartHandle
    // holds a stable pointer into the target map — no registration needed.
    //
    // Throws StateKeyNotFoundException if the parameter doesn't exist in State.
    // Throws std::invalid_argument if the parameter's definition has
    // modulation disabled.
    SmartHandle get_smart_handle(const std::string& param_key);

    // Routing management
    void add_routing(const ModulationRouting& routing);
    void remove_routing(const std::string& source_id,
                        const std::string& target_id);

    // Access the resolved target for reading modulation data.
    const ResolvedTarget* get_target(const std::string& id) const;
    ResolvedTarget* get_target(const std::string& id);

    // Returns a snapshot of the processing schedule (thread-safe).
    std::vector<ScheduleStep> get_schedule() const;

    // Rebuild the processing schedule. Thread-safe (acquires writer mutex).
    void rebuild_schedule();

private:
    // Internal rebuild — must be called with m_writer_mutex held.
    void rebuild_schedule_locked();

    // Ensure a target exists for the given id. Returns a stable pointer.
    // Must be called with m_writer_mutex held.
    ResolvedTarget* ensure_target_locked(const std::string& id);

    // Process helpers — called from within RCU read section.
    void process_source_bulk(const ProcessingConfig& config,
                             ModulationSource* source, size_t num_samples);
    void process_cyclic(const ProcessingConfig& config,
                        const std::vector<ModulationSource*>& sources,
                        size_t num_samples);

    void apply_routing_change_points(const ResolvedRouting& routing,
                                     size_t num_samples);

    // Tarjan SCC helper
    void build_schedule_from_graph(
        const std::vector<std::string>& source_ids,
        const std::unordered_map<std::string, std::vector<std::string>>& adj,
        const std::unordered_map<std::string, bool>& has_self_edge,
        std::vector<ScheduleStep>& out_schedule);

    thl::State& m_state;

    double m_sample_rate = 48000.0;
    size_t m_samples_per_block = 512;

    // Writer mutex — serializes all non-RT methods
    std::mutex m_writer_mutex;

    // Registered sources (not owned) — protected by m_writer_mutex
    std::unordered_map<std::string, ModulationSource*> m_sources;

    // Registered targets (owned) — protected by m_writer_mutex.
    // Pointer stability guaranteed by std::unordered_map.
    std::unordered_map<std::string, ResolvedTarget> m_targets;

    // User-facing routings — protected by m_writer_mutex
    std::vector<ModulationRouting> m_user_routings;

    // RT-safe processing config — RCU-protected for lock-free RT reads
    thl::RCU<ProcessingConfig> m_config;
};

}  // namespace thl::modulation
