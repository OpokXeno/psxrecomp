#pragma once
/* Standalone C ABI for trusted external mods. No runtime/C++ layout dependency.
 * Callbacks run synchronously on the emulation thread. Never throw, longjmp,
 * retain a call/CPU/next pointer, or call host services from another thread.
 * Native code has the privileges of the game process: this is not a sandbox. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PSX_NATIVE_MOD_ABI 1u
#if defined(_WIN32)
#define PSX_NATIVE_MOD_EXPORT __declspec(dllexport)
#define PSX_NATIVE_MOD_CALL __cdecl
#else
#define PSX_NATIVE_MOD_EXPORT __attribute__((visibility("default")))
#define PSX_NATIVE_MOD_CALL
#endif

typedef struct PSXNativeCPU {
    uint32_t gpr[32]; /* a0..a3 = 4..7, v0/v1 = 2/3, sp = 29, ra = 31 */
    uint32_t hi, lo, pc;
} PSXNativeCPU;

typedef struct PSXNativeCall {
    uint32_t size, address, return_address;
    void* context;
    /* Optional: execute the next mod, then the original guest function.
     * A normal return publishes return_address in cpu->pc. At most once,
     * synchronously, with the supplied CPU pointer. Omit to
     * replace the function; publish the desired continuation in cpu->pc
     * (normally return_address). Preserve guest callee-saved registers. */
    int (PSX_NATIVE_MOD_CALL *next)(void* context, PSXNativeCPU* cpu);
} PSXNativeCall;

typedef struct PSXNativeBlock {
    uint32_t size, address, resume_address, byte_count;
    uint32_t return_address; /* Current guest ra, not the block continuation. */
    /* Append-only extension: callback-scoped handle for call_guest(). */
    void* context;
} PSXNativeBlock;

typedef struct PSXNativeGuestResult {
    uint32_t size; /* Initialize to sizeof(PSXNativeGuestResult). */
    uint32_t v0, v1;
} PSXNativeGuestResult;

enum {
    PSX_NATIVE_GUEST_UNAVAILABLE = 0, /* Rejected without executing guest code. */
    PSX_NATIVE_GUEST_RETURNED = 1,
    PSX_NATIVE_GUEST_NONLOCAL = -1, /* Guest control escaped: return your callback. */
    /* Named-call diagnostics; no guest code was executed. */
    PSX_NATIVE_GUEST_UNKNOWN_FUNCTION = -2,
    PSX_NATIVE_GUEST_AMBIGUOUS_FUNCTION = -3,
    PSX_NATIVE_GUEST_FUNCTION_NOT_LOADED = -4
};

enum {
    PSX_NATIVE_FUNCTION_UNKNOWN = 0,
    PSX_NATIVE_FUNCTION_FOUND = 1,
    PSX_NATIVE_FUNCTION_AMBIGUOUS = -1
};

