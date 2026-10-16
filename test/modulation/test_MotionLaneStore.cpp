#include <gtest/gtest.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionLaneProcessing.h>
#include <tanh/modulation/detail/MotionLaneStore.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

using thl::modulation::LoopEnd;
using thl::modulation::MotionLane;
using thl::modulation::MotionTimebase;
using thl::modulation::detail::MotionLaneSource;
using thl::modulation::detail::MotionLaneStore;
using thl::modulation::detail::MotionLoopView;

namespace {

constexpr size_t k_max_points = 4096;

// A take of @p n points over one bar: x ramps from 0.2 to 0.8 (a step back at the seam).
struct Take {
    std::vector<float> m_x;
    std::vector<float> m_y;
    std::vector<uint8_t> m_gate;

    explicit Take(size_t n = 400, float offset = 0.0f) {
        for (size_t i = 0; i < n; ++i) {
            m_x.push_back(0.2f + offset + (0.6f * static_cast<float>(i) / static_cast<float>(n)));
            m_y.push_back(0.5f);
            m_gate.push_back(1);
        }
    }
    [[nodiscard]] MotionLoopView view() const {
        return {.m_x = m_x.data(),
                .m_y = m_y.data(),
                .m_gate = m_gate.data(),
                .m_num_points = m_x.size(),
                .m_timebase = MotionTimebase::Beats,
                .m_length = 4.0};
    }
};

MotionLane lane_of(const Take& take) {
    MotionLane lane;
    lane.m_timebase = MotionTimebase::Beats;
    lane.m_rate = 100.0;
    lane.m_length = 4.0;
    lane.m_x = take.m_x;
    lane.m_y = take.m_y;
    lane.m_gate = take.m_gate;
    return lane;
}

std::unique_ptr<MotionLaneStore> make_store() {
    auto store = std::make_unique<MotionLaneStore>();
    store->prepare(k_max_points);
    return store;
}

// One audio block's choice.
MotionLaneSource select(MotionLaneStore& store) {
    const auto scope = store.read_scope();
    return store.select(scope.data());
}

}  // namespace

TEST(MotionLaneStore, SubmittedTakePlaysAtOnceAndIsPublishedRaw) {
    auto store = make_store();
    const Take take;
    ASSERT_TRUE(store->submit(take.view(), 1));
    const MotionLaneSource playing = select(*store);
    ASSERT_FALSE(playing.empty());
    EXPECT_GT(playing.m_id, 0u);
    ASSERT_EQ(playing.m_view.m_num_points, 400u);
    // The seam is closed where it plays: the last point leads back towards the first.
    EXPECT_LT(playing.m_view.m_x[399], take.m_x[399] - 0.1f);
    EXPECT_EQ(playing.m_view.m_x[100], take.m_x[100]);

    EXPECT_TRUE(store->service());
    const MotionLane lane = store->lane();
    EXPECT_EQ(lane.m_take_id, playing.m_id);
    EXPECT_EQ(lane.m_x, take.m_x);  // raw
    EXPECT_DOUBLE_EQ(lane.m_length, 4.0);
    const MotionLaneSource published = select(*store);
    EXPECT_EQ(published.m_id, playing.m_id);
    EXPECT_NE(published.m_version, playing.m_version);
    EXPECT_FALSE(store->service());
}

TEST(MotionLaneStore, BothSlotsWaitingRefuseATake) {
    auto store = make_store();
    const Take take;
    ASSERT_TRUE(store->submit(take.view(), 1));
    ASSERT_TRUE(store->submit(take.view(), 2));
    EXPECT_FALSE(store->submit(take.view(), 3));
    EXPECT_TRUE(store->service());
    (void)select(*store);  // the published lane replaces both slots
    EXPECT_TRUE(store->submit(take.view(), 3));
}

TEST(MotionLaneStore, NewestIdPlays) {
    auto store = make_store();
    const Take a(400, 0.0f);
    const Take b(400, 0.1f);
    ASSERT_TRUE(store->submit(a.view(), 1));
    const uint32_t loaded = store->load(lane_of(b), k_max_points);
    ASSERT_GT(loaded, 0u);
    EXPECT_EQ(select(*store).m_id, loaded);  // the load came after the take
    EXPECT_FALSE(store->service());          // the take is older: dropped
    EXPECT_EQ(store->lane().m_take_id, loaded);
}

// The audio thread may still read a lane older than a slot service() just published: the slot
// keeps playing until the block reads the newer lane.
TEST(MotionLaneStore, SlotPlaysUntilItsLaneIsRead) {
    auto store = make_store();
    const Take take;
    ASSERT_TRUE(store->submit(take.view(), 1));
    const auto old_scope = store->read_scope();
    EXPECT_TRUE(store->service());
    const MotionLaneSource source = store->select(old_scope.data());
    EXPECT_GT(source.m_id, old_scope.data().m_lane.m_take_id);
    EXPECT_EQ(source.m_view.m_num_points, 400u);
}

