#!/usr/bin/env python3
"""Structural regression checks for bounded overlay-candidate registration.

The full loader depends on the emulator runtime and has no isolated unit-test
link seam. These checks pin the fail-closed capacity contract in both platform
branches; the normal MinGW runtime build supplies the compilation/link proof.
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
LOADER = ROOT / "runtime" / "src" / "overlay_loader.c"
DEBUG_SERVER = ROOT / "runtime" / "src" / "debug_server.c"


def function_body(source: str, name: str, start_at: int = 0) -> str:
    match = re.search(
        rf"\b(?:static\s+)?(?:int|void|uint64_t)\s+{re.escape(name)}\s*"
        rf"\([^;]*?\)\s*\{{",
        source[start_at:],
        re.S,
    )
    if not match:
        raise AssertionError(f"missing function definition: {name}")
    start = start_at + match.end()
    depth = 1
    for pos in range(start, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos]
    raise AssertionError(f"unterminated function definition: {name}")


def require(pattern: str, text: str, message: str) -> None:
    if not re.search(pattern, text, re.S):
        raise AssertionError(message)


def main() -> int:
    source = LOADER.read_text(encoding="utf-8")
    debug = DEBUG_SERVER.read_text(encoding="utf-8")

    cap_match = re.search(r"^#define\s+CAND_CAP\s+(\d+)\s*$", source, re.M)
    if not cap_match:
        raise AssertionError("CAND_CAP must remain an auditable integer literal")
    cap = int(cap_match.group(1))
    if cap < 17_506 or cap & (cap - 1):
        raise AssertionError(f"CAND_CAP must be power-of-two and >= 17,506, got {cap}")
    require(r"#define\s+IDX_CAP\s+\(CAND_CAP\s*\*\s*2u\)", source,
            "candidate hash index must scale at 2x CAND_CAP")
    require(r"#define\s+IDX_MASK\s+\(IDX_CAP\s*-\s*1u\)", source,
            "candidate hash mask must track IDX_CAP")
    require(r"#define\s+RANGE_LINK_CAP\s+\(CAND_CAP\s*\*\s*8\)", source,
            "candidate range links must scale with candidate storage")
    require(r"#define\s+LAZY_ENTRY_CAP\s+\(CAND_CAP\s*\*\s*2u\)", source,
            "lazy entry index must scale at 2x CAND_CAP")
    require(r"#define\s+LAZY_ENTRY_MASK\s+\(LAZY_ENTRY_CAP\s*-\s*1u\)", source,
            "lazy entry hash mask must track LAZY_ENTRY_CAP")
    require(r"#define\s+LAZY_MAN_CAP\s+\(CAND_CAP\s*\*\s*2\)", source,
            "lazy manifest storage must scale at 2x CAND_CAP")
    require(r"#define\s+LAZY_RANGE_LINK_CAP\s+\(LAZY_MAN_CAP\s*\*\s*8\)", source,
            "lazy range links must scale with lazy manifest storage")

    register = function_body(source, "cand_register")
    require(r"if\s*\(s_cand_n\s*>=\s*CAND_CAP\)\s*\{\s*"
            r"s_cand_overflow\+\+;\s*return\s+0\s*;", register,
            "cand_register must count and reject overflow before allocation")
    require(r"\bs_cand_n\+\+", register,
            "cand_register must allocate from the bounded candidate array")
    require(r"return\s+1\s*;\s*$", register,
            "cand_register must report successful registration")
    if len(re.findall(r"registered\s*\+=\s*cand_register\s*\(", source)) != 2:
        raise AssertionError("both platform loaders must count cand_register success honestly")

    suppress = function_body(source, "cache_entry_suppress_at_capacity")
    require(r"s_cand_n\s*<\s*CAND_CAP", suppress,
            "capacity suppression must occur only at the permanent full state")
    require(r"e->capacity_suppressed\s*=\s*1", suppress,
            "full bundles must be memoized to prevent per-dispatch loader churn")
    require(r"overlay_image_warm_drop_all\s*\(\)", suppress,
            "permanent capacity exhaustion must release speculative DLL mappings")
    warm_queue = function_body(source, "overlay_image_warm_queue")
    require(r"s_capacity_warm_dropped\s*\|\|\s*"
            r"s_cand_n\s*>=\s*CAND_CAP", warm_queue,
            "warm queue must stay disabled after permanent capacity exhaustion")
    require(r"s_cand_overflow\s*\+=\s*dropped", suppress,
            "suppressed manifest identities must remain visible in telemetry")

    load_one = function_body(source, "load_one_dll")
    guard_pos = load_one.find("cache_entry_suppress_at_capacity")
    load_pos = load_one.find("load_overlay_dll")
    if guard_pos < 0 or load_pos < 0 or guard_pos > load_pos:
        raise AssertionError("capacity guard must run before platform DLL loading")

    first_loader = source.index("static int load_overlay_dll")
    windows_start = source.rfind("#ifdef _WIN32", 0, first_loader)
    if windows_start < 0:
        raise AssertionError("missing Windows overlay-loader branch")
    posix_start = source.index("#else", windows_start)
    platform_end = source.index("#endif", posix_start)
    windows = source[windows_start:posix_start]
    posix = source[posix_start:platform_end]
    require(r"if\s*\(registered\s*==\s*0\)\s*\{[^}]*"
            r"s_dll_flush\[dll\]\s*=\s*NULL\s*;[^}]*FreeLibrary\(h\)", windows,
            "Windows zero-candidate loads must clear callbacks and release the DLL")
    require(r"if\s*\(registered\s*==\s*0\)\s*\{[^}]*"
            r"s_dll_flush\[dll\]\s*=\s*NULL\s*;[^}]*"
            r"psx_overlay_posix_library_close\(h\)", posix,
            "POSIX zero-candidate loads must clear callbacks and release the DLL")

    require(r'\\"candidate_overflow\\":%llu', debug,
            "debug status JSON must expose candidate overflow")
    require(r"overlay_loader_candidate_overflow\(\)", debug,
            "debug status must source candidate overflow from the loader getter")

    print("PASS: overlay candidate capacity is bounded, visible, and fail-closed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, ValueError) as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
