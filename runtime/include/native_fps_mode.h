#ifndef PSXRECOMP_NATIVE_FPS_MODE_H
#define PSXRECOMP_NATIVE_FPS_MODE_H

typedef enum {
    NATIVE_FPS_MODE_ORIGINAL = 0,
    NATIVE_FPS_MODE_NATIVE_59_94 = 1,
} NativeFpsMode;

typedef enum {
    NATIVE_FPS_STARTUP_ORIGINAL_DEFAULT = 0,
    NATIVE_FPS_STARTUP_EXPLICIT = 1,
} NativeFpsStartupReason;

typedef struct {
    NativeFpsMode requested_mode;
    NativeFpsStartupReason startup_reason;
    int synthesis_disabled;
} NativeFpsStartupPolicy;

#endif