TEST(MotionLaneStore, RevisionsOfOneTakeAreOneUndoStep) {
    auto store = make_store();
    const Take before(400, 0.0f);
    ASSERT_GT(store->load(lane_of(before), k_max_points), 0u);
    store->clear_history();
    const Take first(400, 0.05f);
    const Take second(400, 0.1f);
    ASSERT_TRUE(store->submit(first.view(), 7));
    EXPECT_TRUE(store->service());
    (void)select(*store);
    ASSERT_TRUE(store->submit(second.view(), 7));
    EXPECT_TRUE(store->service());
    EXPECT_EQ(store->lane().m_x, second.m_x);
    ASSERT_TRUE(store->undo());
    EXPECT_EQ(store->lane().m_x, before.m_x);
    ASSERT_TRUE(store->redo());
    EXPECT_EQ(store->lane().m_x, second.m_x);

    // The next take is its own step.
    (void)select(*store);
    const Take next(400, 0.15f);
    ASSERT_TRUE(store->submit(next.view(), 8));
    EXPECT_TRUE(store->service());
    ASSERT_TRUE(store->undo());
    EXPECT_EQ(store->lane().m_x, second.m_x);
}

TEST(MotionLaneStore, SmoothedLanePlaysItsSmoothedCurve) {
    auto store = make_store();
    const Take take;
    ASSERT_GT(store->load(lane_of(take), k_max_points), 0u);
    const uint32_t version = store->version();
    store->set_smoothing(0.6f);
    EXPECT_NE(store->version(), version);
    const MotionLane lane = lane_of(take);
    const auto smoothed = thl::modulation::make_played_curve(lane, 0.6f, LoopEnd::Smooth);
    const MotionLaneSource source = select(*store);
    for (size_t i = 0; i < 400; i += 13) { EXPECT_EQ(source.m_view.m_x[i], smoothed.m_x[i]) << i; }
    EXPECT_EQ(store->played_lane().m_x, smoothed.m_x);
    EXPECT_EQ(store->lane().m_x, take.m_x);
}

TEST(MotionLaneStore, JumpLoopEndLeavesASubmittedTakeOpen) {
    auto store = make_store();
    store->set_loop_end(LoopEnd::Jump);
    const Take take;
    ASSERT_TRUE(store->submit(take.view(), 1));
    const MotionLaneSource playing = select(*store);
    EXPECT_EQ(playing.m_view.m_x[399], take.m_x[399]);
}

TEST(MotionLaneStore, LoadValidatesAndClamps) {
    auto store = make_store();
    MotionLane bad = lane_of(Take());
    bad.m_length = 0.0;
    EXPECT_EQ(store->load(bad, k_max_points), 0u);
    MotionLane loud = lane_of(Take());
    loud.m_x[3] = 1.5f;
    loud.m_gate.clear();  // no gate: every point down
    ASSERT_GT(store->load(loud, k_max_points), 0u);
    EXPECT_EQ(store->lane().m_x[3], 1.0f);
    EXPECT_EQ(store->lane().m_gate.size(), 400u);
    MotionLane big = lane_of(Take(8000));
    ASSERT_GT(store->load(big, k_max_points), 0u);
    EXPECT_EQ(store->lane().num_points(), k_max_points);
}

TEST(MotionLaneStore, ClearIsAnUndoStep) {
    auto store = make_store();
    const Take take;
    ASSERT_GT(store->load(lane_of(take), k_max_points), 0u);
    const uint32_t id = store->clear();
    EXPECT_GT(id, 0u);
    EXPECT_TRUE(store->lane().empty());
    EXPECT_TRUE(select(*store).empty());
    ASSERT_TRUE(store->undo());
    EXPECT_EQ(store->lane().m_x, take.m_x);
    EXPECT_GT(store->lane().m_take_id, id);
}

// A clear drops the take running at the time and any later one until the audio thread ends it.
TEST(MotionLaneStore, DroppedTakesNeitherPlayNorPublish) {
    auto store = make_store();
    const Take take;
    store->drop_takes_from(5);
    ASSERT_TRUE(store->submit(take.view(), 5));
    ASSERT_TRUE(store->submit(take.view(), 6));  // started before the clear was seen
    EXPECT_TRUE(select(*store).empty());
    EXPECT_FALSE(store->service());
    (void)select(*store);  // frees both
    store->stop_dropping_after(6);
    ASSERT_TRUE(store->submit(take.view(), 7));  // after: plays
    EXPECT_FALSE(select(*store).empty());
    EXPECT_TRUE(store->service());
}
