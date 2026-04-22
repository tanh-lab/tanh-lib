#pragma once

#include <cstddef>
#include <list>
#include <mutex>
#include <optional>
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
#include <tanh/state/ModulationScope.h>

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

    // Every registered source — populated in rebuild_schedule_with_lock(). Used
    // by process() to run the pre_process_block() drain pass exactly once per
    // source per block, before any ScheduleStep. Contains every source added
    // via add_source(), including sources with no routings.
    std::vector<ModulationSource*> m_all_sources;

    // Per-target Replace routings sorted ascending by priority. Only populated
    // for targets with >1 Replace/ReplaceHold routing — the common single-
    // Replace case keeps the in-schedule fast path with m_replace_deferred =
    // false. Deferred routings are re-applied after the schedule loop so
    // higher-priority writes overwrite lower-priority ones deterministically
    // regardless of source schedule order.
    std::vector<std::pair<ResolvedTarget*, std::vector<const ResolvedRouting*>>>
        m_multi_replace_targets;
};

class TANH_API ModulationMatrix {
public:
    explicit ModulationMatrix(thl::State& state);
    ~ModulationMatrix();

    ModulationMatrix(const ModulationMatrix&) = delete;
    ModulationMatrix& operator=(const ModulationMatrix&) = delete;

    // Access the underlying State — useful when downstream code holds only a
    // ModulationMatrix& but still needs non-RT writes (e.g. setting string
    // parameters, loading presets) that don't route through the matrix.
    thl::State& state() { return m_state; }
    const thl::State& state() const { return m_state; }

    // Allocate internal buffers sized to samples_per_block and rebuild the
    // schedule. Every target's VoiceBuffers / MonoBuffers is freshly allocated
    // and atomically published — old buffers are retired past a synchronize()
    // barrier so in-flight audio blocks always see either the old or the new
    // buffer in full, never a torn mid-resize state.
    //
    // Threading contract:
    // - Same-size prepare (same samples_per_block) is safe to call concurrently
    //   with process_with_scope(). The routing-churn race is closed by the flag
    //   guards on every target-buffer access.
    // - Shrinking prepare (new samples_per_block < the num_samples the audio
    //   thread is currently calling process_with_scope() with) is NOT safe. The
    //   apply_routing_* loops iterate over num_samples, which may now exceed
    //   the newly-published m_block_size and write OOB into buffer storage.
    //   Callers must quiesce audio — stop the driver, or call prepare() from
    //   the driver's own reconfiguration callback — before reducing block
    //   size. Growing prepare is safe because the old num_samples fits the
    //   new larger storage.
    void prepare(double sample_rate, size_t samples_per_block);

    using ConfigRCU = thl::RCU<ProcessingConfig>;
    using ReadScope = typename ConfigRCU::ReadScope;

    // Open an RCU read section covering the full audio block. Callers that
    // need to reach into ResolvedTarget buffers outside of ModulationMatrix
    // (e.g. DSP processors reading SmartHandle state) must hold a ReadScope
    // for the duration of those reads — the scope prevents the writer from
    // reclaiming retired VoiceBuffers / MonoBuffers while the audio thread
    // still references them.
    //
    // Usage on the audio thread:
    //   auto scope = matrix.read_scope();
    //   matrix.process_with_scope(scope.data(), num_samples);
    //   processor_manager.process(buffer);  // SmartHandle reads safe here
    //   // scope dtor releases the read section
    [[nodiscard]] ReadScope read_scope() const TANH_NONBLOCKING_FUNCTION {
        return m_config.read_scope();
    }

    // Process all sources and fill modulation buffers for all targets.
    // Convenience wrapper that opens a read scope internally — use when the
    // caller doesn't need to extend the scope across downstream DSP work.
    void process(size_t num_samples) TANH_NONBLOCKING_FUNCTION;

    // Process variant assuming the caller already holds an open ReadScope.
    // The config reference must come from that scope's data().
    void process_with_scope(const ProcessingConfig& config,
                            size_t num_samples) TANH_NONBLOCKING_FUNCTION;

    // ── Scope registry ───────────────────────────────────────────────────
    // Register (or look up) a named polyphony scope. Returns a ModulationScope
    // handle whose m_name points into stable storage owned by this matrix.
    //
    // Idempotent on name: a second call with the same name returns the same
    // handle. If voice_count differs from the stored value, the count is
    // updated, the schedule is rebuilt (affected VoiceBuffers re-allocated),
    // and a warning is logged.
    //
    // k_global_scope is pre-registered at construction with
    // voice_count == 1 and the reserved name "global" (k_global_scope_name).
    // Registering that name is rejected with a warning and returns the
    // pre-registered global handle.
    ModulationScope register_scope(std::string_view name, uint32_t voice_count);

