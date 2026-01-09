#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <thread>
#include <chrono>
#include <functional>

#ifdef TANH_WITH_RTSAN
#include <sanitizer/rtsan_interface.h>
#endif

namespace thl {

/**
 * @brief Generic RCU (Read-Copy-Update) container for lock-free reads
 * 
 * This template provides lock-free read access to any data structure while
 * allowing safe updates through copy-on-write semantics. Perfect for real-time
 * audio applications where readers (audio thread) must never block.
 * 
 * @tparam T The type of data to protect (e.g., std::map, std::vector)
 */
template<typename T>
class RCU {
public:
    using DataType = T;
    using DataPtr = std::unique_ptr<T>;
    using ReadFunction = std::function<void(const T&)>;
    using UpdateFunction = std::function<void(T&)>;

    /**
     * @brief Construct RCU with initial data
     * @param initial_data Initial value (will be copied)
     */
    explicit RCU(const T& initial_data = T{}) 
        : m_data_ptr(new T(initial_data)) {
    }
    
    /**
     * @brief Construct RCU with moved data
     * @param initial_data Initial value (will be moved)
     */
    explicit RCU(T&& initial_data) 
        : m_data_ptr(new T(std::move(initial_data))) {
    }

    ~RCU() {
        // Clean up any remaining retired data
        for (auto& retired : m_retired_list) {
            delete retired.ptr;
        }
        m_retired_list.clear();
        
        // Clean up current data
        auto* ptr = m_data_ptr.load(std::memory_order_acquire);
        delete ptr;
        
        // Clean up dead reader nodes from exited threads
        // Safe because dead nodes are from threads that have already exited
        // and won't be reading from any RCU instance anymore
        std::lock_guard<std::mutex> lock(s_writer_mutex);
        cleanup_dead_nodes();
    }

    // Non-copyable, non-movable for simplicity
    RCU(const RCU&) = delete;
    RCU& operator=(const RCU&) = delete;
    RCU(RCU&&) = delete;
    RCU& operator=(RCU&&) = delete;

    /**
     * @brief Lock-free read access
     * 
     * Provides safe read access to the data structure. The provided function
     * is called with a const reference to the current data. The data is
     * guaranteed to remain valid for the duration of the function call.
     * 
     * @param read_func Function to call with const reference to data
     * 
     * Usage:
     * ```cpp
     * rcu_map.read([&](const auto& map) {
     *     auto it = map.find("key");
     *     if (it != map.end()) {
     *         value = it->second;
     *     }
     * });
     * ```
     */
    template<typename Func>
    auto read(Func&& read_func) const -> decltype(read_func(std::declval<const T&>())) {
        ensure_thread_registered();
        
        // Enter RCU read-side critical section
        rcu_read_lock();
        
        // Load current data pointer (guaranteed valid during read section)
        const T* data = m_data_ptr.load(std::memory_order_acquire);
        
        // Call user function with data and handle void/non-void return types
        if constexpr (std::is_void_v<decltype(read_func(*data))>) {
            read_func(*data);
            rcu_read_unlock();
            return;
        } else {
            auto result = read_func(*data);
            rcu_read_unlock();
            return result;
        }
    }

    /**
     * @brief Update data using copy-on-write
     * 
     * Creates a copy of the current data, allows modification via the provided
     * function, then atomically publishes the new version. This operation
     * may block and is not real-time safe.
     * 
     * @param update_func Function to modify the copied data
     * 
     * Usage:
     * ```cpp
     * rcu_map.update([&](auto& map) {
     *     map["new_key"] = value;
     *     map.erase("old_key");
     * });
     * ```
     */
    template<typename Func>
    void update(Func&& update_func) {
        std::lock_guard<std::mutex> lock(s_writer_mutex);
        
        // Load current data
        const T* old_data = m_data_ptr.load(std::memory_order_acquire);
        
        // Create copy for modification
        auto new_data = std::make_unique<T>(*old_data);
        
        // Let user modify the copy
        update_func(*new_data);
        
        // Atomically publish new version
        m_data_ptr.store(new_data.release(), std::memory_order_release);
        
        // Retire old version with current grace period
        uint64_t retire_period = m_grace_period.fetch_add(1, std::memory_order_acq_rel) + 1;
        m_retired_list.push_back({const_cast<T*>(old_data), retire_period});
        
        // ═══════════════════════════════════════════════════
        // TIER 1: Opportunistic cleanup (always try, non-blocking)
        // ═══════════════════════════════════════════════════
        cleanup_safe_versions();
        
        // ═══════════════════════════════════════════════════
        // TIER 2: Threshold cleanup (occasional, still non-blocking)
        // ═══════════════════════════════════════════════════
        if (m_retired_list.size() >= CLEANUP_THRESHOLD) {
            // Try multiple times to catch stragglers
            for (int i = 0; i < 3 && !m_retired_list.empty(); ++i) {
                cleanup_safe_versions();
                if (m_retired_list.size() < CLEANUP_THRESHOLD / 2) {
                    break;  // Good enough
                }
            }
        }
        
        // ═══════════════════════════════════════════════════
        // TIER 3: Emergency cleanup (rare, blocking)
        // ═══════════════════════════════════════════════════
        if (m_retired_list.size() >= EMERGENCY_THRESHOLD) {
            // Pathological case - bite the bullet and wait
            synchronize_rcu();  // BLOCKING
            
            // Now delete everything in retired list
            for (auto& retired : m_retired_list) {
                delete retired.ptr;
            }
            m_retired_list.clear();
        }
        
        // Clean up dead reader nodes periodically
        cleanup_dead_nodes();
    }

