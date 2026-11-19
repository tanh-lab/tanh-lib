// What the writer side of thl::RCU allocates, and what an allocation that fails
// does to it. This file replaces the global operator new and operator delete,
// which is why it is an executable of its own (test_core_allocation): every
// allocation the calling thread makes through them is counted, and the n-th
// one can be made to fail. Not built with ThreadSanitizer, LeakSanitizer or
// MemorySanitizer, whose runtimes define these operators themselves (see
// CMakeLists.txt); what the two steps do between threads is in test_RCU.cpp.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

#include "tanh/core/threading/RCU.h"

using namespace thl;

namespace {

// What the calling thread did through operator new and operator delete.
// Thread-local, so that what another thread allocates meanwhile (a reader that
// registers) is not counted, and constant-initialised, so that reading it
// allocates nothing.
struct AllocationProbe {
    uint64_t m_requests = 0;  // calls of operator new, failed ones included
    uint64_t m_frees = 0;     // calls of operator delete with a block
    uint64_t m_fail_in = 0;   // > 0: the request that many from now fails
};

constinit thread_local AllocationProbe t_probe;

bool request_fails() noexcept {
    ++t_probe.m_requests;
    return t_probe.m_fail_in != 0 && --t_probe.m_fail_in == 0;
}

void* allocate(std::size_t size) noexcept {
    if (request_fails()) { return nullptr; }
    return std::malloc(std::max<std::size_t>(size, 1));
}

void* allocate(std::size_t size, std::align_val_t alignment) noexcept {
    if (request_fails()) { return nullptr; }
#if defined(_WIN32)
    return _aligned_malloc(std::max<std::size_t>(size, 1), static_cast<std::size_t>(alignment));
#else
    void* block = nullptr;
    const std::size_t at_least_a_pointer =
        std::max(static_cast<std::size_t>(alignment), sizeof(void*));
    if (posix_memalign(&block, at_least_a_pointer, std::max<std::size_t>(size, 1)) != 0) {
        return nullptr;
    }
    return block;
#endif
}

void release(void* block) noexcept {
    if (block == nullptr) { return; }
    ++t_probe.m_frees;
    std::free(block);
}

void release_aligned(void* block) noexcept {
    if (block == nullptr) { return; }
    ++t_probe.m_frees;
#if defined(_WIN32)
    _aligned_free(block);
#else
    std::free(block);
#endif
}

void* or_bad_alloc(void* block) {
    if (block == nullptr) { throw std::bad_alloc(); }
    return block;
}

}  // namespace

