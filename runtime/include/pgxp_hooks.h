#ifndef PGXP_HOOKS_H
#define PGXP_HOOKS_H

/* PGXP dataflow-shadowing hook surface (ENHANCEMENTS.md G1.2/G1.3).
 *
 * The runtime keeps a host-only "shadow" of sub-pixel GTE projection results
 * and follows them BY PROVENANCE as the game moves them through registers and
 * RAM into a GP0 packet (pgxp.cpp). Generated code participates by calling
 * these hooks after the guest operation completes.
 *
 * Gating model: the emitter writes PGXP_*() funnels unconditionally. PGXP
 * propagation remains compiled in only for the PSX_PGXP variant and disabled
 * by default. The base variant uses the same funnels solely for the separate,
 * runtime-gated GTE-to-native provenance tracker; it does not enable PGXP,
 * geometry correction, tolerance fallback, or sub-pixel rendering.
 *
 * RULES for emit sites:
 *   - Macro arguments MUST be side-effect free (they vanish in base builds).
 *   - Hooks run AFTER the guest architectural effect (value = what actually
 *     loaded/stored/resulted), so the shadow can validate against reality.
 *   - `cpu` must be in scope at every emit site (it always is in generated C).
 *
 * The interpreters (dirty_ram_interp.c, psx_interpreter.c) call the
 * psx_pgxp_* functions directly — they are runtime code compiled once, and
 * every hook early-outs on a single global when the feature is off.
 *
 * Overlay DLLs cannot link runtime symbols directly; they forward through the
 * PGXPHooks table below (OverlayCallbacks.pgxp, overlay_api.h). A NULL table
 * no-ops every hook — precision shadowing is a visual enhancement, never
 * load-bearing.
 *
 * LICENSING: this is a clean-room implementation of the publicly documented
 * PGXP technique (psx-spx, public design write-ups). Do NOT port code from
 * the vendored duckstation/ (CC BY-NC-ND) or beetle-psx/ (GPL) trees; they
 * are black-box behavioral oracles only. See ENHANCEMENTS.md G1.
 */

#include <stdint.h>
#include "gte_native_provenance.h"
#include "ram_provenance.h"

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* Funnel entry points (implemented in runtime/src/pgxp.cpp). The raw
 * instruction word carries the op/rs/rt/rd/imm decode so the emitted call is
 * uniform per class; `value` is the architectural result of the operation.
 *
 *   load:   rt <- [addr]; value = loaded word/half/byte (post-extension)
 *   store:  [addr] <- rt; value = the stored word/half/byte
 *   alu:    rd/rt <- op(s1, s2); result = the written value (also LUI, HILO
 *           moves and MOVE idioms — the body decodes the class from instr)
 *   muldiv: HI/LO <- op(s1, s2)
 *   cop2:   MFC2/CFC2/MTC2/CTC2 (addr = 0) and LWC2/SWC2 (addr = guest addr);
 *           value = the transferred word
 */
void psx_pgxp_load  (struct CPUState *cpu, uint32_t instr, uint32_t addr, uint32_t value);
void psx_pgxp_store (struct CPUState *cpu, uint32_t instr, uint32_t addr, uint32_t value);
void psx_pgxp_alu   (struct CPUState *cpu, uint32_t instr, uint32_t result, uint32_t s1, uint32_t s2);
void psx_pgxp_muldiv(struct CPUState *cpu, uint32_t instr, uint32_t hi, uint32_t lo, uint32_t s1, uint32_t s2);
void psx_pgxp_cop2  (struct CPUState *cpu, uint32_t instr, uint32_t value, uint32_t addr);

/* Forwarder table for overlay DLLs (OverlayCallbacks.pgxp). Appended-last
 * member semantics apply: a NULL pointer (older host) means "no shadowing". */
typedef struct PGXPHooks {
    void (*load)  (struct CPUState *cpu, uint32_t instr, uint32_t addr, uint32_t value);
    void (*store) (struct CPUState *cpu, uint32_t instr, uint32_t addr, uint32_t value);
    void (*alu)   (struct CPUState *cpu, uint32_t instr, uint32_t result, uint32_t s1, uint32_t s2);
    void (*muldiv)(struct CPUState *cpu, uint32_t instr, uint32_t hi, uint32_t lo, uint32_t s1, uint32_t s2);
    void (*cop2)  (struct CPUState *cpu, uint32_t instr, uint32_t value, uint32_t addr);
} PGXPHooks;

#define GTE_NATIVE_PROVENANCE_LOAD(instr, addr, val) do { \
    if (g_gte_native_provenance_active) \
        gte_native_provenance_cpu_load(cpu, (instr), (addr), (val)); \
} while (0)
#define GTE_NATIVE_PROVENANCE_STORE(instr, addr, val) do { \
    if (g_gte_native_provenance_active) \
        gte_native_provenance_cpu_store(cpu, (instr), (addr), (val)); \
} while (0)
#define GTE_NATIVE_PROVENANCE_ALU(instr, res, s1, s2) do { \
    if (g_gte_native_provenance_active) \
        gte_native_provenance_cpu_alu(cpu, (instr), (res), (s1), (s2)); \
} while (0)
#define GTE_NATIVE_PROVENANCE_COP2(instr, val, addr) do { \
    if (g_gte_native_provenance_active) \
        gte_native_provenance_cpu_cop2(cpu, (instr), (val), (addr)); \
} while (0)
#define CPU_RAM_PROVENANCE_STORE(instr, addr, val) \
    ram_provenance_note_cpu_store((instr), (addr), (val))

#if defined(PSX_PGXP) && PSX_PGXP
#define PGXP_LOAD(instr, addr, val) do { \
    GTE_NATIVE_PROVENANCE_LOAD((instr), (addr), (val)); \
    psx_pgxp_load(cpu, (instr), (addr), (val)); \
} while (0)
#define PGXP_STORE(instr, addr, val) do { \
    GTE_NATIVE_PROVENANCE_STORE((instr), (addr), (val)); \
    psx_pgxp_store(cpu, (instr), (addr), (val)); \
} while (0)
#define PGXP_ALU(instr, res, s1, s2) do { \
    GTE_NATIVE_PROVENANCE_ALU((instr), (res), (s1), (s2)); \
    psx_pgxp_alu(cpu, (instr), (res), (s1), (s2)); \
} while (0)
#define PGXP_MULDIV(instr, hi, lo, s1, s2)       psx_pgxp_muldiv(cpu, (instr), (hi), (lo), (s1), (s2))
#define PGXP_COP2(instr, val, addr) do { \
    GTE_NATIVE_PROVENANCE_COP2((instr), (val), (addr)); \
    psx_pgxp_cop2(cpu, (instr), (val), (addr)); \
} while (0)
#else
#define PGXP_LOAD(instr, addr, val)              GTE_NATIVE_PROVENANCE_LOAD((instr), (addr), (val))
#define PGXP_STORE(instr, addr, val) do { \
    GTE_NATIVE_PROVENANCE_STORE((instr), (addr), (val)); \
    CPU_RAM_PROVENANCE_STORE((instr), (addr), (val)); \
} while (0)
#define PGXP_ALU(instr, res, s1, s2)             GTE_NATIVE_PROVENANCE_ALU((instr), (res), (s1), (s2))
#define PGXP_MULDIV(instr, hi, lo, s1, s2)       ((void)0)
#define PGXP_COP2(instr, val, addr) do { \
    GTE_NATIVE_PROVENANCE_COP2((instr), (val), (addr)); \
    CPU_RAM_PROVENANCE_STORE((instr), (addr), (val)); \
} while (0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* PGXP_HOOKS_H */
