#ifndef PSX_GAME_IDENTITY_TYPES_H
#define PSX_GAME_IDENTITY_TYPES_H

#include <stdint.h>

#define PSX_GAME_IDENTITY_SHA256_BYTES 32u
#define PSX_GAME_IDENTITY_SHA256_HEX_BYTES 65u
#define PSX_GAME_IDENTITY_CACHE_NAMESPACE_BYTES 144u

typedef struct PsxGameIdentity {
    uint8_t game_sha256[PSX_GAME_IDENTITY_SHA256_BYTES];
    uint8_t manifest_sha256[PSX_GAME_IDENTITY_SHA256_BYTES];
} PsxGameIdentity;

#endif
