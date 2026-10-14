#pragma once

#include <cstdint>

namespace scopes
{
// When the renderer updates (SPECIFICATION §4.6). Ticks run on TAI: tick k is at k / rate.
// At a tick the newest grain is analysed, never one twice. `frame` (rate 0), or a rate at or
// above the input rate, updates on every new grain. A tick that passes while an update runs
// is skipped and counted.
class UpdateScheduler
{
public:
    // `rateHz` 0 is `frame`; `inputHz` is the routed flow's grain rate (0 unknown).
    void configure(double rateHz, double inputHz);

    struct Decision
    {
        bool update = false;
        // Ticks (or grains, in frame mode) that passed without an update.
        std::uint64_t skipped = 0;
    };
    // `newest` is the index of the newest complete grain (or none when `hasGrain` is false).
    Decision poll(std::uint64_t nowNs, bool hasGrain, std::uint64_t newest);
    // TAI time of the next tick (frame mode: 0, wait for the next grain).
    [[nodiscard]] std::uint64_t nextTickNs() const;
    [[nodiscard]] bool frameMode() const
    {
        return frame_;
    }
    // The effective update rate for the metrics: the tick rate, or the input rate in frame mode.
    [[nodiscard]] double effectiveHz() const;
    // Forget the analysed index (a new route or flow restarts the indexes).
    void resetInput();

private:
    double rate_ = 25;
    double inputHz_ = 0;
    bool frame_ = false;
    bool started_ = false;
    std::uint64_t nextTick_ = 0;
    bool analysed_ = false;
    std::uint64_t lastIndex_ = 0;
};
} // namespace scopes
