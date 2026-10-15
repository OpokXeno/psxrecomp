#ifndef PSX_HOST_CPU_H
#define PSX_HOST_CPU_H

#ifdef __cplusplus
extern "C" {
#endif

/* Logical CPUs allowed by the OS affinity mask; fallback if unavailable. */
unsigned psx_host_available_cpu_count(unsigned fallback);

#ifdef __cplusplus
}
#endif
#endif
