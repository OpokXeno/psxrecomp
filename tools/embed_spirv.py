#!/usr/bin/env python3
"""embed_spirv.py — compile GLSL shaders to SPIR-V (glslc) and emit one C header.

Each input shader (*.vert / *.frag / *.comp) is compiled to SPIR-V and written
into the output header as a `static const uint32_t spv_<name>_<stage>[]` array
plus its byte length, so the Vulkan backend can create shader modules with zero
runtime file dependencies (keeps the self-contained exe). Stage is inferred from
the file extension.

Variants: a shader may contain directive lines of the exact form

    //! variant <SUFFIX> <DEFINE>[=<value>] [<DEFINE>[=<value>] ...]

For each such line the shader is compiled once more with `-D<DEFINE>[=<value>]`
for every listed define, and emitted as an extra array named
`spv_<name>_<stage>_<SUFFIX>` (plus `_size`). The base array is always emitted.
SUFFIX must match [A-Za-z0-9_]+ and be unique within the shader. Variant lines
are read only from the shader files given on the command line, not from
included files.

Includes: every compile gets `-I <directory of the shader>`, so a shader can
`#include "common.glsl"` relative to its own directory (or via a path in that
directory). glslc enables GL_GOOGLE_include_directive itself, so no
`#extension ... : require` line is needed or injected; a shader may still
declare it explicitly. Shared include files must use the `.glsl` extension and
must not be passed on the command line (they are not compiled on their own;
the CMake glob only picks up .vert/.frag/.comp).

Depfile: with `--depfile <path>`, each compile runs with `-MD -MF` and the
results are merged into one Makefile-style rule
`<header.h>: <every shader> <every included file>`, so CMake (DEPFILE) rebuilds
the header when an included .glsl changes.

Usage:
  embed_spirv.py --glslc <glslc> --out <header.h> [--depfile <deps.d>] shader1.vert shader2.frag ...
"""
import argparse, os, re, subprocess, sys, struct, tempfile

STAGE = {".vert": "vert", ".frag": "frag", ".comp": "comp"}
VARIANT_RE = re.compile(r"^//! variant (.*)$")
SUFFIX_RE = re.compile(r"^[A-Za-z0-9_]+$")
DEFINE_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(=\S+)?$")

def ident(path):
    base = os.path.basename(path)
    name, ext = os.path.splitext(base)
    if ext not in STAGE:
        raise ValueError("%s: unsupported extension '%s' (expected .vert, .frag or .comp; "
                         "shared .glsl includes are not compiled on their own)" % (path, ext))
    return "%s_%s" % (name.replace("-", "_"), STAGE[ext]), STAGE[ext]

