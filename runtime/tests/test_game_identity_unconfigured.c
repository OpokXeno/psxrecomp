#include "game_identity.h"

#include <stdint.h>
#include <stdio.h>

int main(void) {
    PsxGameIdentity static_identity = {{0}, {0}};
    char game_sha256[PSX_GAME_IDENTITY_SHA256_HEX_BYTES];
    char manifest_sha256[PSX_GAME_IDENTITY_SHA256_HEX_BYTES];
    int ok = 1;

    if (psx_game_identity_runtime() != NULL) {
        fprintf(stderr, "FAIL: unconfigured runtime exposed an identity\n");
        ok = 0;
    }
    if (psx_game_identity_format_hex(NULL, game_sha256, manifest_sha256)) {
        fprintf(stderr, "FAIL: missing identity formatted compiler arguments\n");
        ok = 0;
    }
    if (psx_game_identity_bind_static(&static_identity)) {
        fprintf(stderr, "FAIL: missing runtime identity allowed static registration\n");
        ok = 0;
    }
    if (psx_game_identity_gate(&static_identity)) {
        fprintf(stderr, "FAIL: missing runtime identity authorized native dispatch\n");
        ok = 0;
    }
    return ok ? 0 : 1;
}
