// Built instead of avx2.cpp on a compiler or target without AVX2: the scalar reference only.
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
} // namespace scopes