    /**
     * @brief Replace entire data structure
     * 
     * Atomically replaces the entire data structure with a new one.
     * 
     * @param new_data New data to replace current data
     */
    void replace(const T& new_data) {
        update([&](T& data) {
            data = new_data;
        });
    }

    /**
     * @brief Replace entire data structure (move version)
     * 
     * Atomically replaces the entire data structure with a new one.
     * 
     * @param new_data New data to replace current data (will be moved)
     */
    void replace(T&& new_data) {
        update([&](T& data) {
            data = std::move(new_data);
        });
    }

    /**
     * @brief Get a snapshot of current data
     * 
     * Creates a copy of the current data. This is useful for operations
     * that need to work with the data outside of the read() function.
     * Note: This involves copying the entire data structure.
     * 
     * @return Copy of current data
     */
    T snapshot() const {
        return read([](const T& data) { return data; });
    }

    /**
     * @brief Check if data structure is empty
     * 
     * Uses the data structure's empty() method if available.
     * 
     * @return true if empty, false otherwise
     */
    bool empty() const {
        return read([](const T& data) { return data.empty(); });
    }

    /**
     * @brief Get size of data structure
     * 
     * Uses the data structure's size() method if available.
     * 
     * @return Size of data structure
     */
    size_t size() const {
        return read([](const T& data) { return data.size(); });
    }

    /**
     * @brief Set a value at a specific path without full copy-on-write
     * 
     * This function allows setting values in nested data structures using a path,
     * similar to StateGroup::set. It uses a more efficient update mechanism that
     * only copies what's necessary.
     * 
     * @tparam ValueType The type of value to set
     * @param path The path to the value (e.g., "group1.subgroup.parameter")
     * @param value The value to set
     * @param create Whether to create missing intermediate paths
     * 
     * Usage:
     * ```cpp
     * RCU<StateGroup> state_rcu;
     * state_rcu.set("audio.volume", 0.8f);
     * state_rcu.set("effects.reverb.enabled", true);
     * ```
     */
    template<typename ValueType>
    void set(std::string_view path, ValueType&& value, bool create = true) {
        update([&](T& data) {
            // Delegate to the data structure's set method if it has one
            if constexpr (requires { data.set(path, std::forward<ValueType>(value), true, create); }) {
                data.set(path, std::forward<ValueType>(value), true, create);
            } else {
                // For other data structures, we'd need a different approach
                // This could be extended to support map-like structures with path parsing
                static_assert(sizeof(ValueType) == 0, "Type T must have a set(path, value, notify, create) method");
            }
        });
    }

    /**
     * @brief Get a value at a specific path
     * 
     * This function allows getting values from nested data structures using a path,
     * similar to StateGroup::get. This is lock-free and real-time safe.
     * 
     * @tparam ValueType The type of value to get
     * @param path The path to the value (e.g., "group1.subgroup.parameter")
     * @return The value at the specified path
     * 
     * Usage:
     * ```cpp
     * float volume = state_rcu.get<float>("audio.volume");
     * bool enabled = state_rcu.get<bool>("effects.reverb.enabled");
     * ```
     */
    template<typename ValueType>
    ValueType get(std::string_view path) const {
        return read([&](const T& data) {
            // Delegate to the data structure's get method if it has one
            if constexpr (requires { data.template get<ValueType>(path); }) {
                return data.template get<ValueType>(path);
            } else {
                // For other data structures, we'd need a different approach
                static_assert(sizeof(ValueType) == 0, "Type T must have a get<ValueType>(path) method");
            }
        });
    }

