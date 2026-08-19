#include "boot_state.h"
#include "game_identity.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RAM_SIZE (2u * 1024u * 1024u)

static uint8_t s_ram[RAM_SIZE];
static uint8_t s_spad[1024];
static uint8_t s_spuram[1];

uint8_t *memory_get_ram_ptr(void) { return s_ram; }
uint8_t *memory_get_scratchpad_ptr(void) { return s_spad; }
uint32_t dirty_ram_get_bitmap_word(uint32_t index) { (void)index; return 0; }
uint32_t dirty_ram_get_bitmap_word_count(void) { return 1; }
void dirty_ram_set_bitmap_words(const uint32_t *words, uint32_t count) {
    (void)words;
    (void)count;
}
void psx_kernel_bless_note_range(uint32_t phys, uint32_t len) {
    (void)phys;
    (void)len;
}

uint32_t i_stat;
uint32_t i_mask;
uint64_t psx_cycle_count;
uint32_t g_psx_cyc_batch;
uint32_t g_psx_cyc_batch_limit;
uint32_t *g_psx_cyc_local_acc;
int g_ls_replay_active;
int g_event_step_conservative;
int psx_in_device_service;
uint64_t psx_next_service_cycle;
uint32_t g_psx_icache_tv[1024];
int g_psx_vram_dirty_tracking;

static uint32_t s_cycles_since_vblank;
static const uint64_t s_clean_vram_rows[8];

void psx_devices_service_to_now(void) {}
void psx_advance_cycles_slow(uint32_t cycles) { (void)cycles; }
uint32_t interrupts_get_cycles_since_vblank(void) {
    return s_cycles_since_vblank;
}
void interrupts_set_cycles_since_vblank(uint32_t value) {
    s_cycles_since_vblank = value;
}

void timers_get_snapshot(uint16_t counter[3], uint32_t mode[3],
                         uint16_t target[3], int32_t irq_line[3],
                         uint32_t frac[3]) {
    memset(counter, 0, sizeof(uint16_t) * 3);
    memset(mode, 0, sizeof(uint32_t) * 3);
    memset(target, 0, sizeof(uint16_t) * 3);
    memset(irq_line, 0, sizeof(int32_t) * 3);
    memset(frac, 0, sizeof(uint32_t) * 3);
}
void timers_set_snapshot(const uint16_t counter[3], const uint32_t mode[3],
                         const uint16_t target[3], const int32_t irq_line[3],
                         const uint32_t frac[3]) {
    (void)counter;
    (void)mode;
    (void)target;
    (void)irq_line;
    (void)frac;
}

uint32_t gpu_snapshot_bytes(void) { return 0; }
void gpu_snapshot_write(uint8_t *out) { (void)out; }
int gpu_snapshot_read(const uint8_t *in, uint32_t len) { (void)in; return len == 0; }
uint32_t spu_snapshot_bytes(void) { return 0; }
void spu_snapshot_write(uint8_t *out) { (void)out; }
int spu_snapshot_read(const uint8_t *in, uint32_t len) { (void)in; return len == 0; }
uint8_t *spu_get_ram_ptr(void) { return s_spuram; }
uint32_t spu_get_ram_bytes(void) { return sizeof(s_spuram); }
void spu_ram_copy_out(uint8_t *out, uint32_t len) {
    if (out && len <= sizeof(s_spuram)) memcpy(out, s_spuram, len);
}
int spu_ram_copy_in(const uint8_t *in, uint32_t len) {
    if (!in || len != sizeof(s_spuram)) return 0;
    memcpy(s_spuram, in, len);
    return 1;
}
uint32_t cdrom_snapshot_bytes(void) { return 0; }
void cdrom_snapshot_write(uint8_t *out) { (void)out; }
int cdrom_snapshot_read(const uint8_t *in, uint32_t len) { (void)in; return len == 0; }
uint32_t dma_snapshot_bytes(void) { return 0; }
void dma_snapshot_write(uint8_t *out) { (void)out; }
int dma_snapshot_read(const uint8_t *in, uint32_t len) { (void)in; return len == 0; }
uint32_t sio_snapshot_bytes(void) { return 0; }
void sio_snapshot_write(uint8_t *out) { (void)out; }
int sio_snapshot_read(const uint8_t *in, uint32_t len) { (void)in; return len == 0; }
uint32_t mdec_snapshot_bytes(void) { return 0; }
void mdec_snapshot_write(uint8_t *out) { (void)out; }
int mdec_snapshot_read(const uint8_t *in, uint32_t len) {
    (void)in;
    return len == 0;
}

