#include "autocompile.h"
#include "overlay_loader.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int rescans;
static int commits;

void overlay_loader_rescan(void) {
    rescans++;
}

OverlayPreparedImage *overlay_loader_prepare_published(const char *path) {
    assert(strcmp(path, "/tmp/test-shard.so") == 0);
    return (OverlayPreparedImage *)1;
}

int overlay_loader_commit_published(OverlayPreparedImage *image) {
    assert(image == (OverlayPreparedImage *)1);
    commits++;
    return 1;
}

void overlay_loader_discard_prepared(OverlayPreparedImage *image) {
    assert(image == (OverlayPreparedImage *)1);
}

int main(void) {
    char captures[] = "/tmp/psx-autocompile-captures-XXXXXX";
    int capture_fd = mkstemp(captures);
    assert(capture_fd >= 0);
    assert(write(capture_fd, "[{}]", 4) == 4);
    close(capture_fd);

    autocompile_configure(
        "printf 'PSX_SHARD_PUBLISHED /tmp/test-shard.so\\n"
        "PSX_SHARD_RESULT ok=2 failed=0 skipped=1 capacity_fastpath=3\\n'", ".");
    autocompile_set_cache_paths("/tmp", captures);
    assert(!autocompile_request_plan_repair(1));
    assert(autocompile_request_plan_repair(0));

    char status[4096];
    for (int i = 0; i < 500 && (rescans == 0 || commits == 0); i++) {
        autocompile_poll_main();
        usleep(10000);
    }
    assert(rescans == 1);
    assert(commits == 1);
    autocompile_status_json(status, sizeof(status));
    assert(strstr(status, "\"state\":\"idle\""));
    assert(strstr(status, "\"last_exit\":0"));
    assert(strstr(status, "\"shard_ok\":2"));
    assert(strstr(status, "\"shard_skipped\":1"));
    assert(strstr(status, "PSX_SHARD_RESULT"));
    unlink(captures);
    assert(!autocompile_request_plan_repair(0));
    autocompile_shutdown();
    return 0;
}
