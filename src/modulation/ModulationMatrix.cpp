#include "tanh/modulation/ModulationMatrix.h"

#include <tanh/core/Exports.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <stack>
#include <stdexcept>
#include <string_view>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

#include "tanh/core/Logger.h"
#include "tanh/state/ModulationScope.h"
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
    // Pre-register Global scope at id 0 with voice_count == 1. The name is
    // the reserved "global" string from k_global_scope_name —
    // hosts cannot register it (register_scope rejects "global").
    m_scope_names.emplace_back(k_global_scope_name);
    m_scopes.push_back(ScopeEntry{.m_name = m_scope_names.back().c_str(), .m_voice_count = 1});

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

    auto* target = ensure_target_with_lock(param_key);
    return {handle, target};
}

// Explicit instantiations for all supported types.
// TANH_API (__declspec(dllexport) on Windows) is required so that MSVC exports
// these template specialisations from the shared library.
template TANH_API SmartHandle<float> ModulationMatrix::get_smart_handle<float>(std::string_view);
template TANH_API SmartHandle<double> ModulationMatrix::get_smart_handle<double>(std::string_view);
template TANH_API SmartHandle<int> ModulationMatrix::get_smart_handle<int>(std::string_view);
template TANH_API SmartHandle<bool> ModulationMatrix::get_smart_handle<bool>(std::string_view);

ResolvedTarget* ModulationMatrix::ensure_target_with_lock(const std::string_view id) {
    auto it = m_targets.find(id);
    if (it != m_targets.end()) { return &it->second; }
    // ResolvedTarget is non-movable (holds atomics) — use try_emplace to
    // default-construct the value in place.
    auto [target_it, inserted] = m_targets.try_emplace(std::string(id));
    auto& t = target_it->second;
    t.m_id = id;

    // Populate metadata from State for normalized depth processing
    const thl::Parameter param = m_state.get_parameter(id);
    t.m_range = &param.range();
    t.m_type = param.def().m_type;
    t.m_record = m_state.get_record(id);
    t.m_uses_normalized_buffer = t.m_range && !t.m_range->is_linear();
    t.m_scope = resolve_parameter_scope_with_lock(id, param.def().m_modulation_scope);

    return &t;
}

// ── Scope registry ──────────────────────────────────────────────────────────

thl::modulation::ModulationScope ModulationMatrix::register_scope(std::string_view name,
                                                                  uint32_t voice_count) {
    std::scoped_lock const lock(m_writer_mutex);

    // Guard the global sentinel. "global" is reserved for k_global_scope
    // (id 0, voice_count fixed at 1); registering that name would collide with the
    // pre-registered global entry. Warn and hand back the pre-registered handle.
    if (name == k_global_scope_name) {
        thl::Logger::logf(thl::Logger::LogLevel::Warning,
                          "modulation",
                          "register_scope('%.*s'): name is reserved for "
                          "k_global_scope — returning the global handle.",
                          static_cast<int>(name.size()),
                          name.data());
        return k_global_scope;
    }

    // Idempotent on name — search existing registry. Skip id 0 (the global
    // sentinel's name "global" is handled by the reserved-name guard above).
    for (size_t i = 1; i < m_scopes.size(); ++i) {
        if (name == m_scopes[i].m_name) {
            if (m_scopes[i].m_voice_count != voice_count) {
                thl::Logger::logf(thl::Logger::LogLevel::Warning,
                                  "modulation",
                                  "register_scope('%.*s', %u): voice count changed from %u, "
                                  "rebuilding schedule.",
                                  static_cast<int>(name.size()),
                                  name.data(),
                                  voice_count,
                                  m_scopes[i].m_voice_count);
                m_scopes[i].m_voice_count = voice_count;
                rebuild_schedule_with_lock();
            }
            return ModulationScope{.m_id = static_cast<uint16_t>(i), .m_name = m_scopes[i].m_name};
        }
    }

    // New registration — park the name in stable storage, then index it.
    m_scope_names.emplace_back(name);
    const char* stable_name = m_scope_names.back().c_str();
    const auto new_id = static_cast<uint16_t>(m_scopes.size());
    m_scopes.push_back(ScopeEntry{.m_name = stable_name, .m_voice_count = voice_count});
    return ModulationScope{.m_id = new_id, .m_name = stable_name};
}

std::optional<thl::modulation::ModulationScope> ModulationMatrix::find_scope(
    std::string_view name) const {
    // "global" is the canonical human-readable alias for k_global_scope.
    // An empty name is treated as "no scope specified" and returns nullopt — JSON
    // deserialization that encounters a missing modulationScope key must default
    // to k_global_scope itself, not rely on find_scope("") to resolve it.
    if (name == k_global_scope_name) { return k_global_scope; }

    for (size_t i = 1; i < m_scopes.size(); ++i) {
        if (name == m_scopes[i].m_name) {
            return ModulationScope{.m_id = static_cast<uint16_t>(i), .m_name = m_scopes[i].m_name};
        }
    }
    return std::nullopt;
}

uint32_t ModulationMatrix::voice_count(ModulationScope scope) const {
    if (scope.m_id >= m_scopes.size()) {
        thl::Logger::logf(thl::Logger::LogLevel::Warning,
                          "modulation",
                          "voice_count: scope id=%u not registered on this matrix "
                          "(registry size=%zu) — cross-matrix leak or stale handle.",
                          static_cast<unsigned>(scope.m_id),
                          m_scopes.size());
        return 0;
    }
    return m_scopes[scope.m_id].m_voice_count;
}

std::string_view ModulationMatrix::scope_name(ModulationScope scope) const {
    if (scope.m_id >= m_scopes.size()) {
        thl::Logger::logf(thl::Logger::LogLevel::Warning,
                          "modulation",
                          "scope_name: scope id=%u not registered on this matrix "
                          "(registry size=%zu) — cross-matrix leak or stale handle.",
                          static_cast<unsigned>(scope.m_id),
                          m_scopes.size());
        return {};
    }
    return m_scopes[scope.m_id].m_name;
}

