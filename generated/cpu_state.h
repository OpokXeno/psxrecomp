/* ============================================================================
 * cpu_state.h
 * ----------------------------------------------------------------------------
 * COMPILE-ONLY ARTIFACT FOR PHASE 1a PROOF.
 *
 * This header exists so that `generated/boot_slice.c` can be type-checked by
 * a C compiler with `-c` (object emission only). It is NOT a runtime, it is
 * NOT a stub set, and it is NOT the start of the v4 runtime.
 *
 * Linking any translation unit that includes this header into a runnable
 * binary is a build error BY DESIGN: the function-pointer fields in CPUState
 * are unset, and `psx_syscall` has no implementation anywhere in the tree.
 * If your linker fails on `psx_syscall` or on undefined memory accessors,
 * that is the intended behavior. DO NOT add stubs. DO NOT add a default
 * implementation. The runtime is Phase 2 work and will land in `runtime/`
 * with real hardware-simulation backends.
 *
 * The Phase 1a recompiler self-validates `boot_slice.c` with `gcc -c` only.
 * If you find yourself wanting to write `cpu_state.c` to make linking work,
 * stop. Re-read CLAUDE.md sections 0 and 7. Stubs are how every prior
 * version of this project failed.
 * ========================================================================== */

#ifndef PSXRECOMP_V4_CPU_STATE_H
#define PSXRECOMP_V4_CPU_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PS1 R3000A architectural state.
 *
 * Layout matches recompiler/include/code_generator.h `CPUState` so that
 * Phase 2's runtime can drop in without renaming. The cop0 array is added
 * here because Phase 1a's strict translator emits MFC0/MTC0/RFE in cop0[N]
 * form (matching salvaged codegen convention); Phase 2 may revisit per
 * PLAN.md HP1.
 */
typedef struct CPUState {
    uint32_t gpr[32];   /* $0..$31; gpr[0] is hardwired zero, never written */
    uint32_t pc;        /* program counter */
    uint32_t hi, lo;    /* mult/div result registers */
    uint32_t cop0[32];  /* COP0 system control registers (SR, Cause, EPC, ...) */
    uint32_t gte_data[32]; /* COP2 (GTE) data registers */
    uint32_t gte_ctrl[32]; /* COP2 (GTE) control registers */

    /* Memory access function pointers. Phase 1a leaves these NULL.
     * The recompiled boot_slice.c calls them via cpu->write_word(...);
     * dereferencing a NULL function pointer at runtime crashes loudly,
     * which is the desired failure mode if anyone tries to RUN this
     * artifact (vs. only compile it). */
    uint32_t (*read_word)(uint32_t addr);
    void     (*write_word)(uint32_t addr, uint32_t value);
    uint16_t (*read_half)(uint32_t addr);
    void     (*write_half)(uint32_t addr, uint16_t value);
    uint8_t  (*read_byte)(uint32_t addr);
    void     (*write_byte)(uint32_t addr, uint8_t value);
} CPUState;

/* Syscall trampoline. Declared, never defined.
 * Any TU that calls this and then links will fail with an unresolved symbol.
 * That is intentional. The Phase 2 runtime will provide a real definition
 * that routes through the recompiled BIOS exception vector. */
extern void psx_syscall(CPUState* cpu, uint32_t code);

/* Arithmetic-overflow trap trampoline. Declared, never defined.
 * Real R3000A raises an Overflow exception (Cause.ExcCode = 0xC) when a
 * signed ADD/ADDI/SUB result does not fit in 32 bits. The recompiled C
 * for those instructions checks for overflow and calls this function on
 * trap; the Phase 2 runtime will define it to deliver the exception
 * through the recompiled exception vector. Until then, calling this
 * symbol fails to link, which is the intended behavior. DO NOT add a
 * default definition. */
extern void psx_arith_overflow(CPUState* cpu);

/* Unaligned-access fail-loud. Declared, never defined.
 * Phase 1b loads (LW/LH/LHU) and stores (SW/SH) check the effective
 * address alignment before performing the access. On misalignment the
 * recompiled C calls this function with the offending address and the
 * PC of the load/store instruction, then returns from the slice. Real
 * R3000A would raise AdEL (load) or AdES (store) exceptions; we are
 * NOT modeling exception delivery in Phase 1b. The intent here is to
 * fail loud so that any unaligned access is surfaced rather than
 * silently producing the wrong bytes. The Phase 2 runtime will define
 * this function (probably as `abort()` with a diagnostic, or as a
 * proper exception delivery once the exception path exists). DO NOT
 * add a default definition. */
extern void psx_unaligned_access(CPUState* cpu, uint32_t addr, uint32_t pc);

/* BREAK trap fail-loud. Declared, never defined.
 * Phase 1b B(10) translates the BREAK instruction (SPECIAL funct 0x0D)
 * to a call to this function. `code` is the 20-bit immediate in
 * bits [25:6] of the BREAK instruction, `pc` is the address of the
 * BREAK itself. After the call the recompiled C `return`s from the
 * slice — the strict translator does NOT model R3000A's BREAK
 * exception (Cause.ExcCode = 0x9) or the resumption back through
 * RFE. That is Phase 2 work. Until the Phase 2 runtime provides this
 * symbol, linking fails by design. DO NOT add a default definition. */
extern void psx_break(CPUState* cpu, uint32_t code, uint32_t pc);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_CPU_STATE_H */
