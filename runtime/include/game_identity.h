#ifndef PSX_GAME_IDENTITY_H
#define PSX_GAME_IDENTITY_H

#include <stddef.h>
#include "game_identity_types.h"

#ifdef __cplusplus
extern "C" {
#endif

int psx_game_identity_equal(const PsxGameIdentity *left,
                            const PsxGameIdentity *right);
const PsxGameIdentity *psx_game_identity_runtime(void);
int psx_game_identity_bind_static(const PsxGameIdentity *identity);
int psx_game_identity_gate(const PsxGameIdentity *identity);
int psx_game_identity_format_hex(
    const PsxGameIdentity *identity,
    char game_sha256[PSX_GAME_IDENTITY_SHA256_HEX_BYTES],
    char manifest_sha256[PSX_GAME_IDENTITY_SHA256_HEX_BYTES]);
int psx_game_identity_cache_namespace(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