void ModulationMatrix::set_voice_count(ModulationScope scope, uint32_t new_voice_count) {
    std::scoped_lock const lock(m_writer_mutex);
    if (scope.m_id == 0) {
        thl::Logger::log(thl::Logger::LogLevel::Warning,
                         "modulation",
                         "set_voice_count: refusing to resize k_global_scope "
                         "(voice_count is fixed at 1).");
        return;
    }
    if (scope.m_id >= m_scopes.size()) {
        thl::Logger::logf(thl::Logger::LogLevel::Warning,
                          "modulation",
                          "set_voice_count: scope id=%u not registered on this matrix "
                          "(registry size=%zu) — cross-matrix leak or stale handle.",
                          static_cast<unsigned>(scope.m_id),
                          m_scopes.size());
        return;
    }
    if (m_scopes[scope.m_id].m_voice_count == new_voice_count) { return; }
    m_scopes[scope.m_id].m_voice_count = new_voice_count;

    // Re-prepare scoped sources so their voice buffers match the new count.
    for (auto& [id, source] : m_sources) {
        if (source->scope().m_id == scope.m_id) {
            source->prepare(m_sample_rate, m_samples_per_block, new_voice_count);
        }
    }
    rebuild_schedule_with_lock();
}

thl::modulation::ModulationScope ModulationMatrix::resolve_parameter_scope_with_lock(
    std::string_view param_key,
    thl::modulation::ModulationScope declared) {
    if (declared == k_global_scope) { return k_global_scope; }

    // Validate: id in range AND name pointer matches this matrix's stable
    // storage. Pointer equality is sufficient — std::list nodes never move,
    // so the same textual name registered on two different matrices yields
    // two different c_str() pointers.
    if (declared.m_id < m_scopes.size() && m_scopes[declared.m_id].m_name == declared.m_name) {
        return declared;
    }

    thl::Logger::logf(thl::Logger::LogLevel::Warning,
                      "modulation",
                      "Parameter '%.*s' declared scope '%s' (id %u) is not registered in this "
                      "matrix; falling back to Global.",
                      static_cast<int>(param_key.size()),
                      param_key.data(),
                      declared.m_name ? declared.m_name : "",
                      declared.m_id);
    return k_global_scope;
}

void ModulationMatrix::prepare(double sample_rate, size_t samples_per_block) {
    // See the threading contract on the header declaration: same-size and
    // growing prepares are safe under concurrent process_with_scope(); shrinking
    // prepares require quiescing the audio thread first.
    std::scoped_lock const lock(m_writer_mutex);

    m_sample_rate = sample_rate;
    m_samples_per_block = samples_per_block;

    for (auto& [id, source] : m_sources) {
        source->prepare(sample_rate, samples_per_block, voice_count(source->scope()));
    }

    // Buffers are (re)allocated inside rebuild_schedule_with_lock() sized against
    // the new m_samples_per_block — no per-target resize pass needed here.
    rebuild_schedule_with_lock();
}

void ModulationMatrix::process(size_t num_samples) TANH_NONBLOCKING_FUNCTION {
    const auto scope = m_config.read_scope();
    process_with_scope(scope.data(), num_samples);
}

void ModulationMatrix::process_with_scope(const ProcessingConfig& config,
                                          size_t num_samples) TANH_NONBLOCKING_FUNCTION {
    // 1. Source per-block reset. Wipes change-point lists by default. Value
    //    buffers and active masks are intentionally *not* touched here —
    //    those are source-authored state (an event-driven source like
    //    XYTouchSource holds last-value + gate across blocks; an LFO rewrites
    //    its buffer every block anyway). Subclasses that want their masks
    //    zeroed at block start override clear_per_block() or call
    //    clear_output_active() / clear_voice_output_active() from it.
    //
    //    Runs *before* pre_process_block so event-driven sources that write
    //    CPs during draining don't have them immediately wiped.
    for (auto* source : config.m_all_sources) { source->clear_per_block(); }

    // 2. Drain pass — every registered source consumes its event queue and
    //    freezes per-block input state (values + masks + CPs). Runs exactly
    //    once per source per block, before any ScheduleStep. Makes cyclic-SCC
    //    iteration safe: the input snapshot is stable across all cycle
    //    iterations. All three state kinds (value buffer, active mask, CP
    //    list) written here survive into the schedule — step 1 already ran.
    for (auto* source : config.m_all_sources) { source->pre_process_block(); }

    // 3. Target per-block reset. Targets are pure aggregators of multiple
    //    sources; everything they hold is block-local and must be cleared.
    for (auto* target : config.m_active_targets) { target->clear_per_block(); }

    // 4. Execute schedule steps
    for (const auto& step : config.m_schedule) {
        if (auto* bulk = std::get_if<BulkStep>(&step)) {
            process_source_bulk_with_scope(config, bulk->m_source, num_samples);
        } else if (auto* cyclic = std::get_if<CyclicStep>(&step)) {
            process_cyclic_with_scope(config, cyclic->m_sources, num_samples);
        }
    }

    // 5. Multi-Replace composition — re-apply deferred Replace/ReplaceHold
    //    routings on shared targets in ascending priority order so higher
    //    priority wins per sample. Single-Replace targets are untouched.
    apply_multi_replace_composition_with_scope(config, num_samples);

    // 6. Build final sorted change point lists from flags
    for (auto* target : config.m_active_targets) { target->build_change_points(); }
}

