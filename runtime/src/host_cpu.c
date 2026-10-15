#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "host_cpu.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

unsigned psx_host_available_cpu_count(unsigned fallback)
{
#if defined(_WIN32)
    DWORD_PTR process_mask = 0, system_mask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) &&
        process_mask) {
        unsigned count = 0;
        do {
            process_mask &= process_mask - 1;
            ++count;
        } while (process_mask);
        return count;
    }
    /* Windows may return zero for processes spanning processor groups. */
#elif defined(__linux__)
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) == 0) {
        unsigned count = (unsigned)CPU_COUNT(&allowed);
        if (count) return count;
    }
#endif
    return fallback;
}
