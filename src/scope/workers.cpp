#include "scope/workers.hpp"

#include <algorithm>
#include <utility>

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
    // An exception in any part is rethrown here, after every helper has finished its part.
    std::exception_ptr error;
    try
    {
        task(0);
    }
    catch (...)
    {
        error = std::current_exception();
    }
    std::unique_lock lock(mu_);
    done_.wait(lock, [this] { return pending_ == 0; });
    task_ = nullptr;
    if (!error)
    {
        error = std::exchange(helperError_, nullptr);
    }
    helperError_ = nullptr;
    lock.unlock();
    if (error)
    {
        std::rethrow_exception(error);
    }
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
        std::exception_ptr error;
        try
        {
            (*task)(part);
        }
        catch (...)
        {
            error = std::current_exception();
        }
        {
            std::lock_guard lock(mu_);
            if (error && !helperError_)
            {
                helperError_ = error;
            }
            if (--pending_ == 0)
            {
                done_.notify_one();
            }
        }
    }
}
} // namespace scopes
