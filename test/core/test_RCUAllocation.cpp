// What the writer side of thl::RCU allocates, and what an allocation that fails
// does to it. This file replaces the global operator new and operator delete,
// which is why it is an executable of its own (test_core_allocation): every
// allocation the calling thread makes through them is counted, and the n-th
// one can be made to fail. Not built with ThreadSanitizer, LeakSanitizer or
// MemorySanitizer, whose runtimes define these operators themselves (see
// CMakeLists.txt); what the two steps do between threads is in test_RCU.cpp.
//
// An allocation that is made to fail inside a noexcept function ends in
// std::terminate(). The tests keep such functions out of the way (see Table),
// and when the process dies all the same it says where it was (report_death()).

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <new>
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

    // For report_death(): what fail_each_allocation() is doing.
    const char* m_sweep = nullptr;  // the operation it runs, null outside a sweep
    uint64_t m_failing = 0;         // which of the operation's allocations fails in this attempt
    uint64_t m_attempt_from = 0;    // m_requests when the attempt began
    bool m_failed = false;          // the attempt's failure has happened
    std::size_t m_failed_size = 0;  // the size of the request that was failed
    bool m_reported = false;        // report_death() has spoken
};

constinit thread_local AllocationProbe t_probe;

bool request_fails(std::size_t size) noexcept {
    ++t_probe.m_requests;
    if (t_probe.m_fail_in == 0 || --t_probe.m_fail_in != 0) { return false; }
    t_probe.m_failed = true;
    t_probe.m_failed_size = size;
    return true;
}

void* allocate(std::size_t size) noexcept {
    if (request_fails(size)) { return nullptr; }
    return std::malloc(std::max<std::size_t>(size, 1));
}

