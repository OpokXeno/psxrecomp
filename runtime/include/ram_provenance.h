#ifndef PSX_RAM_PROVENANCE_H
#define PSX_RAM_PROVENANCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
void ram_provenance_speculative_begin(void);
void ram_provenance_speculative_end(void);
void ram_provenance_note_command_word(uint32_t address, uint32_t value,
                                      uint32_t writer_pc,
                                      uint32_t return_address);
bool ram_provenance_last_writer(uint32_t address, uint32_t *out_pc,
                                uint32_t *out_return_address);

#ifdef __cplusplus
}
#endif

#endif
