#!/usr/bin/env python3
"""Build/run the randomized dependency-plan equivalence test without SDL/Vulkan."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='native-sched-bounds-') as directory:
    binary = Path(directory) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-Wall', '-Wextra',
                    '-I', str(root / 'runtime/src'), str(root / 'runtime/tests/test_native_sched_bounds.c'),
                    str(root / 'runtime/src/native_gpu_hazard.c'), '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
