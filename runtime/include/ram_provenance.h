#ifndef PSX_RAM_PROVENANCE_H
#define PSX_RAM_PROVENANCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum RamProvenanceSourceKind {
    RAM_PROVENANCE_SOURCE_NONE = 0,
    RAM_PROVENANCE_SOURCE_MDEC_DMA1,
} RamProvenanceSourceKind;

typedef struct RamProvenanceSource {
    RamProvenanceSourceKind kind;
    uint32_t format;
    uint64_t receipt;
} RamProvenanceSource;

typedef struct RamProvenanceSnapshot RamProvenanceSnapshot;

bool ram_provenance_init(size_t main_ram_size);
void ram_provenance_reset(void);
void ram_provenance_set_cpu_tracking(bool enabled);
void ram_provenance_invalidate_range(uint32_t address, uint32_t width);
void ram_provenance_note_cpu_store(uint32_t instruction, uint32_t address,
                                   uint32_t value);
bool ram_provenance_cpu_word_receipt(uint32_t address, uint32_t value,
                                     uint64_t *out_receipt);
bool ram_provenance_word_revision(uint32_t address, uint64_t *out_revision);
uint64_t ram_provenance_publish_event(void);
void ram_provenance_note_source_word(
    uint32_t address, const RamProvenanceSource *source);
bool ram_provenance_source_word(
    uint32_t address, RamProvenanceSource *out_source);
void ram_provenance_speculative_begin(void);
void ram_provenance_speculative_end(void);
void ram_provenance_note_command_word(uint32_t address, uint32_t value,
                                      uint32_t writer_pc,
                                      uint32_t return_address);
bool ram_provenance_last_writer(uint32_t address, uint32_t *out_pc,
                                uint32_t *out_return_address);

/* Versioned little-endian wire state. Only words carrying authority metadata
 * are emitted; omitted words are authoritatively empty on restore. */
uint32_t ram_provenance_snapshot_bytes(void);
/* Guest-owner serialization; consume output only on success. Failure may leave
 * a partial wire buffer, but never changes live provenance or its revision. */
bool ram_provenance_snapshot_write(uint8_t *out, uint32_t size);
/* Upper bound and one-pass capture for an owned growable sink. Same wire format;
 * out_size is the actual validated payload size, zero on failure. */
uint32_t ram_provenance_snapshot_capacity(void);
bool ram_provenance_snapshot_capture(uint8_t *out, uint32_t capacity,
                                     uint32_t *out_size);
/* Copy on the guest owner; encode may then run on a worker. The owned copy
 * contains no guest pointers. Encoding retains all wire validation. */
/* Consumes reuse (including on failure); it must no longer have any readers. */
RamProvenanceSnapshot *ram_provenance_snapshot_clone(RamProvenanceSnapshot *reuse);
RamProvenanceSnapshot *ram_provenance_snapshot_prepare(void);
bool ram_provenance_snapshot_encode(const RamProvenanceSnapshot *snapshot,
                                    uint8_t *out, uint32_t capacity,
                                    uint32_t *out_size);
bool ram_provenance_snapshot_decode(const uint8_t *data, uint32_t size,
                                    size_t expected_ram_size,
                                    RamProvenanceSnapshot **out_snapshot);
void ram_provenance_snapshot_commit(RamProvenanceSnapshot *snapshot);
void ram_provenance_snapshot_free(RamProvenanceSnapshot *snapshot);

#ifdef __cplusplus
}
#endif

#endif
