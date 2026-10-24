#!/usr/bin/env python3
"""Exercise the executor's actual descriptor lookup across collisions/resets."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "src/native_vk_executor.c").read_text()
key = source.split("typedef struct NvkSetKey {", 1)[1].split("} NvkSetKey;", 1)[0]
lookup = source.split("static VkDescriptorSet nvk_set0_cached", 1)[1].split(
    "/* Set 0 of the native pipeline", 1)[0]
harness = r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
typedef uint64_t VkImageView, VkSampler, VkBufferView, VkBuffer, VkDescriptorSet;
#define VK_NULL_HANDLE 0
typedef struct NvkSetKey { KEY } NvkSetKey;
typedef struct { NvkSetKey key; VkDescriptorSet set; } NvkSetEntry;
static struct {
    NvkSetEntry set0[4096];
    uint16_t set0_hints[1024];
    uint32_t set0_count;
    int descriptor_hints;
} S;
static struct { uint64_t descriptor_hint_hits, descriptor_hint_misses; } P;
static VkDescriptorSet nvk_set0_cached LOOKUP
int main(void) {
    S.descriptor_hints = 1;
    NvkSetKey a = {.dest=0x1000, .hd=0x2000, .recipe=1};
    NvkSetKey b = a;
    b.recipe = 2; /* omitted from the hash, deliberately collides */
    S.set0[0] = (NvkSetEntry){a, 101};
    S.set0[1] = (NvkSetEntry){b, 102};
    S.set0_count = 2;
    uint32_t bucket;
    for (unsigned i=0; i<1000; ++i) {
        assert(nvk_set0_cached(&a, &bucket) == 101);
        assert(nvk_set0_cached(&a, &bucket) == 101);
        assert(nvk_set0_cached(&b, &bucket) == 102);
    }
    assert(P.descriptor_hint_hits >= 1000);
    /* Pool reset: old indices and descriptor handles remain in the hint
     * storage but no entry is live. Then reuse index zero for another key. */
    S.set0_count = 0;
    assert(nvk_set0_cached(&b, &bucket) == VK_NULL_HANDLE);
    S.set0[0] = (NvkSetEntry){a, 201};
    S.set0_count = 1;
    assert(nvk_set0_cached(&b, &bucket) == VK_NULL_HANDLE);
    assert(nvk_set0_cached(&a, &bucket) == 201);
    /* A full cache stores index 4096 in uint16_t without truncation. */
    S.set0[4095] = (NvkSetEntry){b, 4096};
    S.set0_count = 4096;
    assert(nvk_set0_cached(&b, &bucket) == 4096);
    assert(S.set0_hints[bucket] == 4096);
    assert(nvk_set0_cached(&b, &bucket) == 4096);
    S.set0_count = 0;
    assert(nvk_set0_cached(&b, &bucket) == VK_NULL_HANDLE);
    S.set0[0] = (NvkSetEntry){b, 301};
    S.set0_count = 1;
    assert(nvk_set0_cached(&b, &bucket) == 301);
    S.descriptor_hints = 0;
    assert(nvk_set0_cached(&b, &bucket) == 301);
    return 0;
}
'''.replace("KEY", key).replace("LOOKUP", lookup)
with tempfile.TemporaryDirectory(prefix="xg-descriptor-hints-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(harness)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS descriptor key collisions, pool reset, capacity rollover, disabled path")
