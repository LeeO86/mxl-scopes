#include "scope/cpu/dispatch.hpp"

#include "scope/analyse.hpp"

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

namespace scopes
{
namespace
{
#if defined(__x86_64__) || defined(__i386__)
unsigned long long xcr0()
{
    unsigned eax = 0;
    unsigned ebx = 0;
    unsigned ecx = 0;
    unsigned edx = 0;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) == 0 || (ecx & (1u << 27)) == 0)
    {
        return 0; // no OSXSAVE
    }
    unsigned low = 0;
    unsigned high = 0;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    return (static_cast<unsigned long long>(high) << 32) | low;
}
#endif
} // namespace

char const* simdName(SimdLevel level)
{
    switch (level)
    {
    case SimdLevel::Avx512:
        return "avx512";
    case SimdLevel::Avx2:
        return "avx2";
    default:
        return "scalar";
    }
}

bool cpuHasAvx2()
{
#if defined(__x86_64__) || defined(__i386__)
    unsigned eax = 0;
    unsigned ebx = 0;
    unsigned ecx = 0;
    unsigned edx = 0;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) == 0)
    {
        return false;
    }
    bool const avx = (ecx & (1u << 28)) != 0;
    bool const fma = (ecx & (1u << 12)) != 0;
    if (!avx || !fma || (xcr0() & 0x6u) != 0x6u)
    {
        return false; // the OS does not save the XMM and YMM state
    }
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) == 0)
    {
        return false;
    }
    return (ebx & (1u << 5)) != 0;
#else
    return false;
#endif
}

bool cpuHasAvx512()
{
#if defined(__x86_64__) || defined(__i386__)
    if (!cpuHasAvx2())
    {
        return false;
    }
    unsigned eax = 0;
    unsigned ebx = 0;
    unsigned ecx = 0;
    unsigned edx = 0;
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) == 0 || (ebx & (1u << 16)) == 0)
    {
        return false;
    }
    // XMM, YMM, opmask, ZMM0-15 upper halves and ZMM16-31.
    return (xcr0() & 0xe6u) == 0xe6u;
#else
    return false;
#endif
}

bool cpuIsSkylakeServer()
{
#if defined(__x86_64__) || defined(__i386__)
    unsigned eax = 0;
    unsigned ebx = 0;
    unsigned ecx = 0;
    unsigned edx = 0;
    if (__get_cpuid(0, &eax, &ebx, &ecx, &edx) == 0 || ebx != 0x756e6547u || edx != 0x49656e69u || ecx != 0x6c65746eu)
    {
        return false; // not GenuineIntel
    }
    __get_cpuid(1, &eax, &ebx, &ecx, &edx);
    unsigned const family = (eax >> 8) & 0xfu;
    unsigned const model = ((eax >> 4) & 0xfu) | (((eax >> 16) & 0xfu) << 4);
    return family == 6 && model == 0x55;
#else
    return false;
#endif
}

SimdChoice chooseSimd(std::string const& requested)
{
    SimdChoice choice;
    choice.requested = requested;
    bool const avx2 = avx2Compiled() && cpuHasAvx2();
    bool const avx512 = avx512Compiled() && cpuHasAvx512();
    choice.cpu = avx512 ? "avx512" : avx2 ? "avx2" : "scalar";
    choice.family = cpuIsSkylakeServer() ? "skylake-sp" : "";
    if (requested == "scalar")
    {
        choice.level = SimdLevel::Scalar;
    }
    else if (requested == "avx512" && avx512)
    {
        choice.level = SimdLevel::Avx512;
    }
    else if (avx2)
    {
        choice.level = SimdLevel::Avx2;
        choice.fellBack = requested == "avx512";
    }
    else
    {
        choice.level = SimdLevel::Scalar;
        choice.fellBack = requested == "avx512" || requested == "avx2";
    }
    return choice;
}
} // namespace scopes
