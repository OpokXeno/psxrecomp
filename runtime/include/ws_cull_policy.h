#ifndef PSXRECOMP_WS_CULL_POLICY_H
#define PSXRECOMP_WS_CULL_POLICY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Semantic classes for guest-side visibility predicates. These describe what
 * a compare means, rather than which instruction encoding happened to carry
 * it. The same classes are consumed by generated code and dirty-RAM code. */
typedef enum PsxWsCullSemantic {
    PSX_WS_CULL_SEMANTIC_NONE = 0,
    PSX_WS_CULL_SEMANTIC_SCREEN_BIAS = 1,
    PSX_WS_CULL_SEMANTIC_WORLD_RANGE = 2,
    PSX_WS_CULL_SEMANTIC_LEFT_EDGE = 3,
    PSX_WS_CULL_SEMANTIC_MASKED_SCREEN_X = 4,
    PSX_WS_CULL_SEMANTIC_FRUSTUM_PLANE_X = 5,
    PSX_WS_CULL_SEMANTIC_SIGNED_SCREEN_X = 6,
    PSX_WS_CULL_SEMANTIC_DEPTH_BOUND = 7,
    PSX_WS_CULL_SEMANTIC_XCLIP_BOUND = 8,
} PsxWsCullSemantic;

/* Guest-side policy helpers. They are identity functions at 4:3 and use the
 * currently active legacy/native guest-cull margin at wider aspects. */
uint32_t psx_ws_guest_cull_screen_bias(uint32_t value, int32_t immediate);
int psx_ws_guest_cull_world_range(uint32_t value, int32_t immediate);
uint32_t psx_ws_guest_cull_left_edge(uint32_t bound);
int psx_ws_guest_cull_masked_screen_x(uint32_t x, uint32_t bound);
int32_t psx_ws_guest_cull_frustum_plane_x(int32_t nx);
int psx_ws_guest_cull_signed_screen_x(int32_t value, int32_t immediate);
int psx_ws_guest_cull_depth_signed(int32_t value, int32_t immediate);
int psx_ws_guest_cull_depth_unsigned(uint32_t value, int32_t immediate);
uint32_t psx_ws_guest_cull_xclip_bound(uint32_t vanilla);

#ifdef __cplusplus
}
#endif

#endif
