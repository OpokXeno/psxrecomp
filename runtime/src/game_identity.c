#include "game_identity.h"

#include <string.h>

static PsxGameIdentity s_runtime_identity;
static int s_runtime_initialized;
static int s_runtime_valid;
static PsxGameIdentity s_static_identity;
static int s_static_bound;

#if defined(PSX_GAME_EXTRA_IDENTITY_SHA256) && defined(PSX_GAME_MANIFEST_DIGEST_SHA256)
static int hex_nibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

static int decode_sha256(const char *text, uint8_t output[PSX_GAME_IDENTITY_SHA256_BYTES]) {
    if (!text || strlen(text) != PSX_GAME_IDENTITY_SHA256_BYTES * 2u) return 0;
    for (size_t index = 0; index < PSX_GAME_IDENTITY_SHA256_BYTES; index++) {
        const int high = hex_nibble(text[index * 2u]);
        const int low = hex_nibble(text[index * 2u + 1u]);
        if (high < 0 || low < 0) return 0;
        output[index] = (uint8_t)((high << 4) | low);
    }
    return 1;
}
#endif

int psx_game_identity_equal(const PsxGameIdentity *left,
                            const PsxGameIdentity *right) {
    return left && right && memcmp(left, right, sizeof(*left)) == 0;
}

const PsxGameIdentity *psx_game_identity_runtime(void) {
    if (!s_runtime_initialized) {
        s_runtime_initialized = 1;
#if defined(PSX_GAME_EXTRA_IDENTITY_SHA256) && defined(PSX_GAME_MANIFEST_DIGEST_SHA256)
        s_runtime_valid = decode_sha256(PSX_GAME_EXTRA_IDENTITY_SHA256,
                                        s_runtime_identity.game_sha256) &&
                          decode_sha256(PSX_GAME_MANIFEST_DIGEST_SHA256,
                                        s_runtime_identity.manifest_sha256);
#endif
    }
    return s_runtime_valid ? &s_runtime_identity : NULL;
}

int psx_game_identity_bind_static(const PsxGameIdentity *identity) {
    const PsxGameIdentity *runtime = psx_game_identity_runtime();
    if (!runtime || !identity || !psx_game_identity_equal(runtime, identity)) return 0;
    if (!s_static_bound) {
        s_static_identity = *identity;
        s_static_bound = 1;
    }
    return psx_game_identity_equal(&s_static_identity, identity);
}

int psx_game_identity_gate(const PsxGameIdentity *identity) {
    return identity && s_static_bound &&
           psx_game_identity_equal(&s_static_identity, identity);
}

static void encode_sha256_hex(
        const uint8_t digest[PSX_GAME_IDENTITY_SHA256_BYTES],
        char output[PSX_GAME_IDENTITY_SHA256_HEX_BYTES]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t index = 0; index < PSX_GAME_IDENTITY_SHA256_BYTES; index++) {
        output[index * 2u] = hex[digest[index] >> 4u];
        output[index * 2u + 1u] = hex[digest[index] & 0x0fu];
    }
    output[PSX_GAME_IDENTITY_SHA256_HEX_BYTES - 1u] = '\0';
}

int psx_game_identity_format_hex(
        const PsxGameIdentity *identity,
        char game_sha256[PSX_GAME_IDENTITY_SHA256_HEX_BYTES],
        char manifest_sha256[PSX_GAME_IDENTITY_SHA256_HEX_BYTES]) {
    if (!identity || !game_sha256 || !manifest_sha256) return 0;
    encode_sha256_hex(identity->game_sha256, game_sha256);
    encode_sha256_hex(identity->manifest_sha256, manifest_sha256);
    return 1;
}

int psx_game_identity_cache_namespace(char *out, size_t out_size) {
    const PsxGameIdentity *identity = psx_game_identity_runtime();
    char game_sha256[PSX_GAME_IDENTITY_SHA256_HEX_BYTES];
    char manifest_sha256[PSX_GAME_IDENTITY_SHA256_HEX_BYTES];
    size_t offset = 0;
    if (!identity || !out ||
        out_size < PSX_GAME_IDENTITY_CACHE_NAMESPACE_BYTES) return 0;
    if (!psx_game_identity_format_hex(identity, game_sha256,
                                      manifest_sha256)) return 0;
    memcpy(out + offset, "game_", 5u);
    offset += 5u;
    memcpy(out + offset, game_sha256, PSX_GAME_IDENTITY_SHA256_BYTES * 2u);
    offset += PSX_GAME_IDENTITY_SHA256_BYTES * 2u;
    memcpy(out + offset, "/manifest_", 10u);
    offset += 10u;
    memcpy(out + offset, manifest_sha256, PSX_GAME_IDENTITY_SHA256_BYTES * 2u);
    offset += PSX_GAME_IDENTITY_SHA256_BYTES * 2u;
    out[offset] = '\0';
    return 1;
}
