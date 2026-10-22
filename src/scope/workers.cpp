#include "scope/workers.hpp"

#include <algorithm>

namespace scopes
{
WorkerPool::WorkerPool(int threads)
    : threads_(std::max(1, threads))
{
    for (int k = 1; k < threads_; ++k)
    {
        helpers_.emplace_back([this, k] { loop(k); });
    }
}

WorkerPool::~WorkerPool()
{
    {
        std::lock_guard lock(mu_);
        stop_ = true;
    }
    start_.notify_all();
    for (auto& helper : helpers_)
    {
        helper.join();
    }
}

void WorkerPool::run(std::function<void(int)> const& task)
{
    if (threads_ == 1)
    {
        task(0);
        return;
    }
    {
        std::lock_guard lock(mu_);
        task_ = &task;
        pending_ = threads_ - 1;
        ++generation_;
    }
    start_.notify_all();
    task(0);
    std::unique_lock lock(mu_);
    done_.wait(lock, [this] { return pending_ == 0; });
    task_ = nullptr;
}

void WorkerPool::loop(int part)
{
    std::uint64_t seen = 0;
    for (;;)
    {
        std::function<void(int)> const* task = nullptr;
        {
            std::unique_lock lock(mu_);
            start_.wait(lock, [&] { return stop_ || generation_ != seen; });
            if (stop_)
            {
                return;
            }
            seen = generation_;
            task = task_;
        }
        (*task)(part);
        {
            std::lock_guard lock(mu_);
            if (--pending_ == 0)
            {
                done_.notify_one();
            }
        }
    }
}
} // namespace scopes