    /**
     * @brief Check if a path exists in the data structure
     * 
     * @param path The path to check
     * @return true if the path exists, false otherwise
     */
    bool has(std::string_view path) const {
        return read([&](const T& data) {
            // Try to use the data structure's has method or similar
            if constexpr (requires { data.has_parameter(path); }) {
                return data.has_parameter(path);
            } else if constexpr (requires { data.contains(path); }) {
                return data.contains(path);
            } else if constexpr (requires { data.count(path) > 0; }) {
                return data.count(path) > 0;
            } else {
                // Try to get the value and catch exceptions
                try {
                    if constexpr (requires { data.template get<int>(path); }) {
                        data.template get<int>(path);  // Just try to access, don't care about type
                        return true;
                    } else {
                        return false;
                    }
                } catch (...) {
                    return false;
                }
            }
        });
    }

    void ensure_thread_registered() const {
#ifdef TANH_WITH_RTSAN
        __rtsan::ScopedDisabler sd; // TODO: Find a better solution
#endif
        if (!t_rcu_state.registered) [[unlikely]] {
            // Allocate node on heap so it persists beyond thread lifetime
            t_rcu_state.node = std::make_unique<ReaderNode>();
            
            // Lock-free registration using atomic compare-and-swap
            ReaderNode* current_head = s_reader_head.load(std::memory_order_acquire);
            do {
                t_rcu_state.node->next.store(current_head, std::memory_order_relaxed);
            } while (!s_reader_head.compare_exchange_weak(
                current_head, t_rcu_state.node.get(), 
                std::memory_order_release, std::memory_order_acquire));
            
            t_rcu_state.registered = true;
        }
    }

private:
    // RCU-protected data pointer
    std::atomic<T*> m_data_ptr;
    
    // RCU control structures
    mutable std::atomic<uint64_t> m_grace_period{0};
    static std::mutex s_writer_mutex;
    
    // Retired data tracking for deferred reclamation
    struct RetiredData {
        T* ptr;
        uint64_t grace_period;
    };
    std::vector<RetiredData> m_retired_list;
    
    // Cleanup thresholds
    static constexpr size_t CLEANUP_THRESHOLD = 8;      // Try harder to cleanup
    static constexpr size_t EMERGENCY_THRESHOLD = 32;   // Force blocking cleanup
    
    // Lock-free linked list node for reader registration
    struct ReaderNode {
        std::atomic<uint64_t> read_generation{0};
        std::atomic<int32_t> read_count{0};  // Count of active read sections
        std::atomic<ReaderNode*> next{nullptr};
        std::atomic<bool> is_dead{false};  // Mark node as dead when thread exits
    };
    
    static std::atomic<ReaderNode*> s_reader_head;
    
    // Thread-local RCU state with automatic registration
    struct ThreadRCUState {
        std::unique_ptr<ReaderNode> node;
        bool registered = false;
        
        ~ThreadRCUState() {
            if (registered && node) {
                // Ensure we're not in a read section before marking as dead
                // This prevents race where thread is marked dead while still reading
                if (node->read_count.load(std::memory_order_acquire) != 0) {
                    // Should never happen, but be defensive
                    node->read_count.store(0, std::memory_order_release);
                }
                
                // Mark node as dead - it will be cleaned up during next synchronize_rcu
                node->is_dead.store(true, std::memory_order_release);
                // Don't delete the node here; let cleanup_dead_nodes handle it
                // to avoid race conditions with ongoing synchronize_rcu operations
                node.release(); // Release ownership to avoid double-delete
            }
        }
    };
    static thread_local ThreadRCUState t_rcu_state;
    
    // RCU operations
    void rcu_read_lock() const {
        if (t_rcu_state.node) {
            // Increment read count to signal we're in a read section
            // Relaxed is sufficient since synchronize_rcu uses acquire when checking
            t_rcu_state.node->read_count.fetch_add(1, std::memory_order_relaxed);
            // Record current grace period when entering read section
            t_rcu_state.node->read_generation.store(
                m_grace_period.load(std::memory_order_acquire), 
                std::memory_order_relaxed
            );
        }
    }
    
    void rcu_read_unlock() const {
        if (t_rcu_state.node) {
            // Mark that we're no longer in a read section
            t_rcu_state.node->read_generation.store(0, std::memory_order_release);
            // Decrement read count
            t_rcu_state.node->read_count.fetch_sub(1, std::memory_order_release);
        }
    }
    
