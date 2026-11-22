#include <gtest/gtest.h>
#include <tanh/midi/BlockEventCollector.h>
#include <tanh/midi/InputQueue.h>

#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

using namespace thl::midi;
using thl::BlockEvent;

namespace {

constexpr double k_sample_rate = 48000.0;
constexpr std::uint32_t k_block = 480;
constexpr std::int64_t k_block_ns = 10'000'000;
constexpr std::int64_t k_t0 = 2'000'000'000;

// The time of the middle of frame `frame` of the window of block `block` (block k is
// asked for at t0 + k blocks and, with no extra latency, covers the block before that).
std::int64_t in_block(int block, double frame) {
    return k_t0 + (block - 1) * k_block_ns + std::llround((frame + 0.5) * 1.0e9 / k_sample_rate);
}

Ump note(std::uint8_t number) {
    return make_midi1_channel_voice(0, 0x90, number, 100);
}

std::vector<BlockEvent> collect(BlockEventCollector& collector, InputQueue& queue, int block) {
    const auto events = collector.collect(queue, k_t0 + block * k_block_ns, k_block);
    return {events.begin(), events.end()};
}

BlockEventCollector make(std::size_t max_events = 256, std::size_t max_held = 1024) {
    BlockEventCollectorConfig config;
    config.m_latency_ns = 0;  // plain arithmetic in these tests
    config.m_max_events_per_block = max_events;
    config.m_max_held = max_held;
    return BlockEventCollector(config);
}

}  // namespace

TEST(MidiBlockEventCollector, PlacesPacketsAtTheirSampleOffset) {
    BlockEventCollector collector = make();
    collector.prepare(k_sample_rate, k_block);
    InputQueue queue(64);
    EXPECT_TRUE(collect(collector, queue, 0).empty());

    queue.push(note(60), in_block(1, 120), 1);
    queue.push(note(61), in_block(1, 360), 1);
    const auto events = collect(collector, queue, 1);

    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].m_type, BlockEvent::Type::Midi);
    EXPECT_EQ(events[0].m_midi, note(60));
    EXPECT_EQ(events[0].m_sample_offset, 120U);
    EXPECT_EQ(events[0].m_source, 1);
    EXPECT_EQ(events[1].m_midi, note(61));
    EXPECT_EQ(events[1].m_sample_offset, 360U);
    EXPECT_TRUE(collect(collector, queue, 2).empty());
    EXPECT_EQ(collector.latency_ns(), k_block_ns);
}

// Two inputs deliver on their own threads: the queue order is not the time order. The
// block's list is in time order all the same.
TEST(MidiBlockEventCollector, SortsAcrossInputs) {
    BlockEventCollector collector = make();
    collector.prepare(k_sample_rate, k_block);
    InputQueue queue(64);
    (void)collect(collector, queue, 0);

    queue.push(note(60), in_block(1, 300), 1);
    queue.push(note(70), in_block(1, 100), 2);
    queue.push(note(61), in_block(1, 400), 1);
    queue.push(note(71), in_block(1, 200), 2);
    const auto events = collect(collector, queue, 1);

    ASSERT_EQ(events.size(), 4U);
    EXPECT_EQ(events[0].m_midi, note(70));
    EXPECT_EQ(events[1].m_midi, note(71));
    EXPECT_EQ(events[2].m_midi, note(60));
    EXPECT_EQ(events[3].m_midi, note(61));
    for (std::size_t i = 1; i < events.size(); ++i) {
        EXPECT_LE(events[i - 1].m_sample_offset, events[i].m_sample_offset);
    }
}

// A chord: several packets with one timestamp stay in the order they were sent.
TEST(MidiBlockEventCollector, EqualOffsetsKeepArrivalOrder) {
    BlockEventCollector collector = make();
    collector.prepare(k_sample_rate, k_block);
    InputQueue queue(64);
    (void)collect(collector, queue, 0);

    for (std::uint8_t n = 60; n < 68; ++n) { queue.push(note(n), in_block(1, 240), 0); }
    const auto events = collect(collector, queue, 1);

    ASSERT_EQ(events.size(), 8U);
    for (std::uint8_t i = 0; i < 8; ++i) {
        EXPECT_EQ(events[i].m_midi, note(static_cast<std::uint8_t>(60 + i)));
        EXPECT_EQ(events[i].m_sample_offset, 240U);
    }
}

