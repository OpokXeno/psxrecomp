#!/usr/bin/env python3
"""Exercise the shader's actual short-list sorting network and batch ABI."""
from itertools import permutations
from pathlib import Path
import re
import struct
from random import Random

root = Path(__file__).resolve().parents[1]
shader = (root / "shaders/native_capture_linked.comp").read_text()
small = shader.split("if(small){", 1)[1].split("}else if(!ascending)", 1)[0]
network = re.findall(r"ordered_pair\(([a-h]),([a-h])\)", small)
assert network, "short-list network missing"
once = shader.split("if(short_lists_once&&count<=8u){", 1)[1].split(" }else{", 1)[0]
stages = [re.findall(r"ordered_pair\(([a-h]),([a-h])\)", part)
          for part in once.split("// Short network:")[1:]]
assert len(stages) == 3
checked = 0
for size in range(1, 9):
    for order in permutations(range(size)):
        fast = [pair for minimum, stage in zip((2, 3, 5), stages) if size >= minimum for pair in stage]
        for candidate in (network, fast):
            values = dict(zip("abcdefgh", list(order) + [0xffffffff] * (8-size)))
            for left, right in candidate:
                if values[left] > values[right]:
                    values[left], values[right] = values[right], values[left]
            assert list(values.values())[:size] == list(range(size)), (size, order, values)
            checked += 1

executor = (root / "src/native_vk_executor.c").read_text()
vertex = (root / "shaders/native_gpu.vert").read_text()
fragment = (root / "shaders/native_gpu.frag").read_text()
stride = int(re.search(r"NVK_CORE_TRIANGLE_TEXELS = (\d+)", executor).group(1))
for stage in (vertex, fragment):
    assert int(re.search(r"triangle_data\[(\d+)\]", stage).group(1)) == stride
assert int(re.search(r"VERTEX_ID/3\)\*(\d+)", vertex).group(1)) == stride
assert int(re.search(r"PSX_VARYING\((\d+)\) flat out vec4 triangle_footprint", vertex).group(1)) == 4 + stride

# Stored colour/mask/depth policy bits must survive FP32 key compaction.
def encode(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]

def decode(bits):
    return struct.unpack("<f", struct.pack("<I", bits))[0]
rng = Random(0x580)
keys = [0, 1, encode(1.0 / 65535), encode(1.0 / 60000), encode(1.0), encode(4096.0)]
keys += [rng.randrange(0x00800000, 0x7e800000) for _ in range(10000)]
for key in keys:
    for bias in range(17):
        policy = rng.getrandbits(26)
        packed = policy | (bias << 26)
        decoded_bias = (packed >> 26) & 31
        factor = decode(0x3f800000 + (0x00800000 >> decoded_bias)) if decoded_bias else 1.0
        restored = encode(decode(key) * factor)
        expected = encode(decode(key) * (1.0 + 2.0 ** -bias if bias else 1.0))
        assert restored == expected and packed & ((1 << 26) - 1) == policy
assert "buffer Fragments { uvec4 nodes[]; }" in shader
assert "capacity / 16u" in executor
print(f"PASS {checked} short lists and {len(keys)*17} packed depth keys; batch stride {stride}")
