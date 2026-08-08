#include "overlay_api.h"
#include "game_identity.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define TEST_EXPORT __declspec(dllexport)
#else
#define TEST_EXPORT __attribute__((visibility("default")))
#endif

#ifndef TEST_PAIR_ID
#define TEST_PAIR_ID UINT64_C(0x1020304050607080)
#endif

#ifndef TEST_MARKER
#define TEST_MARKER 0xC001CAFEu
#endif

#ifndef TEST_INSTANCE
#define TEST_INSTANCE 0
#endif

static int s_init_count;
static int s_flush_count;
static int s_call_count;
static const OverlayCallbacks *s_callbacks;

static void trace(const char *event) {
    const char *path = getenv("PSX_PAIR_TEST_TRACE");
    if (!path || !*path) return;
    FILE *out = fopen(path, "ab");
    if (!out) return;
    fprintf(out, "%s %d\n", event, TEST_INSTANCE);
    fclose(out);
}

TEST_EXPORT int overlay_abi(void) { return PSX_OVERLAY_ABI_TAG; }
TEST_EXPORT uint64_t overlay_pair_id(void) { return TEST_PAIR_ID; }
static const PsxGameIdentity s_identity = {
    {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u,
     0x08u, 0x09u, 0x0Au, 0x0Bu, 0x0Cu, 0x0Du, 0x0Eu, 0x0Fu,
     0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u,
     0x18u, 0x19u, 0x1Au, 0x1Bu, 0x1Cu, 0x1Du, 0x1Eu, 0x1Fu},
    {0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x27u,
     0x28u, 0x29u, 0x2Au, 0x2Bu, 0x2Cu, 0x2Du, 0x2Eu, 0x2Fu,
     0x30u, 0x31u, 0x32u, 0x33u, 0x34u, 0x35u, 0x36u, 0x37u,
     0x38u, 0x39u, 0x3Au, 0x3Bu, 0x3Cu, 0x3Du, 0x3Eu, 0x3Fu},
};
TEST_EXPORT const PsxGameIdentity *overlay_game_identity(void) { return &s_identity; }

TEST_EXPORT void overlay_init(const OverlayCallbacks *callbacks) {
    s_callbacks = callbacks;
    s_init_count++;
    trace("init");
}

TEST_EXPORT void overlay_flush_cycles(void) {
    s_flush_count++;
    trace("flush");
}

TEST_EXPORT int test_init_count(void) { return s_init_count; }
TEST_EXPORT int test_flush_count(void) { return s_flush_count; }
TEST_EXPORT int test_call_count(void) { return s_call_count; }

TEST_EXPORT void func_80010000(CPUState *cpu) {
    if (s_callbacks && s_callbacks->log_call_entry)
        s_callbacks->log_call_entry(0x80010000u);
    s_call_count++;
    cpu->gpr[2] = TEST_MARKER;
    trace("call");
}

#ifndef TEST_PARTIAL_EXPORTS
TEST_EXPORT void func_80010004(CPUState *cpu) { cpu->gpr[3] = TEST_MARKER; }
TEST_EXPORT void func_80010008(CPUState *cpu) { cpu->gpr[4] = TEST_MARKER; }
TEST_EXPORT void func_8001000C(CPUState *cpu) { cpu->gpr[5] = TEST_MARKER; }
#endif
