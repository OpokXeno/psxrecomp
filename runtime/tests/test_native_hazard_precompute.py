#!/usr/bin/env python3
"""Check cached separating axes against the original pairwise arithmetic."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='native-hazard-precompute-') as directory:
    binary = Path(directory) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-Wall', '-Wextra',
                    '-I', str(root / 'runtime/src'), str(root / 'runtime/tests/test_native_hazard_precompute.c'),
                    '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
