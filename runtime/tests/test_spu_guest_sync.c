/* Device-boundary regressions for the guest-clock SPU producer. The real
 * replay is the audio acceptance check; this harness checks causal ordering
 * independently of SDL's playback clock and the frontend's frame batching. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/spu.c"

uint64_t s_frame_count;
uint32_t g_debug_last_store_pc;
uint32_t g_debug_current_func_addr;
static uint64_t guest_cycle;
uint64_t psx_get_cycle_count(void) { return guest_cycle; }
void audio_trace_pcm(int tap, const int16_t *data, int frames) {
    (void)tap; (void)data; (void)frames;
}
void audio_trace_event(uint16_t kind, uint32_t a, uint32_t b) {
    (void)kind; (void)a; (void)b;
}
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)bit; (void)detail; }
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    (void)data; (void)len; return crc;
}
bool spu_shadow_enabled(void) { return false; }
void spu_shadow_reset(void) {}
void spu_shadow_process(int16_t *canon, int frames) {
    (void)canon; (void)frames;
}

static int failures;
#define CHECK(expr) do { if (!(expr)) { \
    printf("FAIL line %d: %s\n", __LINE__, #expr); ++failures; \
} } while (0)

static int16_t output[2352 * 8 * 2];
static unsigned output_frames;

static void sync_guest(void) {
    /* The test owns the source sample schedule. The device must request it
     * before changing state, including when no VBlank occurs between calls. */
    unsigned due = (unsigned)(guest_cycle / 768u) - output_frames;
    while (due) {
        unsigned chunk = due > 256 ? 256 : due;
        spu_render(output + output_frames * 2, (int)chunk);
        output_frames += chunk;
        due -= chunk;
    }
}

static void reset(void) {
    spu_set_sync_callback(NULL);
    guest_cycle = 0;
    output_frames = 0;
    memset(output, 0, sizeof(output));
    spu_init();
    spu_write(0x1F801DAA, 0xC001); /* SPU + CD, no reverb */
    spu_write(0x1F801D80, 0x3FFF);
    spu_write(0x1F801D82, 0x3FFF);
    spu_write(0x1F801DB0, 0x7FFF);
    spu_write(0x1F801DB2, 0x7FFF);
    spu_set_sync_callback(sync_guest);
}

static void test_sector_continuity(void) {
    int16_t sector[2352 * 2];
    reset();
    for (unsigned i = 0; i < 2352; ++i) {
        sector[2 * i] = 1000;
        sector[2 * i + 1] = -1000;
    }
    for (unsigned n = 0; n < 8; ++n) {
        guest_cycle = (uint64_t)n * 2352u * 768u;
        spu_cd_audio_push(sector, 2352);
        CHECK(output_frames == n * 2352u);
        CHECK(cd_frame_count == 2352u);
        /* Uneven sync partitions model host work between device boundaries.
         * Repeating a read at the same guest cycle must consume nothing. */
        guest_cycle += 137u * 768u + 767u;
        (void)spu_read(0x1F801DAE);
        CHECK(output_frames == n * 2352u + 137u);
        (void)spu_read(0x1F801DAE);
        CHECK(output_frames == n * 2352u + 137u);
    }
    guest_cycle = 8u * 2352u * 768u;
    (void)spu_read(0x1F801DAE);
    CHECK(output_frames == 8u * 2352u);
    CHECK(cd_underflow_frames == 0);
    CHECK(cd_overflow_frames == 0);
    for (unsigned i = 0; i < output_frames; ++i) {
        /* Two documented signed Q15 multiplications, CD then main gain. */
        CHECK(output[2 * i] == 998);
        CHECK(output[2 * i + 1] == -1000);
    }
}

static void test_volume_and_cd_reset_boundaries(void) {
    const int16_t input[] = {1000, -1000, 1000, -1000,
                             1000, -1000, 1000, -1000};
    reset();
    spu_cd_audio_push(input, 4);
    guest_cycle = 2u * 768u;
    spu_write(0x1F801DB0, 0); /* elapsed left samples retain their old gain */
    CHECK(output_frames == 2);
    CHECK(output[0] == 998 && output[2] == 998);
    guest_cycle = 4u * 768u;
    spu_cd_audio_reset(); /* drain elapsed samples before clearing the queue */
    CHECK(output_frames == 4);
    CHECK(output[4] == 0 && output[6] == 0);
    CHECK(output[5] == -1000 && output[7] == -1000);
}

static void test_late_sector_does_not_fill_past_time(void) {
    const int16_t input[] = {1000, -1000, 1000, -1000};
    reset();
    spu_cd_audio_push(input, 2);
    guest_cycle = 4u * 768u;
    spu_cd_audio_push(input, 2);
    CHECK(output_frames == 4);
    CHECK(cd_underflow_frames == 2);
    CHECK(output[4] == 0 && output[6] == 0);
    CHECK(cd_frame_count == 2);
    /* DMA capture must also observe the SPU at the current guest time. */
    guest_cycle = 6u * 768u;
    (void)spu_dma_read();
    CHECK(output_frames == 6);
    CHECK(output[8] == 998 && output[10] == 998);
    guest_cycle = 7u * 768u;
    spu_dma_write(0);
    CHECK(output_frames == 7);
    spu_set_sync_callback(NULL);
    guest_cycle += 768u;
    (void)spu_read(0x1F801DAE);
    CHECK(output_frames == 7);
}

int main(void) {
    test_sector_continuity();
    test_volume_and_cd_reset_boundaries();
    test_late_sector_does_not_fill_past_time();
    spu_set_sync_callback(NULL);
    printf("SPU guest sync: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
