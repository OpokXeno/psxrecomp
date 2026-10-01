#ifndef PSX_GTE_PRECISION_STATE_H
#define PSX_GTE_PRECISION_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Portable, sparse visual state: Native projection provenance, PGXP dataflow
 * and the geometry fallback cache. Runtime user settings are not serialized.
 * Prepare validates and allocates without changing the live timeline; commit
 * replaces its precision caches after guest RAM/registers have been restored. */
typedef struct GtePrecisionSnapshot GtePrecisionSnapshot;
uint32_t gte_precision_snapshot_bytes(void);
int gte_precision_snapshot_write(uint8_t *out, uint32_t size);
int gte_precision_snapshot_prepare(const uint8_t *data, uint32_t size,
                                   GtePrecisionSnapshot **out);
void gte_precision_snapshot_commit(GtePrecisionSnapshot *snapshot);
void gte_precision_snapshot_cancel(GtePrecisionSnapshot *snapshot);

#ifdef __cplusplus
}
#endif

#endif
