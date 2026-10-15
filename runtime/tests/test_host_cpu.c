#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "host_cpu.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

static void check_workers(unsigned cpus)
{
    assert(psx_host_worker_count(123, 0) == 0);
    assert(psx_host_worker_count(123, 1) == 1);
    assert(psx_host_worker_count(123, UINT_MAX) == cpus);
    assert(psx_host_worker_count(0, UINT_MAX) == cpus);
    for (unsigned jobs = 1; jobs <= cpus + 1; ++jobs)
        assert(psx_host_worker_count(123, jobs) == (jobs < cpus ? jobs : cpus));
}

int main(void)
{
#if defined(_WIN32)
    DWORD_PTR original, system;
    assert(GetProcessAffinityMask(GetCurrentProcess(), &original, &system));
    assert(original);
    DWORD_PTR limited = 0, rest = original;
    unsigned count = 0;
    while (rest) {
        DWORD_PTR one = rest & (~rest + 1);
        rest &= ~one;
        limited |= one;
        ++count;
        assert(SetProcessAffinityMask(GetCurrentProcess(), limited));
        assert(psx_host_available_cpu_count(123) == count);
        check_workers(count);
    }
    assert(SetProcessAffinityMask(GetCurrentProcess(), original));
#elif defined(__linux__)
    cpu_set_t original, limited;
    assert(sched_getaffinity(0, sizeof(original), &original) == 0);
    CPU_ZERO(&limited);
    unsigned count = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &original)) continue;
        CPU_SET(cpu, &limited);
        ++count;
        assert(sched_setaffinity(0, sizeof(limited), &limited) == 0);
        assert(psx_host_available_cpu_count(123) == count);
        check_workers(count);
    }
    assert(count);
    assert(sched_setaffinity(0, sizeof(original), &original) == 0);
#else
    assert(psx_host_available_cpu_count(7) == 7);
    assert(psx_host_worker_count(0, 3) == 1);
    assert(psx_host_worker_count(24, 100) == 24);
    assert(psx_host_worker_count(24, 5) == 5);
#endif
    puts("host CPU affinity: OK");
    return 0;
}
