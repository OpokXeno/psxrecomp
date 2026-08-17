#define PSX_OVERLAY_DLL_BUILD 1
#include "overlay_loader.h"
#include "game_identity.h"
#include "guest_render_native_stream.h"
#include "native_render_baseline.h"
#include "xg_render_auth_runtime.h"
#undef PSX_OVERLAY_DLL_BUILD

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define TEST_MARKER 0xC001CAFEu
#define RAM_SIZE (2u * 1024u * 1024u)

_Static_assert(PSX_OVERLAY_TEST_CANDIDATE_CAP == 4,
               "pair-dedup harness must exercise the exact four-slot cap");
int overlay_loader_test_force_stale_chain(uint32_t phys, int all);

static uint8_t s_ram[RAM_SIZE];
static uint8_t s_scratch[1024];
static PsxXgRenderAuthCandidate s_renderer_candidate;
static uint32_t s_renderer_candidate_calls;
static uint32_t s_loader_mismatch_calls;
static const PsxGameIdentity s_fixture_identity = {
    {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u,
     0x08u, 0x09u, 0x0Au, 0x0Bu, 0x0Cu, 0x0Du, 0x0Eu, 0x0Fu,
     0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u,
     0x18u, 0x19u, 0x1Au, 0x1Bu, 0x1Cu, 0x1Du, 0x1Eu, 0x1Fu},
    {0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x27u,
     0x28u, 0x29u, 0x2Au, 0x2Bu, 0x2Cu, 0x2Du, 0x2Eu, 0x2Fu,
     0x30u, 0x31u, 0x32u, 0x33u, 0x34u, 0x35u, 0x36u, 0x37u,
     0x38u, 0x39u, 0x3Au, 0x3Bu, 0x3Cu, 0x3Du, 0x3Eu, 0x3Fu},
};

uint32_t g_debug_current_func_addr;
uint32_t g_debug_last_store_pc;
uint32_t g_overlay_region_floor;
uint32_t i_stat, i_mask;
int g_exec_phase;
int g_idle_note_suppress;
int g_psx_call_bail;
uint32_t g_dirty_ram_code_gen;
uint64_t g_psx_bail_first, g_psx_bail_resolved;
uint64_t s_frame_count;
int g_shadow_mmio_watch;
uint64_t g_shadow_mmio_hits;

void native_render_baseline_runtime_reset(void) {}
void native_render_baseline_runtime_arm(void) {}
NativeRenderBaselineReason native_render_baseline_runtime_observe(
        NativeRenderBaselineSnapshot *snapshot) {
    (void)snapshot;
    return NATIVE_RENDER_BASELINE_COMPLETE;
}

void psx_xg_render_auth_native_bad_entry(uint32_t owner, uint32_t pc) {
    (void)owner;
    (void)pc;
}
void psx_xg_render_auth_note_candidate_dispatch(
        const PsxXgRenderAuthCandidate *candidate) {
    if (candidate == NULL) return;
    s_renderer_candidate = *candidate;
    s_renderer_candidate_calls++;
}
void psx_xg_render_auth_note_artifact_candidate(
        const PsxXgRenderAuthCandidate *candidate) {
    (void)candidate;
}
void psx_xg_render_auth_warm_hook(CPUState *cpu, uint32_t hook, uint32_t pc,
                                  uint32_t instruction_word,
                                  uint32_t delay_slot_word) {
    (void)cpu;
    (void)hook;
    (void)pc;
    (void)instruction_word;
    (void)delay_slot_word;
}
void psx_xg_render_auth_loader_mismatch(uint32_t pc) {
    (void)pc;
    s_loader_mismatch_calls++;
}
bool psx_xg_render_auth_native_ft4_bypass(
        CPUState *cpu, uint32_t pc, uint32_t instruction) {
    (void)cpu;
    (void)pc;
    (void)instruction;
    return false;
}

