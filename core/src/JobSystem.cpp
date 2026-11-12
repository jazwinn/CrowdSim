#include "crowdsim/core/JobSystem.hpp"

#include <TaskScheduler.h>

#include <algorithm>
#include <thread>

namespace crowdsim::core {

namespace {

class FunctionTaskSet final : public enki::ITaskSet {
public:
    FunctionTaskSet(std::uint32_t count, std::uint32_t minChunk,
                    const std::function<void(IndexRange, std::uint32_t)>& body)
        : enki::ITaskSet(count, std::max<std::uint32_t>(1, minChunk)), body_(body) {}

    void ExecuteRange(enki::TaskSetPartition range, std::uint32_t threadnum) override {
        body_(IndexRange{range.start, range.end}, threadnum);
    }

private:
    const std::function<void(IndexRange, std::uint32_t)>& body_;
};

}  // namespace

struct JobSystem::Impl {
    enki::TaskScheduler scheduler;
};

JobSystem::JobSystem(std::uint32_t threadCount) : impl_(std::make_unique<Impl>()) {
    enki::TaskSchedulerConfig config;
    const std::uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
    config.numTaskThreadsToCreate = (threadCount == 0 ? hw : threadCount) - 1;
    impl_->scheduler.Initialize(config);
}

JobSystem::~JobSystem() {
    impl_->scheduler.WaitforAllAndShutdown();
}

std::uint32_t JobSystem::threadCount() const noexcept {
    return impl_->scheduler.GetNumTaskThreads();
}

void JobSystem::parallelFor(std::uint32_t count, std::uint32_t minChunk,
                            const std::function<void(IndexRange, std::uint32_t)>& body) {
    if (count == 0) {
        return;
    }
    if (count <= minChunk || threadCount() <= 1) {
        body(IndexRange{0, count}, 0);
        return;
    }
    FunctionTaskSet task(count, minChunk, body);
    impl_->scheduler.AddTaskSetToPipe(&task);
    impl_->scheduler.WaitforTask(&task);
}

}  // namespace crowdsim::core
