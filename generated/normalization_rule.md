# Address Normalization Rule — Phase 1e

## Rule

A function's identity is its **canonical physical address**, computed
in two steps:

### Step 1: KSEG strip

    physical = virtual_address & 0x1FFFFFFF

This removes the MIPS KSEG segment prefix. All four segments map to
the same physical address space:

| Segment | Virtual range         | Physical range      |
|---------|-----------------------|---------------------|
| KUSEG   | 0x00000000-0x7FFFFFFF | 0x00000000-0x1FFFFFFF |
| KSEG0   | 0x80000000-0x9FFFFFFF | 0x00000000-0x1FFFFFFF |
| KSEG1   | 0xA0000000-0xBFFFFFFF | 0x00000000-0x1FFFFFFF |
| KSEG2   | 0xC0000000-0xFFFFFFFF | (memory-mapped I/O)  |

### Step 2: ROM-to-RAM copy offset

If the physical address falls within the BIOS ROM copy source range,
map it to the RAM destination:

    if 0x1FC10000 <= physical <= 0x1FC18BEF:
        physical = physical - 0x1FC10000 + 0x00000500

This accounts for the single proven ROM-to-RAM copy performed by
`FUN_bfc00420` during BIOS init:

| Property  | Value        |
|-----------|--------------|
| Source    | ROM 0xBFC10000 (physical 0x1FC10000) |
| Dest      | RAM 0xA0000500 (physical 0x00000500)  |
| Length    | 0x8BF0 bytes (35,824 bytes)            |
| Proof     | generated/relocation_proofs/primary_copy/proof.json |

### Combined formula

    normalize(addr):
        phys = addr & 0x1FFFFFFF
        if 0x1FC10000 <= phys <= 0x1FC18BEF:
            phys = phys - 0x1FC10000 + 0x00000500
        return phys

### Examples

| Input address | After KSEG strip | After ROM-to-RAM | Canonical |
|---------------|------------------|-------------------|-----------|
| 0xBFC10C74    | 0x1FC10C74       | 0x00001174        | 0x00001174 |
| 0x80001174    | 0x00001174       | (not in ROM range) | 0x00001174 |
| 0xA0001174    | 0x00001174       | (not in ROM range) | 0x00001174 |
| 0xBFC00420    | 0x1FC00420       | (not in copy range) | 0x1FC00420 |
| 0xBFC06EC4    | 0x1FC06EC4       | (not in copy range) | 0x1FC06EC4 |

## Evidence

- The copy parameters (source, destination, length) are all immediate
  constants in the BIOS code. No runtime computation is involved.
- Ghidra decompilation confirms the loop structure and parameters.
- 69 Ghidra-known functions fall within the copy source range.
- The normalization formula maps all 69 ROM-source functions to their
  RAM-destination equivalents, producing 69 alias pairs.
- No alias was added without tracing back to the proof artifact at
  `generated/relocation_proofs/primary_copy/proof.json`.

## Scope limitation

This rule covers only the single ROM-to-RAM copy identified in Phase
1e. If additional copies are discovered in later phases (e.g., from
RAM-side code that runs after the initial copy), the rule must be
extended with new proof artifacts.

The exception vector trampoline at 0x80000080 is installed by
intra-RAM code at ROM 0xBFC109B0 (RAM 0x80000EB0) which copies 16
bytes from RAM 0x00000F0C to RAM 0x80000080. This is an intra-RAM
copy (not ROM-to-RAM) and does not require a normalization entry
because both addresses are already in RAM physical space.
