#include "scope/cpu/dispatch.hpp"

#include "scope/analyse.hpp"

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

namespace scopes
{
char const* simdName(SimdLevel level)
{
    return level == SimdLevel::Avx2 ? "avx2" : "scalar";
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
    bool const osxsave = (ecx & (1u << 27)) != 0;
    bool const avx = (ecx & (1u << 28)) != 0;
    bool const fma = (ecx & (1u << 12)) != 0;
    if (!osxsave || !avx || !fma)
    {
        return false;
    }
    unsigned xcr0Low = 0;
    unsigned xcr0High = 0;
    __asm__ volatile("xgetbv" : "=a"(xcr0Low), "=d"(xcr0High) : "c"(0));
    if ((xcr0Low & 0x6u) != 0x6u)
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

SimdChoice chooseSimd(std::string const& requested)
{
    SimdChoice choice;
    choice.requested = requested;
    bool const avx2 = avx2Compiled() && cpuHasAvx2();
    choice.cpu = avx2 ? "avx2" : "scalar";
    if (requested == "scalar")
    {
        choice.level = SimdLevel::Scalar;
    }
    else if (avx2)
    {
        choice.level = SimdLevel::Avx2;
    }
    else
    {
        choice.level = SimdLevel::Scalar;
        choice.fellBack = requested == "avx2";
    }
    return choice;
}
} // namespace scopes
