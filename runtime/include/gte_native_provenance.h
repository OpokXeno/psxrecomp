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

#ifdef __cplusplus
}
#endif

#endif