void* allocate(std::size_t size, std::align_val_t alignment) noexcept {
    if (request_fails(size)) { return nullptr; }
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
// which is how many the operation makes when it gets through. `sweep` names the
// operation for report_death().
template <typename Operation, typename Unchanged>
uint64_t fail_each_allocation(const char* sweep, Operation&& operation, Unchanged&& unchanged) {
    for (uint64_t nth = 1;; ++nth) {
        const AllocationProbe before = t_probe;
        bool threw = false;
        t_probe.m_sweep = sweep;
        t_probe.m_failing = nth;
        t_probe.m_attempt_from = before.m_requests;
        t_probe.m_failed = false;
        t_probe.m_fail_in = nth;
        try {
            operation();
        } catch (const std::bad_alloc&) { threw = true; }
        const bool failed = t_probe.m_failed;
        t_probe.m_fail_in = 0;
        t_probe.m_sweep = nullptr;
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

// Says where the process is when it dies: the test, the sweep of failing
// allocations and the allocation that fails in it. A failed allocation whose
// std::bad_alloc reaches a noexcept function ends in std::terminate(), and
// googletest silences abort() on Windows: without this, such a death leaves
// nothing behind the test's "[ RUN ]" line.
void report_death(const char* how) noexcept {
    const AllocationProbe probe = t_probe;
    t_probe.m_fail_in = 0;  // whatever runs from here on may allocate
    if (probe.m_reported) { return; }
    t_probe.m_reported = true;

    const testing::TestInfo* const test = testing::UnitTest::GetInstance()->current_test_info();
    std::fprintf(stderr,
                 "\ntest_core_allocation: %s in %s.%s\n",
                 how,
                 test != nullptr ? test->test_suite_name() : "no",
                 test != nullptr ? test->name() : "test");
    const auto into_the_attempt =
        static_cast<unsigned long long>(probe.m_requests - probe.m_attempt_from);
    if (probe.m_sweep == nullptr) {
        std::fprintf(stderr, "  outside a sweep of failing allocations\n");
    } else if (probe.m_failed) {
        std::fprintf(
            stderr,
            "  sweep: %s\n"
            "  allocation %llu of the operation was failed (%zu bytes, request %llu of the "
            "attempt) and its std::bad_alloc was not caught:\n"
            "  a noexcept function between that allocation and the test allocates\n",
            probe.m_sweep,
            static_cast<unsigned long long>(probe.m_failing),
            probe.m_failed_size,
            into_the_attempt);
    } else {
        std::fprintf(
            stderr,
            "  sweep: %s\n"
            "  allocation %llu of the operation is the one to fail; %llu requests into the "
            "attempt, it has not been reached\n",
            probe.m_sweep,
            static_cast<unsigned long long>(probe.m_failing),
            into_the_attempt);
    }
    std::fflush(stderr);
}

[[noreturn]] void on_terminate() noexcept {
    report_death("std::terminate()");
    std::abort();
}

void on_abort(int /*signal*/) noexcept {
    report_death("abort()");
}

// The tests' fixture: installs the report for this executable, per test and on
// the thread that runs it (Microsoft's runtime keeps the terminate handler per
// thread).
class RCUAllocation : public testing::Test {
protected:
    void SetUp() override {
        t_probe.m_sweep = nullptr;
        t_probe.m_reported = false;
        std::set_terminate(on_terminate);
        std::signal(SIGABRT, on_abort);
    }
};

// A version that owns two heap blocks and holds no standard container, so that
// every allocation it makes is made where it may throw. A std::vector or a
// std::string member would not do: with the debug iterators of Microsoft's
// standard library (_ITERATOR_DEBUG_LEVEL 2, what a Debug build has) those
// allocate a proxy in their default and move constructors, which are noexcept
// all the same. Making that allocation fail ends in std::terminate() inside the
// payload's constructor, whatever the RCU does.
class Table {
public:
    Table() = default;  // owns nothing yet, allocates nothing
    explicit Table(char letter)
        : m_rows(block_of<int>(k_rows, letter)), m_name(block_of<char>(k_name, letter)) {}
    Table(const Table& other)
        : m_rows(copy_of(other.m_rows, k_rows)), m_name(copy_of(other.m_name, k_name)) {}
    Table(Table&&) noexcept = default;
    Table& operator=(const Table&) = delete;
    Table& operator=(Table&&) = delete;
    ~Table() = default;

    // For a fresh version: two allocations, the rows and the name.
    void fill(char letter) {
        m_rows = block_of<int>(k_rows, letter);
        m_name = block_of<char>(k_name, letter);
    }

    // For a copy: the rows in place, and one allocation, the new name.
    void rename(char letter) {
        if (m_rows != nullptr) { std::fill_n(m_rows.get(), k_rows, letter); }
        m_name = block_of<char>(k_name, letter);
    }

    bool operator==(const Table& other) const {
        return same(m_rows, other.m_rows, k_rows) && same(m_name, other.m_name, k_name);
    }

private:
    static constexpr std::size_t k_rows = 8;
    static constexpr std::size_t k_name = 64;

    template <typename Element>
    static std::unique_ptr<Element[]> block_of(std::size_t count, char letter) {
        auto block = std::make_unique<Element[]>(count);
        std::fill_n(block.get(), count, static_cast<Element>(letter));
        return block;
    }

    template <typename Element>
    static std::unique_ptr<Element[]> copy_of(const std::unique_ptr<Element[]>& from,
                                              std::size_t count) {
        if (from == nullptr) { return nullptr; }
        auto block = std::make_unique<Element[]>(count);
        std::copy_n(from.get(), count, block.get());
        return block;
    }

    template <typename Element>
    static bool same(const std::unique_ptr<Element[]>& left,
                     const std::unique_ptr<Element[]>& right,
                     std::size_t count) {
        if (left == nullptr || right == nullptr) { return left == right; }
        return std::equal(left.get(), left.get() + count, right.get());
    }

    std::unique_ptr<int[]> m_rows;
    std::unique_ptr<char[]> m_name;
};

void fill_b(Table& table) {
    table.fill('b');
}

void rename_b(Table& table) {
    table.rename('b');
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
TEST_F(RCUAllocation, CommitAllocatesNothing) {
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
    RCU<Table> tables(Table('a'));
    const auto table_slot = tables.add_reader();
    uint64_t requests = 0;
    {
        const auto held = tables.read_scope(table_slot);
        auto next = tables.stage_replace(fill_b);
        requests += counted([&] { tables.commit(std::move(next)); }).m_requests;
        EXPECT_EQ(held.data(), Table('a'));
    }
    auto next = tables.stage_update(rename_b);
    const Counts last = counted([&] { tables.commit(std::move(next)); });
    requests += last.m_requests;
    EXPECT_EQ(requests, 0u);
    EXPECT_EQ(last.m_frees, 2u * 3u);  // two versions: the Table, its rows, its name
    EXPECT_EQ(current(tables, table_slot), Table('b'));
}

// None of a publication's reclamation tiers allocates in commit().
TEST_F(RCUAllocation, CommitAllocatesNothingInAnyReclamationTier) {
    // Tier 2, the threshold cleanup: a reader holds the versions back, so the
    // retired list passes a cleanup threshold of 2.
    {
        RCU<Table> rcu(Table('a'), 2, 1000);
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
        EXPECT_EQ(last.m_frees, 7u * 3u);  // the seven versions held back until now
    }

    // Tier 3, the emergency cleanup: with a threshold of 0 every publication
    // takes it. No reader is inside a section, so it has nothing to wait for.
    {
        RCU<Table> rcu(Table('a'), 8, 0);
        const auto slot = rcu.add_reader();
        uint64_t requests = 0;
        for (int i = 0; i < 4; ++i) {
            auto staged = rcu.stage_replace(fill_b);
            requests += counted([&] { rcu.commit(std::move(staged)); }).m_requests;
        }
        EXPECT_EQ(requests, 0u);
        EXPECT_EQ(current(rcu, slot), Table('b'));
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
TEST_F(RCUAllocation, StageAllocatesTheVersionAndTheRoomToRetireOne) {
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
//
// The allocations of a stage, in the order in which the sweep fails them.
// stage_replace(): the Table (the RCU's make_unique), its rows and its name
// (fill_b), the retired list's first entry (the RCU's reserve). stage_update():
// the Table, its rows and its name (the copy), the new name (rename_b), the
// entry.
TEST_F(RCUAllocation, StageThatCannotAllocateChangesNothing) {
    for (const bool copy : {false, true}) {
        RCU<Table> rcu(Table('a'));
        const auto slot = rcu.add_reader();
        const Table* const version = &rcu.read_scope(slot).data();
        RCU<Table>::Staged staged;

        const uint64_t failed = fail_each_allocation(
            copy ? "stage_update() of an RCU<Table>" : "stage_replace() of an RCU<Table>",
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
                EXPECT_EQ(scope.data(), Table('a')) << nth;
            });
        EXPECT_EQ(failed, copy ? 5u : 4u);

        ASSERT_TRUE(static_cast<bool>(staged));
        EXPECT_EQ(staged.data(), Table('b'));
        EXPECT_EQ(counted([&] { rcu.commit(std::move(staged)); }).m_requests, 0u);
        EXPECT_EQ(current(rcu, slot), Table('b'));
    }
}

// update() and replace() make the same allocations in the same order, all of
// them before they publish: they publish, or they throw and nothing has changed.
TEST_F(RCUAllocation, UpdateAndReplaceThatCannotAllocateChangeNothing) {
    for (const bool copy : {false, true}) {
        RCU<Table> rcu(Table('a'));
        const auto slot = rcu.add_reader();
        const Table* const version = &rcu.read_scope(slot).data();

        const uint64_t failed = fail_each_allocation(
            copy ? "update() of an RCU<Table>" : "replace() of an RCU<Table>",
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
                EXPECT_EQ(scope.data(), Table('a')) << nth;
            });
        EXPECT_EQ(failed, copy ? 5u : 4u);
        EXPECT_EQ(current(rcu, slot), Table('b'));
    }
}

// The change the two steps are for, with an allocation failing anywhere in it:
// two RCUs change together or not at all, and the tail that commits them asks
// for no memory.
TEST_F(RCUAllocation, TwoRcusChangeTogetherOrNotAtAll) {
    RCU<Table> tables(Table('a'));
    RCU<Table> rows(Table('a'));
    const auto table_slot = tables.add_reader();
    const auto rows_slot = rows.add_reader();
    uint64_t tail_requests = 0;

    // One publication each beforehand, so that both retired lists have their
    // entry: a list that grew in a stage keeps its room when a later step of
    // the change fails, and the sweep below counts every block that stays.
    tables.update([](Table& /*table*/) {});
    rows.update([](Table& /*table*/) {});

    const auto change_both = [&] {
        auto next_table = tables.stage_replace(fill_b);
        auto next_rows = rows.stage_update(rename_b);
        const AllocationProbe before = t_probe;
        tables.commit(std::move(next_table));
        rows.commit(std::move(next_rows));
        tail_requests += t_probe.m_requests - before.m_requests;
    };

    const uint64_t failed = fail_each_allocation(
        "two RCU<Table>: stage_replace() of the first, stage_update() of the second, both commits",
        change_both,
        [&](uint64_t nth) {
            EXPECT_EQ(current(tables, table_slot), Table('a')) << nth;
            EXPECT_EQ(current(rows, rows_slot), Table('a')) << nth;
        });
    // The first's Table, rows and name; the second's Table, rows, name and new name.
    EXPECT_EQ(failed, 7u);
    EXPECT_EQ(tail_requests, 0u);
    EXPECT_EQ(current(tables, table_slot), Table('b'));
    EXPECT_EQ(current(rows, rows_slot), Table('b'));
}
