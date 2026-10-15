#ifndef PSX_HOST_PARALLEL_H
#define PSX_HOST_PARALLEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Synchronous, disjoint ranges. Inputs must stay immutable until return and
 * callbacks must not throw. Nested/concurrent calls share one CPU-sized pool;
 * callers help execute work, including nested work, instead of blocking it. */
typedef void (*PsxParallelRange)(uint32_t begin, uint32_t end, void *context);
void psx_parallel_for(uint32_t count, uint32_t grain,
                      PsxParallelRange function, void *context);

#ifdef __cplusplus
}
#endif
#endif
