/* Both paths share the faithful integer transforms; compare FIFO bytes,
 * parser stop/counter state and the published RGBA frame across affinities. */
#include "../src/mdec.c"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#if defined(__linux__)
#include <time.h>
#endif

uint64_t s_frame_count;
uint64_t psx_cycle_count;
void gpu_get_display_info(GpuDisplayInfo *out) {
    memset(out, 0, sizeof(*out));
    out->width = 320u;
}

static uint32_t seed = 0x1234abcdu;
static uint16_t random_word(void) {
    seed = seed * 1664525u + 1013904223u;
    return (uint16_t)(seed >> 16);
}

static void compare(uint32_t depth, uint32_t signed_output, uint32_t bit15, uint32_t input_end) {
    mdec.output_depth = depth;
    mdec.output_signed = signed_output;
    mdec.output_bit15 = bit15;
    mdec.input_count = input_end;
    const uint64_t generation = mdec_native_frame_generation;
    execute_decode_serial();
    const uint32_t size = mdec.output_size;
    uint8_t *expected = malloc(size ? size : 1u);
    assert(expected);
    memcpy(expected, mdec.output, size);
    const uint32_t blocks = mdec.decode_blocks, macroblocks = mdec.decode_macroblocks;
    const uint32_t stop = mdec.decode_stop_reason, position = mdec.decode_input_pos;
    const size_t frame_bytes = (size_t)mdec_native_frame_width * mdec_native_frame_height * 4u;
    uint32_t *frame = frame_bytes ? malloc(frame_bytes) : NULL;
    if (frame_bytes) { assert(frame); memcpy(frame, mdec_native_frame, frame_bytes); }
    const uint32_t state_bytes = mdec_snapshot_bytes();
    uint8_t *state = malloc(state_bytes), *actual_state = malloc(state_bytes);
    assert(state && actual_state);
    mdec_snapshot_write(state);
    if (mdec_native_frame_generation != generation) {
        const uint32_t width = mdec_native_frame_width, height = mdec_native_frame_height;
        for (uint32_t y = 0u; y < height; ++y)
            for (uint32_t x = 0u; x < width; ++x) {
                const uint32_t index = ((y / 16u) * (width / 16u) + x / 16u) * 256u +
                    (y % 16u) * 16u + x % 16u;
                uint32_t rgb;
                if (depth == 3u) {
                    const uint16_t pixel = expected[index * 2u] | (uint16_t)expected[index * 2u + 1u] << 8u;
                    rgb = (pixel & 31u) << 19u | ((pixel >> 5u) & 31u) << 11u | ((pixel >> 10u) & 31u) << 3u;
                } else rgb = (uint32_t)expected[index * 3u] << 16u |
                    (uint32_t)expected[index * 3u + 1u] << 8u | expected[index * 3u + 2u];
                assert(frame[(size_t)y * width + x] == (UINT32_C(0xff000000) | rgb));
            }
    }
    execute_decode();
    assert(mdec.output_size == size && mdec.output_pos == 0u);
    assert(memcmp(expected, mdec.output, size) == 0);
    assert(mdec.decode_blocks == blocks && mdec.decode_macroblocks == macroblocks);
    assert(mdec.decode_stop_reason == stop && mdec.decode_input_pos == position);
    if (frame_bytes) assert(memcmp(frame, mdec_native_frame, frame_bytes) == 0);
    assert(mdec_snapshot_bytes() == state_bytes);
    mdec_snapshot_write(actual_state);
    assert(memcmp(state, actual_state, state_bytes) == 0);
    free(state); free(actual_state); free(frame); free(expected);
}

int main(int argc, char **argv) {
    mdec_init();
    assert(ensure_input_capacity(65536u));
    for (uint32_t i = 0u; i < 64u; ++i) {
        mdec.y_quant[i] = 1u + i;
        mdec.uv_quant[i] = 64u - i;
        mdec.scale[i] = (int16_t)((int)(random_word() % 8192u) - 4096);
    }
    uint32_t words = 0u;
    /* 300 color macroblocks, with nonzero AC and padding markers. */
    for (uint32_t b = 0u; b < 1800u; ++b) {
        if (b % 17u == 0u) mdec.input[words++] = 0xfe00u;
        mdec.input[words++] = random_word() & 0x7fffu;
        for (uint32_t c = 0u; c < 5u; ++c)
            mdec.input[words++] = random_word() & 0x1fffu;
        mdec.input[words++] = 0xfe00u;
    }
#if defined(__linux__)
    if (argc == 2 && strcmp(argv[1], "--benchmark") == 0) {
        mdec.input_count = words; mdec.output_depth = 2;
        execute_decode();
        for (unsigned parallel = 0; parallel < 2u; ++parallel) {
            struct timespec start, end;
            clock_gettime(CLOCK_MONOTONIC, &start);
            for (unsigned frame = 0u; frame < 100u; ++frame)
                if (parallel) execute_decode(); else execute_decode_serial();
            clock_gettime(CLOCK_MONOTONIC, &end);
            const double ms = (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1e6;
            printf("MDEC %s: %.3f ms/frame\n", parallel ? "parallel" : "serial", ms / 100.0);
        }
        return 0;
    }
#else
    (void)argc; (void)argv;
#endif
    for (uint32_t depth = 0u; depth < 4u; ++depth)
        for (uint32_t sign = 0u; sign < 2u; ++sign)
            for (uint32_t bit = 0u; bit < 2u; ++bit) {
                compare(depth, sign, bit, words);
                for (uint32_t tail = 0u; tail < 17u; ++tail)
                    compare(depth, sign, bit, tail);
                compare(depth, sign, bit, words - 3u);
            }
    puts("MDEC: FIFO, RGBA, signed/bit15 modes and truncated streams match serial");
    free(mdec.input); free(mdec.output); free(mdec_native_frame);
    return 0;
}
