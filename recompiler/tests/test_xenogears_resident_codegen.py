#!/usr/bin/env python3
import argparse
import os
import shutil
import struct
import subprocess
import tempfile

SITE = 0x800758E4


def make_exe(word: int, site: int = SITE) -> bytes:
    body = bytearray(12)
    struct.pack_into("<I", body, 0, word)
    struct.pack_into("<I", body, 4, 0x03E00008)
    header = bytearray(2048)
    header[0:8] = b"PS-X EXE"
    struct.pack_into("<I", header, 0x10, site)
    struct.pack_into("<I", header, 0x18, site)
    struct.pack_into("<I", header, 0x1C, len(body))
    return bytes(header) + body


def generate(recompiler: str, word: int, overlay: bool, root: str, site: int = SITE,
             cutover_transfer: str | None = None) -> str:
    psx = os.path.join(root, f"{site:08X}-{word:08X}.psx")
    seeds = os.path.join(root, f"{site:08X}-{word:08X}.txt")
    out = os.path.join(root, f"out-{site:08X}-{word:08X}-{int(overlay)}")
    os.makedirs(out)
    with open(psx, "wb") as stream:
        stream.write(make_exe(word, site))
    with open(seeds, "w", encoding="ascii") as stream:
        stream.write(f"0x{site:08X}\n")
    command = [recompiler, psx, "--seeds", seeds, "--out-dir", out]
    if cutover_transfer is not None:
        plan = os.path.join(root, f"plan-{site:08X}-{cutover_transfer}.txt")
        with open(plan, "w", encoding="ascii", newline="\n") as stream:
            stream.write("psxrecomp-source-observation-plan-v5\n")
            stream.write(
                f"cutover {site:08X} {word:08X} {cutover_transfer} 00000000\n")
        command.extend(("--source-observation-plan", plan))
    if overlay:
        command.append("--overlay")
    result = subprocess.run(command, capture_output=True, text=True, cwd=root)
    if result.returncode != 0:
        raise SystemExit(result.stderr or result.stdout)
    chunks = []
    for name in os.listdir(out):
        if "_full" in name and name.endswith(".c"):
            with open(os.path.join(out, name), encoding="utf-8") as stream:
                chunks.append(stream.read())
    return "\n".join(chunks)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--recompiler", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as root:
        bios = os.path.join(root, "bios")
        os.makedirs(bios)
        framework = os.path.dirname(os.path.dirname(os.path.dirname(args.recompiler)))
        shutil.copyfile(os.path.join(framework, "SCPH1001.toml"),
                        os.path.join(bios, "SCPH1001.toml"))
        exact = generate(args.recompiler, 0x24630002, True, root)
        wrong = generate(args.recompiler, 0x24630003, True, root)
        static = generate(args.recompiler, 0x24630002, False, root)
        ft4_first = generate(args.recompiler, 0xE90C0000, False, root, 0x8004A7E8)
        ft4_wrong_site = generate(args.recompiler, 0xE90C0000, False, root, 0x8004A7EC)
        ft4_overlay = generate(args.recompiler, 0xE90C0000, True, root, 0x8004A7E8)
        resident_cutover = generate(
            args.recompiler, 0x27BDFF80, False, root, 0x800273C4, "return")
        model_observer = generate(
            args.recompiler, 0x27BDFFD0, False, root, 0x8002C700, "observe")
        model_post_observer = generate(
            args.recompiler, 0xE8B60000, False, root, 0x8004A1AC,
            "observe-after")
    helper = "psx_xenogears_field_frame_step"
    old = ("psx_xenogears_field_vsync_arg", "psx_xenogears_field_skip_discrete",
           "psx_xenogears_field_scale_delta", "psx_xenogears_field_integrate_velocity_hook")
    assert exact.count(helper) == 2 and "0x800758E4u" in exact and "0x24630002u" in exact
    assert "cpu->gpr[17]" in exact
    assert wrong.count(helper) == 1
    assert static.count(helper) == 1 and "cpu->gpr[17]" in static
    assert all(symbol not in output for output in (exact, wrong, static) for symbol in old)
    resident_ft4 = "psx_xg_render_auth_resident_ft4_observe"
    assert ft4_first.count(resident_ft4) == 2
    assert ft4_first.index("psx_gte_stall(cpu)") < ft4_first.index(resident_ft4, ft4_first.index("psx_gte_stall(cpu)"))
    first_pre = ft4_first.index("cpu, 0u, 0x8004A7E8u, 0xE90C0000u")
    store = ft4_first.index("cpu->write_word(_pgxa, _pgxv)")
    first_commit = ft4_first.index("cpu, 1u, 0x8004A7E8u, 0xE90C0000u")
    assert first_pre < store < first_commit
    assert resident_ft4 not in ft4_wrong_site
    assert resident_ft4 not in ft4_overlay
    resident_bypass = (
        "psx_xg_render_auth_native_ft4_bypass(cpu, 0x800273C4u, 0x27BDFF80u)")
    assert resident_bypass in resident_cutover
    assert "cpu->pc = cpu->gpr[31]; return;" in resident_cutover
    model_bypass = (
        "psx_xg_render_auth_native_ft4_bypass(cpu, 0x8002C700u, 0x27BDFFD0u)")
    assert model_bypass in model_observer
    assert "if (" + model_bypass not in model_observer
    model_post_bypass = (
        "psx_xg_render_auth_native_ft4_bypass(cpu, 0x8004A1ACu, 0xE8B60000u)")
    model_post_store = "cpu->write_word(_pgxa, _pgxv)"
    assert model_post_observer.index(model_post_store) < \
        model_post_observer.index(model_post_bypass)
    assert "if (" + model_post_bypass not in model_post_observer
    with open(os.path.join(framework, "runtime", "src", "dirty_ram_interp.c"),
              encoding="utf-8") as stream:
        interpreter = stream.read()
    assert "pc == 0x800758E4u && insn == 0x24630002u" in interpreter
    assert "psx_xenogears_field_frame_step(" in interpreter
    assert "pc, insn, simm, cpu->gpr[17]" in interpreter
    assert all(symbol not in interpreter for symbol in old)
    assert "pc == 0x8004A7E8u && insn == 0xE90C0000u" in interpreter
    assert "pc == 0x8004A814u && insn == 0xE90E0000u" in interpreter
    assert interpreter.count("psx_xg_render_auth_resident_ft4_observe(") == 2
    print("PASS: exact resident field hooks preserve frame-step and FT4 store boundaries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
