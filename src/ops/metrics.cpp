#include "ops/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

namespace scopes
{
namespace
{
constexpr char kPrefix[] = "mxl_scopes_";

std::string labelText(Labels const& labels)
{
    if (labels.empty())
    {
        return {};
    }
    std::ostringstream out;
    out << '{';
    for (std::size_t i = 0; i < labels.size(); ++i)
    {
        out << (i != 0 ? "," : "") << labels[i].first << "=\"" << labels[i].second << '"';
    }
    out << '}';
    return out.str();
}

// Integral values (every counter) as integers, others with full precision: the default six
// digits printed 1234567 as 1.23457e+06.
std::string number(double value)
{
    if (std::isfinite(value) && std::floor(value) == value && std::fabs(value) < 9.0e15)
    {
        return std::to_string(static_cast<long long>(value));
    }
    std::ostringstream out;
    out.precision(17);
    out << value;
    return out.str();
}

std::string baseName(std::string const& key)
{
    return key.substr(0, key.find('{'));
}

// The label part of a key without its braces, or empty.
std::string innerLabels(std::string const& key)
{
    auto const brace = key.find('{');
    return brace == std::string::npos ? std::string{} : key.substr(brace + 1, key.size() - brace - 2);
}
} // namespace

std::string Metrics::key(std::string const& name, Labels const& labels)
{
    auto sorted = labels;
    std::sort(sorted.begin(), sorted.end());
    return name + labelText(sorted);
}

void Metrics::inc(std::string const& name, Labels const& labels, double value)
{
    std::lock_guard lock{mutex_};
    series_[key(name, labels)].value += value;
}

void Metrics::set(std::string const& name, Labels const& labels, double value)
{
    std::lock_guard lock{mutex_};
    auto& series = series_[key(name, labels)];
    series.gauge = true;
    series.value = value;
}

void Metrics::observe(std::string const& name, Labels const& labels, double seconds)
{
    std::lock_guard lock{mutex_};
    auto& hist = histograms_[key(name, labels)];
    if (hist.buckets.empty())
    {
        hist.buckets.assign(bounds_.size(), 0);
    }
    ++hist.count;
    hist.sum += seconds;
    // Each bucket counts the observations at or below its bound (cumulative "le").
    for (std::size_t i = 0; i < bounds_.size(); ++i)
    {
        if (seconds <= bounds_[i])
        {
            ++hist.buckets[i];
        }
    }
}

double Metrics::value(std::string const& name, Labels const& labels) const
{
    std::lock_guard lock{mutex_};
    auto const it = series_.find(key(name, labels));
    return it == series_.end() ? 0 : it->second.value;
}

std::string Metrics::render() const
{
    std::lock_guard lock{mutex_};
    std::ostringstream out;
    std::set<std::string> typed;
    for (auto const& [name, series] : series_)
    {
        auto const base = baseName(name);
        if (typed.insert(base).second)
        {
            out << "# TYPE " << kPrefix << base << ' ' << (series.gauge ? "gauge" : "counter") << '\n';
        }
        out << kPrefix << name << ' ' << number(series.value) << '\n';
    }
    for (auto const& [name, hist] : histograms_)
    {
        auto const base = baseName(name);
        auto const labels = innerLabels(name);
        if (typed.insert(base).second)
        {
            out << "# TYPE " << kPrefix << base << " histogram\n";
        }
        auto const sep = labels.empty() ? std::string{} : "," + labels;
        for (std::size_t i = 0; i < bounds_.size(); ++i)
        {
            out << kPrefix << base << "_bucket{le=\"" << bounds_[i] << '"' << sep << "} " << hist.buckets[i] << '\n';
        }
        out << kPrefix << base << "_bucket{le=\"+Inf\"" << sep << "} " << hist.count << '\n';
        auto const braces = labels.empty() ? std::string{} : "{" + labels + "}";
        out << kPrefix << base << "_sum" << braces << ' ' << number(hist.sum) << '\n';
        out << kPrefix << base << "_count" << braces << ' ' << hist.count << '\n';
    }
    return out.str();
}
} // namespace scopes
