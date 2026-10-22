#pragma once

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace scopes
{
// The strip workers of SPECIFICATION §2.1: `threads` − 1 helper threads that run one part of a
// step each while the calling thread (the renderer) runs part 0, then wait for the next step.
// `run` returns when every part is done. The helpers sleep between steps.
class WorkerPool
{
public:
    explicit WorkerPool(int threads);
    ~WorkerPool();
    WorkerPool(WorkerPool const&) = delete;
    WorkerPool& operator=(WorkerPool const&) = delete;

    [[nodiscard]] int threads() const
    {
        return threads_;
    }
    // Calls task(k) for k = 0 .. threads() − 1, part 0 on the calling thread, and waits for all.
    // An exception of any part is rethrown after all parts are done (the first one).
    void run(std::function<void(int)> const& task);

private:
    void loop(int part);

    int threads_ = 1;
    std::mutex mu_;
    std::condition_variable start_;
    std::condition_variable done_;
    std::uint64_t generation_ = 0;
    int pending_ = 0;
    bool stop_ = false;
    std::function<void(int)> const* task_ = nullptr;
    std::exception_ptr helperError_;
    std::vector<std::thread> helpers_;
};
} // namespace scopes
