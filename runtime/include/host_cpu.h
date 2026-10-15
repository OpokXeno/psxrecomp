#ifndef PSX_HOST_CPU_H
#define PSX_HOST_CPU_H

#ifdef __cplusplus
extern "C" {
#endif

/* Logical CPUs allowed by the OS affinity mask; fallback if unavailable. */
unsigned psx_host_available_cpu_count(unsigned fallback);

/* One worker per allowed logical CPU, bounded only by independent work items.
 * Empty work needs no workers; a missing CPU count still permits serial work. */
unsigned psx_host_worker_count(unsigned fallback, unsigned work_items);

#ifdef __cplusplus
}
#endif
#endif