typedef struct PSXNativeHost {
    uint32_t size, abi_version;
    void* context;
    const char* package_id;
    const char* feature_id;
    uint8_t (PSX_NATIVE_MOD_CALL *read_byte)(uint32_t address);
    uint16_t (PSX_NATIVE_MOD_CALL *read_half)(uint32_t address);
    uint32_t (PSX_NATIVE_MOD_CALL *read_word)(uint32_t address);
    void (PSX_NATIVE_MOD_CALL *write_byte)(uint32_t address, uint8_t value);
    void (PSX_NATIVE_MOD_CALL *write_half)(uint32_t address, uint16_t value);
    void (PSX_NATIVE_MOD_CALL *write_word)(uint32_t address, uint32_t value);
    /* Committed feature options; copies NUL-terminated text, returns 0 for
     * undeclared options or insufficient capacity. */
    int (PSX_NATIVE_MOD_CALL *option)(void* context, const char* id,
                                     char* buffer, uint32_t capacity);
    void (PSX_NATIVE_MOD_CALL *log)(void* context, const char* message);
    void (PSX_NATIVE_MOD_CALL *advance_cycles)(uint32_t cycles);
    int (PSX_NATIVE_MOD_CALL *game_started)(void);
    /* Optional append-only ABI v1 extension. Use call->context or block->context,
     * and the exact CPU pointer supplied to that callback. Calls a live game-RAM
     * function through normal dispatch, including other mods. Arguments are
     * 32-bit o32 words: first four in a0..a3, remaining words on a temporary,
     * 8-byte-aligned guest stack frame with the mandatory 16-byte home area.
     * Normal return restores the callback's registers/PC and argument-frame
     * bytes; memory, COP0/GTE effects and elapsed guest cycles remain. Results
     * are returned separately in v0/v1. On NONLOCAL, CPU publishes the guest
     * continuation instead; return immediately (no next or further calls).
     * Only hook/block callbacks can call guest code, never lifecycle callbacks.
     * Check host->size before reading this field on an older runtime. */
    int (PSX_NATIVE_MOD_CALL *call_guest)(void* callback_context, PSXNativeCPU* cpu,
                                        uint32_t address, const uint32_t* arguments,
                                        uint32_t argument_count,
                                        PSXNativeGuestResult* result);
    /* Optional append-only services. Names come from the game's annotations.
     * An unqualified name must be unique; IMAGE/NAME disambiguates it.
     * find_function resolves metadata, even before an overlay is loaded.
     * It also accepts a hexadecimal address or NAME+OFFSET; a returned address
     * is not a persistent guarantee that the corresponding code is resident.
     * Use host->context here, including from start(). Output is unchanged on
     * UNKNOWN/AMBIGUOUS. Names are case-sensitive, at most 255 bytes. */
    int (PSX_NATIVE_MOD_CALL *find_function)(void* host_context, const char* name,
                                            uint32_t* address);
    /* Same callback context, CPU, arguments and return rules as call_guest.
     * Resolves and validates live code on EVERY named call. A missing, reused
     * or modified overlay returns FUNCTION_NOT_LOADED. Unknown and ambiguous
     * names have separate statuses. Numeric strings retain raw-address behavior.
     * Call named functions at their entry; offsets are for find_function and
     * hook ranges, not call_guest_named. Check size before using either field. */
    int (PSX_NATIVE_MOD_CALL *call_guest_named)(void* callback_context, PSXNativeCPU* cpu,
                                              const char* name, const uint32_t* arguments,
                                              uint32_t argument_count,
                                              PSXNativeGuestResult* result);
} PSXNativeHost;

typedef struct PSXNativeMod {
    uint32_t size, abi_version;
    void* userdata;
    /* Optional. Return 0 to reject activation; stop is still called. */
    int (PSX_NATIVE_MOD_CALL *start)(void* userdata, const PSXNativeHost* host);
    void (PSX_NATIVE_MOD_CALL *stop)(void* userdata);
    void (PSX_NATIVE_MOD_CALL *hook)(void* userdata, PSXNativeCPU* cpu,
                                    const PSXNativeCall* call);
    void (PSX_NATIVE_MOD_CALL *vblank)(void* userdata);
    /* Optional: reset mod-owned host state after a guest savestate load. */
    void (PSX_NATIVE_MOD_CALL *savestate_loaded)(void* userdata);
    /* Optional append-only ABI v1 extension. Replace a guarded instruction
     * range. No next/original call: normal return resumes at resume_address.
     * A nonlocal exit from call_guest propagates the guest continuation instead.
     * Preserve all live registers needed by the remaining guest instructions,
     * including sp/ra. cpu->pc is set to resume_address by the host on return.
     * Hooks do not run inside branch or load delay slots. */
    void (PSX_NATIVE_MOD_CALL *block)(void* userdata, PSXNativeCPU* cpu,
                                     const PSXNativeBlock* block);
} PSXNativeMod;

/* Export exactly this symbol (extern "C" in C++). Descriptor and userdata
 * live until stop. Host table lives until stop; only use services in callbacks.
 * Build separately for each OS/CPU. The host requires ABI and struct size. */
typedef const PSXNativeMod* (PSX_NATIVE_MOD_CALL *PSXNativeModEntry)(void);
PSX_NATIVE_MOD_EXPORT const PSXNativeMod* PSX_NATIVE_MOD_CALL psx_native_mod_v1(void);
#ifdef __cplusplus
}
#endif
