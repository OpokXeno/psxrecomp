#include "mod_memory.h"

#include <stdio.h>

uint32_t g_psx_ram_mask = PSX_MAIN_RAM_RETAIL_SIZE - 1u;

static int failures;

static void check(int condition, const char *name) {
    if (condition) {
        printf("PASS  %s\n", name);
    } else {
        fprintf(stderr, "FAIL  %s\n", name);
        failures++;
    }
}

int main(void) {
    uint32_t off = 0;

    check(psx_mod_gpu_dma_resolve_address_for(0x00F01234u, 0u) ==
              (0x00F01234u & 0x001FFFFCu),
          "unallocated aperture preserves retail 2 MiB folding");
    check(psx_mod_gpu_dma_resolve_address_for(0x80F01234u, 0x20000u) ==
              0x00F01234u,
          "allocated KSEG0 aperture pointer survives 24-bit DMA");
    check(psx_mod_gpu_dma_resolve_address_for(0x00F30000u, 0x20000u) ==
              (0x00F30000u & 0x001FFFFCu),
          "unallocated aperture tail remains folded");
    check(psx_mod_gpu_dma_resolve_address_for(0x001ABCDEu, 0x20000u) ==
              0x001ABCDCu,
          "ordinary main RAM remains word-aligned and unchanged");
    check(psx_mod_gpu_dma_resolve_address_for(0x002ABCDEu, 0x20000u) ==
              0x000ABCDCu,
          "retail profile folds upper main-RAM aperture addresses");

    g_psx_ram_mask = PSX_MAIN_RAM_DEVELOPER_SIZE - 1u;
    check(psx_mod_gpu_dma_resolve_address_for(0x002ABCDEu, 0x20000u) ==
              0x002ABCDCu,
          "developer profile preserves upper main-RAM address identity");
    check(psx_mod_gpu_dma_resolve_address_for(0x00AABCDEu, 0x20000u) ==
              0x002ABCDCu,
          "developer profile folds only beyond the 8 MiB aperture");
    check(psx_mod_gpu_dma_aperture_offset_for(
              0x00F1FFFCu, 4u, 0x20000u, &off) &&
              off == 0x1FFFCu,
          "last allocated aperture word is accessible");
    check(!psx_mod_gpu_dma_aperture_offset_for(
              0x00F20000u, 4u, 0x20000u, &off),
          "first unallocated aperture word is inaccessible");
    check(!psx_mod_gpu_dma_aperture_offset_for(
              0x00FFFFFCu, 8u, PSX_MOD_GPU_DMA_APERTURE_SIZE, &off),
          "cross-boundary access is rejected");

    return failures ? 1 : 0;
}
