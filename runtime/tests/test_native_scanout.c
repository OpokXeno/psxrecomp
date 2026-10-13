/* Exact logical scanout conversion, without SDL initialization or a GPU.
 * ELF section GC discards unrelated renderer functions. --benchmark compares
 * the previous conversion with the new one, including unchanged count/hash
 * work needed by reference/history checks. */
#include "../src/gpu_gl_renderer.c"

#undef NDEBUG
#include <assert.h>

typedef void (*RowConvert)(uint32_t *, const uint16_t *, uint32_t, uint32_t);

static void baseline_row_15(uint32_t *output, const uint16_t *row, uint32_t x, uint32_t width) {
    for (uint32_t i = 0u; i < width; ++i)
        output[i] = conv_1555_to_rgba8(row[(x + i) & (VRAM_W - 1u)]) | UINT32_C(0xff000000);
}

static void baseline_row_24(uint32_t *output, const uint16_t *row, uint32_t x, uint32_t width) {
    for (uint32_t i = 0u; i < width; ++i) {
        uint32_t color = UINT32_C(0xff000000);
        for (uint32_t channel = 0u; channel < 3u; ++channel) {
            const uint32_t byte_x = (x * 2u + i * 3u + channel) & (VRAM_W * 2u - 1u);
            const uint16_t word = row[byte_x / 2u];
            color |= ((uint32_t)(word >> ((byte_x & 1u) * 8u)) & 255u) << (channel * 8u);
        }
        output[i] = color;
    }
}

static uint32_t reference_15(uint16_t word) {
    const uint32_t red = word & 31u, green = (word >> 5u) & 31u, blue = (word >> 10u) & 31u;
    return UINT32_C(0xff000000) | red * 8u | green * 2048u | blue * 524288u;
}

static void check_rows(void) {
    const uint32_t widths[] = {0u, 1u, 2u, 3u, 7u, 31u, 319u, 320u, 368u, 640u, 1023u, 1024u};
    uint16_t words[VRAM_W];
    uint8_t bytes[VRAM_W * 2u];
    uint32_t output[VRAM_W + 2u];
    uint32_t seed = UINT32_C(0x9e3779b9);
    for (uint32_t i = 0u; i < VRAM_W; ++i) {
        seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
        words[i] = (uint16_t)(seed >> 8u);
        bytes[i * 2u] = (uint8_t)words[i];
        bytes[i * 2u + 1u] = (uint8_t)(words[i] >> 8u);
    }
    for (uint32_t x = 0u; x < VRAM_W; ++x) {
        for (size_t w = 0u; w < sizeof(widths) / sizeof(widths[0]); ++w) {
            const uint32_t width = widths[w];
            memset(output, 0x5a, sizeof(output));
            native_scanout_row_15(output + 1u, words, x, width);
            assert(output[0] == UINT32_C(0x5a5a5a5a));
            assert(output[width + 1u] == UINT32_C(0x5a5a5a5a));
            for (uint32_t i = 0u; i < width; ++i)
                assert(output[i + 1u] == reference_15(words[(x + i) % VRAM_W]));
            memset(output, 0x5a, sizeof(output));
            native_scanout_row_24(output + 1u, words, x, width);
            assert(output[0] == UINT32_C(0x5a5a5a5a));
            assert(output[width + 1u] == UINT32_C(0x5a5a5a5a));
            for (uint32_t i = 0u; i < width; ++i) {
                const uint32_t start = x * 2u + i * 3u;
                const uint32_t expected = UINT32_C(0xff000000) |
                    bytes[start % sizeof(bytes)] |
                    ((uint32_t)bytes[(start + 1u) % sizeof(bytes)] << 8u) |
                    ((uint32_t)bytes[(start + 2u) % sizeof(bytes)] << 16u);
                assert(output[i + 1u] == expected);
            }
        }
    }
    /* Every RGB555/mask-bit combination, independently of wrapping fixtures. */
    for (uint32_t page = 0u; page < 64u; ++page) {
        for (uint32_t i = 0u; i < VRAM_W; ++i) words[i] = (uint16_t)(page * VRAM_W + i);
        native_scanout_row_15(output, words, 0u, VRAM_W);
        for (uint32_t i = 0u; i < VRAM_W; ++i) assert(output[i] == reference_15(words[i]));
    }
    puts("logical scanout: all row origins, odd/wrapped widths and all 65536 RGB555 words verified");
}

