#pragma once
#include <stdint.h>
#ifdef __cplusplus
#include "mod_packages.h"
namespace PSXRecompV4 {
bool mod_native_prepare(const ModResolution& plan, std::string* error);
void mod_native_reset();
void mod_native_activate();
void mod_native_vblank();
void mod_native_savestate_loaded();
}
extern "C" {
#endif
/* Game-owned, process-lifetime catalog, sorted by name. Duplicate unqualified
 * names are intentional; qualified names must be unique. The availability
 * predicate verifies the current code identity without executing guest code.
 * Registration is independent of installed mods and survives session resets. */
typedef struct ModNativeSymbol {
    const char* name;
    uint32_t address;
    const void* identity;
} ModNativeSymbol;
typedef int (*ModNativeSymbolAvailable)(const void* identity);
void mod_native_register_symbols(const ModNativeSymbol* symbols, uint32_t count,
                                 ModNativeSymbolAvailable available);
struct CPUState;
/* Cheap disabled path. Call at a guest dispatch entry; handled = 1. */
#if defined(PSX_NATIVE_MODS) || defined(__cplusplus)
int mod_native_on_dispatch(struct CPUState* cpu, uint32_t address,
                           uint32_t return_address);
/* Reject compiled footprints intersecting a partial hook, even when RAM is
 * pristine. Queries are cheap and never execute native mod callbacks. */
int mod_native_blocks_intersect(uint32_t address, uint32_t size);
int mod_native_has_block(uint32_t address);
int mod_native_on_block(struct CPUState* cpu, uint32_t address);
#else
static inline int mod_native_on_dispatch(struct CPUState* cpu, uint32_t address,
                                         uint32_t return_address) {
    (void)cpu; (void)address; (void)return_address;
    return 0;
}
static inline int mod_native_blocks_intersect(uint32_t address, uint32_t size) {
    (void)address; (void)size; return 0;
}
static inline int mod_native_has_block(uint32_t address) { (void)address; return 0; }
static inline int mod_native_on_block(struct CPUState* cpu, uint32_t address) {
    (void)cpu; (void)address; return 0;
}
#endif
/* Overlay bodies can make direct calls inside a linked module, whose public
 * loader entry cannot guard an interior hook. Keep those modules interpreted
 * while any partial hook is active; resident game footprints stay selective. */
static inline int mod_native_requires_overlay_interpreter(void) {
    return mod_native_blocks_intersect(0x10000u, 0x1f0000u);
}
#ifdef __cplusplus
}
#endif
