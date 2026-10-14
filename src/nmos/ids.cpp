#include "nmos/ids.hpp"

#include "util/uuid.hpp"

namespace scopes
{
namespace
{
std::string nameFor(std::string const& seed, std::string const& tail)
{
    return uuidV5(kUuidNamespaceUrl, "mxl-scopes/" + seed + "/" + tail);
}
} // namespace

std::string NmosIds::flow(std::string const& formatToken) const
{
    return nameFor(seed, "out/video/flow/" + formatToken);
}

NmosIds makeNmosIds(std::string const& seed)
{
    NmosIds ids;
    ids.seed = seed;
    ids.node = nameFor(seed, "node");
    ids.device = nameFor(seed, "device");
    ids.domain = nameFor(seed, "domain");
    ids.receiver = nameFor(seed, "in/video");
    ids.source = nameFor(seed, "out/video/source");
    ids.sender = nameFor(seed, "out/video/sender");
    return ids;
}
} // namespace scopes