    /**
     * @brief Non-blocking cleanup of retired versions that are safe to delete
     * 
     * Finds the minimum grace period any reader is currently in, then batch
     * deletes all retired versions older than that period.
     * 
     * Must be called while holding s_writer_mutex.
     */
    void cleanup_safe_versions() {
        if (m_retired_list.empty()) return;
        
        // Find minimum period any reader is still in
        uint64_t min_active_period = UINT64_MAX;
        bool any_active_readers = false;
        
        ReaderNode* current = s_reader_head.load(std::memory_order_acquire);
        while (current != nullptr) {
            if (!current->is_dead.load(std::memory_order_acquire)) {
                int32_t count = current->read_count.load(std::memory_order_acquire);
                if (count > 0) {
                    any_active_readers = true;
                    uint64_t reader_period = current->read_generation.load(std::memory_order_acquire);
                    if (reader_period > 0) {
                        min_active_period = std::min(min_active_period, reader_period);
                    }
                }
            }
            current = current->next.load(std::memory_order_acquire);
        }
        
        // If no active readers, can delete everything!
        if (!any_active_readers) {
            for (auto& retired : m_retired_list) {
                delete retired.ptr;
            }
            m_retired_list.clear();
            return;
        }
        
        // Delete all versions older than minimum active reader period
        auto it = m_retired_list.begin();
        while (it != m_retired_list.end()) {
            if (it->grace_period < min_active_period) {
                delete it->ptr;  // Safe - all readers past this period
                it = m_retired_list.erase(it);
            } else {
                ++it;  // Keep newer versions
            }
        }
    }
    
    /**
     * @brief Blocking wait for all current readers to finish
     * 
     * Used as emergency cleanup when retired list grows too large.
     * Must be called while holding s_writer_mutex.
     */
    void synchronize_rcu() {
        // Start new grace period
        uint64_t new_period = m_grace_period.fetch_add(1, std::memory_order_acq_rel) + 1;
        
        // Wait for all live readers to finish their read sections
        ReaderNode* current = s_reader_head.load(std::memory_order_acquire);
        while (current != nullptr) {
            // Skip dead nodes - they can't be in read sections
            if (!current->is_dead.load(std::memory_order_acquire)) {
                // Wait for the reader to exit its read section
                while (true) {
                    int32_t count = current->read_count.load(std::memory_order_acquire);
                    uint64_t reader_period = current->read_generation.load(std::memory_order_acquire);
                    
                    // Reader is safe if:
                    // 1. Not in a read section (count == 0), OR
                    // 2. Started reading after grace period began (reader_period >= new_period)
                    if (count == 0 || reader_period >= new_period) {
                        break;
                    }
                    
                    // Yield to give readers a chance to complete
                    std::this_thread::sleep_for(std::chrono::microseconds(1));
                }
            }
            current = current->next.load(std::memory_order_acquire);
        }
    }
    
private:
    // Clean up dead nodes from the linked list
    // Must be called while holding s_writer_mutex
    void cleanup_dead_nodes() {
        // Rebuild the list without dead nodes
        std::vector<ReaderNode*> live_nodes;
        std::vector<std::unique_ptr<ReaderNode>> dead_nodes;
        
        ReaderNode* current = s_reader_head.load(std::memory_order_relaxed);
        while (current != nullptr) {
            ReaderNode* next = current->next.load(std::memory_order_relaxed);
            
            if (!current->is_dead.load(std::memory_order_acquire)) {
                live_nodes.push_back(current);
            } else {
                // Transfer ownership to unique_ptr for automatic cleanup
                dead_nodes.emplace_back(current);
            }
            
            current = next;
        }
        
        // Rebuild the linked list with only live nodes
        if (live_nodes.empty()) {
            s_reader_head.store(nullptr, std::memory_order_relaxed);
        } else {
            for (size_t i = 0; i < live_nodes.size() - 1; ++i) {
                live_nodes[i]->next.store(live_nodes[i + 1], std::memory_order_relaxed);
            }
            live_nodes.back()->next.store(nullptr, std::memory_order_relaxed);
            s_reader_head.store(live_nodes[0], std::memory_order_relaxed);
        }
        
        // Dead nodes automatically cleaned up when unique_ptrs go out of scope
        // Exception-safe: if vector reallocation throws, already-collected nodes are freed
    }
};

// Static member definitions
template<typename T>
std::mutex RCU<T>::s_writer_mutex;

template<typename T>
std::atomic<typename RCU<T>::ReaderNode*> RCU<T>::s_reader_head{nullptr};

template<typename T>
thread_local typename RCU<T>::ThreadRCUState RCU<T>::t_rcu_state;

} // namespace thl
