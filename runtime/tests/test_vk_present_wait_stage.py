#!/usr/bin/env python3
from pathlib import Path
import re

source = (Path(__file__).parents[1] / "src" / "gpu_vk_renderer.c").read_text()
match = re.search(
    r"static void submit_present\([^)]*\)\s*\{.*?^\}",
    source,
    flags=re.DOTALL | re.MULTILINE,
)
assert match, "submit_present definition not found"
body = match.group(0)
assert "VK_PIPELINE_STAGE_TRANSFER_BIT" in body
assert "VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT" in body
assert "pWaitDstStageMask = &wait_stage" in body
print("Vulkan present wait-stage test passed")
