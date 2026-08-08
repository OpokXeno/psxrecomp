#include "overlay_api.h"

#include <stdint.h>
#include <stdio.h>

static uint32_t seen_token;

static int32_t frame_step(uint32_t site_pc, uint32_t word, int32_t step,
                          uint32_t frame_token, uint32_t tier)
{
    if (site_pc != 0x800758E4u || word != 0x24630002u || step != 2 || tier != 1u)
        return step;
    seen_token = frame_token;
    return 1;
}

int main(void)
{
    OverlayCallbacks callbacks = {0};
    callbacks.xg_field_frame_step = frame_step;
    if (PSX_OVERLAY_ABI_VERSION != 25 || PSX_OVERLAY_CODEGEN_VER != 18) return 1;
    if (callbacks.xg_field_frame_step(0x800758E4u, 0x24630002u, 2,
                                      0x12345678u, 1u) != 1) return 1;
    if (seen_token != 0x12345678u) return 1;
    puts("PASS: overlay provenance ABI forwards the authored-frame token");
    return 0;
}
