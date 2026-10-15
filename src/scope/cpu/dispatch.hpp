#pragma once

#include <string>

namespace scopes
{
enum class SimdLevel
{
    Scalar,
    Avx2,
    Avx512
};

char const* simdName(SimdLevel level);

struct SimdChoice
{
    SimdLevel level = SimdLevel::Scalar;
    std::string requested = "auto";
    // The best level the CPU (and this build) offers: "avx512", "avx2" or "scalar".
    std::string cpu = "scalar";
    // "skylake-sp" for Skylake-SP and Cascade Lake (Intel family 6, model 0x55), else empty.
    std::string family;
    // True when the requested level was missing and the next one below was taken.
    bool fellBack = false;
};

// cpuid and XGETBV: AVX2 with FMA and the OS saving the YMM state.
bool cpuHasAvx2();
// AVX-512F with the OS saving the opmask and ZMM state.
bool cpuHasAvx512();
// Skylake-SP and Cascade Lake: 512-bit code lowers the core clock for the other hyperthread too.
bool cpuIsSkylakeServer();
// SCOPES_SIMD (§3.2): auto, avx512, avx2, scalar. `auto` takes AVX-512 where it costs nothing
// (not on Skylake-SP and Cascade Lake, as the sibling functions), else AVX2, else scalar.
// A missing level falls back to the next one below with fellBack set.
SimdChoice chooseSimd(std::string const& requested);
} // namespace scopes
