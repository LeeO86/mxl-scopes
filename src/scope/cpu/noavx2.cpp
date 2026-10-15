// Built instead of avx2.cpp and avx512.cpp on a compiler or target without them: the scalar
// reference only.
#include "scope/analyse.hpp"

namespace scopes
{
bool avx2Compiled()
{
    return false;
}

void analyseAvx2(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist)
{
    analyseScalar(plan, v210, hist);
}

bool avx512Compiled()
{
    return false;
}

void analyseAvx512(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist)
{
    analyseScalar(plan, v210, hist);
}
} // namespace scopes