void ModulationMatrix::add_source(const std::string_view id, ModulationSource* source) {
    std::scoped_lock const lock(m_writer_mutex);

    // Validate the source's scope against the registry. Pointer equality on
    // m_name is sufficient — std::list nodes never move, so the same textual
    // name registered on two different matrices yields two different c_str()
    // pointers (matches resolve_parameter_scope_with_lock's logic).
    const ModulationScope declared = source->scope();
    const bool scope_ok =
        declared == k_global_scope ||
        (declared.m_id < m_scopes.size() && m_scopes[declared.m_id].m_name == declared.m_name);
    if (!scope_ok) {
        thl::Logger::logf(thl::Logger::LogLevel::Warning,
                          "modulation",
                          "add_source('%.*s'): source's scope '%s' (id %u) is not registered in "
                          "this matrix — source will be treated as unreachable.",
                          static_cast<int>(id.size()),
                          id.data(),
                          declared.m_name ? declared.m_name : "",
                          declared.m_id);
        return;
    }

    // If the matrix has already been prepared, immediately prepare the source
    // so its m_num_voices is populated. Without this, a source registered
    // after matrix.prepare() stays at 0 voices until the next scope resize.
    if (m_sample_rate > 0.0) {
        source->prepare(m_sample_rate, m_samples_per_block, voice_count(declared));
    }

    auto it = m_sources.find(id);
    if (it != m_sources.end()) {
        it->second = source;
        rebuild_schedule_with_lock();
        return;
    }
    m_sources.emplace(std::string(id), source);
    rebuild_schedule_with_lock();
}

void ModulationMatrix::remove_source(const std::string_view id) {
    std::scoped_lock const lock(m_writer_mutex);
    m_sources.erase(std::string(id));

    // Remove user-facing routings that reference this source.
    std::erase_if(m_user_routings, [&](const ModulationRouting& r) { return r.m_source_id == id; });

    rebuild_schedule_with_lock();
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

    // Note: multiple Replace/ReplaceHold routings may target the same parameter.
    // Priority (ModulationRouting::m_replace_priority) decides the winner per
    // sample — see the deferred Replace-composition pass in process().
    // Same-priority equals fall back to add-order among active routings.

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
    ensure_target_with_lock(routing.m_target_id);

    const uint32_t id = m_next_routing_id++;
    m_user_routings.push_back(routing);
    m_user_routings.back().m_id = id;
    rebuild_schedule_with_lock();
    return id;
}

void ModulationMatrix::remove_routing(std::string_view source_id, std::string_view target_id) {
    std::scoped_lock const lock(m_writer_mutex);
    std::erase_if(m_user_routings, [&](const ModulationRouting& r) {
        return r.m_source_id == source_id && r.m_target_id == target_id;
    });
    rebuild_schedule_with_lock();
}

void ModulationMatrix::remove_routing(uint32_t routing_id) {
    std::scoped_lock const lock(m_writer_mutex);
    std::erase_if(m_user_routings,
                  [&](const ModulationRouting& r) { return r.m_id == routing_id; });
    rebuild_schedule_with_lock();
}

// ── Private routing helpers ──────────────────────────────────────────────────

ModulationRouting* ModulationMatrix::find_user_routing_with_lock(std::string_view source_id,
                                                                 std::string_view target_id) {
    for (auto& r : m_user_routings) {
        if (r.m_source_id == source_id && r.m_target_id == target_id) { return &r; }
    }
    return nullptr;
}

