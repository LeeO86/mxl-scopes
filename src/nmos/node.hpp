#pragma once

#include <functional>
#include <memory>
#include <string>

#include "app/state.hpp"
#include "config/config.hpp"
#include "mxlio/io.hpp"

namespace scopes
{
// One NMOS node (nmos-cpp, SCOPES_WITH_NMOS): one video receiver, one video sender with its
// source and flow (SPECIFICATION §6, §7.1), BCP-007-03 MXL transport.
class NmosNode
{
public:
    using RouteFn = std::function<void(Route const& route)>;

    NmosNode(Config config, RouteFn route);
    ~NmosNode();

    void start();
    void stop();
    // True when the Query API knows the node (or no registry is set).
    [[nodiscard]] bool registered() const;
    // A new output flow (start, rate change): the flow resource, the sender's flow_id and its
    // IS-05 transport parameters follow. Safe to call before start().
    void setOutputFlow(OutputFlow const& flow, std::string const& domainId);
    // Shows a route restored at start in the receiver's IS-05 active and staged documents and
    // its IS-04 subscription. Does not call the route callback.
    void restoreRoute(Route const& route);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace scopes