    // Look up an already-registered scope by name. "global" is the canonical
    // human-readable alias for k_global_scope. An empty name, or
    // any other unregistered name, returns std::nullopt — JSON deserialization
    // must handle a missing "modulationScope" key by defaulting to
    // k_global_scope itself, not by asking find_scope("").
    [[nodiscard]] std::optional<ModulationScope> find_scope(std::string_view name) const;

    // Voice count for a registered scope. Returns 1 for
    // k_global_scope, 0 if the handle is not registered on this
    // matrix.
    [[nodiscard]] uint32_t voice_count(ModulationScope scope) const;

    // Name lookup — returns the stored name for a registered scope;
    // k_global_scope_name ("global") for the global handle; an
    // empty string_view for handles not registered on this matrix.
    [[nodiscard]] std::string_view scope_name(ModulationScope scope) const;

    // Runtime voice-count update for a registered scope. Triggers a schedule
    // rebuild so affected VoiceBuffers are re-sized. No-op if scope is
    // k_global_scope or not registered.
    void set_voice_count(ModulationScope scope, uint32_t voice_count);

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

    // Normalized-[0,1] variant: looks up the target parameter's Range via the
    // matrix's State reference and converts the endpoints to plain units
    // internally. Callers that already work in normalized coordinates can
    // avoid a separate State lookup.
    // Returns false if the target is not in State or the routing was not found.
    bool update_routing_replace_range_normalized(std::string_view source_id,
                                                 std::string_view target_id,
                                                 float norm_min,
                                                 float norm_max);

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
    void rebuild_schedule_with_lock();

    // Ensure a target exists for the given id. Returns a stable pointer.
    // Must be called with m_writer_mutex held.
    ResolvedTarget* ensure_target_with_lock(const std::string_view id);

    // Routing lookup helpers — must be called with m_writer_mutex held.
    ModulationRouting* find_user_routing_with_lock(std::string_view source_id,
                                                   std::string_view target_id);
    ModulationRouting* find_user_routing_with_lock(uint32_t routing_id);

    // Compute the precomputed depth value for a routing given its target.
    static float compute_depth_precomputed(const ModulationRouting& routing,
                                           const ResolvedTarget& target);

    // Routing update helpers — must be called with m_writer_mutex held.
    bool update_routing_depth_with_lock(ModulationRouting& user_routing, float new_depth);
    bool update_routing_replace_range_with_lock(ModulationRouting& user_routing,
                                                float range_min,
                                                float range_max);
    bool clear_routing_replace_range_with_lock(ModulationRouting& user_routing);

    // Process helpers — called from within RCU read section.
    void process_source_bulk_with_scope(const ProcessingConfig& config,
                                        ModulationSource* source,
                                        size_t num_samples);
    void process_cyclic_with_scope(const ProcessingConfig& config,
                                   const std::vector<ModulationSource*>& sources,
                                   size_t num_samples);

    void apply_routing_change_points_with_scope(const ResolvedRouting& routing, size_t num_samples);

    // Post-schedule composition pass for targets with multiple Replace/ReplaceHold
    // routings. Re-applies the deferred routings in ascending priority order so
    // higher-priority writes land last and therefore win within their active
    // regions. Called from process() after the schedule loop.
    void apply_multi_replace_composition_with_scope(const ProcessingConfig& config,
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

    // Scope registry. Entries in m_scope_names own the name strings; the
    // c_str() pointers from these std::string nodes are stored on
    // ModulationScope handles and in m_scopes below. std::list guarantees
    // node stability so the stored c_str() stays valid for the matrix's
    // lifetime (or until deletion — scope deletion is not supported).
    struct ScopeEntry {
        const char* m_name;  // Points at m_scope_names node's c_str()
        uint32_t m_voice_count;
    };
    std::list<std::string> m_scope_names;
    std::vector<ScopeEntry> m_scopes;  // Indexed by ModulationScope::m_id

    // Resolve a parameter's declared ModulationScope against this matrix's
    // registry. Called from ensure_target_with_lock. Validates three
    // fault modes (unknown id, id out of range, name mismatch) and falls
    // back to k_global_scope with a one-time warning.
    [[nodiscard]] ModulationScope resolve_parameter_scope_with_lock(std::string_view param_key,
                                                                    ModulationScope declared);
};

}  // namespace thl::modulation
