// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE {

namespace detail {

/// Who of a thread node's two owners has gone (RcuReaderNode::m_state).
enum class RcuNodeState : uint8_t {
    Live,        ///< thread and RCU instance both alive
    ThreadGone,  ///< the registering thread exited: the RCU instance frees the node
    RcuGone,     ///< the RCU instance was destroyed: the thread frees the node
};

/// A reader of one RCU instance, linked into its lock-free reader list.
///
/// A thread node (per-thread registration) lives as long as both the thread
/// and the RCU instance: whichever goes first marks it (m_state, one atomic
/// exchange on each side), the other one frees it. A slot from add_reader()
/// belongs to the RCU instance alone.
struct RcuReaderNode {
    std::atomic<uint64_t> m_read_generation{0};
    // Open read sections of the node's one user at a time (its thread, or
    // whoever holds the slot). Atomic so that two threads on one slot are never
    // a data race on the counter itself; that is all the relaxed orderings
    // give. They do not put one user's last store to m_read_generation before
    // the next user's first one: the users of a slot must be ordered by
    // happens-before (see RCU::add_reader()).
    std::atomic<uint32_t> m_depth{0};
    std::atomic<RcuReaderNode*> m_next{nullptr};
    std::atomic<RcuNodeState> m_state{RcuNodeState::Live};
    bool m_instance_owned = false;  // add_reader(): set before linking, then constant

    /// The registering thread exited: not inside a read section any more.
    [[nodiscard]] bool thread_gone() const {
        return m_state.load(std::memory_order_acquire) == RcuNodeState::ThreadGone;
    }
};

/// Thread-local registry mapping RCU instance -> this thread's reader node.
///
/// Deliberately a non-template type whose accessor is defined once in
/// tanh_core (RCU.cpp) rather than a function-local `static thread_local`
/// inside RCU<T>: a template-local thread_local is instantiated separately in
/// every module that uses it, and on Windows shared-library builds each DLL
/// and the executable then get their own copy. A read section opened in one
/// module (e.g. an exported ReadScope factory inside a DLL) and closed in
/// another (the ReadScope destructor instantiated in the executable) would
/// look the node up in two different maps, silently skip the unlock and leave
/// the node's generation stale — which pins every later synchronize() once the
/// reader thread stops. One process-wide registry makes the lookup identical
/// everywhere.
class RcuThreadState {
public:
    RcuThreadState() = default;
    ~RcuThreadState();
    RcuThreadState(const RcuThreadState&) = delete;
    RcuThreadState& operator=(const RcuThreadState&) = delete;
    RcuThreadState(RcuThreadState&&) = delete;
    RcuThreadState& operator=(RcuThreadState&&) = delete;

    RcuReaderNode* get_node(const void* instance) const {
        auto it = m_nodes.find(instance);
        return it != m_nodes.end() ? it->second.get() : nullptr;
    }

    std::unordered_map<const void*, std::unique_ptr<RcuReaderNode>> m_nodes;
};

/// The calling thread's registry. Defined in tanh_core so that all modules
/// share a single thread_local instance per thread.
TANH_API RcuThreadState& rcu_thread_state();

}  // namespace detail

/**
 * @brief Generic RCU (Read-Copy-Update) container for lock-free reads
 *
 * This template provides lock-free read access to any data structure while
 * allowing safe updates through copy-on-write semantics. Perfect for real-time
 * audio applications where readers (audio thread) must never block.
 *
 * IMPORTANT: For real-time threads, call register_reader_thread() before
 * entering the real-time context (e.g., during audio setup). This allocates
 * the reader node outside the real-time path. A reader that runs on changing
 * threads (a plugin whose host moves the render callback from thread to
 * thread) cannot register in advance: it reads through a reader slot,
 * add_reader() and read_scope(Reader), which never registers.
 *
 * A writer publishes with update() or replace(), or in two steps: stage_update()
 * / stage_replace() make the next version and everything its publication needs
 * (they may throw, and then nothing has changed), commit() publishes a staged
 * version and cannot fail. The two steps are for a change that spans several
 * RCUs, or an RCU and state of the caller: stage all of it, then commit all of
 * it. See Staged and commit().
 *
 * @tparam T The type of data to protect (e.g., std::map, std::vector)
 */
