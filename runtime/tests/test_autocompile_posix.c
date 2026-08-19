#include "autocompile.h"
#include "overlay_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        abort(); \
    } \
} while (0)

static int rescans;
static int commits;

#define TAIL_EVICTION_NOISE \
    "i=0; while [ \"$i\" -lt 100 ]; do " \
    "printf 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" \
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\\n'; i=$((i + 1)); done; "

#ifdef __linux__
#define COMPILE_POLICY_PROBE \
    "python3 -c 'import os; print(\"PSX_POLICY %d %d %d\" % " \
    "(os.sched_getscheduler(0), os.getpriority(os.PRIO_PROCESS, 0), " \
    "len(os.sched_getaffinity(0))))'; "
#else
#define COMPILE_POLICY_PROBE ""
#endif

void overlay_loader_rescan(void) {
    rescans++;
}

OverlayPreparedImage *overlay_loader_prepare_published(const char *path) {
    CHECK(strcmp(path, "/tmp/test-shard.so") == 0);
    return (OverlayPreparedImage *)1;
}

int overlay_loader_commit_published(OverlayPreparedImage *image) {
    CHECK(image == (OverlayPreparedImage *)1);
    commits++;
    return 1;
}

void overlay_loader_discard_prepared(OverlayPreparedImage *image) {
    CHECK(image == (OverlayPreparedImage *)1);
}

int main(void) {
    char compiler_dir[] = "/tmp/psx-autocompile-compiler-XXXXXX";
    CHECK(mkdtemp(compiler_dir) != NULL);
    char compiler_path[512];
    CHECK(snprintf(compiler_path, sizeof(compiler_path), "%s/gcc",
                   compiler_dir) > 0);
    FILE *compiler = fopen(compiler_path, "wb");
    CHECK(compiler != NULL);
    CHECK(fputs("#!/bin/sh\nexit 0\n", compiler) >= 0);
    CHECK(fclose(compiler) == 0);
    CHECK(chmod(compiler_path, 0700) == 0);
    const char *saved_path = getenv("PATH");
    char *saved_path_copy = saved_path ? strdup(saved_path) : NULL;
    CHECK(setenv("PATH", compiler_dir, 1) == 0);
    const char *detected_compiler = autocompile_c_compiler();
    CHECK(detected_compiler != NULL);
    CHECK(detected_compiler[0] == '/');
    CHECK(strcmp(detected_compiler, compiler_path) == 0);
    CHECK(autocompile_toolchain_available());
    if (saved_path_copy) {
        CHECK(setenv("PATH", saved_path_copy, 1) == 0);
        free(saved_path_copy);
    } else {
        CHECK(unsetenv("PATH") == 0);
    }
    CHECK(unlink(compiler_path) == 0);
    CHECK(rmdir(compiler_dir) == 0);

    char captures[] = "/tmp/psx-autocompile-captures-XXXXXX";
    int capture_fd = mkstemp(captures);
    CHECK(capture_fd >= 0);
    CHECK(write(capture_fd, "[{}]", 4) == 4);
    close(capture_fd);

    autocompile_configure(
        "printf 'PSX_SHARD_RESULT ok=1 failed=0 skipped=1 "
        "capacity_fastpath=3\\n'; "
        TAIL_EVICTION_NOISE
        COMPILE_POLICY_PROBE
        "printf 'PSX_SHARD_PUBLISHED /tmp/test-shard.so\\n'", ".");
    autocompile_set_cache_paths("/tmp", captures);
    CHECK(!autocompile_request_plan_repair(1));
    CHECK(autocompile_request_plan_repair(0));

    char status[4096];
    for (int i = 0; i < 500 && autocompile_busy(); i++) {
        autocompile_poll_main();
        usleep(10000);
    }
    CHECK(rescans == 0);
    CHECK(commits == 0);
    autocompile_status_json(status, sizeof(status));
    CHECK(strstr(status, "\"state\":\"idle\""));
    CHECK(strstr(status, "\"last_exit\":0"));
    CHECK(strstr(status, "\"shard_ok\":1"));
    CHECK(strstr(status, "\"shard_skipped\":1"));
    CHECK(strstr(status, "\"shard_result_seen\":1"));
    CHECK(strstr(status, "\"publish_commit_run\":0"));
    CHECK(strstr(status, "\"publish_deferred_run\":1"));
    CHECK(strstr(status, "\"activation\":\"next_launch\""));
    CHECK(!strstr(status, "PSX_SHARD_RESULT"));
#ifdef __linux__
    CHECK(strstr(status, "PSX_POLICY 5 19 1"));
#endif

    /* Missing or malformed marker streams cannot cause gameplay-time recovery
     * work. Startup owns activation and complete cache discovery. */
    autocompile_configure(
        "printf 'PSX_SHARD_RESULT ok=1 failed=0 skipped=0\\n'", ".");
    CHECK(autocompile_request());
    for (int i = 0; i < 500 && autocompile_busy(); i++) {
        autocompile_poll_main();
        usleep(10000);
    }
    CHECK(rescans == 0);
    CHECK(commits == 0);
    unlink(captures);
    CHECK(!autocompile_request_plan_repair(0));
    autocompile_shutdown();
    return 0;
}