// A packet from before the block's window (it was handed over late) goes to the start.
TEST(MidiBlockEventCollector, LatePacketsGoToTheBlockStart) {
    BlockEventCollector collector = make();
    collector.prepare(k_sample_rate, k_block);
    InputQueue queue(64);
    (void)collect(collector, queue, 0);
    (void)collect(collector, queue, 1);

    queue.push(note(60), in_block(2, 200), 0);
    queue.push(note(61), in_block(1, 400), 0);  // belongs to the block before
    const auto events = collect(collector, queue, 2);

    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].m_midi, note(61));
    EXPECT_EQ(events[0].m_sample_offset, 0U);
    EXPECT_EQ(events[1].m_midi, note(60));
    EXPECT_EQ(events[1].m_sample_offset, 200U);
}

// A packet stamped ahead of the stream waits and comes out in the block it belongs to.
TEST(MidiBlockEventCollector, HoldsPacketsUntilTheyAreDue) {
    BlockEventCollector collector = make();
    collector.prepare(k_sample_rate, k_block);
    InputQueue queue(64);
    (void)collect(collector, queue, 0);

    queue.push(note(60), in_block(3, 240), 0);
    queue.push(note(61), in_block(1, 10), 0);
    auto events = collect(collector, queue, 1);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].m_midi, note(61));
    EXPECT_TRUE(collect(collector, queue, 2).empty());

    events = collect(collector, queue, 3);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].m_midi, note(60));
    EXPECT_EQ(events[0].m_sample_offset, 240U);
}

// More packets than a block may carry: the rest follow in the next blocks, in order, and
// nothing is lost.
TEST(MidiBlockEventCollector, OverfullBlockCarriesOver) {
    BlockEventCollector collector = make(2);
    collector.prepare(k_sample_rate, k_block);
    InputQueue queue(64);
    (void)collect(collector, queue, 0);

    for (std::uint8_t n = 60; n < 65; ++n) { queue.push(note(n), in_block(1, 10.0 * (n - 59)), 0); }
    std::vector<Ump> seen;
    for (int block = 1; block <= 3; ++block) {
        const auto events = collect(collector, queue, block);
        EXPECT_LE(events.size(), 2U);
        for (const BlockEvent& event : events) { seen.push_back(event.m_midi); }
    }
    ASSERT_EQ(seen.size(), 5U);
    for (std::uint8_t i = 0; i < 5; ++i) {
        EXPECT_EQ(seen[i], note(static_cast<std::uint8_t>(60 + i)));
    }
    EXPECT_EQ(collector.dropped(), 0U);
}

// More waiting packets than there is room for are counted as dropped.
TEST(MidiBlockEventCollector, CountsWhatItCannotHold) {
    BlockEventCollector collector = make(256, 4);
    collector.prepare(k_sample_rate, k_block);
    InputQueue queue(64);
    (void)collect(collector, queue, 0);

    // All far in the future, so none leaves in the next block.
    for (std::uint8_t n = 60; n < 70; ++n) { queue.push(note(n), in_block(50, 0), 0); }
    EXPECT_TRUE(collect(collector, queue, 1).empty());
    EXPECT_EQ(collector.dropped(), 6U);
}

TEST(MidiBlockEventCollector, ResetDropsWhatIsWaiting) {
    BlockEventCollector collector = make();
    collector.prepare(k_sample_rate, k_block);
    InputQueue queue(64);
    (void)collect(collector, queue, 0);
    queue.push(note(60), in_block(3, 0), 0);
    (void)collect(collector, queue, 1);  // now held

    collector.reset();
    for (int block = 0; block < 6; ++block) {
        EXPECT_TRUE(collect(collector, queue, block).empty());
    }
}
