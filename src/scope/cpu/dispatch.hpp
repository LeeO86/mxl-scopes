#pragma once

#include <string>

namespace scopes
{
enum class SimdLevel
{
    Scalar,
    Avx2
};

char const* simdName(SimdLevel level);

struct SimdChoice
{
    SimdLevel level = SimdLevel::Scalar;
    std::string requested = "auto";
    // What the CPU offers, for /api/v1/info: "avx2" or "scalar".
    std::string cpu = "scalar";
    // True when the requested level was missing and the next one below was taken.
    bool fellBack = false;
};

// cpuid and XGETBV (the OS saves the YMM state). AVX-512 is not used in v1 (§3.2).
bool cpuHasAvx2();
// SCOPES_SIMD: auto, avx2, scalar (§3.2). A missing level falls back with fellBack set.
SimdChoice chooseSimd(std::string const& requested);
} // namespace scopes