void gte_attribution_set_enabled(bool enabled) { (void)enabled; }
void gte_attribution_reset(void) {}
GteAttributionResult gte_attribution_snapshot(
        GteAttributionSnapshot *out_snapshot,
        GteAttributionContextCounter *context_counters,
        size_t context_capacity,
        GteAttributionSiteCounter *site_counters,
        size_t site_capacity) {
    (void)context_counters;
    (void)context_capacity;
    (void)site_counters;
    (void)site_capacity;
    if (out_snapshot == NULL) return GTE_ATTRIBUTION_INVALID_ARGUMENT;
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    return GTE_ATTRIBUTION_OK;
}
GuestRenderStatus guest_render_bridge_snapshot(
        GuestRenderBridgeSnapshot *out_snapshot) {
    (void)out_snapshot;
    return GUEST_RENDER_NO_COMPLETED_STATE;
}
GuestRenderStatus guest_render_bridge_present(
        GuestRenderCompletedState *out_completed) {
    (void)out_completed;
    return GUEST_RENDER_NO_COMPLETED_STATE;
}
GuestRenderStatus guest_render_bridge_last_completed(
        GuestRenderBridgeSnapshot *out_snapshot,
        GuestRenderCompletedState *out_completed) {
    (void)out_snapshot;
    (void)out_completed;
    return GUEST_RENDER_NO_COMPLETED_STATE;
}
void guest_render_native_stream_set_material_observer(
        GuestRenderNativeStreamMaterialObserver observer) {
    (void)observer;
}

uint8_t *memory_get_ram_ptr(void) { return s_ram; }
uint8_t *memory_get_scratchpad_ptr(void) { return s_scratch; }
uint32_t overlay_watch_pagegen_sum(uint32_t phys, uint32_t len) {
    (void)phys; (void)len; return 0;
}
void overlay_watch_set_range(uint32_t phys, uint32_t len) {
    (void)phys; (void)len;
}
void ds_init(const char *cache_dir, const char *game_id) {
    (void)cache_dir; (void)game_id;
}
uint32_t dirty_ram_get_bitmap_word(uint32_t word) { (void)word; return 0; }
int dirty_ram_is_dirty(uint32_t phys) { (void)phys; return 0; }
int dirty_ram_dispatch(CPUState *cpu, uint32_t addr, uint32_t stop) {
    (void)cpu; (void)addr; (void)stop; return 0;
}
void dirty_ram_xprobe_call_note(CPUState *cpu, uint32_t target,
                                uint32_t ra, uint8_t phase) {
    (void)cpu; (void)target; (void)ra; (void)phase;
}

void psx_dispatch_call(CPUState *cpu, uint32_t addr, uint32_t ra) {
    (void)cpu; (void)addr; (void)ra;
}
void psx_check_interrupts(CPUState *cpu) { (void)cpu; }
int psx_check_interrupts_at(CPUState *cpu, uint32_t pc) {
    (void)cpu; (void)pc; return 0;
}
int psx_interrupt_delivery_needed(const CPUState *cpu) { (void)cpu; return 0; }
int psx_get_in_exception(void) { return 0; }
uint64_t psx_exception_setjmp_epoch(void) { return 0; }
void psx_restore_state_escape(void) {}
void psx_rfe_mark_escape(void) {}
int psx_syscall(CPUState *cpu, uint32_t code) {
    (void)cpu; (void)code; return 0;
}
void psx_unknown_dispatch(CPUState *cpu, uint32_t addr, uint32_t phys) {
    (void)cpu; (void)addr; (void)phys;
}

