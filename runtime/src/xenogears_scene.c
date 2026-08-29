#include "xenogears_scene.h"

#include <string.h>

extern uint8_t psx_read_byte(uint32_t address);

enum {
    XG_RAM_LAST_WORD = 0x001FFFFCu,
    XG_CACHED_BASE = 0x80000000u,
    XG_UNCACHED_BASE = 0xA0000000u,
    XG_FIELD_CONTEXT = 0x800B0078u,
    XG_FIELD_ID = 0x8006F94Eu,
    XG_MODULE_REQUESTED = 0x80018088u,
    XG_MODULE_ACTIVE = 0x800592C0u,
    XG_MODULE_POINTER = 0x800592BCu,
    XG_GAME_PROGRESS = 0x8006EF64u,
    XG_FIELD_ID_MASK = 0x07FFu,
    XG_FIELD_ID_LIMIT = 0x0400u,
};

typedef struct {
    XgScene scene;
    uint32_t generation;
    uint8_t have_scene;
    uint8_t fmv_active;
} XgSceneState;

static XgSceneState s_scene;

static uint16_t read_u16(uint32_t address)
{
    return (uint16_t)(psx_read_byte(address) |
                      ((uint32_t)psx_read_byte(address + 1u) << 8));
}

static uint32_t read_u32(uint32_t address)
{
    return read_u16(address) |
           ((uint32_t)read_u16(address + 2u) << 16);
}

static int valid_pointer(uint32_t pointer)
{
    uint32_t physical;

    if (pointer <= XG_RAM_LAST_WORD)
        physical = pointer;
    else if (pointer >= XG_CACHED_BASE &&
             pointer <= XG_CACHED_BASE + XG_RAM_LAST_WORD)
        physical = pointer - XG_CACHED_BASE;
    else if (pointer >= XG_UNCACHED_BASE &&
             pointer <= XG_UNCACHED_BASE + XG_RAM_LAST_WORD)
        physical = pointer - XG_UNCACHED_BASE;
    else
        return 0;
    return physical != 0u && (physical & 3u) == 0u;
}

void psx_xenogears_read_scene(XgScene *out)
{
    XgScene scene = { 0 };

    if (out == NULL)
        return;
    scene.field_context = read_u32(XG_FIELD_CONTEXT);
    scene.requested_module = read_u32(XG_MODULE_REQUESTED);
    scene.active_module = read_u32(XG_MODULE_ACTIVE);
    scene.module_pointer = read_u32(XG_MODULE_POINTER);
    scene.raw_field_id = read_u16(XG_FIELD_ID);
    scene.masked_field_id = scene.raw_field_id & XG_FIELD_ID_MASK;
    if (valid_pointer(scene.field_context)) {
        scene.field_id = scene.masked_field_id;
        scene.game_progress = read_u16(XG_GAME_PROGRESS);
        scene.valid_field = (uint8_t)(scene.masked_field_id < XG_FIELD_ID_LIMIT);
    }
    *out = scene;
}

static int same_scene(const XgScene *left, const XgScene *right)
{
    if (left->valid_field != right->valid_field)
        return 0;
    return !left->valid_field ||
           (left->field_context == right->field_context &&
            left->field_id == right->field_id);
}

void psx_xenogears_scene_reset(void)
{
    s_scene.have_scene = 0u;
    s_scene.fmv_active = 0u;
    memset(&s_scene.scene, 0, sizeof(s_scene.scene));
}

void psx_xenogears_scene_vblank_boundary(int fmv_active)
{
    XgScene scene;
    const uint8_t fmv = (uint8_t)(fmv_active != 0);

    psx_xenogears_read_scene(&scene);
    if (fmv != s_scene.fmv_active) {
        psx_xenogears_scene_reset();
        s_scene.fmv_active = fmv;
    }
    if (s_scene.fmv_active ||
        (s_scene.have_scene && same_scene(&s_scene.scene, &scene)))
        return;
    psx_xenogears_scene_reset();
    s_scene.scene = scene;
    s_scene.have_scene = 1u;
    s_scene.generation++;
}

uint32_t psx_xenogears_scene_generation(void)
{
    return s_scene.generation;
}
