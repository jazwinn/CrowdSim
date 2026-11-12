#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <numeric>
#include <vector>

#include "crowdsim/core/JobSystem.hpp"

using crowdsim::core::IndexRange;
using crowdsim::core::JobSystem;

TEST_CASE("JobSystem parallelFor visits every index exactly once") {
    JobSystem jobs(4);
    CHECK(jobs.threadCount() == 4);

    std::vector<int> visits(100'000, 0);
    jobs.parallelFor(static_cast<std::uint32_t>(visits.size()), 1024, [&](IndexRange r, std::uint32_t) {
        for (std::uint32_t i = r.begin; i < r.end; ++i) {
            visits[i] += 1;
        }
    });
    CHECK(std::accumulate(visits.begin(), visits.end(), 0) == 100'000);
    CHECK(std::all_of(visits.begin(), visits.end(), [](int v) { return v == 1; }));
}

TEST_CASE("JobSystem parallelFor handles empty and tiny ranges") {
    JobSystem jobs(2);
    std::atomic<int> calls{0};
    jobs.parallelFor(0, 16, [&](IndexRange, std::uint32_t) { ++calls; });
    CHECK(calls == 0);
    jobs.parallelFor(3, 16, [&](IndexRange r, std::uint32_t) {
        CHECK(r.begin == 0);
        CHECK(r.end == 3);
        ++calls;
    });
    CHECK(calls == 1);
}

TEST_CASE("Two JobSystems can coexist (no hidden global scheduler)") {
    JobSystem a(2), b(2);
    std::atomic<int> sum{0};
    a.parallelFor(1000, 10, [&](IndexRange r, std::uint32_t) { sum += static_cast<int>(r.end - r.begin); });
    b.parallelFor(1000, 10, [&](IndexRange r, std::uint32_t) { sum += static_cast<int>(r.end - r.begin); });
    CHECK(sum == 2000);
}
