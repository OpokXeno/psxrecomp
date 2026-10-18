#!/usr/bin/env python3
"""Exercise actual ImGui geometry rather than treating its scissor as coverage."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
ui = Path(os.environ.get('RECOMP_UI_ROOT', str(root / 'recomp-ui')))
imgui = ui / 'src/third_party/imgui'
with tempfile.TemporaryDirectory(prefix='runtime-menu-bounds-') as directory:
    binary = Path(directory) / 'test'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O0', '-g0',
                    '-I', str(imgui), '-I', str(root / 'psxrecomp/runtime/src'),
                    str(root / 'psxrecomp/runtime/tests/test_runtime_menu_draw_bounds.cpp'),
                    *[str(imgui / name) for name in ('imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp')],
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
