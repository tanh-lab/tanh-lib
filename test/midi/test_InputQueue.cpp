#include <gtest/gtest.h>
#include <tanh/midi/InputQueue.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using namespace thl::midi;

TEST(MidiInputQueue, DeliversInOrderWithTimeAndSource) {
    InputQueue queue(16);
    EXPECT_TRUE(queue.push(make_midi1_channel_voice(0, 0x90, 60, 100), 1000, 2));
    EXPECT_TRUE(queue.push(make_midi1_channel_voice(0, 0x80, 60, 0), 2000, 3));

    std::vector<TimedUmp> packets;
    EXPECT_EQ(queue.drain([&](const TimedUmp& packet) { packets.push_back(packet); }), 2U);

    ASSERT_EQ(packets.size(), 2U);
    EXPECT_EQ(packets[0].m_ump, make_midi1_channel_voice(0, 0x90, 60, 100));
    EXPECT_EQ(packets[0].m_time_ns, 1000);
    EXPECT_EQ(packets[0].m_source, 2);
    EXPECT_EQ(packets[1].m_ump, make_midi1_channel_voice(0, 0x80, 60, 0));
    EXPECT_EQ(packets[1].m_time_ns, 2000);
    EXPECT_EQ(packets[1].m_source, 3);

    TimedUmp packet;
    EXPECT_FALSE(queue.pop(packet));
    EXPECT_EQ(queue.dropped(), 0U);
}

TEST(MidiInputQueue, CapacityRoundsUpToAPowerOfTwo) {
    EXPECT_EQ(InputQueue(100).capacity(), 128U);
    EXPECT_EQ(InputQueue(64).capacity(), 64U);
    EXPECT_EQ(InputQueue().capacity(), InputQueue::k_default_capacity);
}

// A full queue drops the new packet, keeps what it holds, and counts the loss so the
// consumer can release its notes.
TEST(MidiInputQueue, FullQueueDropsTheNewPacketAndCounts) {
    InputQueue queue(4);
    for (std::uint8_t note = 0; note < 4; ++note) {
        EXPECT_TRUE(queue.push(make_midi1_channel_voice(0, 0x90, note, 100), note));
    }
    EXPECT_FALSE(queue.push(make_midi1_channel_voice(0, 0x90, 4, 100), 4));
    EXPECT_FALSE(queue.push(make_midi1_channel_voice(0, 0x90, 5, 100), 5));
    EXPECT_EQ(queue.dropped(), 2U);

    std::vector<std::uint8_t> notes;
    queue.drain([&](const TimedUmp& packet) { notes.push_back(packet.m_ump.data1()); });
    EXPECT_EQ(notes, (std::vector<std::uint8_t>{0, 1, 2, 3}));

    // Room again once drained; the count stays.
    EXPECT_TRUE(queue.push(make_midi1_channel_voice(0, 0x90, 6, 100), 6));
    EXPECT_EQ(queue.dropped(), 2U);
}

// Each platform input may have its own receive thread: several producers, one consumer,
// nothing lost or duplicated, and every producer's packets still in its own order.
TEST(MidiInputQueue, SeveralProducersOneConsumer) {
    constexpr std::uint16_t k_producers = 4;
    constexpr std::uint32_t k_packets_per_producer = 20000;

    InputQueue queue(256);
    std::atomic<bool> start{false};
    std::atomic<std::uint16_t> finished{0};

    std::vector<std::thread> producers;
    for (std::uint16_t source = 0; source < k_producers; ++source) {
        producers.emplace_back([&, source] {
            while (!start.load(std::memory_order_acquire)) { std::this_thread::yield(); }
            for (std::uint32_t i = 0; i < k_packets_per_producer; ++i) {
                Ump ump;
                ump.m_words[0] = 0x40900000U;
                ump.m_words[1] = i;
                ump.m_size = 2;
                // Retry instead of dropping, so the test can demand every packet.
                TimedUmp packet{.m_time_ns = i, .m_ump = ump, .m_source = source};
                while (!queue.push(packet)) { std::this_thread::yield(); }
            }
            finished.fetch_add(1, std::memory_order_release);
        });
    }

    std::array<std::uint32_t, k_producers> next{};
    std::uint64_t received = 0;
    bool in_order = true;
    const auto sink = [&](const TimedUmp& packet) {
        ASSERT_LT(packet.m_source, k_producers);
        if (packet.m_ump.m_words[1] != next[packet.m_source]) { in_order = false; }
        ++next[packet.m_source];
        ++received;
    };

    start.store(true, std::memory_order_release);
    while (finished.load(std::memory_order_acquire) < k_producers) {
        queue.drain(sink);
        std::this_thread::yield();
    }
    for (auto& producer : producers) { producer.join(); }
    queue.drain(sink);

    EXPECT_TRUE(in_order);
    EXPECT_EQ(received, std::uint64_t{k_producers} * k_packets_per_producer);
    for (const std::uint32_t count : next) { EXPECT_EQ(count, k_packets_per_producer); }
}

// drain() takes at most one queue's worth per call, so a busy producer cannot pin it.
TEST(MidiInputQueue, DrainIsBoundedByCapacity) {
    InputQueue queue(8);
    std::size_t refills = 0;
    const std::size_t drained = queue.drain([&](const TimedUmp&) {});
    EXPECT_EQ(drained, 0U);

    for (std::uint8_t i = 0; i < 8; ++i) { queue.push(make_system(0, 0xF8), i); }
    // The sink keeps the queue full, as a producer that never pauses would.
    const std::size_t count = queue.drain([&](const TimedUmp&) {
        queue.push(make_system(0, 0xF8), 0);
        ++refills;
    });
    EXPECT_EQ(count, queue.capacity());
    EXPECT_EQ(refills, queue.capacity());
}
