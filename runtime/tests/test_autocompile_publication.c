#include "autocompile.h"
#include "overlay_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void autocompile_test_feed_output(const char *buf, int n);

static int prepared;
static int committed;
static int discarded;

OverlayPreparedImage *overlay_loader_prepare_published(const char *path) {
    (void)path;
    prepared++;
    return (OverlayPreparedImage *)1;
}

int overlay_loader_commit_published(OverlayPreparedImage *image) {
    (void)image;
    committed++;
    return 1;
}

void overlay_loader_discard_prepared(OverlayPreparedImage *image) {
    (void)image;
    discarded++;
}

void overlay_loader_rescan(void) {
    fputs("FAIL: deferred activation attempted a live cache rescan\n", stderr);
    abort();
}

int main(void) {
    enum { ITEM_COUNT = 300 };
    autocompile_poll_main();
    autocompile_configure("unused", ".");

    size_t capacity = (size_t)ITEM_COUNT * 96u;
    char *text = (char *)malloc(capacity);
    if (!text) return 1;
    int length = snprintf(text, capacity,
        "PSX_SHARD_RESULT ok=17 failed=0 skipped=4\n");
    for (unsigned i = 0; i < ITEM_COUNT; ++i) {
        length += snprintf(text + length, capacity - (size_t)length,
            "PSX_SHARD_PUBLISHED C:\\cache\\00010000_DEADBEEF_%08X.dll\n", i);
    }
    autocompile_test_feed_output(text, length);
    free(text);

    char status[2048];
    autocompile_status_json(status, sizeof(status));
    if (!strstr(status, "\"shard_ok\":17") ||
        !strstr(status, "\"shard_fail\":0") ||
        !strstr(status, "\"shard_skipped\":4") ||
        !strstr(status, "\"shard_result_seen\":1") ||
        !strstr(status, "\"publish_deferred_run\":300") ||
        !strstr(status, "\"activation\":\"next_launch\"")) {
        fprintf(stderr, "FAIL: deferred publication status is incomplete: %s\n",
                status);
        return 1;
    }
    if (strstr(status, "PSX_SHARD_RESULT") || prepared || committed || discarded) {
        fprintf(stderr,
                "FAIL: publication entered live loader prep=%d commit=%d discard=%d\n",
                prepared, committed, discarded);
        return 1;
    }

    autocompile_shutdown();
    puts("PASS: live autocompile output is retained for diagnostics while all "
         "overlay activation is deferred to the next launch");
    return 0;
}
