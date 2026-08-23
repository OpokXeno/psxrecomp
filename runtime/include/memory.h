#ifndef PSXRECOMP_MEMORY_H
#define PSXRECOMP_MEMORY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_MAIN_RAM_RETAIL_SIZE 0x00200000u
#define PSX_MAIN_RAM_DEVELOPER_SIZE 0x00800000u
#define PSX_MAIN_RAM_APERTURE_SIZE PSX_MAIN_RAM_DEVELOPER_SIZE

extern uint8_t *g_psx_ram;
extern uint32_t g_psx_ram_size;
extern uint32_t g_psx_ram_mask;

uint8_t *memory_get_ram_ptr(void);
uint8_t *memory_get_scratchpad_ptr(void);
uint32_t memory_get_ram_size(void);
uint32_t memory_get_ram_mask(void);
uint32_t memory_get_ram_word_mask(void);
int memory_developer_ram_enabled(void);

/* One-way transition for a running developer session. The existing retail
 * 2 MiB are cloned into the other three banks so every value previously
 * observed through a hardware mirror remains unchanged after the switch. */
int memory_enable_developer_ram(void);

/* Translate an address inside the physical 8 MiB main-RAM aperture according
 * to the active profile. Callers must first establish phys < 0x00800000. */
static inline uint32_t memory_main_ram_offset(uint32_t phys)
{
    return phys & g_psx_ram_mask;
}

static inline uint32_t memory_main_ram_word_offset(uint32_t address)
{
    return address & (g_psx_ram_mask & ~3u);
}

#ifdef __cplusplus
}
#endif

#endif
