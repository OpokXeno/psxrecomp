#include "memory.h"

uint8_t *g_psx_ram = 0;
uint32_t g_psx_ram_size = PSX_MAIN_RAM_RETAIL_SIZE;
uint32_t g_psx_ram_mask = PSX_MAIN_RAM_RETAIL_SIZE - 1u;
