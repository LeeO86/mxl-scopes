#include <doctest/doctest.h>

#include "media/timebase.hpp"
#include "scope/scheduler.hpp"

#include <set>

using namespace scopes;

namespace
{
// Runs `seconds` of input at num/den, polling the scheduler at every grain arrival.
struct Run
{
    std::uint64_t updates = 0;
    std::uint64_t skipped = 0;
    std::set<std::uint64_t> analysed;
    bool twice = false;
};

Run simulate(double rate, int num, int den, int seconds, std::uint64_t slowEvery = 0)
{
    UpdateScheduler scheduler;
    scheduler.configure(rate, static_cast<double>(num) / den);
    Run run;
    std::uint64_t const start = indexToTimestamp(num, den, timestampToIndex(num, den, 1'700'000'000'000'000'000ull));
    std::uint64_t const first = timestampToIndex(num, den, start);
    auto const grains = static_cast<std::uint64_t>(seconds) * static_cast<std::uint64_t>(num) / static_cast<std::uint64_t>(den);
    std::uint64_t busyUntil = 0;
    for (std::uint64_t i = 0; i < grains; ++i)
    {
        auto const index = first + i;
        auto const now = indexToTimestamp(num, den, index) + 1'000'000; // 1 ms after the grain's time
        if (now < busyUntil)
        {
            continue; // the renderer is still busy; it polls again when it is done
        }
        auto const d = scheduler.poll(now, true, index);
        run.skipped += d.skipped;
        if (d.update)
        {
            ++run.updates;
            run.twice = run.twice || !run.analysed.insert(index).second;
            if (slowEvery != 0 && run.updates % slowEvery == 0)
            {
                busyUntil = now + 100'000'000; // one update takes 100 ms
            }
        }
    }
    return run;
}
} // namespace

TEST_CASE("25 Hz from 50p takes every second grain")
{
    auto const run = simulate(25, 50, 1, 10);
    CHECK(run.updates >= 249);
    CHECK(run.updates <= 251);
    CHECK_FALSE(run.twice);
    CHECK(run.skipped == 0);
}

TEST_CASE("10 Hz from 59.94p: never a grain twice, no drift")
{
    auto const run = simulate(10, 60000, 1001, 60);
    CHECK_FALSE(run.twice);
    CHECK(run.updates >= 599);
    CHECK(run.updates <= 601);
}

TEST_CASE("frame analyses every grain once")
{
    auto const run = simulate(0, 50, 1, 4);
    CHECK(run.updates == 200);
    CHECK_FALSE(run.twice);
    auto const same = simulate(60, 50, 1, 4); // at or above the input rate: frame
    CHECK(same.updates == 200);
}

TEST_CASE("a slow update skips ticks and counts them")
{
    auto const run = simulate(25, 50, 1, 10, 10);
    CHECK(run.skipped > 0);
    CHECK(run.updates < 250);
    CHECK_FALSE(run.twice);
}

TEST_CASE("no grain: a tick passes without an update")
{
    UpdateScheduler scheduler;
    scheduler.configure(25, 50);
    CHECK_FALSE(scheduler.poll(1'000'000'000'000ull, false, 0).update);
    CHECK(scheduler.nextTickNs() > 1'000'000'000'000ull);
}
