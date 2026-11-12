#include <doctest/doctest.h>

#include <atomic>
#include <thread>

#include "crowdsim/core/TripleBuffer.hpp"

using crowdsim::core::TripleBuffer;

TEST_CASE("TripleBuffer hands over the latest published value") {
    TripleBuffer<int> tb;
    CHECK_FALSE(tb.acquire());

    tb.writeSlot() = 1;
    tb.publish();
    tb.writeSlot() = 2;
    tb.publish();
    REQUIRE(tb.acquire());
    CHECK(tb.readSlot() == 2);  // intermediate value skipped
    CHECK_FALSE(tb.acquire());  // nothing new
    CHECK(tb.readSlot() == 2);
}

TEST_CASE("TripleBuffer: producer and consumer threads never see torn values") {
    struct Pair {
        int a = 0;
        int b = 0;
    };
    TripleBuffer<Pair> tb;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (int i = 1; i <= 200000; ++i) {
            tb.writeSlot() = Pair{i, -i};
            tb.publish();
        }
        done = true;
    });
    int last = 0;
    bool consistent = true;
    while (!done || tb.acquire()) {
        if (tb.acquire()) {
            const Pair p = tb.readSlot();
            consistent = consistent && p.a == -p.b && p.a >= last;
            last = p.a;
        }
    }
    producer.join();
    CHECK(consistent);
}
