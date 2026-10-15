#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "host_cpu.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

int main(void)
{
#if defined(_WIN32)
    DWORD_PTR original, system;
    assert(GetProcessAffinityMask(GetCurrentProcess(), &original, &system));
    assert(original);
    DWORD_PTR one = original & (~original + 1);
    assert(SetProcessAffinityMask(GetCurrentProcess(), one));
    assert(psx_host_available_cpu_count(123) == 1);
    DWORD_PTR rest = original & ~one;
    if (rest) {
        assert(SetProcessAffinityMask(GetCurrentProcess(), one | (rest & (~rest + 1))));
        assert(psx_host_available_cpu_count(123) == 2);
    }
    assert(SetProcessAffinityMask(GetCurrentProcess(), original));
#elif defined(__linux__)
    cpu_set_t original, limited;
    assert(sched_getaffinity(0, sizeof(original), &original) == 0);
    CPU_ZERO(&limited);
    unsigned count = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE && count < 2; ++cpu) {
        if (!CPU_ISSET(cpu, &original)) continue;
        CPU_SET(cpu, &limited);
        ++count;
        assert(sched_setaffinity(0, sizeof(limited), &limited) == 0);
        assert(psx_host_available_cpu_count(123) == count);
    }
    assert(count);
    assert(sched_setaffinity(0, sizeof(original), &original) == 0);
#else
    assert(psx_host_available_cpu_count(7) == 7);
#endif
    puts("host CPU affinity: OK");
    return 0;
}