const uint16_t *gpu_get_vram(void) { return NULL; }
int gpu_vram_dirty_tracking(void) { return 0; }
uint32_t gpu_vram_dirty_row_count(void) { return 0; }
const uint64_t *gpu_vram_dirty_mask(void) { return s_clean_vram_rows; }
int gpu_vram_dirty_verify_enabled(void) { return 0; }
void gpu_vram_dirty_clear(void) {}

void overlay_watch_invalidate_after_ram_restore(void) {}
void gte_canonicalize_cpu_state(CPUState *cpu) { (void)cpu; }

void gr_vram_transfer_in(int x, int y, int width, int height, const uint16_t *pixels) {
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)pixels;
}
void gr_vram_transfer_out(int x, int y, int width, int height, uint16_t *pixels) {
    (void)x;
    (void)y;
    memset(pixels, 0, (size_t)width * (size_t)height * sizeof(*pixels));
}

static int check(int condition, const char *message) {
    if (condition) return 1;
    fprintf(stderr, "FAIL: %s\n", message);
    return 0;
}

int main(void) {
    const char *path = "test_game_identity_snapshot.pst";
    CPUState cpu;
    const PsxGameIdentity *identity = psx_game_identity_runtime();
    PsxGameIdentity mismatch;
    char game_sha256[PSX_GAME_IDENTITY_SHA256_HEX_BYTES];
    char manifest_sha256[PSX_GAME_IDENTITY_SHA256_HEX_BYTES];
    char cache_namespace[PSX_GAME_IDENTITY_CACHE_NAMESPACE_BYTES];
    int ok = 1;

    memset(&cpu, 0, sizeof(cpu));
    ok &= check(identity != NULL, "runtime identity is configured");
    ok &= check(psx_game_identity_bind_static(identity), "static identity binds");
    ok &= check(psx_game_identity_gate(identity), "matching identity passes gate");
    ok &= check(psx_game_identity_format_hex(identity, game_sha256,
                                             manifest_sha256),
                "runtime identity formats for compiler arguments");
    ok &= check(strcmp(
                    game_sha256,
                    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f") == 0,
                "game identity uses canonical lowercase SHA-256 hex");
    ok &= check(strcmp(
                    manifest_sha256,
                    "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f") == 0,
                "manifest identity uses canonical lowercase SHA-256 hex");
    ok &= check(strcmp(game_sha256, manifest_sha256) != 0,
                "runtime identity pair uses distinct SHA-256 values");
    ok &= check(psx_game_identity_cache_namespace(
                    cache_namespace, sizeof(cache_namespace)),
                "identity cache namespace fits its declared size");
    ok &= check(strlen(cache_namespace) ==
                    PSX_GAME_IDENTITY_CACHE_NAMESPACE_BYTES - 1u,
                "identity cache namespace is complete");
    mismatch = *identity;
    mismatch.game_sha256[4] ^= 1u;
    ok &= check(memcmp(identity->game_sha256, mismatch.game_sha256,
                       sizeof(uint32_t)) == 0,
                "mismatch fixture preserves the first uint32");
    ok &= check(!psx_game_identity_equal(identity, &mismatch),
                "same first uint32 cannot authorize a different SHA-256");
    ok &= check(!psx_game_identity_gate(&mismatch),
                "mismatched identity fails gate");
    ok &= check(boot_state_save(&cpu, 0x12345678u, 0x80010000u, path),
                "identity-bearing snapshot saves");
    ok &= check(boot_state_load(path, 0x12345678u, 0x80010000u, &cpu),
                "matching identity snapshot loads");

    {
        FILE *snapshot = fopen(path, "r+b");
        uint8_t byte;
        ok &= check(snapshot != NULL, "snapshot opens for stale-identity mutation");
        if (snapshot) {
            ok &= check(fseek(snapshot, BOOT_STATE_GAME_IDENTITY_OFFSET, SEEK_SET) == 0,
                        "game identity offset is seekable");
            ok &= check(fread(&byte, 1, 1, snapshot) == 1,
                        "game identity byte is readable");
            byte ^= 1u;
            ok &= check(fseek(snapshot, BOOT_STATE_GAME_IDENTITY_OFFSET, SEEK_SET) == 0,
                        "game identity offset rewinds");
            ok &= check(fwrite(&byte, 1, 1, snapshot) == 1,
                        "game identity byte is writable");
            fclose(snapshot);
        }
    }
    ok &= check(!boot_state_load(path, 0x12345678u, 0x80010000u, &cpu),
                "stale game identity snapshot is rejected");
    remove(path);
    return ok ? 0 : 1;
}
