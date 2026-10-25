#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='native-depth-bounds-') as directory:
    binary = Path(directory) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-Wall', '-Wextra',
                    '-I', str(root / 'runtime/src'), str(root / 'runtime/tests/test_native_vk_depth_bounds.c'),
                    '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True)