template <typename T>
class RCU {
public:
    using DataType = T;
    using DataPtr = std::unique_ptr<T>;
    using ReadFunction = std::function<void(const T&)>;
    using UpdateFunction = std::function<void(T&)>;

    /**
     * @brief Construct RCU with initial data
     * @param initial_data Initial value (will be copied)
     * @param cleanup_threshold Number of retired versions before aggressive
     * cleanup
     * @param emergency_threshold Number of retired versions before blocking
     * cleanup
     */
    explicit RCU(const T& initial_data = T{},
                 size_t cleanup_threshold = 8,
                 size_t emergency_threshold = 32)
        : m_data_ptr(new T(initial_data))
        , m_cleanup_threshold(cleanup_threshold)
        , m_emergency_threshold(emergency_threshold) {}

    /**
     * @brief Construct RCU with moved data
     * @param initial_data Initial value (will be moved)
     * @param cleanup_threshold Number of retired versions before aggressive
     * cleanup
     * @param emergency_threshold Number of retired versions before blocking
     * cleanup
     */
    explicit RCU(T&& initial_data, size_t cleanup_threshold = 8, size_t emergency_threshold = 32)
        : m_data_ptr(new T(std::move(initial_data)))
        , m_cleanup_threshold(cleanup_threshold)
        , m_emergency_threshold(emergency_threshold) {}

