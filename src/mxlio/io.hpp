#pragma once

#include <functional>
#include <memory>
#include <string>

#include "app/state.hpp"
#include "config/config.hpp"
#include "ops/metrics.hpp"
#include "scope/cpu/dispatch.hpp"

namespace scopes
{
// The output flow as the NMOS node announces it (SPECIFICATION §7.1).
struct OutputFlow
{
    std::string flowId;
    int width = 0;
    int height = 0;
    Rate rate;
    std::string description;
    // The scope mode: the flow's description and tag follow a live mode change (same flow id).
    std::string mode;
};

// The media threads (SPECIFICATION §2.1): the renderer, which follows the routed flow and
// analyses its grains in place, and the TAI-paced output writer. Needs libmxl.
class MediaIo
{
public:
    MediaIo(Config config, AppState& state, Metrics& metrics, SimdLevel level);
    ~MediaIo();
    MediaIo(MediaIo const&) = delete;
    MediaIo& operator=(MediaIo const&) = delete;

    // Creates or opens the output domain (ConfigError for a mirror or a directory that cannot
    // be created) and starts the threads.
    void start();
    void stop();
    // Deletes the output domain directory. Refuses the scan root and mirror domains.
    void removeOwnDomain();
    [[nodiscard]] std::string domainId() const;
    // Called from the writer thread whenever it (re)creates the output flow, and when the mode
    // changes (same flow id, new description).
    void setFlowCallback(std::function<void(OutputFlow const&)> callback);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace scopes
