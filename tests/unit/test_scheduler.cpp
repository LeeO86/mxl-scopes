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

TEST_CASE("interlaced frame mode: one update per field pair, nothing counted as skipped")
{
    // 1080i50: field grains at 50 Hz, frames at 25 Hz; the renderer passes the odd field that
    // completes each frame, so `frame` updates 25 times a second and never re-reads a field.
    UpdateScheduler scheduler;
    scheduler.configure(0, 25, 2);
    std::uint64_t updates = 0;
    std::uint64_t skipped = 0;
    for (std::uint64_t field = 1000; field < 1100; ++field)
    {
        auto const target = field % 2 == 0 ? field - 1 : field;
        auto const d = scheduler.poll(field * 20'000'000ull, true, target);
        updates += d.update ? 1 : 0;
        skipped += d.skipped;
    }
    CHECK(updates == 51); // targets 999, 1001, ..., 1099
    CHECK(skipped == 0);
    CHECK(scheduler.effectiveHz() == 25);
}

TEST_CASE("rgb-overlay updates at most every second frame above 30 Hz")
{
    // 50p: `frame` and any rate above 25 Hz become 25 Hz; lower rates stay.
    CHECK(modeUpdateRate("rgb-overlay", 0, 50) == 25);
    CHECK(modeUpdateRate("rgb-overlay", 50, 50) == 25);
    CHECK(modeUpdateRate("rgb-overlay", 30, 50) == 25);
    CHECK(modeUpdateRate("rgb-overlay", 25, 50) == 25);
    CHECK(modeUpdateRate("rgb-overlay", 10, 50) == 10);
    // 59.94p: 29.97 Hz; 60p: 30 Hz.
    CHECK(modeUpdateRate("rgb-overlay", 0, 60000.0 / 1001) == doctest::Approx(30000.0 / 1001));
    CHECK(modeUpdateRate("rgb-overlay", 0, 60) == 30);
    // 25p, 29.97p and 1080i50 (25 frames/s): every frame.
    CHECK(modeUpdateRate("rgb-overlay", 0, 25) == 0);
    CHECK(modeUpdateRate("rgb-overlay", 0, 30000.0 / 1001) == 0);
    // Unknown input rate, and the other modes: the setting.
    CHECK(modeUpdateRate("rgb-overlay", 0, 0) == 0);
    for (auto const* mode : {"luma", "ycbcr-parade", "rgb-parade"})
    {
        CHECK(modeUpdateRate(mode, 0, 50) == 0);
        CHECK(modeUpdateRate(mode, 50, 50) == 50);
    }
}

TEST_CASE("rgb-overlay `frame` from 50p and 59.94p takes every second grain")
{
    auto const run = simulate(modeUpdateRate("rgb-overlay", 0, 50), 50, 1, 10);
    CHECK(run.updates >= 249);
    CHECK(run.updates <= 251);
    CHECK_FALSE(run.twice);
    CHECK(run.skipped == 0);
    auto const ntsc = simulate(modeUpdateRate("rgb-overlay", 0, 60000.0 / 1001), 60000, 1001, 10);
    CHECK(ntsc.updates >= 299);
    CHECK(ntsc.updates <= 301);
    CHECK_FALSE(ntsc.twice);
    CHECK(ntsc.skipped == 0);
}