// Every replaceable form, so that no allocation of this executable goes around
// the probe and no block is freed by another allocator than the one it is from.
void* operator new(std::size_t size) {
    return or_bad_alloc(allocate(size));
}
void* operator new[](std::size_t size) {
    return or_bad_alloc(allocate(size));
}
void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return allocate(size);
}
void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return allocate(size);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return or_bad_alloc(allocate(size, alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return or_bad_alloc(allocate(size, alignment));
}
void* operator new(std::size_t size,
                   std::align_val_t alignment,
                   const std::nothrow_t& /*tag*/) noexcept {
    return allocate(size, alignment);
}
void* operator new[](std::size_t size,
                     std::align_val_t alignment,
                     const std::nothrow_t& /*tag*/) noexcept {
    return allocate(size, alignment);
}

void operator delete(void* block) noexcept {
    release(block);
}
void operator delete[](void* block) noexcept {
    release(block);
}
void operator delete(void* block, std::size_t /*size*/) noexcept {
    release(block);
}
void operator delete[](void* block, std::size_t /*size*/) noexcept {
    release(block);
}
void operator delete(void* block, const std::nothrow_t& /*tag*/) noexcept {
    release(block);
}
void operator delete[](void* block, const std::nothrow_t& /*tag*/) noexcept {
    release(block);
}
void operator delete(void* block, std::align_val_t /*alignment*/) noexcept {
    release_aligned(block);
}
void operator delete[](void* block, std::align_val_t /*alignment*/) noexcept {
    release_aligned(block);
}
void operator delete(void* block, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
    release_aligned(block);
}
void operator delete[](void* block, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
    release_aligned(block);
}
void operator delete(void* block,
                     std::align_val_t /*alignment*/,
                     const std::nothrow_t& /*tag*/) noexcept {
    release_aligned(block);
}
void operator delete[](void* block,
                       std::align_val_t /*alignment*/,
                       const std::nothrow_t& /*tag*/) noexcept {
    release_aligned(block);
}

namespace {

struct Counts {
    uint64_t m_requests = 0;
    uint64_t m_frees = 0;
};

// What the calling thread allocated and freed while `operation` ran.
template <typename Operation>
Counts counted(Operation&& operation) {
    const AllocationProbe before = t_probe;
    operation();
    return {t_probe.m_requests - before.m_requests, t_probe.m_frees - before.m_frees};
}

// Runs `operation` with its first allocation failing, then with its second, and
// so on until it gets through. Each time one failed, the operation must have
// thrown std::bad_alloc and given back every block it had got until then, and
// `unchanged(n)` checks the rest. Returns how many allocations were failed,
// which is how many the operation makes when it gets through.
template <typename Operation, typename Unchanged>
uint64_t fail_each_allocation(Operation&& operation, Unchanged&& unchanged) {
    for (uint64_t nth = 1;; ++nth) {
        const AllocationProbe before = t_probe;
        bool threw = false;
        t_probe.m_fail_in = nth;
        try {
            operation();
        } catch (const std::bad_alloc&) { threw = true; }
        const bool failed = t_probe.m_fail_in == 0;
        t_probe.m_fail_in = 0;
        const uint64_t requests = t_probe.m_requests - before.m_requests;
        const uint64_t frees = t_probe.m_frees - before.m_frees;

        if (!failed) {
            EXPECT_FALSE(threw);
            return nth - 1;
        }
        EXPECT_TRUE(threw) << "allocation " << nth << " failed and nothing was thrown";
        EXPECT_EQ(frees, requests - 1) << "allocation " << nth << " failed: blocks left behind";
        unchanged(nth);
    }
}

// A version that owns heap blocks of its own, so that building one and copying
// one take several allocations.
struct Table {
    std::vector<int> m_rows;
    std::string m_name;

    bool operator==(const Table&) const = default;
};

Table table_named(char letter) {
    return {std::vector<int>(8, letter), std::string(64, letter)};  // 64: not in the string itself
}

void fill_b(Table& table) {
    table.m_rows.assign(8, 'b');
    table.m_name.assign(64, 'b');
}

void rename_b(Table& table) {
    std::ranges::fill(table.m_rows, 'b');
    table.m_name = std::string(64, 'b');
}

Table current(const RCU<Table>& rcu, RCU<Table>::Reader reader) {
    const auto scope = rcu.read_scope(reader);
    return scope.data();
}

void bump(int& value) {
    ++value;
}

}  // namespace

// commit() publishes, retires and reclaims without an allocation, with a slot
// and a registered thread to scan and the node of an exited thread to unlink.
TEST(RCUAllocation, CommitAllocatesNothing) {
    RCU<int> rcu(1);
    const auto slot = rcu.add_reader();
    rcu.register_reader_thread();
    std::thread exited([&rcu] { rcu.register_reader_thread(); });
    exited.join();

    auto staged = rcu.stage_update(bump);
    const Counts commit = counted([&] { rcu.commit(std::move(staged)); });
    EXPECT_EQ(commit.m_requests, 0u);
    // Reclamation is part of the commit: the version it replaced and the exited
    // thread's reader node.
    EXPECT_EQ(commit.m_frees, 2u);
    EXPECT_EQ(rcu.read_scope(slot).data(), 2);

    // Versions that own heap blocks, one of them held back by a reader for a
    // while: freed by the commit after the reader left, still without a request.
    RCU<Table> tables(table_named('a'));
    const auto table_slot = tables.add_reader();
    uint64_t requests = 0;
    {
        const auto held = tables.read_scope(table_slot);
        auto next = tables.stage_replace(fill_b);
        requests += counted([&] { tables.commit(std::move(next)); }).m_requests;
        EXPECT_EQ(held.data(), table_named('a'));
    }
    auto next = tables.stage_update(rename_b);
    const Counts last = counted([&] { tables.commit(std::move(next)); });
    requests += last.m_requests;
    EXPECT_EQ(requests, 0u);
    EXPECT_GE(last.m_frees, 6u);  // two versions: the Table, its rows, its name
    EXPECT_EQ(current(tables, table_slot), table_named('b'));
}

// None of a publication's reclamation tiers allocates in commit().
TEST(RCUAllocation, CommitAllocatesNothingInAnyReclamationTier) {
    // Tier 2, the threshold cleanup: a reader holds the versions back, so the
    // retired list passes a cleanup threshold of 2.
    {
        RCU<Table> rcu(table_named('a'), 2, 1000);
        const auto slot = rcu.add_reader();
        uint64_t requests = 0;
        {
            const auto held = rcu.read_scope(slot);
            for (int i = 0; i < 6; ++i) {
                auto staged = rcu.stage_update(rename_b);
                requests += counted([&] { rcu.commit(std::move(staged)); }).m_requests;
            }
        }
        auto staged = rcu.stage_update(rename_b);
        const Counts last = counted([&] { rcu.commit(std::move(staged)); });
        EXPECT_EQ(requests + last.m_requests, 0u);
        EXPECT_GE(last.m_frees, 7u * 3u);  // the seven versions held back until now
    }

    // Tier 3, the emergency cleanup: with a threshold of 0 every publication
    // takes it. No reader is inside a section, so it has nothing to wait for.
    {
        RCU<Table> rcu(table_named('a'), 8, 0);
        const auto slot = rcu.add_reader();
        uint64_t requests = 0;
        for (int i = 0; i < 4; ++i) {
            auto staged = rcu.stage_replace(fill_b);
            requests += counted([&] { rcu.commit(std::move(staged)); }).m_requests;
        }
        EXPECT_EQ(requests, 0u);
        EXPECT_EQ(current(rcu, slot), table_named('b'));
    }

    // Tier 3 with a reader to wait for. The reader stays inside its section
    // until it has seen the third publication, and 50 ms longer: by then the
    // writer has found three retired versions (the threshold) and polls for
    // this section to end.
    {
        RCU<int> rcu(0, 2, 3);
        const auto slot = rcu.add_reader();
        const auto peek = rcu.add_reader();
        std::atomic<bool> inside{false};
        std::thread reader([&] {
            const auto held = rcu.read_scope(slot);
            inside.store(true);
            while (rcu.read_scope(peek).data() != 3) { std::this_thread::yield(); }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        });
        while (!inside.load()) { std::this_thread::yield(); }

        uint64_t requests = 0;
        for (int i = 0; i < 3; ++i) {
            auto staged = rcu.stage_update(bump);
            requests += counted([&] { rcu.commit(std::move(staged)); }).m_requests;
        }
        reader.join();
        EXPECT_EQ(requests, 0u);
        EXPECT_EQ(rcu.read_scope(peek).data(), 3);
    }
}

// What a stage allocates: the version, and room in the retired list when that
// is full. The list grows as push_back() grows it, so a stage and its commit
// ask for as many blocks as update() and replace() do, at the same publications.
TEST(RCUAllocation, StageAllocatesTheVersionAndTheRoomToRetireOne) {
    RCU<int> two_step(0);
    RCU<int> one_step(0);
    RCU<int>::Staged staged;

    // An empty retired list: the version and the list's first entry.
    EXPECT_EQ(counted([&] { staged = two_step.stage_replace(bump); }).m_requests, 2u);
    EXPECT_EQ(counted([&] { two_step.commit(std::move(staged)); }).m_requests, 0u);
    EXPECT_EQ(counted([&] { one_step.replace(bump); }).m_requests, 2u);

    // Nothing held the replaced version back, so the entry is free again: the
    // version alone.
    EXPECT_EQ(counted([&] { staged = two_step.stage_update(bump); }).m_requests, 1u);
    EXPECT_EQ(counted([&] { two_step.commit(std::move(staged)); }).m_requests, 0u);
    EXPECT_EQ(counted([&] { one_step.update(bump); }).m_requests, 1u);

    // A stage that is dropped gives its version back.
    const Counts dropped = counted([&] { (void)two_step.stage_update(bump); });
    EXPECT_EQ(dropped.m_requests, 1u);
    EXPECT_EQ(dropped.m_frees, 1u);

    // A reader holds every version back: the list has to grow, and doubles.
    // Fewer publications than the emergency threshold: the reader is this thread.
    constexpr size_t k_publications = 20;
    const auto two_step_slot = two_step.add_reader();
    const auto one_step_slot = one_step.add_reader();
    const auto two_step_held = two_step.read_scope(two_step_slot);
    const auto one_step_held = one_step.read_scope(one_step_slot);
    std::vector<uint64_t> two_step_requests(k_publications);
    std::vector<uint64_t> one_step_requests(k_publications);
    uint64_t total = 0;
    for (size_t i = 0; i < k_publications; ++i) {
        two_step_requests[i] = counted([&] { staged = two_step.stage_update(bump); }).m_requests;
        EXPECT_EQ(counted([&] { two_step.commit(std::move(staged)); }).m_requests, 0u);
        one_step_requests[i] = counted([&] { one_step.update(bump); }).m_requests;
        total += two_step_requests[i];
    }
    EXPECT_EQ(two_step_requests, one_step_requests);
    // The versions, and a list of one entry grown to 2, 4, 8, 16 and 32.
    EXPECT_EQ(total, k_publications + 5u);
}

// A stage throws std::bad_alloc whichever of its allocations fails: the
// version, what the function allocates in it, the room in the retired list.
// Then nothing has changed and nothing is left behind.
TEST(RCUAllocation, StageThatCannotAllocateChangesNothing) {
    for (const bool copy : {false, true}) {
        RCU<Table> rcu(table_named('a'));
        const auto slot = rcu.add_reader();
        const Table* const version = &rcu.read_scope(slot).data();
        RCU<Table>::Staged staged;

        const uint64_t failed = fail_each_allocation(
            [&] {
                if (copy) {
                    staged = rcu.stage_update(rename_b);
                } else {
                    staged = rcu.stage_replace(fill_b);
                }
            },
            [&](uint64_t nth) {
                EXPECT_FALSE(static_cast<bool>(staged)) << nth;
                const auto scope = rcu.read_scope(slot);
                EXPECT_EQ(&scope.data(), version) << nth;
                EXPECT_EQ(scope.data(), table_named('a')) << nth;
            });
        EXPECT_GE(failed, 4u) << copy;  // the Table, its rows, its name, the retired list's entry

        ASSERT_TRUE(static_cast<bool>(staged));
        EXPECT_EQ(staged.data(), table_named('b'));
        EXPECT_EQ(counted([&] { rcu.commit(std::move(staged)); }).m_requests, 0u);
        EXPECT_EQ(current(rcu, slot), table_named('b'));
    }
}

// update() and replace() make the same allocations, all of them before they
// publish: they publish, or they throw and nothing has changed.
TEST(RCUAllocation, UpdateAndReplaceThatCannotAllocateChangeNothing) {
    for (const bool copy : {false, true}) {
        RCU<Table> rcu(table_named('a'));
        const auto slot = rcu.add_reader();
        const Table* const version = &rcu.read_scope(slot).data();

        const uint64_t failed = fail_each_allocation(
            [&] {
                if (copy) {
                    rcu.update(rename_b);
                } else {
                    rcu.replace(fill_b);
                }
            },
            [&](uint64_t nth) {
                const auto scope = rcu.read_scope(slot);
                EXPECT_EQ(&scope.data(), version) << nth;
                EXPECT_EQ(scope.data(), table_named('a')) << nth;
            });
        EXPECT_GE(failed, 4u) << copy;
        EXPECT_EQ(current(rcu, slot), table_named('b'));
    }
}

// The change the two steps are for, with an allocation failing anywhere in it:
// two RCUs change together or not at all, and the tail that commits them asks
// for no memory.
TEST(RCUAllocation, TwoRcusChangeTogetherOrNotAtAll) {
    RCU<Table> tables(table_named('a'));
    RCU<std::vector<int>> rows(std::vector<int>{1});
    const auto table_slot = tables.add_reader();
    const auto rows_slot = rows.add_reader();
    uint64_t tail_requests = 0;

    // One publication each beforehand, so that both retired lists have their
    // entry: a list that grew in a stage keeps its room when a later step of
    // the change fails, and the sweep below counts every block that stays.
    tables.update([](Table& /*table*/) {});
    rows.update([](std::vector<int>& /*values*/) {});

    const auto change_both = [&] {
        auto next_table = tables.stage_replace(fill_b);
        auto next_rows = rows.stage_update([](std::vector<int>& values) { values.push_back(2); });
        const AllocationProbe before = t_probe;
        tables.commit(std::move(next_table));
        rows.commit(std::move(next_rows));
        tail_requests += t_probe.m_requests - before.m_requests;
    };

    const uint64_t failed = fail_each_allocation(change_both, [&](uint64_t nth) {
        EXPECT_EQ(current(tables, table_slot), table_named('a')) << nth;
        EXPECT_EQ(rows.read_scope(rows_slot).data(), (std::vector<int>{1})) << nth;
    });
    // The Table, its rows and its name; the vector, its elements and their growth.
    EXPECT_GE(failed, 6u);
    EXPECT_EQ(tail_requests, 0u);
    EXPECT_EQ(current(tables, table_slot), table_named('b'));
    EXPECT_EQ(rows.read_scope(rows_slot).data(), (std::vector<int>{1, 2}));
}
