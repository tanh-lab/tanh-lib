#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <map>
#include <variant>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include <tanh/core/Exports.h>
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
    ModulationSource* m_source;
};

struct CyclicStep {
    std::vector<ModulationSource*> m_sources;
};

using ScheduleStep = std::variant<BulkStep, CyclicStep>;

// All state read by the RT thread — bundled into a single RCU instance for
// atomic publication. Pointers in routings_by_source reference elements in
// the routings vector of the same ProcessingConfig instance; they stay valid
// for the duration of an RCU read section.
struct ProcessingConfig {
    std::vector<ResolvedRouting> m_routings;
    std::vector<ScheduleStep> m_schedule;
    std::unordered_map<ModulationSource*, std::vector<const ResolvedRouting*>> m_routings_by_source;
    std::vector<ResolvedTarget*> m_active_targets;
};

class TANH_API ModulationMatrix {
public:
    explicit ModulationMatrix(thl::State& state);
    ~ModulationMatrix() = default;

    ModulationMatrix(const ModulationMatrix&) = delete;
    ModulationMatrix& operator=(const ModulationMatrix&) = delete;

    void prepare(double sample_rate, size_t samples_per_block);

    // Process all sources and fill modulation buffers for all targets.
    void process(size_t num_samples) TANH_NONBLOCKING_FUNCTION;

    // Source management
    void add_source(const std::string_view id, ModulationSource* source);

    // Remove source and block until all RT readers have finished with the old
    // config. After this returns the caller may safely delete the source.
    void remove_source(const std::string_view id);

    // Get a SmartHandle for a State parameter. Lazily creates a ResolvedTarget
    // the first time a parameter key is requested. The returned SmartHandle
    // holds a stable pointer into the target map — no registration needed.
    //
    // T must match the parameter's native type (float, double, int, bool).
    //
    // Throws StateKeyNotFoundException if the parameter doesn't exist in State.
    // Throws std::invalid_argument if the parameter's definition has
    // modulation disabled or if T doesn't match the parameter's type.
    template <typename T>
    SmartHandle<T> get_smart_handle(std::string_view param_key);

    // Routing management
    // Returns a unique routing ID, or k_invalid_routing_id (0) if rejected.
    // Rejects duplicate (source, target) pairs and duplicate Replace on the same target.
    uint32_t add_routing(const ModulationRouting& routing);
    void remove_routing(std::string_view source_id, std::string_view target_id);
    void remove_routing(uint32_t routing_id);

    // Update routing depth without schedule rebuild. Thread-safe.
    // Returns false if the routing was not found.
    bool update_routing_depth(std::string_view source_id,
                              std::string_view target_id,
                              float new_depth);
    bool update_routing_depth(uint32_t routing_id, float new_depth);

    // Set replace range on a routing without schedule rebuild. Thread-safe.
    // Maps source [0,1] to [range_min, range_max] in plain parameter units.
    // Only meaningful for Replace/ReplaceHold combine modes.
    // Returns false if the routing was not found.
    bool update_routing_replace_range(std::string_view source_id,
                                      std::string_view target_id,
                                      float range_min,
                                      float range_max);
    bool update_routing_replace_range(uint32_t routing_id, float range_min, float range_max);

    // Clear replace range (revert to src * depth_precomputed behavior). Thread-safe.
    // Returns false if the routing was not found.
    bool clear_routing_replace_range(std::string_view source_id, std::string_view target_id);
    bool clear_routing_replace_range(uint32_t routing_id);

    // Access the resolved target for reading modulation data.
    const ResolvedTarget* get_target(const std::string_view id) const;
    ResolvedTarget* get_target(const std::string_view id);

    // Returns a snapshot of the processing schedule (thread-safe).
    std::vector<ScheduleStep> get_schedule() const;

    // Rebuild the processing schedule. Thread-safe (acquires writer mutex).
    void rebuild_schedule();

    // ── Serialization ───────────────────────────────────────────────────
    // Serialize modulation routings (and optionally State parameters) to JSON.
    // include_state=true wraps both under {"parameters":..., "modulation_routings":...}.
    // include_state=false returns just the routings array.
    nlohmann::json to_json(bool include_state = true);

    // Deserialize from JSON. Reads "modulation_routings" if present, replaces
    // all user routings, and rebuilds the schedule. Forwards "parameters" to
    // State::from_json() if present.
    void from_json(const nlohmann::json& json);

private:
    // Internal rebuild — must be called with m_writer_mutex held.
    void rebuild_schedule_locked();

    // Ensure a target exists for the given id. Returns a stable pointer.
    // Must be called with m_writer_mutex held.
    ResolvedTarget* ensure_target_locked(const std::string_view id);

    // Routing lookup helpers — must be called with m_writer_mutex held.
    ModulationRouting* find_user_routing_locked(std::string_view source_id,
                                                std::string_view target_id);
    ModulationRouting* find_user_routing_locked(uint32_t routing_id);

    // Compute the precomputed depth value for a routing given its target.
    static float compute_depth_precomputed(const ModulationRouting& routing,
                                           const ResolvedTarget& target);

    // Routing update helpers — must be called with m_writer_mutex held.
    bool update_routing_depth_locked(ModulationRouting& user_routing, float new_depth);
    bool update_routing_replace_range_locked(ModulationRouting& user_routing,
                                             float range_min,
                                             float range_max);
    bool clear_routing_replace_range_locked(ModulationRouting& user_routing);

    // Process helpers — called from within RCU read section.
    void process_source_bulk(const ProcessingConfig& config,
                             ModulationSource* source,
                             size_t num_samples);
    void process_cyclic(const ProcessingConfig& config,
                        const std::vector<ModulationSource*>& sources,
                        size_t num_samples);

    void apply_routing_change_points(const ResolvedRouting& routing, size_t num_samples);

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
    std::map<std::string, ModulationSource*, std::less<>> m_sources;

    // Registered targets (owned) — protected by m_writer_mutex.
    // Pointer stability guaranteed by std::unordered_map.
    std::map<std::string, ResolvedTarget, std::less<>> m_targets;

    // User-facing routings — protected by m_writer_mutex
    std::vector<ModulationRouting> m_user_routings;

    // Monotonically increasing routing ID counter — protected by m_writer_mutex.
    // Starts at 1; 0 is k_invalid_routing_id.
    uint32_t m_next_routing_id = 1;

    // RT-safe processing config — RCU-protected for lock-free RT reads
    thl::RCU<ProcessingConfig> m_config;
};

}  // namespace thl::modulation
