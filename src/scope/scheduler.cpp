#include "scope/scheduler.hpp"

#include <cmath>

namespace scopes
{
namespace
{
std::uint64_t tickOf(std::uint64_t nowNs, double rate)
{
    // floor(now × rate / 1e9) in long double: exact enough for TAI ns and rates up to 60 Hz.
    return static_cast<std::uint64_t>(std::floor(static_cast<long double>(nowNs) * static_cast<long double>(rate) / 1e9L));
}

std::uint64_t tickTime(std::uint64_t tick, double rate)
{
    return static_cast<std::uint64_t>(std::ceil(static_cast<long double>(tick) * 1e9L / static_cast<long double>(rate)));
}
} // namespace

void UpdateScheduler::configure(double rateHz, double inputHz)
{
    bool const frame = rateHz <= 0 || (inputHz > 0 && rateHz >= inputHz);
    if (frame != frame_ || rateHz != rate_)
    {
        started_ = false;
    }
    rate_ = rateHz;
    inputHz_ = inputHz;
    frame_ = frame;
}

void UpdateScheduler::resetInput()
{
    analysed_ = false;
}

double UpdateScheduler::effectiveHz() const
{
    return frame_ ? inputHz_ : rate_;
}

std::uint64_t UpdateScheduler::nextTickNs() const
{
    return frame_ || !started_ ? 0 : tickTime(nextTick_, rate_);
}

UpdateScheduler::Decision UpdateScheduler::poll(std::uint64_t nowNs, bool hasGrain, std::uint64_t newest)
{
    Decision decision;
    if (frame_)
    {
        if (!hasGrain || (analysed_ && newest <= lastIndex_))
        {
            return decision;
        }
        if (analysed_ && newest > lastIndex_ + 1)
        {
            decision.skipped = newest - lastIndex_ - 1;
        }
        decision.update = true;
        analysed_ = true;
        lastIndex_ = newest;
        return decision;
    }
    auto const now = tickOf(nowNs, rate_);
    if (!started_)
    {
        started_ = true;
        nextTick_ = now;
    }
    if (now < nextTick_)
    {
        return decision;
    }
    decision.skipped = now - nextTick_;
    nextTick_ = now + 1;
    if (hasGrain && (!analysed_ || newest > lastIndex_))
    {
        decision.update = true;
        analysed_ = true;
        lastIndex_ = newest;
    }
    return decision;
}
} // namespace scopes
