#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace scopes
{
using Labels = std::vector<std::pair<std::string, std::string>>;

// Prometheus text exposition with the prefix mxl_scopes_ (SPECIFICATION §10).
class Metrics
{
public:
    void inc(std::string const& name, Labels const& labels, double value = 1);
    void set(std::string const& name, Labels const& labels, double value);
    // Drops a series (a gauge without a value, such as a measurement the mode does not have).
    void remove(std::string const& name, Labels const& labels);
    // Histograms use the buckets of §10: 0.5, 1, 2, 4, 8, 16, 32 ms.
    void observe(std::string const& name, Labels const& labels, double seconds);
    [[nodiscard]] double value(std::string const& name, Labels const& labels) const;
    [[nodiscard]] std::string render() const;
    // The standard process metrics without the prefix (as the sibling functions export them):
    // process_cpu_seconds_total, process_resident_memory_bytes, process_start_time_seconds,
    // process_open_fds; from /proc/self.
    [[nodiscard]] static std::string renderProcess();

private:
    struct Series
    {
        double value = 0;
        bool gauge = false;
    };
    struct Hist
    {
        std::uint64_t count = 0;
        double sum = 0;
        std::vector<std::uint64_t> buckets;
    };
    static std::string key(std::string const& name, Labels const& labels);

    mutable std::mutex mutex_;
    std::map<std::string, Series> series_;
    std::map<std::string, Hist> histograms_;
    std::vector<double> bounds_{0.0005, 0.001, 0.002, 0.004, 0.008, 0.016, 0.032};
};
} // namespace scopes