void psx_advance_cycles(uint32_t cycles) { (void)cycles; }
uint64_t psx_get_cycle_count(void) { return 0; }
int32_t psx_xenogears_field_frame_step(uint32_t site_pc,
                                       uint32_t instruction_word,
                                       int32_t immediate,
                                       uint32_t register_value,
                                       uint32_t frame_count) {
    (void)site_pc;
    (void)instruction_word;
    (void)immediate;
    (void)register_value;
    (void)frame_count;
    return immediate;
}
int psx_cycle_replay_begin(uint64_t cycle) { (void)cycle; return 1; }
uint64_t psx_cycle_replay_end(void) { return 0; }
uint32_t psx_cyc_load_word(CPUState *cpu, uint32_t addr, uint32_t rt,
                           uint32_t mask) {
    (void)cpu; (void)addr; (void)rt; (void)mask; return 0;
}
uint32_t psx_cyc_load_word_slow(CPUState *cpu, uint32_t addr, uint32_t rt,
                                uint32_t mask) {
    return psx_cyc_load_word(cpu, addr, rt, mask);
}
uint16_t psx_cyc_load_half(CPUState *cpu, uint32_t addr, uint32_t rt,
                           uint32_t mask) {
    (void)cpu; (void)addr; (void)rt; (void)mask; return 0;
}
uint16_t psx_cyc_load_half_slow(CPUState *cpu, uint32_t addr, uint32_t rt,
                                uint32_t mask) {
    return psx_cyc_load_half(cpu, addr, rt, mask);
}
uint8_t psx_cyc_load_byte(CPUState *cpu, uint32_t addr, uint32_t rt,
                          uint32_t mask) {
    (void)cpu; (void)addr; (void)rt; (void)mask; return 0;
}
uint32_t psx_cyc_lwc2_read(CPUState *cpu, uint32_t addr) {
    (void)cpu; (void)addr; return 0;
}
int psx_icache_shadow_record_begin(void) { return 1; }
int psx_icache_shadow_replay_begin(void) { return 1; }
void psx_icache_shadow_replay_end(void) {}
void psx_icache_shadow_abort(void) {}
void psx_icache_fetch(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; }
void psx_icache_fetch_fn(CPUState *cpu, uint32_t addr) {
    psx_icache_fetch(cpu, addr);
}
void psx_muldiv_set(CPUState *cpu, uint32_t latency) {
    (void)cpu; (void)latency;
}
void psx_muldiv_stall(CPUState *cpu) { (void)cpu; }
uint32_t psx_mult_latency_s(uint32_t rs) { (void)rs; return 1; }
uint32_t psx_mult_latency_u(uint32_t rs) { (void)rs; return 1; }
void psx_gte_stall(CPUState *cpu) { (void)cpu; }
void psx_gte_read(CPUState *cpu, uint32_t rt) { (void)cpu; (void)rt; }
int psx_slice_block(CPUState *cpu, uint32_t addr, uint32_t cycles,
                    int side_effects) {
    (void)cpu; (void)addr; (void)cycles; (void)side_effects; return 0;
}
int psx_slice_block_impl(CPUState *cpu, uint32_t addr, uint32_t cycles,
                         int side_effects) {
    return psx_slice_block(cpu, addr, cycles, side_effects);
}

void gte_execute_at(CPUState *cpu, uint32_t cmd, uint32_t guest_pc) {
    (void)cpu;
    (void)cmd;
    (void)guest_pc;
}
void gte_execute_at_tier(CPUState *cpu, uint32_t cmd, uint32_t guest_pc,
                         GteAttributionExecutionTier tier) {
    (void)tier;
    gte_execute_at(cpu, cmd, guest_pc);
}
uint32_t gte_read_data(CPUState *cpu, uint8_t reg) {
    (void)cpu; (void)reg; return 0;
}
uint32_t gte_read_ctrl(CPUState *cpu, uint8_t reg) {
    (void)cpu; (void)reg; return 0;
}
void gte_write_data(CPUState *cpu, uint8_t reg, uint32_t val) {
    (void)cpu; (void)reg; (void)val;
}
void gte_write_ctrl(CPUState *cpu, uint8_t reg, uint32_t val) {
    (void)cpu; (void)reg; (void)val;
}
int32_t psx_ws_plane_nx(int32_t nx) { return nx; }
uint32_t psx_ws_xclip_bound(uint32_t vanilla) { return vanilla; }
void gte_precision_store_word(uint32_t addr, uint8_t reg) {
    (void)addr; (void)reg;
}
void gte_precision_speculative_begin(void) {}
void gte_precision_speculative_end(void) {}
int gte_replay_side_effects_begin(void) { return 1; }
void gte_replay_side_effects_end(void) {}