    ~RCU() {
        // Clean up any remaining retired data
        for (auto& retired : m_retired_list) { delete retired.m_ptr; }
        m_retired_list.clear();

        // Clean up current data
        auto* ptr = m_data_ptr.load(std::memory_order_acquire);
        delete ptr;

        // Free the reader nodes this instance owns. A live thread's node is
        // still in that thread's RcuThreadState: mark it, and the thread frees it
        // (on exit, or at its next registration with any RCU). The destroying
        // thread's own node is no exception: this destructor never looks into
        // the calling thread's registry, which is already destroyed when the RCU
        // outlives it (an RCU with static storage duration read by the main
        // thread, or one owned by a thread_local constructed before the
        // thread's first read).
        const std::scoped_lock lock(m_writer_mutex);

        detail::RcuReaderNode* current = m_reader_head.load(std::memory_order_relaxed);
        while (current != nullptr) {
            detail::RcuReaderNode* next = current->m_next.load(std::memory_order_relaxed);

            if (current->m_instance_owned ||
                current->m_state.exchange(detail::RcuNodeState::RcuGone,
                                          std::memory_order_acq_rel) ==
                    detail::RcuNodeState::ThreadGone) {
                delete current;  // ours alone: a slot, or a node its thread left to us
            }

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
    template <typename Func>
    // NOLINTNEXTLINE(misc-no-recursion)
    auto read(Func&& read_func) const -> decltype(read_func(std::declval<const T&>())) {
        // Auto-register thread if not already registered (not real-time safe
        // not registered already) Make sure to register before calling first
        // read if you want full real-time safety
        detail::RcuReaderNode& reader = thread_reader();

        // RAII guard to ensure rcu_read_unlock() is always called, even if
        // callback throws
        struct ReadGuard {
            const RCU* m_rcu;
            detail::RcuReaderNode& m_reader;
            ReadGuard(const RCU* r, detail::RcuReaderNode& n) : m_rcu(r), m_reader(n) {
                m_rcu->rcu_read_lock(m_reader);
            }
            ~ReadGuard() { m_rcu->rcu_read_unlock(m_reader); }
        };

        const ReadGuard guard(this, reader);

        // Load current data pointer (guaranteed valid during read section).
        // seq_cst: second half of the reader's handshake, see rcu_read_lock().
        const T* data = m_data_ptr.load(std::memory_order_seq_cst);

        // Call user function with data - guard ensures unlock on normal return
        // or exception
        return read_func(*data);
    }

    /**
     * @brief Update data using copy-on-write
     *
     * Creates a copy of the current data, allows modification via the provided
     * function, then atomically publishes the new version. This operation
     * may block and is not real-time safe.
     *
     * When it throws (an allocation that fails, or update_func) nothing was
     * published: everything that can fail comes before the publication.
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
    template <typename Func>
    void update(Func&& update_func) {
        const std::scoped_lock lock(m_writer_mutex);
        publish_with_lock(stage_copy_with_lock(update_func));
    }

    /**
     * @brief Publish a freshly built version, without reading the current one
     *
     * Same publication and reclamation as update(), but the new version is
     * default-constructed and filled in by build_func rather than copied from
     * the live one. Use this when the writer rebuilds T from scratch anyway —
     * it skips a pointless deep copy, and, more importantly, it never reads the
     * version the readers are using. That matters when readers mutate state
     * inside T (RT scratch state reached through a const reference, for
     * example): update()'s copy would race with those writes, this does not.
     *
     * Requires T to be default-constructible. May block; not real-time safe.
     * When it throws (an allocation that fails, or build_func) nothing was
     * published.
     *
     * @param build_func Function that fills in the fresh, empty value
     *
     * Usage:
     * ```cpp
     * rcu_config.replace([&](auto& config) {
     *     config.entries = std::move(freshly_built_entries);
     * });
     * ```
     */
    template <typename Func>
    void replace(Func&& build_func) {
        const std::scoped_lock lock(m_writer_mutex);
        publish_with_lock(stage_fresh_with_lock(build_func));
    }

    /**
     * @brief A version made by stage_update() / stage_replace() and not yet
     * published: the first half of a publication in two steps.
     *
     * Move-only. It owns the version until commit() takes it. Destroyed without
     * a commit it frees the version, on the destroying thread, and the RCU stays
     * as it was; it does not touch the RCU then, so it may outlive it. No reader
     * sees the version before commit(), so data() may still be changed.
     *
     * Default-constructed or moved-from it is empty.
     */
    class [[nodiscard]] Staged {
    public:
        Staged() = default;
        ~Staged() = default;
        Staged(Staged&&) noexcept = default;
        Staged& operator=(Staged&&) noexcept = default;
        Staged(const Staged&) = delete;
        Staged& operator=(const Staged&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept { return m_data != nullptr; }

        /// The staged version. Not for an empty Staged (asserted).
        [[nodiscard]] T& data() const noexcept {
            assert(m_data && "RCU::Staged::data: empty Staged");
            return *m_data;
        }
        T* operator->() const noexcept {
            assert(m_data && "RCU::Staged::operator->: empty Staged");
            return m_data.get();
        }

    private:
        friend class RCU;
        Staged(const RCU& owner, std::unique_ptr<T> data, uint64_t publications)
            : m_owner(&owner), m_data(std::move(data)), m_publications(publications) {}

        const RCU* m_owner = nullptr;
        std::unique_ptr<T> m_data;
        // The owner's m_grace_period when this was staged; it moves with every
        // publication. A member in every build, so that code built with and
        // without NDEBUG agrees on the layout; only commit()'s check reads it.
        uint64_t m_publications = 0;
    };

    /**
     * @brief First half of update(): make the next version from a copy of the
     * current one, publish nothing
     *
     * Copies the current version, lets update_func modify the copy and makes
     * the room its publication needs. Everything of a publication that can fail
     * happens here: when this throws (std::bad_alloc, or whatever update_func
     * throws) the RCU is as it was, readers see what they saw, and nothing is
     * leaked. commit() publishes the returned version and cannot fail.
     *
     * Like update() it reads the live version (see there) and holds the writer
     * mutex while update_func runs; unlike update() it does not hold it
     * afterwards. See commit() for what may happen between the two calls.
     * May block; not real-time safe.
     *
     * @param update_func Function to modify the copied data
     */
    template <typename Func>
    [[nodiscard]] Staged stage_update(Func&& update_func) {
        const std::scoped_lock lock(m_writer_mutex);
        return staged_with_lock(stage_copy_with_lock(update_func));
    }

    /**
     * @brief First half of replace(): build the next version from scratch,
     * publish nothing
     *
     * Default-constructs a T, lets build_func fill it in and makes the room its
     * publication needs. Everything of a publication that can fail happens
     * here: when this throws (std::bad_alloc, or whatever build_func throws)
     * the RCU is as it was, readers see what they saw, and nothing is leaked.
     * commit() publishes the returned version and cannot fail.
     *
     * Like replace() it never reads the live version and holds the writer mutex
     * while build_func runs; unlike replace() it does not hold it afterwards.
     * See commit() for what may happen between the two calls. Requires T to be
     * default-constructible. May block; not real-time safe.
     *
     * @param build_func Function that fills in the fresh, empty value
     *
     * Usage, two RCUs that change together:
     * ```cpp
     * auto table = rcu_table.stage_replace([&](auto& t) { t = build_table(); });
     * auto rows = rcu_rows.stage_replace([&](auto& r) { r = build_rows(); });
     * // ... whatever else can fail. A throw up to here leaves both RCUs alone.
     * rcu_table.commit(std::move(table));  // noexcept from here on
     * rcu_rows.commit(std::move(rows));
     * ```
     */
    template <typename Func>
    [[nodiscard]] Staged stage_replace(Func&& build_func) {
        const std::scoped_lock lock(m_writer_mutex);
        return staged_with_lock(stage_fresh_with_lock(build_func));
    }

    /**
     * @brief Second half of a publication in two steps: publish a staged version
     *
     * Publishes the version, retires the one it replaces and reclaims the
     * retired versions no reader uses any more, exactly as update() / replace()
     * do after they have built theirs. It cannot fail and allocates nothing:
     * the retired version goes into the room its stage_update() /
     * stage_replace() made. @p staged is empty afterwards.
     *
     * It blocks where update() / replace() block and nowhere else: on the
     * writer mutex (another writer, a thread registering as a reader,
     * add_reader(), get_reader_count(), a synchronize() in progress), and on
     * readers only in the emergency tier, when the number of retired versions
     * reaches the emergency threshold. Not real-time safe; like them, not to be
     * called from inside a read section of this RCU, which that tier would wait
     * for.
     *
     * One writer from stage to commit: between a stage_update() /
     * stage_replace() and the commit() of its result nothing else may be
     * published in this RCU, no update(), no replace() and no commit() of
     * another staged version. The RCU does not hold its writer mutex between
     * the two calls (readers register, add_reader() and synchronize() go on),
     * so keeping other writers out is the caller's job: one writer thread, or
     * a mutex of the caller's around stage and commit. Under that exclusion the
     * two calls may run on different threads. Several versions may be staged
     * at once as long as at most one of them is committed: drop the others and
     * stage them again.
     *
     * commit() asserts the rule. A release build that breaks it still
     * publishes @p staged, but the room the stage made may be used up, so the
     * commit may allocate (and terminates when that fails), and a version from
     * stage_update() replaces whatever was published in between with a copy
     * that never contained it.
     *
     * @p staged must be this instance's (asserted; a release build publishes it
     * here all the same, under the consequences above). An empty one is a
     * contract violation too (asserted; a release build does nothing).
     */
    void commit(Staged&& staged) noexcept {
        assert(staged && "RCU::commit: empty Staged");
        assert((!staged || staged.m_owner == this) && "RCU::commit: another RCU's Staged");
        if (!staged) { return; }

        const std::scoped_lock lock(m_writer_mutex);
        assert(staged.m_publications == m_grace_period.load(std::memory_order_relaxed) &&
               "RCU::commit: a version was published since this one was staged");
        publish_with_lock(std::move(staged.m_data));
    }

private:
    // First half of update() / stage_update(): a copy of the current version,
    // modified by update_func, and room to retire the version it will replace.
    // All of a publication that can fail. Caller must hold m_writer_mutex.
    template <typename Func>
    std::unique_ptr<T> stage_copy_with_lock(Func& update_func) {
        // Load current data
        const T* old_data = m_data_ptr.load(std::memory_order_acquire);

        // Create copy for modification. Note this reads every byte of the live
        // version: only safe when readers never mutate what they read. If the
        // read side writes into T (mutable per-reader state, say), copying it
        // here races with those writes — use replace() instead.
        auto new_data = std::make_unique<T>(*old_data);

        // Let user modify the copy
        update_func(*new_data);

        reserve_retired_with_lock();
        return new_data;
    }

    // First half of replace() / stage_replace(): a fresh version filled in by
    // build_func, and room to retire the version it will replace. All of a
    // publication that can fail. Caller must hold m_writer_mutex.
    template <typename Func>
    std::unique_ptr<T> stage_fresh_with_lock(Func& build_func) {
        auto new_data = std::make_unique<T>();
        build_func(*new_data);

        reserve_retired_with_lock();
        return new_data;
    }

    // Room in the retired list for one more version, so that the publication
    // that follows pushes without allocating. The last step of a stage: when it
    // throws, the version just built is freed and nothing else was touched.
    // Doubles the capacity when the list is full, so a writer allocates here as
    // rarely as it did when the publication's push_back() grew the list.
    // Nothing shrinks the capacity, so the room lasts until a publication uses
    // it. Caller must hold m_writer_mutex.
    void reserve_retired_with_lock() {
        const size_t capacity = m_retired_list.capacity();
        if (m_retired_list.size() < capacity) { return; }
        m_retired_list.reserve(std::max<size_t>(1, 2 * capacity));
    }

    // The staged form of a version made by stage_*_with_lock(). Caller must
    // hold m_writer_mutex.
    Staged staged_with_lock(std::unique_ptr<T> new_data) const noexcept {
        return Staged(*this, std::move(new_data), m_grace_period.load(std::memory_order_relaxed));
    }

    // Second half of update() / replace(), and commit(): publish, retire the old
    // version into the room reserve_retired_with_lock() made and run the
    // reclamation tiers. Allocates nothing and cannot fail. Caller must hold
    // m_writer_mutex.
    void publish_with_lock(std::unique_ptr<T> new_data) noexcept {
        T* old_data = m_data_ptr.load(std::memory_order_acquire);

        // Atomically publish new version. seq_cst: first half of the writer's
        // handshake with rcu_read_lock() — the generation loads in
        // cleanup_safe_versions() / synchronize_rcu() are the second half.
        m_data_ptr.store(new_data.release(), std::memory_order_seq_cst);

        // Retire old version with current grace period
        // Note: At 1 billion updates/sec, takes 584 years to overflow uint64_t
        const uint64_t retire_period = m_grace_period.fetch_add(1, std::memory_order_acq_rel);
        m_retired_list.push_back({old_data, retire_period});

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
            for (auto& retired : m_retired_list) { delete retired.m_ptr; }
            m_retired_list.clear();
        }

        // Unlink and free the reader nodes of exited threads
        cleanup_dead_nodes();
    }

public:
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
    void register_reader_thread() const { (void)thread_reader(); }

    /**
     * @brief A reader slot from add_reader(): a non-owning handle, cheap to copy.
     *
     * Default-constructed it is empty. It belongs to the RCU instance that
     * made it and only opens sections there.
     */
    class Reader {
    public:
        Reader() = default;
        [[nodiscard]] explicit operator bool() const { return m_node != nullptr; }

    private:
        friend class RCU;
        Reader(const RCU& owner, detail::RcuReaderNode& node) : m_owner(&owner), m_node(&node) {}
        const RCU* m_owner = nullptr;
        detail::RcuReaderNode* m_node = nullptr;
    };

    /**
     * @brief Create a reader slot owned by this instance instead of by a thread
     *
     * For a reader that is one logical client but may run on changing threads,
     * e.g. a plugin whose host calls the render callback from different
     * threads: the per-thread registration would allocate (and lock) on the
     * real-time path every time the thread changes. Pass the slot to
     * read_scope(reader) instead; that path never allocates or locks.
     *
     * NOT real-time safe: call once per client during setup (e.g. in a
     * constructor, not in a host's prepareToPlay): slots are not removed, they
     * stay valid until this RCU is destroyed and every writer scans them.
     *
     * A slot has one user at a time. That user's scopes on the slot nest (see
     * ReadScope); two threads must never be inside read_scope(reader) for one
     * slot at the same time. "One after the other" means ordered by
     * happens-before, not merely by the clock: the hand-over from one thread to
     * the next has to go through something that orders the two, such as a host
     * that calls its render callback from one thread after the other, a mutex,
     * a thread join, or an atomic flag that is claimed with at least
     * std::memory_order_acquire and given back, after the scope has closed,
     * with at least std::memory_order_release. Nothing checks this: a slot used
     * by two threads at once, or passed on through a relaxed flag, can read a
     * version that the writer has already freed.
     *
     * A section that is never closed (a leaked ReadScope, a thread killed
     * inside one) blocks synchronize() for ever, and update() / replace() once
     * enough versions are retired: a slot has no thread whose exit would
     * release it.
     */
    [[nodiscard]] Reader add_reader() const {
        const std::scoped_lock lock(m_writer_mutex);
        // Owned by this instance: the destructor deletes it.
        auto* node = new detail::RcuReaderNode();
        node->m_instance_owned = true;
        link_reader_with_lock(*node);
        return Reader(*this, *node);
    }

    /**
     * @brief Number of readers a writer scans: the reader slots from
     * add_reader() plus the registered threads that are still alive.
     *
     * Takes the writer mutex: not real-time safe.
     */
    unsigned int get_reader_count() const {
        // Serialize with cleanup and registration
        const std::scoped_lock lock(m_writer_mutex);
        unsigned int count = 0;
        detail::RcuReaderNode* node = m_reader_head.load(std::memory_order_acquire);
        while (node != nullptr) {
            if (!node->thread_gone()) { ++count; }
            node = node->m_next.load(std::memory_order_acquire);
        }
        return count;
    }

    /**
     * @brief Wait until no reader is inside a read section that began before
     * the latest publication.
     *
     * When it returns, every reader (registered thread or reader slot) is
     * outside a read section or inside one that sees the current version, so
     * whatever only older versions referred to is no longer in use. Blocks (it
     * polls) and takes the writer mutex: not real-time safe.
     *
     * A registered thread that has exited is skipped. A reader slot whose
     * section is never closed is waited for indefinitely. Must not be called
     * from inside a read section of this RCU that was opened before the latest
     * update() / replace(): the caller would wait for itself.
     */
    void synchronize() {
        const std::scoped_lock lock(m_writer_mutex);
        synchronize_rcu();
    }

    /**
     * @brief RAII scope that holds an RCU read section open.
     *
     * Use when a caller needs to keep the read section open across multiple
     * operations (e.g. wrapping a whole audio block rather than each per-sample
     * access). Opens rcu_read_lock() in the ctor and closes it in the dtor,
     * while exposing the current data pointer.
     *
     * Non-copyable, non-movable. Must not outlive the RCU instance. Scopes on
     * the same thread or reader slot nest: the outermost one holds the section.
     * An inner scope may see a newer version than the outer one; each scope's
     * data() stays valid until the outermost scope is closed.
     *
     * Usage:
     * ```cpp
     * auto scope = rcu.read_scope();        // auto-registers, enters section
     * use(scope.data());                    // data() valid until dtor
     * ```                                   // dtor releases section
     */
    class [[nodiscard]] ReadScope {
    public:
        explicit ReadScope(const RCU* rcu) : ReadScope(rcu, rcu->thread_reader()) {}
        ~ReadScope() { m_rcu->rcu_read_unlock(*m_reader); }
        ReadScope(const ReadScope&) = delete;
        ReadScope& operator=(const ReadScope&) = delete;
        ReadScope(ReadScope&&) = delete;
        ReadScope& operator=(ReadScope&&) = delete;

        const T& data() const TANH_NONBLOCKING_FUNCTION { return *m_data; }
        const T* operator->() const TANH_NONBLOCKING_FUNCTION { return m_data; }

    private:
        friend class RCU;
        // Read section on a given reader node: no registration, no lookup.
        ReadScope(const RCU* rcu, detail::RcuReaderNode& reader) : m_rcu(rcu), m_reader(&reader) {
            m_rcu->rcu_read_lock(reader);
            // seq_cst: second half of the reader's handshake, see rcu_read_lock().
            m_data = m_rcu->m_data_ptr.load(std::memory_order_seq_cst);
        }

        const RCU* m_rcu;
        detail::RcuReaderNode* m_reader;
        const T* m_data = nullptr;
    };

    /**
     * @brief Open an RCU read section as an RAII scope. See ReadScope.
     */
    ReadScope read_scope() const TANH_NONBLOCKING_FUNCTION { return ReadScope(this); }

    /**
     * @brief Open a read section on a reader slot from add_reader(). Real-time
     * safe on any thread: no registration, no allocation, no lock and no
     * thread-local access.
     *
     * The slot must have one user at a time, ordered as add_reader() describes.
     *
     * @p reader must be this instance's (asserted). An empty one is a contract
     * violation too (asserted); a release build then falls back to the calling
     * thread's registration, which is not real-time safe on first use.
     */
    ReadScope read_scope(Reader reader) const TANH_NONBLOCKING_FUNCTION {
        assert(reader && "RCU::read_scope: empty Reader");
        assert((!reader || reader.m_owner == this) && "RCU::read_scope: another RCU's Reader");
        if (!reader) { return ReadScope(this); }
        return ReadScope(this, *reader.m_node);
    }

private:
    // RCU-protected data pointer
    std::atomic<T*> m_data_ptr;

    // RCU control structures (per-instance for independence)
    // Start grace period at 1 so that readers not reading can be marked with
    // period 0
    mutable std::atomic<uint64_t> m_grace_period{1};
    mutable std::mutex m_writer_mutex;

    // Retired data tracking for deferred reclamation
    struct RetiredData {
        T* m_ptr;
        uint64_t m_grace_period;
    };
    std::vector<RetiredData> m_retired_list;

    // Per-instance cleanup thresholds (tunable per use case)
    size_t m_cleanup_threshold;    // Try harder to cleanup
    size_t m_emergency_threshold;  // Force blocking cleanup

    // Per-instance reader list head. Thread nodes are owned by the registering
    // thread's detail::RcuThreadState (see RcuThreadState for why that
    // registry lives outside this template); add_reader() slots by this
    // instance.
    mutable std::atomic<detail::RcuReaderNode*> m_reader_head{nullptr};

    // The calling thread's reader node, registered on first use. Registration
    // allocates and locks; later calls are one hash lookup.
    detail::RcuReaderNode& thread_reader() const {
        auto& thread_state = detail::rcu_thread_state();
        if (auto* node = thread_state.get_node(this)) {
            if (node->m_state.load(std::memory_order_acquire) != detail::RcuNodeState::RcuGone) {
                return *node;
            }
            // Left by an RCU destroyed at this address: not in our list. Register
            // anew (the sweep below frees it).
        }

        // Free what destroyed RCUs left in this thread's registry (those nodes
        // are ours now), so the registry holds at most this thread's live RCUs
        // plus the ones destroyed since its last registration.
        std::erase_if(thread_state.m_nodes, [](const auto& entry) {
            return entry.second->m_state.load(std::memory_order_acquire) ==
                   detail::RcuNodeState::RcuGone;
        });

        // Serialize with cleanup and count
        const std::scoped_lock lock(m_writer_mutex);
        // Allocate node on heap so it persists beyond thread lifetime
        auto node = std::make_unique<detail::RcuReaderNode>();
        link_reader_with_lock(*node);
        auto& reader = *node;
        thread_state.m_nodes.emplace(this, std::move(node));
        return reader;
    }

    // Push node onto the reader list. Must be called with m_writer_mutex held,
    // as every walk of the list is: readers never walk it, only writers, the
    // count and the destructor do.
    void link_reader_with_lock(detail::RcuReaderNode& node) const {
        detail::RcuReaderNode* current_head = m_reader_head.load(std::memory_order_acquire);
        do {
            node.m_next.store(current_head, std::memory_order_relaxed);
        } while (!m_reader_head.compare_exchange_weak(current_head,
                                                      &node,
                                                      std::memory_order_release,
                                                      std::memory_order_acquire));
    }

    // RCU operations
    //
    // Ordering contract between a reader entering a section and the writer
    // reclaiming a version. Both sides do a store followed by a load of the
    // *other* side's variable:
    //
    //   reader: store m_read_generation  →  load m_data_ptr
    //   writer: store m_data_ptr         →  load m_read_generation
    //
    // Reclamation is only safe if at least one of them observes the other's
    // store: either the reader sees the new pointer, or the writer sees the
    // reader's generation and keeps the old version alive. That is a store-load
    // (Dekker) handshake and needs sequential consistency on all four
    // operations. memory_order_release / acquire do not provide it: a release
    // store is a plain move that can still sit in the core's store buffer while
    // the following load executes, so both sides can miss each other — the
    // writer reads generation 0, frees the version, and the reader dereferences
    // it. Observed as an audio-thread SIGSEGV on a freed ProcessingConfig.
    //
    // seq_cst costs the reader one locked exchange per section entry (wait-free,
    // no syscall) and nothing on the loads.
    void rcu_read_lock(detail::RcuReaderNode& node) const {
        // Nested: the outer section holds.
        if (node.m_depth.fetch_add(1, std::memory_order_relaxed) > 0) { return; }
        const uint64_t current_period = m_grace_period.load(std::memory_order_acquire);
        node.m_read_generation.store(current_period, std::memory_order_seq_cst);
    }

    // Leaving a section is not part of the handshake: a writer that misses this
    // store still sees the old generation and merely waits a little longer.
    void rcu_read_unlock(detail::RcuReaderNode& node) const {
        if (node.m_depth.fetch_sub(1, std::memory_order_relaxed) > 1) { return; }
        node.m_read_generation.store(0, std::memory_order_release);
    }

    /**
     * @brief Non-blocking cleanup of retired versions that are safe to delete
     *
     * Finds the minimum grace period any reader is currently in, then batch
     * deletes all retired versions older than that period.
     *
     * Allocates nothing (it frees, and erases in place). Must be called while
     * holding m_writer_mutex.
     */
    void cleanup_safe_versions() noexcept {
        if (m_retired_list.empty()) { return; }

        // Get current grace period - we can never delete data from current or
        // previous period
        const uint64_t current_period = m_grace_period.load(std::memory_order_acquire);

        // Find minimum period any active reader is in
        uint64_t min_active_period = current_period;

        const detail::RcuReaderNode* node = m_reader_head.load(std::memory_order_acquire);
        while (node != nullptr) {
            if (!node->thread_gone()) {
                // seq_cst: second half of the writer's handshake, see
                // rcu_read_lock().
                const uint64_t reader_period =
                    node->m_read_generation.load(std::memory_order_seq_cst);

                if (reader_period != 0) {
                    // Active reader - consider its period
                    if (reader_period < min_active_period) { min_active_period = reader_period; }
                }
            }
            node = node->m_next.load(std::memory_order_acquire);
        }

        // Delete all versions older than the threshold
        auto it = m_retired_list.begin();
        while (it != m_retired_list.end()) {
            if (it->m_grace_period < min_active_period) {
                delete it->m_ptr;  // Safe - all readers past this period
                it = m_retired_list.erase(it);
            } else {
                ++it;  // Keep newer versions
            }
        }
    }

    /**
     * @brief Blocking wait for all current readers to finish
     *
     * Used as emergency cleanup when retired list grows too large. Allocates
     * nothing (it polls). Must be called while holding m_writer_mutex.
     */
    void synchronize_rcu() noexcept {
        // Start new grace period
        const uint64_t current_period = m_grace_period.load(std::memory_order_acquire);

        // Wait for all live readers to finish their read sections
        const detail::RcuReaderNode* current = m_reader_head.load(std::memory_order_acquire);
        while (current != nullptr) {
            // Wait for the reader to exit its read section. The dead flag is
            // re-checked on every pass: a thread that has exited cannot be
            // inside a read section any more, so its node must never pin the
            // writer — even if the generation it left behind is stale.
            while (!current->thread_gone()) {
                // seq_cst: second half of the writer's handshake, see
                // rcu_read_lock().
                const uint64_t reader_period =
                    current->m_read_generation.load(std::memory_order_seq_cst);
                if (reader_period == 0) {
                    break;  // Truly idle - not reading and not starting
                }
                if (reader_period == current_period) {
                    break;  // Reading current data, safe to proceed
                }
                // Yield to give readers a chance to complete
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
            current = current->m_next.load(std::memory_order_acquire);
        }
    }

    // Unlink and free the nodes of exited threads, in place: each dead node is
    // taken out of the list before it is freed, and nothing is allocated, so the
    // sweep cannot fail half-way and is part of commit(). The relaxed orderings
    // are enough because every walk of the list holds m_writer_mutex (see
    // link_reader_with_lock()). Must be called while holding m_writer_mutex.
    void cleanup_dead_nodes() noexcept {
        // The pointer that leads to `current`: the head, or the m_next of the
        // last node kept.
        std::atomic<detail::RcuReaderNode*>* link = &m_reader_head;
        detail::RcuReaderNode* current = link->load(std::memory_order_relaxed);
        while (current != nullptr) {
            detail::RcuReaderNode* next = current->m_next.load(std::memory_order_relaxed);

            if (current->thread_gone()) {
                link->store(next, std::memory_order_relaxed);
                delete current;  // its thread left it to us
            } else {
                link = &current->m_next;
            }

            current = next;
        }
    }
};

}  // namespace thl::inline THL_ABI_NAMESPACE