ModulationRouting* ModulationMatrix::find_user_routing_with_lock(uint32_t routing_id) {
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

bool ModulationMatrix::update_routing_depth_with_lock(ModulationRouting& user_routing,
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

bool ModulationMatrix::update_routing_replace_range_with_lock(ModulationRouting& user_routing,
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

bool ModulationMatrix::clear_routing_replace_range_with_lock(ModulationRouting& user_routing) {
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
    auto* routing = find_user_routing_with_lock(source_id, target_id);
    if (!routing) { return false; }
    return update_routing_depth_with_lock(*routing, new_depth);
}

bool ModulationMatrix::update_routing_depth(uint32_t routing_id, float new_depth) {
    std::scoped_lock const lock(m_writer_mutex);
    auto* routing = find_user_routing_with_lock(routing_id);
    if (!routing) { return false; }
    return update_routing_depth_with_lock(*routing, new_depth);
}

bool ModulationMatrix::update_routing_replace_range(std::string_view source_id,
                                                    std::string_view target_id,
                                                    float range_min,
                                                    float range_max) {
    std::scoped_lock const lock(m_writer_mutex);
    auto* routing = find_user_routing_with_lock(source_id, target_id);
    if (!routing) { return false; }
    return update_routing_replace_range_with_lock(*routing, range_min, range_max);
}

bool ModulationMatrix::update_routing_replace_range(uint32_t routing_id,
                                                    float range_min,
                                                    float range_max) {
    std::scoped_lock const lock(m_writer_mutex);
    auto* routing = find_user_routing_with_lock(routing_id);
    if (!routing) { return false; }
    return update_routing_replace_range_with_lock(*routing, range_min, range_max);
}

bool ModulationMatrix::update_routing_replace_range_normalized(std::string_view source_id,
                                                               std::string_view target_id,
                                                               float norm_min,
                                                               float norm_max) {
    const thl::ParameterRecord* record = m_state.get_record(target_id);
    if (!record) { return false; }
    const auto& range = record->m_def.m_range;
    return update_routing_replace_range(source_id,
                                        target_id,
                                        range.from_normalized(norm_min),
                                        range.from_normalized(norm_max));
}

bool ModulationMatrix::clear_routing_replace_range(std::string_view source_id,
                                                   std::string_view target_id) {
    std::scoped_lock const lock(m_writer_mutex);
    auto* routing = find_user_routing_with_lock(source_id, target_id);
    if (!routing) { return false; }
    return clear_routing_replace_range_with_lock(*routing);
}

bool ModulationMatrix::clear_routing_replace_range(uint32_t routing_id) {
    std::scoped_lock const lock(m_writer_mutex);
    auto* routing = find_user_routing_with_lock(routing_id);
    if (!routing) { return false; }
    return clear_routing_replace_range_with_lock(*routing);
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
    rebuild_schedule_with_lock();
}

std::vector<ScheduleStep> ModulationMatrix::get_schedule() const {
    return m_config.read([](const ProcessingConfig& config) { return config.m_schedule; });
}

void ModulationMatrix::rebuild_schedule_with_lock() {
    // ── Pass 1: Validate routings by scope and collect per-target needs ──
    //
    // Scope validity table (src.scope × tgt.scope):
    //   (Global, Global)           → accepted as GlobalToGlobal
    //   (Global, S ≠ Global)       → accepted as GlobalToScoped (broadcast)
    //   (S, S)                     → accepted as ScopedToScoped (same non-global scope)
    //   (S1, S2) both non-global,
    //     S1 != S2                 → rejected as CrossScope
    //   (S ≠ Global, Global)       → rejected as ScopedToGlobal (scope-narrowing)
    //
    // Allocation is still routing-derived: we collect per-target flags for
    // additive/replace and same-scope-routing presence, then allocate only
    // what's needed. A per-voice-scope parameter with only Global routings
    // allocates MonoBuffers only.
    struct TargetInfo {
        bool m_has_same_scope_routing = false;
        bool m_global_additive = false;
        bool m_global_replace = false;
        bool m_same_scope_additive = false;
        bool m_same_scope_replace = false;
    };
    std::unordered_map<std::string, TargetInfo> target_info;

    // Tracks routings rejected during pass 1a so pass 2 skips them cleanly.
    std::unordered_set<uint32_t> rejected_routing_ids;

    for (const auto& routing : m_user_routings) {
        auto src_it = m_sources.find(routing.m_source_id);
        auto tgt_it = m_targets.find(routing.m_target_id);
        if (src_it == m_sources.end() || tgt_it == m_targets.end()) { continue; }

        const ModulationScope src_scope = src_it->second->scope();
        const ModulationScope tgt_scope = tgt_it->second.m_scope;
        const bool src_global = src_scope == k_global_scope;
        const bool tgt_global = tgt_scope == k_global_scope;

        // Reject ScopedToGlobal: voice source into a mono-only parameter.
        if (!src_global && tgt_global) {
            thl::Logger::logf(
                thl::Logger::LogLevel::Warning,
                "modulation",
                "Routing rejected (ScopedToGlobal): scoped source '%s' (scope '%s') -> "
                "global target '%s'. Scope narrowing is not supported.",
                routing.m_source_id.c_str(),
                src_scope.m_name,
                routing.m_target_id.c_str());
            rejected_routing_ids.insert(routing.m_id);
            continue;
        }

        // Reject CrossScope: two different non-global scopes.
        if (!src_global && !tgt_global && src_scope != tgt_scope) {
            thl::Logger::logf(
                thl::Logger::LogLevel::Warning,
                "modulation",
                "Routing rejected (CrossScope): source '%s' scope '%s' != target '%s' scope '%s'. "
                "Cross-scope modulation is not supported.",
                routing.m_source_id.c_str(),
                src_scope.m_name,
                routing.m_target_id.c_str(),
                tgt_scope.m_name);
            rejected_routing_ids.insert(routing.m_id);
            continue;
        }

        const bool same_scope = !src_global && !tgt_global && src_scope == tgt_scope;
        const bool is_replace = routing.m_combine_mode == CombineMode::Replace ||
                                routing.m_combine_mode == CombineMode::ReplaceHold;
        auto& info = target_info[routing.m_target_id];
        if (same_scope) {
            info.m_has_same_scope_routing = true;
            if (is_replace) {
                info.m_same_scope_replace = true;
            } else {
                info.m_same_scope_additive = true;
            }
        } else {
            // src_global == true (the only remaining accepted case)
            if (is_replace) {
                info.m_global_replace = true;
            } else {
                info.m_global_additive = true;
            }
        }
    }

    // ── Pass 1b: Allocate per-target buffers ─────────────────────────────
    // Rule:
    //   - Any same-scope routing → VoiceBuffers (sized via matrix voice_count
    //     of the target's scope). Global routings to the same target broadcast
    //     into those VoiceBuffers (the existing GlobalToScoped write path).
    //   - No same-scope routing, only global routings → MonoBuffers.
    //   - No routings → no buffers.
    for (auto& [id, target] : m_targets) {
        auto it = target_info.find(id);
        const bool has_any = it != target_info.end();
        const bool has_poly = has_any && it->second.m_has_same_scope_routing;
        const bool has_mono =
            has_any && !has_poly && (it->second.m_global_additive || it->second.m_global_replace);

        // Voice buffers. Skip allocation when the scope is registered with zero
        // voices — there's nothing for downstream consumers to index into.
        const uint32_t nv = has_poly ? voice_count(target.m_scope) : 0u;
        if (has_poly && nv > 0) {
            const bool va = it->second.m_same_scope_additive || it->second.m_global_additive;
            const bool vr = it->second.m_same_scope_replace || it->second.m_global_replace;
            auto* cur = target.m_voice_owner.get();
            const bool geometry_matches = cur != nullptr && cur->m_num_voices == nv &&
                                          cur->m_block_size == m_samples_per_block &&
                                          cur->m_has_additive == va && cur->m_has_replace == vr;
            if (!geometry_matches) {
                auto fresh = std::make_unique<VoiceBuffers>(nv, m_samples_per_block, va, vr);
                target.m_voice.store(fresh.get(), std::memory_order_release);
                if (target.m_voice_owner) {
                    target.m_voice_retired.push_back(std::move(target.m_voice_owner));
                }
                target.m_voice_owner = std::move(fresh);
            }
        } else if (target.m_voice_owner) {
            target.m_voice.store(nullptr, std::memory_order_release);
            target.m_voice_retired.push_back(std::move(target.m_voice_owner));
        }

        // Mono buffers
        if (has_mono) {
            const bool ma = it->second.m_global_additive;
            const bool mr = it->second.m_global_replace;
            auto* cur = target.m_mono_owner.get();
            const bool geometry_matches = cur != nullptr &&
                                          cur->m_block_size == m_samples_per_block &&
                                          cur->m_has_additive == ma && cur->m_has_replace == mr;
            if (!geometry_matches) {
                auto fresh = std::make_unique<MonoBuffers>(m_samples_per_block, ma, mr);
                target.m_mono.store(fresh.get(), std::memory_order_release);
                if (target.m_mono_owner) {
                    target.m_mono_retired.push_back(std::move(target.m_mono_owner));
                }
                target.m_mono_owner = std::move(fresh);
            }
        } else if (target.m_mono_owner) {
            target.m_mono.store(nullptr, std::memory_order_release);
            target.m_mono_retired.push_back(std::move(target.m_mono_owner));
        }
    }

    // ── Pass 2: Resolve routings ────────────────────────────────────────
    std::vector<ResolvedRouting> new_routings;
    for (auto& routing : m_user_routings) {
        auto src_it = m_sources.find(routing.m_source_id);
        auto tgt_it = m_targets.find(routing.m_target_id);

        if (src_it == m_sources.end() || tgt_it == m_targets.end()) { continue; }
        if (rejected_routing_ids.contains(routing.m_id)) { continue; }

        // Derive routing mode from target buffer allocation + source scope.
        // Pass 1 rejected the CrossScope and ScopedToGlobal cases, so only
        // three accepted modes remain.
        const bool src_global = src_it->second->is_global();
        const bool tgt_poly = tgt_it->second.m_voice_owner != nullptr;

        RoutingMode routing_mode;
        if (tgt_poly) {
            routing_mode = src_global ? RoutingMode::GlobalToScoped : RoutingMode::ScopedToScoped;
        } else {
            routing_mode = RoutingMode::GlobalToGlobal;
        }

        // Voice mismatch warning for ScopedToScoped. Read both counts from the
        // scope registry rather than from the live source/target state: the
        // source hasn't necessarily been prepare()'d yet when add_routing is
        // called before matrix.prepare(), and the target's voice_owner may
        // not be allocated at this point either. The scope registry is the
        // authoritative count for both ends.
        if (routing_mode == RoutingMode::ScopedToScoped) {
            const uint32_t src_v = voice_count(src_it->second->scope());
            const uint32_t tgt_v = voice_count(tgt_it->second.m_scope);
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
        r.m_replace_priority = routing.m_replace_priority;
        r.m_skip_during_gesture = routing.m_skip_during_gesture;
        r.m_samples_until_update = 0;

        // Replace range — copy from user routing.
        r.m_replace_range_min.store(routing.m_replace_range_min, std::memory_order_relaxed);
        r.m_replace_range_max.store(routing.m_replace_range_max, std::memory_order_relaxed);
        r.m_has_replace_range.store(routing.m_has_replace_range, std::memory_order_relaxed);

        // Size per-voice held values for polyphonic ReplaceHold routings.
        // m_held_voice_active is parallel and gates the ReplaceHold fallback
        // so voices that have never received a live contribution stay silent
        // instead of writing the default-0 held value with active=1.
        if (r.m_combine_mode == CombineMode::ReplaceHold && tgt_poly) {
            const uint32_t nv = tgt_it->second.m_voice_owner->m_num_voices;
            r.m_held_voice_values.assign(nv, 0.0f);
            r.m_held_voice_active.assign(nv, uint8_t{0});
        }
        r.m_held_mono_active = false;

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

    // Collect every registered source for the drain pass. Includes sources
    // with no routings — pre_process_block() must still run so input-driven
    // sources can consume events and maintain their internal state.
    std::vector<ModulationSource*> new_all_sources;
    new_all_sources.reserve(m_sources.size());
    for (auto& [id, source] : m_sources) { new_all_sources.push_back(source); }

    // Publish everything atomically via RCU
    m_config.update([&](ProcessingConfig& config) {
        config.m_routings = std::move(new_routings);
        config.m_schedule = std::move(new_schedule);
        config.m_active_targets = std::move(new_active_targets);
        config.m_all_sources = std::move(new_all_sources);
        config.m_routings_by_source.clear();
        for (const auto& r : config.m_routings) {
            config.m_routings_by_source[r.m_source].push_back(&r);
        }

        // Group Replace routings by target. Targets with >1 Replace routing
        // get deferred composition: in-schedule routings skip the Replace
        // write, and a post-pass re-applies all of them in ascending-priority
        // order so higher-priority wins deterministically.
        //
        // Iteration is in-order over config.m_routings (a std::vector) so
        // output ordering is deterministic regardless of pointer hashing.
        config.m_multi_replace_targets.clear();
        std::vector<ResolvedTarget*> target_order;
        std::unordered_map<ResolvedTarget*, size_t> target_index;
        std::vector<std::vector<ResolvedRouting*>> grouped;
        for (auto& r : config.m_routings) {
            if (r.m_combine_mode != CombineMode::Replace &&
                r.m_combine_mode != CombineMode::ReplaceHold) {
                continue;
            }
            auto [it, inserted] = target_index.try_emplace(r.m_target, target_order.size());
            if (inserted) {
                target_order.push_back(r.m_target);
                grouped.emplace_back();
            }
            grouped[it->second].push_back(&r);
        }
        for (size_t i = 0; i < target_order.size(); ++i) {
            auto& routings = grouped[i];
            if (routings.size() < 2) { continue; }
            // Stable-sort by priority ascending — equal priorities keep add-order.
            // Higher-priority routings are applied last in the deferred post-
            // pass, so their writes overwrite lower-priority writes in the
            // overlapping active regions.
            std::ranges::stable_sort(routings,
                                     [](const ResolvedRouting* a, const ResolvedRouting* b) {
                                         return a->m_replace_priority < b->m_replace_priority;
                                     });
            std::vector<const ResolvedRouting*> ordered;
            ordered.reserve(routings.size());
            for (auto* r : routings) {
                r->m_replace_deferred = true;
                ordered.push_back(r);
            }
            config.m_multi_replace_targets.emplace_back(target_order[i], std::move(ordered));
        }
    });

    // Drain retired buffers. After m_config.update() the old ProcessingConfig
    // is retired into the RCU's deferred-reclamation list; after synchronize()
    // returns, every in-flight audio block that was still inside a ReadScope
    // has left it, so the target-owned buffer pointers they may have read are
    // guaranteed to be past their last use. Only then is it safe to destroy
    // the retired VoiceBuffers / MonoBuffers instances.
    m_config.synchronize();
    for (auto& [id, target] : m_targets) {
        target.m_voice_retired.clear();
        target.m_mono_retired.clear();
    }
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
        routing.m_held_mono_active = true;
    } else if (routing.m_combine_mode == CombineMode::ReplaceHold && routing.m_held_mono_active) {
        // Only hold after the source has been active at least once — a routing
        // that has never seen a live sample must not broadcast active=1 with
        // the default-0 held value.
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
            routing.m_held_voice_active[voice] = 1;
        }
    } else if (routing.m_combine_mode == CombineMode::ReplaceHold &&
               voice < routing.m_held_voice_values.size() && routing.m_held_voice_active[voice]) {
        // Per-voice gate on the fallback: voices that have never contributed
        // a live sample stay inactive instead of holding the default-0 value.
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
        const float depth_abs = std::abs(raw_depth);
        if (raw_depth >= 0.0f) { return rmin + src_sample * depth_abs * (rmax - rmin); }
        return rmax - src_sample * depth_abs * (rmax - rmin);
    }
    return src_sample * routing.m_depth_abs_precomputed.load(std::memory_order_relaxed);
}

// GlobalToGlobal: write to mono additive or mono replace buffer.
//
// Flag-gate rationale (applies to every apply_routing_* helper and to the
// change-point propagation sites below).
//
// Rebuild discipline is:
//   1. writer atomically publishes fresh VoiceBuffers / MonoBuffers on each
//      target (with the new set of m_has_additive / m_has_replace flags);
//   2. writer publishes the new ProcessingConfig via m_config.update;
//   3. writer calls m_config.synchronize() to drain in-flight readers;
//   4. writer reclaims retired buffers.
//
// Between steps 1 and 2, an audio block that entered its RCU read scope
// before step 2 is still executing the *old* routings — a routing that was
// classified against the old buffer's flags — but every buffer pointer it
// loads already points at the *new* buffer (step 1 already happened). If the
// new buffer's flag set has shrunk (e.g. the last Additive routing for this
// target was removed, so m_has_additive went true → false), the old routing
// will try to touch m_additive_storage / m_change_point_flags_storage etc.
// that were never allocated on the new buffer → assert / UB.
//
// Gating on the flags we just loaded from the atomic-published buffer turns
// that stale write into a safe no-op. Correctness for the *next* block is
// restored automatically because step 3 guarantees it sees the new config.
void apply_routing_global_to_global(const ResolvedRouting& routing,
                                    const ModulationSource* source,
                                    size_t num_samples) TANH_NONBLOCKING_FUNCTION {
    auto* mb = routing.m_target->m_mono.load(std::memory_order_acquire);
    if (mb == nullptr) { return; }
    const auto& src_output = source->get_output_buffer();
    if (routing.m_combine_mode == CombineMode::Additive) {
        if (!mb->m_has_additive) { return; }
        const float depth = routing.m_depth_abs_precomputed.load(std::memory_order_relaxed);
        if (source->is_fully_active()) {
            for (size_t i = 0; i < num_samples; ++i) {
                mb->m_additive_buffer[i] += src_output[i] * depth;
            }
        } else {
            for (size_t i = 0; i < num_samples; ++i) {
                if (source->get_output_active_at(static_cast<uint32_t>(i))) {
                    mb->m_additive_buffer[i] += src_output[i] * depth;
                }
            }
        }
    } else {
        if (!mb->m_has_replace) { return; }
        if (source->is_fully_active()) {
            for (size_t i = 0; i < num_samples; ++i) {
                apply_replace_sample(routing,
                                     mb->m_replace_buffer.data(),
                                     mb->m_replace_active.data(),
                                     i,
                                     compute_replace_value(routing, src_output[i]),
                                     true);
            }
        } else {
            for (size_t i = 0; i < num_samples; ++i) {
                const bool active = source->get_output_active_at(static_cast<uint32_t>(i));
                apply_replace_sample(routing,
                                     mb->m_replace_buffer.data(),
                                     mb->m_replace_active.data(),
                                     i,
                                     compute_replace_value(routing, src_output[i]),
                                     active);
            }
        }
    }
}

// ScopedToScoped: write to per-voice additive or replace buffers.
void apply_routing_scoped_to_scoped(const ResolvedRouting& routing,
                                    const ModulationSource* source,
                                    size_t num_samples) TANH_NONBLOCKING_FUNCTION {
    auto* vb = routing.m_target->m_voice.load(std::memory_order_acquire);
    if (vb == nullptr) { return; }
    const uint32_t nv = std::min(source->num_voices(), vb->m_num_voices);
    if (routing.m_combine_mode == CombineMode::Additive) {
        // See flag-gate rationale on apply_routing_mono_to_mono.
        if (!vb->m_has_additive) { return; }
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
        if (!vb->m_has_replace) { return; }
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

// GlobalToScoped: broadcast mono source to all voice buffers.
void apply_routing_global_to_scoped(const ResolvedRouting& routing,
                                    const ModulationSource* source,
                                    size_t num_samples) TANH_NONBLOCKING_FUNCTION {
    const auto& src_output = source->get_output_buffer();
    auto* vb = routing.m_target->m_voice.load(std::memory_order_acquire);
    if (vb == nullptr) { return; }
    if (routing.m_combine_mode == CombineMode::Additive) {
        if (!vb->m_has_additive) { return; }
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
        if (!vb->m_has_replace) { return; }
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
    auto* mb = routing.m_target->m_mono.load(std::memory_order_acquire);
    if (mb == nullptr) { return; }
    if (routing.m_combine_mode == CombineMode::Additive) {
        if (!mb->m_has_additive) { return; }
        const float depth = routing.m_depth_abs_precomputed.load(std::memory_order_relaxed);
        if (src_active) { mb->m_additive_buffer[i] += src_sample * depth; }
    } else {
        if (!mb->m_has_replace) { return; }
        apply_replace_sample(routing,
                             mb->m_replace_buffer.data(),
                             mb->m_replace_active.data(),
                             i,
                             compute_replace_value(routing, src_sample),
                             src_active);
    }
}

}  // namespace

// ── Per-source bulk processing ────────────────────────────────────────────────

void ModulationMatrix::process_source_bulk_with_scope(const ProcessingConfig& config,
                                                      ModulationSource* source,
                                                      size_t num_samples) {
    auto it = config.m_routings_by_source.find(source);
    if (it == config.m_routings_by_source.end()) { return; }

    // Per-source state reset has already happened in process_with_scope step 1
    // (clear_per_block) and step 2 (pre_process_block may have repopulated
    // masks + CPs). Do NOT re-clear here — that would discard event-driven
    // sources' freshly drained input.

    // Source scope dictates which process variant to call:
    //   - global scope  → one mono process(num_samples)
    //   - scoped source → process_voice(v, num_samples) per voice
    // GlobalToScoped routings still use the mono buffer, so a global source
    // is processed once regardless of how many routings it feeds.
    if (source->is_global()) {
        source->process(num_samples);
    } else {
        for (uint32_t v = 0; v < source->num_voices(); ++v) {
            source->process_voice(v, num_samples);
        }
    }

    for (const auto* routing : it->second) {
        if (routing->m_skip_during_gesture &&
            routing->m_target->m_record->m_in_gesture.load(std::memory_order_relaxed)) {
            continue;
        }

        // Deferred multi-Replace routings skip the write here — they are re-
        // applied in apply_multi_replace_composition_with_scope() after the schedule loop
        // in priority order. Change-point propagation below still runs so the
        // target evaluates transition points regardless.
        if (!routing->m_replace_deferred) {
            switch (routing->m_routing_mode) {
                case RoutingMode::GlobalToGlobal:
                    apply_routing_global_to_global(*routing, source, num_samples);
                    break;
                case RoutingMode::ScopedToScoped:
                    apply_routing_scoped_to_scoped(*routing, source, num_samples);
                    break;
                case RoutingMode::GlobalToScoped:
                    apply_routing_global_to_scoped(*routing, source, num_samples);
                    break;
                case RoutingMode::ScopedToGlobal:
                case RoutingMode::CrossScope: break;  // rejected at schedule-build time
            }
        }

        // Propagate change points to target flags. m_change_point_flags[_storage]
        // is only allocated when at least one of m_has_additive / m_has_replace
        // is set; guard so old-config routings landing on a flag-shrunken buffer
        // become a no-op instead of writing to empty storage.
        if (routing->m_routing_mode == RoutingMode::GlobalToGlobal) {
            if (auto* mb = routing->m_target->m_mono.load(std::memory_order_acquire);
                mb != nullptr && (mb->m_has_additive || mb->m_has_replace)) {
                for (const uint32_t cp : source->get_change_points()) {
                    if (cp < num_samples) { mb->m_change_point_flags[cp] = 1; }
                }
            }
        } else if (routing->m_routing_mode == RoutingMode::ScopedToScoped) {
            if (auto* vb = routing->m_target->m_voice.load(std::memory_order_acquire);
                vb != nullptr && (vb->m_has_additive || vb->m_has_replace)) {
                const uint32_t nv = std::min(source->num_voices(), vb->m_num_voices);
                for (uint32_t v = 0; v < nv; ++v) {
                    const auto& vcp = source->get_voice_change_points(v);
                    const size_t base = static_cast<size_t>(v) * vb->m_block_size;
                    for (const uint32_t cp : vcp) {
                        if (cp < num_samples) { vb->m_change_point_flags_storage[base + cp] = 1; }
                    }
                }
            }
        } else if (routing->m_routing_mode == RoutingMode::GlobalToScoped) {
            if (auto* vb = routing->m_target->m_voice.load(std::memory_order_acquire);
                vb != nullptr && (vb->m_has_additive || vb->m_has_replace)) {
                // Broadcast mono change points to all voices
                for (const uint32_t cp : source->get_change_points()) {
                    if (cp >= num_samples) { continue; }
                    for (uint32_t v = 0; v < vb->m_num_voices; ++v) {
                        const size_t base = static_cast<size_t>(v) * vb->m_block_size;
                        vb->m_change_point_flags_storage[base + cp] = 1;
                    }
                }
            }
        }

        apply_routing_change_points_with_scope(*routing, num_samples);
    }
}

// ── Cyclic group processing (per-sample with z⁻¹) ────────────────────────────

void ModulationMatrix::process_cyclic_with_scope(const ProcessingConfig& config,
                                                 const std::vector<ModulationSource*>& sources,
                                                 size_t num_samples) {
    // Per-source state reset was done in process_with_scope step 1
    // (clear_per_block). Do NOT re-clear — same reasoning as the bulk path.

    // Per-sample processing: interleave sources, apply routings immediately
    for (size_t i = 0; i < num_samples; ++i) {
        for (auto* source : sources) {
            // Mono path — only global-scope sources
            if (source->is_global()) {
                source->process(1, i);
                const float sample = source->get_output_at(static_cast<uint32_t>(i));
                const bool active = source->is_fully_active() ||
                                    source->get_output_active_at(static_cast<uint32_t>(i)) != 0;

                auto it = config.m_routings_by_source.find(source);
                if (it != config.m_routings_by_source.end()) {
                    for (const auto* routing : it->second) {
                        if (routing->m_routing_mode == RoutingMode::GlobalToGlobal) {
                            if (routing->m_skip_during_gesture &&
                                routing->m_target->m_record->m_in_gesture.load(
                                    std::memory_order_relaxed)) {
                                continue;
                            }
                            // Deferred Replace routings are composed post-schedule.
                            if (routing->m_replace_deferred &&
                                routing->m_combine_mode != CombineMode::Additive) {
                                continue;
                            }
                            apply_modulation_sample_mono(*routing, sample, active, i);
                        }
                    }
                }
            }

            // Voice path — only scoped (non-global) sources
            if (!source->is_global()) {
                auto it = config.m_routings_by_source.find(source);
                for (uint32_t v = 0; v < source->num_voices(); ++v) {
                    source->process_voice(v, 1, i);
                    const float sample = source->voice_output(v)[i];
                    const bool active =
                        source->is_fully_active() || source->voice_output_active(v)[i] != 0;

                    if (it != config.m_routings_by_source.end()) {
                        for (const auto* routing : it->second) {
                            if (routing->m_routing_mode != RoutingMode::ScopedToScoped) {
                                continue;
                            }
                            auto* vb = routing->m_target->m_voice.load(std::memory_order_acquire);
                            if (vb == nullptr || v >= vb->m_num_voices) { continue; }
                            if (routing->m_skip_during_gesture &&
                                routing->m_target->m_record->m_in_gesture.load(
                                    std::memory_order_relaxed)) {
                                continue;
                            }
                            if (routing->m_combine_mode == CombineMode::Additive) {
                                if (!vb->m_has_additive) { continue; }
                                if (active) {
                                    const float depth = routing->m_depth_abs_precomputed.load(
                                        std::memory_order_relaxed);
                                    vb->additive_voice(v)[i] += sample * depth;
                                }
                            } else if (!routing->m_replace_deferred) {
                                if (!vb->m_has_replace) { continue; }
                                // Deferred Replace routings are composed post-schedule.
                                apply_replace_sample_voice(*routing,
                                                           vb->replace_voice(v),
                                                           vb->replace_active_voice(v),
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

    // Propagate change points from sources to target flags
    for (auto* source : sources) {
        auto it = config.m_routings_by_source.find(source);
        if (it == config.m_routings_by_source.end()) { continue; }

        for (const auto* routing : it->second) {
            if (routing->m_skip_during_gesture &&
                routing->m_target->m_record->m_in_gesture.load(std::memory_order_relaxed)) {
                continue;
            }

            if (routing->m_routing_mode == RoutingMode::GlobalToGlobal) {
                if (auto* mb = routing->m_target->m_mono.load(std::memory_order_acquire);
                    mb != nullptr && (mb->m_has_additive || mb->m_has_replace)) {
                    for (const uint32_t cp : source->get_change_points()) {
                        if (cp < num_samples) { mb->m_change_point_flags[cp] = 1; }
                    }
                }
            } else if (routing->m_routing_mode == RoutingMode::ScopedToScoped) {
                if (auto* vb = routing->m_target->m_voice.load(std::memory_order_acquire);
                    vb != nullptr && (vb->m_has_additive || vb->m_has_replace)) {
                    const uint32_t nv = std::min(source->num_voices(), vb->m_num_voices);
                    for (uint32_t v = 0; v < nv; ++v) {
                        const auto& vcp = source->get_voice_change_points(v);
                        const size_t base = static_cast<size_t>(v) * vb->m_block_size;
                        for (const uint32_t cp : vcp) {
                            if (cp < num_samples) {
                                vb->m_change_point_flags_storage[base + cp] = 1;
                            }
                        }
                    }
                }
            }
            apply_routing_change_points_with_scope(*routing, num_samples);
        }
    }
}

// ── Multi-Replace composition (post-schedule) ────────────────────────────────

void ModulationMatrix::apply_multi_replace_composition_with_scope(const ProcessingConfig& config,
                                                                  size_t num_samples) {
    // Empty in the common single-Replace case — early out keeps cost zero.
    if (config.m_multi_replace_targets.empty()) { return; }

    for (const auto& [target, ordered] : config.m_multi_replace_targets) {
        for (const auto* routing : ordered) {
            if (routing->m_skip_during_gesture &&
                target->m_record->m_in_gesture.load(std::memory_order_relaxed)) {
                continue;
            }
            auto* source = routing->m_source;
            if (source == nullptr) { continue; }
            switch (routing->m_routing_mode) {
                case RoutingMode::GlobalToGlobal:
                    apply_routing_global_to_global(*routing, source, num_samples);
                    break;
                case RoutingMode::ScopedToScoped:
                    apply_routing_scoped_to_scoped(*routing, source, num_samples);
                    break;
                case RoutingMode::GlobalToScoped:
                    apply_routing_global_to_scoped(*routing, source, num_samples);
                    break;
                case RoutingMode::ScopedToGlobal:
                case RoutingMode::CrossScope: break;  // rejected at schedule-build time
            }
        }
    }
}

// ── Routing change point helper ───────────────────────────────────────────────

void ModulationMatrix::apply_routing_change_points_with_scope(const ResolvedRouting& routing,
                                                              size_t num_samples) {
    if (routing.m_max_decimation == 0) { return; }

    auto* mb = routing.m_target->m_mono.load(std::memory_order_acquire);
    auto* vb = routing.m_target->m_voice.load(std::memory_order_acquire);
    // Gate by allocation of the change-point flag storage — only valid when at
    // least one of additive / replace is present on the corresponding buffer.
    const bool mb_ok = mb != nullptr && (mb->m_has_additive || mb->m_has_replace);
    const bool vb_ok = vb != nullptr && (vb->m_has_additive || vb->m_has_replace);
    if (!mb_ok && !vb_ok) { return; }
    for (size_t i = 0; i < num_samples; ++i) {
        if (routing.m_samples_until_update == 0) {
            if (mb_ok) { mb->m_change_point_flags[i] = 1; }
            if (vb_ok) {
                for (uint32_t v = 0; v < vb->m_num_voices; ++v) {
                    const size_t base = static_cast<size_t>(v) * vb->m_block_size;
                    vb->m_change_point_flags_storage[base + i] = 1;
                }
            }
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
        if (r.m_replace_priority != 0) { obj["replace_priority"] = r.m_replace_priority; }
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
        r.m_replace_priority = obj.value("replace_priority", uint32_t{0});
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
        rebuild_schedule_with_lock();
    } else if (json.is_array()) {
        // Bare routings array (from to_json(false))
        m_user_routings.clear();
        for (const auto& obj : json) { m_user_routings.push_back(parse_routing(obj)); }
        finalize_ids();
        rebuild_schedule_with_lock();
    }

    // Forward parameters to State
    if (json.contains("parameters") && json["parameters"].is_array()) {
        m_state.from_json(json["parameters"]);
    }
}
