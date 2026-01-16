#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <thread>
#include <chrono>
#include <functional>

namespace thl {

/**
 * @brief Generic RCU (Read-Copy-Update) container for lock-free reads
 * 
 * This template provides lock-free read access to any data structure while
 * allowing safe updates through copy-on-write semantics. Perfect for real-time
 * audio applications where readers (audio thread) must never block.
 * 
 * IMPORTANT: For real-time threads, call register_reader_thread() before
 * entering the real-time context (e.g., during audio setup). This allocates
 * the reader node outside the real-time path.
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
     * @param cleanup_threshold Number of retired versions before aggressive cleanup
     * @param emergency_threshold Number of retired versions before blocking cleanup
     */
    explicit RCU(const T& initial_data = T{}, 
                 size_t cleanup_threshold = 8, 
                 size_t emergency_threshold = 32) 
        : m_data_ptr(new T(initial_data))
        , m_cleanup_threshold(cleanup_threshold)
        , m_emergency_threshold(emergency_threshold) {
    }
    
    /**
     * @brief Construct RCU with moved data
     * @param initial_data Initial value (will be moved)
     * @param cleanup_threshold Number of retired versions before aggressive cleanup
     * @param emergency_threshold Number of retired versions before blocking cleanup
     */
    explicit RCU(T&& initial_data, 
                 size_t cleanup_threshold = 8, 
                 size_t emergency_threshold = 32) 
        : m_data_ptr(new T(std::move(initial_data)))
        , m_cleanup_threshold(cleanup_threshold)
        , m_emergency_threshold(emergency_threshold) {
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
        
        // Clean up ALL reader nodes - both live and dead
        // We must remove ourselves from the thread-local state first to prevent
        // threads from accessing a destroyed RCU instance
        std::lock_guard<std::mutex> lock(m_writer_mutex);
        
        ReaderNode* current = m_reader_head.load(std::memory_order_relaxed);
        while (current != nullptr) {
            ReaderNode* next = current->next.load(std::memory_order_relaxed);
            
            // Remove from thread-local state if the node is still tracked there
            // Note: For live threads, the node ownership is in t_rcu_state.nodes
            // We need to take ownership back before deleting
            auto it = t_rcu_state.nodes.find(this);
            if (it != t_rcu_state.nodes.end() && it->second.get() == current) {
                // This is the current thread's node - transfer ownership back
                it->second.release();
                t_rcu_state.nodes.erase(it);
            }
            
            // For dead nodes or nodes from other threads (which released ownership), 
            // we can safely delete
            delete current;
            
            current = next;
        }
        m_reader_head.store(nullptr, std::memory_order_relaxed);
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
        // Auto-register thread if not already registered (not real-time safe not registered already)
        // Make sure to register before calling first read if you want full real-time safety
        register_reader_thread();
        
        // RAII guard to ensure rcu_read_unlock() is always called, even if callback throws
        struct ReadGuard {
            const RCU* rcu;
            ReadGuard(const RCU* r) : rcu(r) { rcu->rcu_read_lock(); }
            ~ReadGuard() { rcu->rcu_read_unlock(); }
        };
        
        ReadGuard guard(this);
        
        // Load current data pointer (guaranteed valid during read section)
        const T* data = m_data_ptr.load(std::memory_order_acquire);
        
        // Call user function with data - guard ensures unlock on normal return or exception
        return read_func(*data);
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
        std::lock_guard<std::mutex> lock(m_writer_mutex);
        
        // Load current data
        const T* old_data = m_data_ptr.load(std::memory_order_acquire);
        
        // Create copy for modification
        auto new_data = std::make_unique<T>(*old_data);
        
        // Let user modify the copy
        update_func(*new_data);
        
        // Atomically publish new version
        m_data_ptr.store(new_data.release(), std::memory_order_release);
        
        // Retire old version with current grace period
        // Note: At 1 billion updates/sec, takes 584 years to overflow uint64_t
        uint64_t retire_period = m_grace_period.fetch_add(1, std::memory_order_acq_rel);
        m_retired_list.push_back({const_cast<T*>(old_data), retire_period});
        
        // ═══════════════════════════════════════════════════
        // TIER 1: Opportunistic cleanup (always try, non-blocking)
        // ═══════════════════════════════════════════════════
        cleanup_safe_versions();
        
        // ═══════════════════════════════════════════════════
        // TIER 2: Threshold cleanup (occasional, still non-blocking)
        // ═══════════════════════════════════════════════════
        if (m_retired_list.size() >= m_cleanup_threshold) {
            // Try multiple times to catch stragglers
            for (int i = 0; i < 3 && !m_retired_list.empty(); ++i) {
                cleanup_safe_versions();
                if (m_retired_list.size() < m_cleanup_threshold / 2) {
                    break;  // Good enough
                }
            }
        }
        
        // ═══════════════════════════════════════════════════
        // TIER 3: Emergency cleanup (rare, blocking)
        // ═══════════════════════════════════════════════════
        if (m_retired_list.size() >= m_emergency_threshold) {
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
     * @brief Register the current thread for real-time safe reads
     * 
     * Call this method from any thread that will use read() before entering
     * a real-time context. This allocates the reader node, so subsequent
     * read() calls are allocation-free and real-time safe.
     * 
     * Safe to call multiple times - subsequent calls are no-ops.
     * NOT real-time safe (allocates memory on first call per thread).
     * 
     * @note If not called explicitly, read() will auto-register on first use.
     *       But this first call is NOT real-time safe.
     * 
     * Usage:
     * ```cpp
     * // During audio setup (non-RT context) - preferred
     * rcu_state.register_reader_thread();
     * 
     * // Or just call read() - auto-registers on first use
     * rcu_state.read([](const auto& data) {
     *     // First call may allocate, subsequent calls are RT-safe
     * });
     * ```
     */
    void register_reader_thread() const {
        if (!t_rcu_state.get_node(this)) {
            // Serialize with cleanup and count
            std::lock_guard<std::mutex> lock(m_writer_mutex);

            // Allocate node on heap so it persists beyond thread lifetime
            auto node = std::make_unique<ReaderNode>();
            ReaderNode* node_ptr = node.get();
            // Lock-free registration using atomic compare-and-swap
            ReaderNode* current_head = m_reader_head.load(std::memory_order_acquire);
            do {
                node->next.store(current_head, std::memory_order_relaxed);
            } while (!m_reader_head.compare_exchange_weak(
                current_head, node_ptr, 
                std::memory_order_release, std::memory_order_acquire));
            
            t_rcu_state.nodes.emplace(this, std::move(node));
        }
    }

    unsigned int get_reader_count() const {
        // Serialize with cleanup and registration
        std::lock_guard<std::mutex> lock(m_writer_mutex);
        unsigned int count = 0;
        ReaderNode* node = m_reader_head.load(std::memory_order_acquire);
        while (node != nullptr) {
            if (!node->is_dead.load(std::memory_order_acquire)) {
                ++count;
            }
            node = node->next.load(std::memory_order_acquire);
        }
        return count;
    }

private:
    // RCU-protected data pointer
    std::atomic<T*> m_data_ptr;
    
    // RCU control structures (per-instance for independence)
    // Start grace period at 1 so that readers not reading can be marked with period 0
    mutable std::atomic<uint64_t> m_grace_period{1};
    mutable std::mutex m_writer_mutex;
    
    // Retired data tracking for deferred reclamation
    struct RetiredData {
        T* ptr;
        uint64_t grace_period;
    };
    std::vector<RetiredData> m_retired_list;
    
    // Per-instance cleanup thresholds (tunable per use case)
    size_t m_cleanup_threshold;     // Try harder to cleanup
    size_t m_emergency_threshold;   // Force blocking cleanup
    
    // Lock-free linked list node for reader registration
    struct ReaderNode {
        std::atomic<uint64_t> read_generation{0};
        std::atomic<ReaderNode*> next{nullptr};
        std::atomic<bool> is_dead{false};  // Mark node as dead when thread exits
    };
    
    // Per-instance reader list head
    mutable std::atomic<ReaderNode*> m_reader_head{nullptr};
    
    // Thread-local RCU state with per-instance registration tracking
    struct ThreadRCUState {
        // Map from RCU instance pointer to this thread's reader node for that instance
        std::unordered_map<const void*, std::unique_ptr<ReaderNode>> nodes;
        
        ReaderNode* get_node(const void* instance) const {
            auto it = nodes.find(instance);
            return it != nodes.end() ? it->second.get() : nullptr;
        }
        
        ~ThreadRCUState() {
            // Mark all nodes as dead - they'll be cleaned up by respective RCU instances
            for (auto& [_, node] : nodes) {
                if (node) {
                    // Ensure we're not in a read section before marking as dead
                    while (node->read_generation.load(std::memory_order_acquire) != 0) {
                        std::this_thread::yield();
                    }
                    node->is_dead.store(true, std::memory_order_release);
                    node.release(); // Let cleanup_dead_nodes handle deletion
                }
            }
        }
    };
    static thread_local ThreadRCUState t_rcu_state;
    
    // RCU operations
    void rcu_read_lock() const {
        if (auto* node = t_rcu_state.get_node(this)) {          
            uint64_t current_period = m_grace_period.load(std::memory_order_acquire);
            node->read_generation.store(current_period, std::memory_order_release);
        }
    }
    
    void rcu_read_unlock() const {
        if (auto* node = t_rcu_state.get_node(this)) {
            node->read_generation.store(0, std::memory_order_release);
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
        
        // Get current grace period - we can never delete data from current or previous period
        uint64_t current_period = m_grace_period.load(std::memory_order_acquire);
        
        // Find minimum period any active reader is in
        uint64_t min_active_period = current_period;
        
        ReaderNode* node = m_reader_head.load(std::memory_order_acquire);
        while (node != nullptr) {
            if (!node->is_dead.load(std::memory_order_acquire)) {
                uint64_t reader_period = node->read_generation.load(std::memory_order_acquire);
                
                if (reader_period != 0) {
                    // Active reader - consider its period
                    if (reader_period < min_active_period) {
                        min_active_period = reader_period;
                    }
                }
            }
            node = node->next.load(std::memory_order_acquire);
        }
        
        // Delete all versions older than the threshold
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
        uint64_t current_period = m_grace_period.load(std::memory_order_acquire);

        // Wait for all live readers to finish their read sections
        ReaderNode* current = m_reader_head.load(std::memory_order_acquire);
        while (current != nullptr) {
            // Skip dead nodes - they can't be in read sections
            if (!current->is_dead.load(std::memory_order_acquire)) {
                // Wait for the reader to exit its read section
                while (true) {
                    uint64_t reader_period = current->read_generation.load(std::memory_order_acquire);
                    if (reader_period == 0) {
                        break;  // Truly idle - not reading and not starting
                    }
                    if (reader_period == current_period) {
                        break;  // Reading current data, safe to proceed
                    }
                    // Yield to give readers a chance to complete
                    std::this_thread::sleep_for(std::chrono::microseconds(1));
                }
            }
            current = current->next.load(std::memory_order_acquire);
        }
    }
    
    // Clean up dead nodes from the linked list
    // Must be called while holding m_writer_mutex
    void cleanup_dead_nodes() {
        // Rebuild the list without dead nodes
        std::vector<ReaderNode*> live_nodes;
        std::vector<std::unique_ptr<ReaderNode>> dead_nodes;
        
        ReaderNode* current = m_reader_head.load(std::memory_order_relaxed);
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
            m_reader_head.store(nullptr, std::memory_order_relaxed);
        } else {
            for (size_t i = 0; i < live_nodes.size() - 1; ++i) {
                live_nodes[i]->next.store(live_nodes[i + 1], std::memory_order_relaxed);
            }
            live_nodes.back()->next.store(nullptr, std::memory_order_relaxed);
            m_reader_head.store(live_nodes[0], std::memory_order_relaxed);
        }
        
        // Dead nodes automatically cleaned up when unique_ptrs go out of scope
        // Exception-safe: if vector reallocation throws, already-collected nodes are freed
    }
};

// Static member definition for thread-local state
template<typename T>
thread_local typename RCU<T>::ThreadRCUState RCU<T>::t_rcu_state;

} // namespace thl
