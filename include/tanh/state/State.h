#pragma once

#include "StateGroup.h"
#include "tanh/utils/RealtimeSanitizer.h"
#include <nlohmann/json.hpp>

#ifdef TANH_WITH_RTSAN
#include <sanitizer/rtsan_interface.h>
#endif

namespace thl {

class State : public StateGroup {
public:
    State(size_t max_string_size = 512, size_t max_levels = 10);
    ~State();

    // Ensure RCU is initialized for the current thread
    void ensure_rcu_initialized();

    // Direct parameter setters
    template<typename T>
    void set_in_root(std::string_view key, T value, NotifyStrategies strategy = NotifyStrategies::all, ParameterListener* source = nullptr);

    // Special case for const char* to std::string
    void set_in_root(std::string_view key, const char* value, NotifyStrategies strategy = NotifyStrategies::all, ParameterListener* source = nullptr);

    // Update state from JSON
    void update_from_json(const nlohmann::json& json_data, NotifyStrategies strategy = NotifyStrategies::none, ParameterListener* source = nullptr);

    // Direct parameter getters (real-time safe for numeric types)
    template<typename T>
    T get_from_root(std::string_view key) const;

    Parameter get_from_root(std::string_view key) const;

    // Get direct parameter type
    ParameterType get_type_from_root(std::string_view key) const;
    
    // State management
    void clear() override;
    bool is_empty() const override;
    
    // State Dump
    std::string get_state_dump() const;

private:
    friend class StateGroup;
    
    // Real-time safe string buffers for path operations
    static inline thread_local std::string m_path_buffer_1; // TODO: Problem if these are thread_local, how to initialize on audio thread?
    static inline thread_local std::string m_path_buffer_2; 
    static inline thread_local std::string m_path_buffer_3;
    static inline thread_local std::string m_temp_buffer;

    // RCU-protected parameter map for lock-free reads
    using ParameterMap = std::map<std::string, ParameterData, std::less<>>;
    mutable RCU<ParameterMap> m_parameters_rcu;

    // Maximum string size and levels for path resolution
    size_t m_max_string_size;
    size_t m_max_levels;

    static inline thread_local bool thread_registered = false;
};

} // namespace thl
