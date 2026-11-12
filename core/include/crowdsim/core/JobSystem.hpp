#pragma once

/// @file JobSystem.hpp
/// Thin, owned wrapper around an enkiTS scheduler. Exactly one instance is
/// created by the composition root and injected wherever parallel work runs.

#include <cstdint>
#include <functional>
#include <memory>

namespace crowdsim::core {

/// Half-open index range [begin, end) handed to a parallel-for body.
struct IndexRange {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
};

class JobSystem {
public:
    /// @param threadCount total worker threads including the caller; 0 = hardware concurrency.
    explicit JobSystem(std::uint32_t threadCount = 0);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    /// Number of threads that execute tasks (including the calling thread).
    [[nodiscard]] std::uint32_t threadCount() const noexcept;

    /// Runs `body(range, threadIndex)` over [0, count) split into chunks of at
    /// least `minChunk` items, and blocks until all chunks are done. The calling
    /// thread participates. Chunk boundaries depend only on count/minChunk, but
    /// the assignment of chunks to threads is not deterministic, so bodies must
    /// write to disjoint outputs.
    void parallelFor(std::uint32_t count, std::uint32_t minChunk,
                     const std::function<void(IndexRange range, std::uint32_t threadIndex)>& body);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace crowdsim::core