def parse_variants(path):
    """Return [(suffix, [define, ...]), ...] from `//! variant` lines in path."""
    variants, seen = [], set()
    with open(path, "r", encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            m = VARIANT_RE.match(line.rstrip("\r\n"))
            if not m:
                continue
            toks = m.group(1).split()
            where = "%s:%d" % (path, lineno)
            if not toks:
                raise ValueError("%s: variant line has no suffix" % where)
            suffix, defines = toks[0], toks[1:]
            if not SUFFIX_RE.match(suffix):
                raise ValueError("%s: variant suffix '%s' must match [A-Za-z0-9_]+" % (where, suffix))
            if suffix in seen:
                raise ValueError("%s: duplicate variant suffix '%s' in %s" % (where, suffix, path))
            if not defines:
                raise ValueError("%s: variant '%s' lists no defines" % (where, suffix))
            for d in defines:
                if not DEFINE_RE.match(d):
                    raise ValueError("%s: malformed define '%s'" % (where, d))
            seen.add(suffix)
            variants.append((suffix, defines))
    return variants

def read_depfile(path):
    """Parse a glslc/make depfile into its dependency list (target ignored)."""
    with open(path, "r", encoding="utf-8") as f:
        text = f.read().replace("\\\n", " ")
    m = re.search(r"(?<!\\):(?=\s|$)", text)
    if not m:
        return []
    deps = re.findall(r"(?:\\[ \t#$]|[^ \t\n\\]|\\(?![ \t#$]))+", text[m.end():])
    return [re.sub(r"\\([ \t#$])", r"\1", d) for d in deps]

def write_depfile(path, target, deps):
    def esc(p):
        return p.replace("$", "$$").replace(" ", "\\ ").replace("#", "\\#")
    lines = [esc(target) + ": \\"]
    for i, d in enumerate(deps):
        lines.append("  " + esc(d) + (" \\" if i + 1 < len(deps) else ""))
    if not deps:
        lines[0] = esc(target) + ":"
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--glslc", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--depfile", help="write a Makefile-style depfile for the header")
    ap.add_argument("shaders", nargs="+")
    a = ap.parse_args()

    try:
        jobs = []  # (shader, array_name, [extra -D flags], stage)
        for sh in a.shaders:
            name, stage = ident(sh)
            jobs.append((sh, "spv_%s" % name, [], stage))
            for suffix, defines in parse_variants(sh):
                jobs.append((sh, "spv_%s_%s" % (name, suffix),
                             ["-D" + d for d in defines], stage))
    except ValueError as e:
        sys.stderr.write("embed_spirv.py: %s\n" % e)
        return 1

    names = [j[1] for j in jobs]
    dupes = sorted({n for n in names if names.count(n) > 1})
    if dupes:
        sys.stderr.write("embed_spirv.py: duplicate output array names: %s\n" % ", ".join(dupes))
        return 1

    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    out = ["/* Auto-generated by embed_spirv.py — do not edit. */",
           "#ifndef PSX_VK_SHADERS_SPV_H",
           "#define PSX_VK_SHADERS_SPV_H",
           "#include <stdint.h>", ""]
    deps = []  # every shader and every included file, first-seen order

    # Scratch dir for the intermediate .spv files. It MUST NOT be the shader
    # source dir: every runtime target (psx-runtime, psx-oracle, each game exe)
    # runs this script over the SAME shaders, and under `ninja -j N` two of them
    # overlap. Writing "<shader>.spv" next to the source made them race — one
    # process removed the file the other had just written, and the loser died
    # with FileNotFoundError on a shader it had compiled successfully. Keeping
    # the scratch per-invocation makes concurrent runs independent, and the
    # TemporaryDirectory also stops a crash from leaving .spv litter in the
    # source tree. The per-compile .d files live in the same scratch dir.
    with tempfile.TemporaryDirectory(prefix="psx_spirv_") as scratch:
      for idx, (sh, arr, defs, stage) in enumerate(jobs):
        spv = os.path.join(scratch, "%d_%s.spv" % (idx, os.path.basename(sh)))
        dep = spv + ".d"
        cmd = [a.glslc, "--target-env=vulkan1.1", "-O",
               "-I", os.path.dirname(os.path.abspath(sh)) or ".", *defs,
               sh, "-o", spv]
        if a.depfile:
            cmd += ["-MD", "-MF", dep]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            label = sh if not defs else "%s (%s)" % (sh, " ".join(defs))
            sys.stderr.write("glslc failed for %s:\n%s\n" % (label, r.stderr))
            return 1
        if a.depfile:
            for d in read_depfile(dep) + [sh]:
                if d not in deps:
                    deps.append(d)
        with open(spv, "rb") as f:
            data = f.read()
        os.remove(spv)
        if a.depfile:
            os.remove(dep)
        if len(data) % 4 != 0:
            sys.stderr.write("SPIR-V for %s not 4-byte aligned\n" % sh)
            return 1
        words = struct.unpack("<%dI" % (len(data) // 4), data)
        out.append("static const uint32_t %s[] = {" % arr)
        line = "  "
        for w in words:
            tok = "0x%08x," % w
            if len(line) + len(tok) > 96:
                out.append(line); line = "  "
            line += tok
        if line.strip():
            out.append(line)
        out.append("};")
        out.append("static const uint32_t %s_size = sizeof(%s);" % (arr, arr))
        out.append("")

    out.append("#endif")
    with open(a.out, "w") as f:
        f.write("\n".join(out))
    if a.depfile:
        write_depfile(a.depfile, a.out, deps)
    return 0

if __name__ == "__main__":
    sys.exit(main())
