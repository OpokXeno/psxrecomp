#ifndef PSXRECOMP_GTE_NATIVE_PROVENANCE_H
#define PSXRECOMP_GTE_NATIVE_PROVENANCE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

typedef struct GteNativeVertexProvenance {
    int32_t x_16_16;
    int32_t y_16_16;
    int32_t view_x;
    int32_t view_y;
    int32_t view_z;
    int32_t projection_offset_x_16_16;
    int32_t projection_offset_y_16_16;
    uint64_t receipt;
    uint32_t packed_sxy;
    uint16_t projection_distance;
    uint16_t depth;
    uint8_t projective_valid;
} GteNativeVertexProvenance;

void gte_native_provenance_set_enabled(int enabled);
/* CPU/RAM dataflow only (GP0 preflight binding); register shadow unaffected. */
void gte_native_provenance_set_dataflow(int enabled);
/* Game integration identifies authenticated rendering-only NCLIP sites.
 * Generic GTE/collision/camera uses retain hardware arithmetic. */
void gte_native_provenance_set_render_nclip_filter(int (*filter)(uint32_t pc));
void gte_native_provenance_invalidate_range(uint32_t address, uint32_t width);
int gte_native_provenance_load(uint32_t address, uint32_t packed_sxy,
                               GteNativeVertexProvenance *out);
void gte_native_provenance_cpu_load(struct CPUState *cpu, uint32_t instruction,
                                    uint32_t address, uint32_t value);
void gte_native_provenance_cpu_store(struct CPUState *cpu, uint32_t instruction,
                                     uint32_t address, uint32_t value);
void gte_native_provenance_cpu_alu(struct CPUState *cpu, uint32_t instruction,
                                   uint32_t result, uint32_t source1,
                                   uint32_t source2);
void gte_native_provenance_cpu_cop2(struct CPUState *cpu, uint32_t instruction,
                                    uint32_t value, uint32_t address);
extern int g_gte_native_provenance_active;

/* Projection tap: while configured, every RTPS/RTPT executed at a guest PC in
 * one of the given [begin, end) ranges records its native projection, keyed
 * by the packed SXY it produced. A game producer can then recover the native
 * position of packets that copy those SXY words unchanged. Lookups accept the
 * current and the previous generation; the producer advances it once per
 * guest frame. count == 0 disables the tap. */
#define GTE_NATIVE_PROJECTION_TAP_MAX_RANGES 16u
void gte_native_projection_tap_configure(const uint32_t (*ranges)[2],
                                         uint32_t count);
void gte_native_projection_tap_advance(void);
int gte_native_projection_tap_lookup(uint32_t packed_sxy,
                                     GteNativeVertexProvenance *out);

#ifdef __cplusplus
}
#endif

#endif
