#pragma once

#include <cstdint>
#include <string_view>

namespace scopes
{
// When the renderer updates (SPECIFICATION §4.6). Ticks run on TAI: tick k is at k / rate.
// At a tick the newest grain is analysed, never one twice. `frame` (rate 0), or a rate at or
// above the input rate, updates on every new grain. A tick that passes while an update runs
// is skipped and counted.
// The update rate a mode runs at, from the `SCOPES_UPDATE_RATE` setting (0 is `frame`) and the
// input frame rate (0 unknown). `rgb-overlay` updates at most every n-th input frame, n the
// smallest that keeps it at or below 30 Hz: 25 Hz from 50p, 29.97 Hz from 59.94p, every frame
// from 25p. Its render is too slow for every frame at 1080p50 (SPECIFICATION §4.6).
double modeUpdateRate(std::string_view mode, double rateHz, double inputHz);

class UpdateScheduler
{
public:
    // `rateHz` 0 is `frame`; `inputHz` is the routed flow's frame rate (0 unknown). `step` is the
    // index distance of two frames: 2 for an interlaced flow, whose grains are fields.
    void configure(double rateHz, double inputHz, std::uint64_t step = 1);

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
    std::uint64_t step_ = 1;
    std::uint64_t lastIndex_ = 0;
};
} // namespace scopes
