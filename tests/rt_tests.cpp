#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <vector>

#include "core/rt/AudioStats.h"
#include "core/rt/CommandQueue.h"
#include "core/rt/SpscRingBuffer.h"

using namespace onda::rt;

TEST_CASE("SpscRingBuffer FIFO order and capacity") {
    SpscRingBuffer<int, 8> rb; // usable capacity = 7

    for (int i = 0; i < 7; ++i)
        REQUIRE(rb.tryPush(i));
    REQUIRE_FALSE(rb.tryPush(99)); // full

    for (int i = 0; i < 7; ++i) {
        int v = -1;
        REQUIRE(rb.tryPop(v));
        REQUIRE(v == i);
    }
    REQUIRE(rb.empty());

    int v = 0;
    REQUIRE_FALSE(rb.tryPop(v)); // empty
}

TEST_CASE("SpscRingBuffer wraparound stays consistent") {
    SpscRingBuffer<int, 4> rb; // capacity 3

    for (int round = 0; round < 100; ++round) {
        for (int i = 0; i < 3; ++i)
            REQUIRE(rb.tryPush(round * 10 + i));
        for (int i = 0; i < 3; ++i) {
            int v = -1;
            REQUIRE(rb.tryPop(v));
            REQUIRE(v == round * 10 + i);
        }
    }
}

TEST_CASE("SpscRingBuffer concurrent producer/consumer") {
    SpscRingBuffer<int, 1024> rb;
    constexpr int kCount = 50000;
    std::vector<int> received;
    received.reserve(kCount);

    std::thread consumer([&] {
        int v;
        while ((int)received.size() < kCount)
            if (rb.tryPop(v)) received.push_back(v);
    });
    std::thread producer([&] {
        for (int i = 0; i < kCount; ++i)
            while (!rb.tryPush(i)) { /* spin */ }
    });

    producer.join();
    consumer.join();
    for (int i = 0; i < kCount; ++i)
        REQUIRE(received[(size_t)i] == i);
}

TEST_CASE("CommandQueue delivers typed commands") {
    CommandQueue q;
    REQUIRE(q.tryPush(SetMasterGain{0.5f}));
    REQUIRE(q.tryPush(SetLimiterOn{false}));

    Command cmd;
    REQUIRE(q.tryPop(cmd));
    REQUIRE(std::get<SetMasterGain>(cmd).gain == 0.5f);
    REQUIRE(q.tryPop(cmd));
    REQUIRE(std::get<SetLimiterOn>(cmd).enabled == false);
}

TEST_CASE("AudioStats fields are atomics and start zeroed") {
    AudioStats s;
    REQUIRE(s.callbackCount.load() == 0);
    REQUIRE(s.xrunCount.load() == 0);
    REQUIRE_FALSE(s.running.load());
    s.xrunCount.fetch_add(3);
    REQUIRE(s.xrunCount.load() == 3);
}