static void check_pixel_hash(void) {
    enum { capacity = 4096 };
    uint32_t pixels[capacity];
    uint8_t bytes[sizeof(pixels) + 7u];
    uint32_t seed = UINT32_C(0x12345678);
    for (size_t i = 0u; i < capacity; ++i) {
        seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
        pixels[i] = i % 37u ? seed : seed & UINT32_C(0xff000000);
    }
    for (size_t offset = 0u; offset < 8u; ++offset) {
        memcpy(bytes + offset, pixels, sizeof(pixels));
        uint64_t expected_visible = 0u;
        for (size_t count = 0u; count <= capacity; ++count) {
            if (count) expected_visible += (pixels[count - 1u] & UINT32_C(0x00ffffff)) != 0u;
            const uint64_t expected = pres_hash_bytes(bytes + offset, count * sizeof(uint32_t));
            const uint64_t initial = count % 2u ? UINT64_MAX - 2u : 123u;
            uint64_t visible = initial;
            assert(pres_hash_pixels_visible(bytes + offset, count, &visible) == expected);
            assert(visible == initial + expected_visible);
        }
    }
    uint64_t visible = 42u;
    assert(pres_hash_pixels_visible(NULL, 0u, &visible) == GL_PRESENT_HASH_FNV_OFFSET);
    assert(visible == 42u);
    puts("pixel hash: exact previous digest/count for lengths 0..4096 and byte alignments 0..7");
}

static uint64_t prepare_frame(RowConvert volatile convert, const uint16_t *words,
        uint32_t *pixels, uint32_t x, uint32_t width, uint32_t height, bool fused) {
    for (uint32_t y = 0u; y < height; ++y)
        convert(pixels + (size_t)y * width, words + (size_t)y * VRAM_W, x, width);
    uint64_t visible = 0u;
    const size_t count = (size_t)width * height;
    if (fused) {
        const uint64_t digest = pres_hash_pixels_visible(pixels, count, &visible);
        return digest ^ visible;
    }
    for (size_t i = 0u; i < count; ++i) visible += (pixels[i] & UINT32_C(0x00ffffff)) != 0u;
    return pres_hash_bytes((const uint8_t *)pixels, count * sizeof(*pixels)) ^ visible;
}

static void benchmark_scanout(bool hash_only) {
    enum { iterations = 100, trials = 5 };
    const struct { uint32_t bits, x, width, height; } cases[] = {
        {15u, 0u, 320u, 224u}, {15u, 900u, 320u, 224u}, {15u, 900u, 640u, 480u},
        {24u, 0u, 320u, 224u}, {24u, 900u, 320u, 224u}, {24u, 900u, 640u, 480u},
    };
    uint16_t *words = malloc((size_t)VRAM_W * VRAM_H * sizeof(*words));
    uint32_t *pixels = malloc((size_t)VRAM_W * VRAM_H * sizeof(*pixels));
    assert(words && pixels);
    for (size_t i = 0u; i < VRAM_W * VRAM_H; ++i) words[i] = (uint16_t)(i * 40503u);
    for (size_t c = 0u; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        RowConvert optimized = cases[c].bits == 15u ? native_scanout_row_15 : native_scanout_row_24;
        RowConvert original = hash_only ? optimized : cases[c].bits == 15u ? baseline_row_15 : baseline_row_24;
        const uint64_t expected = prepare_frame(original, words, pixels,
            cases[c].x, cases[c].width, cases[c].height, false);
        assert(prepare_frame(optimized, words, pixels, cases[c].x, cases[c].width, cases[c].height, true) == expected);
        for (unsigned trial = 0u; trial < trials; ++trial) {
            uint64_t digests[2] = {0};
            for (unsigned pass = 0u; pass < 2u; ++pass) {
                const bool use_new = (pass ^ (trial & 1u)) != 0u;
                const uint64_t started = native_thread_cpu_ns();
                assert(started);
                for (unsigned i = 0u; i < iterations; ++i) {
                    words[0] ^= 1u; /* Real mutations prevent invariant-frame elimination. */
                    digests[use_new] += prepare_frame(use_new ? optimized : original, words, pixels,
                        cases[c].x, cases[c].width, cases[c].height, use_new);
                }
                const uint64_t elapsed = native_thread_cpu_ns() - started;
                printf("bits=%u x=%u width=%u height=%u trial=%u optimized=%d iterations=%u cpu_ns=%llu\n",
                    cases[c].bits, cases[c].x, cases[c].width, cases[c].height,
                    trial, use_new, iterations, (unsigned long long)elapsed);
            }
            assert(digests[0] == digests[1]);
        }
    }
    free(words); free(pixels);
}

int main(int argc, char **argv) {
    check_rows();
    check_pixel_hash();
    if (argc == 2 && strcmp(argv[1], "--benchmark") == 0) benchmark_scanout(false);
    if (argc == 2 && strcmp(argv[1], "--benchmark-hash") == 0) benchmark_scanout(true);
    return 0;
}