int ls_shadow_record_begin(void) { return 1; }
int ls_shadow_record_end(uint32_t *ops, int *exc) {
    if (ops) *ops = 0;
    if (exc) *exc = 0;
    return 1;
}
int ls_shadow_replay_begin(void) { return 1; }
int ls_shadow_replay_end(uint32_t *ops, int *kind, uint32_t *pc,
                         uint32_t *addr, uint32_t *expected,
                         uint32_t *actual) {
    (void)ops; (void)kind; (void)pc; (void)addr; (void)expected; (void)actual;
    return 1;
}
void ls_shadow_abort(void) {}

int psx_ws_backdrop_x(int x) { return x; }
int psx_ws_x_margin(void) { return 0; }
void psx_ws_sprite_tag(CPUState *cpu) { (void)cpu; }
uint32_t psx_ws_backdrop_value(uint32_t orig, int end, int cols) {
    (void)end; (void)cols; return orig;
}
int32_t psx_ws_depth_bound(int32_t imm) { return imm; }
int32_t psx_ws_player_x_bound(int32_t vanilla) { return vanilla; }

typedef int (*CounterFn)(void);

#ifdef _WIN32
typedef HMODULE TestModule;
static int module_is_loaded(const char *path) {
    return GetModuleHandleA(path) != NULL;
}
static TestModule module_open(const char *path) { return LoadLibraryA(path); }
static CounterFn module_counter(TestModule module, const char *name) {
    return (CounterFn)GetProcAddress(module, name);
}
static void module_close(TestModule module) { if (module) FreeLibrary(module); }
#else
typedef void *TestModule;
static int module_is_loaded(const char *path) {
#ifndef RTLD_NOLOAD
#error "pair-dedup handle ownership test requires RTLD_NOLOAD"
#else
    void *module = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    if (!module) return 0;
    dlclose(module);
    return 1;
#endif
}
static TestModule module_open(const char *path) {
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
static CounterFn module_counter(TestModule module, const char *name) {
    return (CounterFn)dlsym(module, name);
}
static void module_close(TestModule module) { if (module) dlclose(module); }
#endif

static int counter_value(const char *path, const char *name) {
    TestModule module = module_open(path);
    if (!module) return -1000;
    CounterFn counter = module_counter(module, name);
    int value = counter ? counter() : -1001;
    module_close(module);
    return value;
}

static int expect_int(const char *what, long long actual, long long expected) {
    if (actual == expected) return 1;
    fprintf(stderr, "%s: got %lld, expected %lld\n", what, actual, expected);
    return 0;
}

static uint32_t loader_owner_count(void) {
    uint32_t loads = 0;
    overlay_loader_get_counters(&loads, NULL, NULL, NULL, NULL, NULL,
                                NULL, NULL, NULL, NULL, NULL);
    return loads;
}

static int replace_suffix(char *out, size_t cap, const char *path,
                          const char *suffix) {
    size_t path_len = strlen(path);
#ifdef _WIN32
    const char *extension = ".dll";
#else
    const char *extension = ".so";
#endif
    size_t extension_len = strlen(extension);
    if (path_len < extension_len ||
        strcmp(path + path_len - extension_len, extension) != 0)
        return 0;
    int written = snprintf(out, cap, "%.*s%s",
                           (int)(path_len - extension_len), path, suffix);
    return written > 0 && (size_t)written < cap;
}

static int reveal_second_pair(const char *second) {
    char pending[1024], ranges[1024], ranges_pending[1024];
    char resident[1024], resident_pending[1024];
    int pending_n = snprintf(pending, sizeof(pending), "%s.pending", second);
    if (pending_n <= 0 || (size_t)pending_n >= sizeof(pending) ||
        !replace_suffix(ranges, sizeof(ranges), second, ".ranges") ||
        !replace_suffix(resident, sizeof(resident), second, ".resident"))
        return 0;
    int ranges_n = snprintf(ranges_pending, sizeof(ranges_pending),
                            "%s.pending", ranges);
    int resident_n = snprintf(resident_pending, sizeof(resident_pending),
                              "%s.pending", resident);
    if (ranges_n <= 0 || (size_t)ranges_n >= sizeof(ranges_pending) ||
        resident_n <= 0 || (size_t)resident_n >= sizeof(resident_pending))
        return 0;
    /* Reveal metadata first and the DLL last, matching transactional publication. */
    if (rename(ranges_pending, ranges) != 0 ||
        rename(resident_pending, resident) != 0 ||
        rename(pending, second) != 0) {
        perror("reveal staged pair");
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s <cache-root> <scenario> <first> <second>\n",
                argv[0]);
        return 2;
    }
    const char *scenario = argv[2];
    const char *first = argv[3];
    const char *second = argv[4];
    memset(s_ram, 0, sizeof(s_ram));
    overlay_loader_init(argv[1], "PAIR-TEST");

    int alias = strcmp(scenario, "alias-at-cap") == 0;
    int variant_chain = strcmp(scenario, "variant-chain") == 0;
    int all_stale = strcmp(scenario, "all-stale") == 0;
    int partial = strcmp(scenario, "partial-first") == 0;
    int rejected = strcmp(scenario, "missing-identity") == 0 ||
                   strcmp(scenario, "lowercase-identity") == 0 ||
                   strcmp(scenario, "flat-cache") == 0;
    int ok = 1;
    const PsxGameIdentity *identity = psx_game_identity_runtime();
    ok &= expect_int("runtime identity", identity != NULL, 1);
    ok &= expect_int("bound runtime identity", psx_game_identity_gate(identity), 1);

    /* Only the first physical pair exists at init. This pins canonical order,
     * then makes rescan responsible for discovering the staged second pair. */
    ok &= expect_int("pre-rescan registered", overlay_loader_registered_count(),
                     rejected || partial ? 0 : (alias ? 4 : 2));
    ok &= expect_int("pre-rescan aliases",
                     (long long)overlay_loader_pair_aliases(), 0);
    ok &= expect_int("pre-rescan owners", loader_owner_count(),
                     rejected || partial ? 0 : 1);
    ok &= expect_int("pre-rescan first init",
                     counter_value(first, "test_init_count"),
                     rejected || partial ? 0 : 1);
    ok &= expect_int("pre-rescan first retained", module_is_loaded(first),
                     rejected || partial ? 0 : 1);
    if (!reveal_second_pair(second)) return 3;
    if (strcmp(scenario, "live-publication") == 0) {
        OverlayPreparedImage *image = overlay_loader_prepare_published(second);
        ok &= expect_int("live publication prepared", image != NULL, 1);
        ok &= expect_int("live publication committed",
                         overlay_loader_commit_published(image), 2);
        ok &= expect_int("live publication lazy index",
                         overlay_loader_lazy_manifest_count(), 4);
    } else {
        overlay_loader_rescan();
    }

    ok &= expect_int("registered", overlay_loader_registered_count(),
                     rejected ? 0 : (alias ? 4 : (partial ? 2 : 4)));
    ok &= expect_int("pair aliases", (long long)overlay_loader_pair_aliases(),
                     alias ? 1 : 0);
    ok &= expect_int("candidate overflow",
                     (long long)overlay_loader_candidate_overflow(), 0);
    ok &= expect_int("loader owners", loader_owner_count(),
                     rejected ? 0 : (alias || partial ? 1 : 2));
    ok &= expect_int("first init", counter_value(first, "test_init_count"),
                     rejected || partial ? 0 : 1);
    ok &= expect_int("second init", counter_value(second, "test_init_count"),
                     rejected || alias ? 0 : 1);

    int first_loaded = module_is_loaded(first);
    int second_loaded = module_is_loaded(second);
    ok &= expect_int("first retained", first_loaded, rejected || partial ? 0 : 1);
    ok &= expect_int("second retained", second_loaded, rejected || alias ? 0 : 1);

    if (variant_chain || all_stale)
        ok &= expect_int("forced stale candidates",
                         overlay_loader_test_force_stale_chain(
                             0x00010000u, all_stale),
                         all_stale ? 2 : 1);

    if (alias) {
        CPUState cpu;
        NativeRenderBaselineConfig config;
        NativeRenderBaselineSnapshot baseline;
        memset(&cpu, 0, sizeof(cpu));
        memset(&config, 0, sizeof(config));
        config.authenticated_producer_address = 0x80010000u;
        config.game_digest = 1u;
        config.max_vblanks = 1u;
        ok &= expect_int("baseline arm", native_render_baseline_arm(&config), 1);
        ok &= expect_int("canonical dispatch",
                         overlay_loader_dispatch(&cpu, 0x80010000u), 1);
        native_render_baseline_snapshot(&baseline);
        ok &= expect_int("native producer count",
                          (long long)baseline.native_calls, 1);
        ok &= expect_int("renderer candidate calls",
                          s_renderer_candidate_calls, 1);
        ok &= expect_int("renderer authority provenance",
                          s_renderer_candidate.authority_provenance, 1);
        ok &= expect_int("renderer pair binding",
                          s_renderer_candidate.pair_bound, 1);
        ok &= expect_int("renderer pair id",
                          (long long)s_renderer_candidate.pair_id,
                          (long long)UINT64_C(0x1020304050607080));
        ok &= expect_int("renderer identity",
                          memcmp(&s_renderer_candidate.identity,
                                 &s_fixture_identity,
                                 sizeof(s_fixture_identity)), 0);
        ok &= expect_int("renderer artifact base",
                          s_renderer_candidate.artifact_base & 0x1FFFFFFFu,
                          0x00010000u);
        ok &= expect_int("renderer artifact size",
                          s_renderer_candidate.artifact_size, 16);
        ok &= expect_int("renderer artifact crc",
                          s_renderer_candidate.artifact_crc32, 0x11111111u);
        ok &= expect_int("renderer producer entry",
                          s_renderer_candidate.producer_entry & 0x1FFFFFFFu,
                          0x00010000u);
        ok &= expect_int("renderer dispatch pc",
                          s_renderer_candidate.dispatch_pc & 0x1FFFFFFFu,
                          0x00010000u);
        ok &= expect_int("renderer function range start",
                          s_renderer_candidate.range_start, 0x00010000u);
        ok &= expect_int("renderer function range size",
                          s_renderer_candidate.range_size, 4);
        ok &= expect_int("canonical marker", cpu.gpr[2], TEST_MARKER);
        ok &= expect_int("canonical call count",
                         counter_value(first, "test_call_count"), 1);
        ok &= expect_int("redundant call count",
                         counter_value(second, "test_call_count"), 0);
        ok &= expect_int("canonical flush count",
                         counter_value(first, "test_flush_count"), 1);
        ok &= expect_int("redundant flush count",
                         counter_value(second, "test_flush_count"), 0);
    }
    if (variant_chain || all_stale) {
        CPUState cpu;
        memset(&cpu, 0, sizeof(cpu));
        ok &= expect_int("variant dispatch",
                         overlay_loader_dispatch(&cpu, 0x80010000u),
                         variant_chain ? 1 : 0);
        ok &= expect_int("loader mismatch calls", s_loader_mismatch_calls,
                         all_stale ? 1 : 0);
        ok &= expect_int("variant marker", cpu.gpr[2],
                         variant_chain ? TEST_MARKER : 0);
        ok &= expect_int("first candidate call count",
                         counter_value(first, "test_call_count"),
                         0);
        ok &= expect_int("fallback candidate call count",
                         counter_value(second, "test_call_count"),
                         variant_chain ? 1 : 0);
    }

    /* A second rescan must neither reacquire an alias handle nor publish
     * another owner/candidate set for an already satisfied physical path. */
    overlay_loader_rescan();
    ok &= expect_int("idempotent registered", overlay_loader_registered_count(),
                     rejected ? 0 : (alias ? 4 : (partial ? 2 : 4)));
    ok &= expect_int("idempotent aliases",
                     (long long)overlay_loader_pair_aliases(), alias ? 1 : 0);
    ok &= expect_int("idempotent owners", loader_owner_count(),
                     rejected ? 0 : (alias || partial ? 1 : 2));

    if (!ok) {
        fprintf(stderr, "loader: %s; lazy=%d overflow=%d\n",
                overlay_loader_last_msg(), overlay_loader_lazy_manifest_count(),
                overlay_loader_lazy_manifest_overflow());
        return 1;
    }
    printf("PASS %s registered=%d aliases=%llu\n", scenario,
           overlay_loader_registered_count(),
           (unsigned long long)overlay_loader_pair_aliases());
    return 0;
}
