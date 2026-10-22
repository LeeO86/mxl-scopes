#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "scope/settings.hpp"

namespace scopes
{
// The one input route (SPECIFICATION §6), from IS-05 or routes.json.
struct Route
{
    bool enable = false;
    std::string domainId;
    std::string flowId;
    std::string senderId;
    [[nodiscard]] bool operator==(Route const&) const = default;
};

struct InputView
{
    // not_routed, waiting, running, holding, no_signal, format_mismatch (§10).
    std::string state = "not_routed";
    std::string reason;
    Route route;
    std::string label;
    std::string mediaType;
    int width = 0;
    int height = 0;
    int rateNum = 0;
    int rateDen = 1;
    bool interlaced = false;
    std::string colorimetry;
    bool colorimetryWarning = false;
    std::uint64_t grains = 0;
    std::uint64_t resyncs = 0;
    double latencyMs = 0;
};

struct OutputView
{
    std::string flowId;
    std::string domainId;
    int width = 0;
    int height = 0;
    int rateNum = 0;
    int rateDen = 1;
    bool key = false;
    bool writing = false;
    std::uint64_t frames = 0;
    std::uint64_t late = 0;
    std::uint64_t missed = 0;
    // The mode of the picture on the output (a change shows at the next update).
    std::string mode;
};

struct RendererView
{
    std::uint64_t updates = 0;
    std::uint64_t skipped = 0;
    std::uint64_t torn = 0;
    double updateHz = 0;
    double analyseMs = 0;
    // Thread CPU time of the same analyse step: far below analyseMs means the thread waited for
    // the core (another thread on it), equal means the core itself was slow.
    double analyseCpuMs = 0;
    double renderMs = 0;
    std::uint64_t lastIndex = 0;
    // The line step in use (SCOPES_LINE_STEP, raised when the counts would not fit 16 bits).
    int lineStep = 1;
};

// The latest scope picture for GET /api/v1/snapshot.png (packed v210).
struct Snapshot
{
    int width = 0;
    int height = 0;
    std::uint64_t update = 0;
    std::vector<std::uint8_t> v210;
};

// State shared by the media threads, the API and the NMOS node. Each thread writes its own part.
class AppState
{
public:
    explicit AppState(DisplaySettings settings);

    [[nodiscard]] std::shared_ptr<DisplaySettings const> settings() const;
    void setSettings(DisplaySettings settings);

    [[nodiscard]] Route route() const;
    // Applies and persists the route (IS-05 or the restored routes.json).
    void setRoute(Route route);

    [[nodiscard]] InputView input() const;
    void setInput(InputView view);
    [[nodiscard]] OutputView output() const;
    void setOutput(OutputView view);
    [[nodiscard]] RendererView renderer() const;
    void setRenderer(RendererView view);

    void setSnapshot(std::shared_ptr<Snapshot const> snapshot);
    [[nodiscard]] std::shared_ptr<Snapshot const> snapshot() const;

    void beatRenderer(std::uint64_t taiNs);
    void beatWriter(std::uint64_t taiNs);
    [[nodiscard]] std::uint64_t rendererBeat() const;
    [[nodiscard]] std::uint64_t writerBeat() const;
    void setNmosUp(bool up);
    [[nodiscard]] bool nmosUp() const;

private:
    mutable std::mutex mutex_;
    std::shared_ptr<DisplaySettings const> settings_;
    Route route_;
    InputView input_;
    OutputView output_;
    RendererView renderer_;
    std::shared_ptr<Snapshot const> snapshot_;
    std::uint64_t rendererBeat_ = 0;
    std::uint64_t writerBeat_ = 0;
    bool nmosUp_ = false;
};

// routes.json in the state directory (§6): {"routes":[{"input":1,"video":true,...}]}.
std::optional<Route> loadRoute(std::string const& stateDir);
bool saveRoute(std::string const& stateDir, Route const& route);
std::string routeJson(Route const& route);
} // namespace scopes
