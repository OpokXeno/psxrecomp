# VBlank registrar — DuckStation dynamic diagnosis (2026-04-13)

## TL;DR — prior diagnosis was wrong

The previous handoff claimed "VBlank handler 0x8005A5BC is never installed in our runtime". **This is FALSE.** Dynamic TCP-probing of our own runtime proved the handler IS installed at 0x800DFEE0. The real gap is one level deeper: the **kernel's root-counter IRQ chain head** at `*(0xA000E014)` is null in our runtime but populated (=`0x000074A8`) in DuckStation.

## Method

Launched DuckStation headlessly (`./duckstation/build/bin/duckstation-qt.exe -bios -nogui -fastboot`) and connected to its TCP debug server on port 4371. Launched our own runtime on port 4370. Then diffed RAM state between the two at matching boot phases.

Tools added this session (all in `tools/`):
- `duck_query.py`, `duck_query2.py`, `duck_query3.py`, `duck_query4.py`, `duck_query5.py` — focused state-diff scripts
- `duck_search.py` — 2 MB RAM scan for marker addresses
- `disasm_ram.py` — live disassembler over TCP (works against either runtime)
- `scan_imm.py` — find addiu/ori instructions with a specific 16-bit immediate

## State comparison: our runtime vs DuckStation

| address | what | our runtime | DuckStation |
|---|---|---|---|
| 0x80079D9C | VSync counter | 0 (never ticks) | 0x254E (~9550, incrementing) |
| 0x80079D48 | shell init-done flag | **0x1** (SET) | 0x1 |
| 0x800DFEE0 | shell VBlank chain entry | **0x8005A5BC,0,0,0x8005A70C** (handler installed!) | 0x8005A5BC,0,0,0x8005A70C |
| 0x800DFF40 | user VSync callback | 0 | 0 |
| 0x80000100 | kernel PCB/TCB/EvCB table | identical to Duck | identical |
| 0xA000E004 | DCB chain head 1 | 0x80140014 | 0x80140014 |
| 0xA000E00C | DCB chain head 2 | 0x00006D88 | 0x00006D88 |
| **0xA000E014** | **DCB chain head 3 (root counters)** | **0x00000000 — MISSING** | **0x000074A8 — populated** |
| 0xA000E01C | DCB chain head 4 | 0x00006D98 | 0x00006D98 |
| 0x8000E028 | EvCB | **all zeros — empty** | populated with 4 `0xF0000003` RCnt events |

**Our shell-level init (0xBFC42160) ran successfully** — 0x80079D48=1 and the 0x800DFEE0 table is populated. But the **kernel-level init that sets `*(0xA000E014)`** never ran.

## The kernel IRQ chain walker (at 0x80000C80, verified via live disasm)

The kernel exception handler walks four chain heads at 0xA000E004, 0xA000E00C, 0xA000E014, 0xA000E01C. For each non-null head, it iterates a linked list and calls each entry's `+0x08` function:

```
0x80000DE4: addi $s4,$s3,32            ; iterate 4×8-byte slots in DCB
0x80000DE8: lw $s6,0($s3)               ; load chain head
0x80000DF0: beq $s6,$zr, skip_null
0x80000DF8: lw $s1,8($s6)               ; handler fn
0x80000DFC: lw $s0,4($s6)               ; verify fn
0x80000E08: jalr $s1                    ; call handler
0x80000E10: beq $v0,$zr, next
0x80000E18: beq $s0,$zr, next
0x80000E20: jalr $s0                    ; call verify
0x80000E28: lw $s6,0($s6)               ; next link
0x80000E30: bne $s6,$zr, loop
0x80000E38: addi $s3,$s3,8              ; next chain head
0x80000E3C: bne $s4,$s3, outer_loop
```

With `*(0xA000E014)=0x000074A8` (Duck), the walker reaches a chain entry at `0x000074A8` containing `{next=0, verify=0x000049BC, handler=0x00004A4C}`. The handler is then responsible for walking the **shell-level** chain at 0x800DFEE0 and calling 0x8005A5BC.

Our runtime has `*(0xA000E014)=0` so the walker skips this chain head entirely — VBlank handler is never reached.

## The missing installer

The function that populates `*(0xA000E014)=0x000074A8` is:

- **`func_00004B90`** — ROM `0xBFC14690`, primary-copy alias RAM `0x80004B90`. Already seeded in dispatch table.
- Calls `SysEnqIntRP(priority=2, chain_entry_ptr=0x800074A8)` via `jal 0xB0001420`.
- 6 different kernel sites synthesize `0x000074A8`: 0xBFC146A8, 0xBFC146B8, 0xBFC1472C, 0xBFC14788, 0xBFC14798, 0xBFC14810 — all inside `InitRCnt`-family functions in kernel primary-copy region.
- `func_00004B90` has one static caller: `0xBFC15C94` (inside `func_1FC15B9C` = **B0:0x15 / ChangeClearRCnt** — RAM alias `0x8000609C`, in our B0 table from DuckStation).
- `func_1FC15B9C` has 0 static callers. One kernel B0:0x15 trampoline exists at `0xBFC0DAC0`, called statically from `0xBFC0C748` (inside `func_1FC0C720` — itself a 0-caller orphan).

## Remaining unknown

Who, in the kernel boot sequence, invokes B0:0x15 (`ChangeClearRCnt`) or directly calls `func_1FC15B9C` / `func_1FC0C720` with appropriate args during boot? In DuckStation this runs; in our runtime it does not. The call path is indirect (function-pointer, probably through an init-function-pointer table).

## No dispatch misses in our runtime

`psx_dispatch_misses.txt` and `psx_crash.txt` are absent (no dispatch misses, no traps). The runtime isn't failing — it's simply not executing the code path that populates the kernel IRQ chain. Some kernel boot function is returning early or not being reached at all.

## Next session

Options in priority order:

1. **Patch DuckStation** to log PC-level trace over a narrow window (first ~10k instructions from reset). Diff against our runtime's function-dispatch log to find the first function DuckStation enters that our runtime doesn't.
2. **Add a custom `pc_break` command to DuckStation's debug server** (`duckstation/src/core/psxrecomp_debug_server.cpp`) — execute-breakpoint at `0xBFC14690`. When it fires, dump call stack to see who called it. This is a ~30-line patch + rebuild.
3. **Instrument our runtime** with a dispatch-trace command that logs the first call to every function. Compare against the call tree expected by DuckStation.

Option 2 is cleanest: the existing `psxrecomp_debug_server.cpp` has `pause` / `continue` / `step N frames` — adding `pc_break` requires a per-instruction hook in the CPU core. More work than it sounds. Option 3 is easier and stays within our own codebase.

## Caveat about auto-memory

The handoff memory `phase4_vsync_diagnosis.md` now contains a false statement ("VSync counter 0x80079D9C never written; EvCB 0xF2000003 never opened; B0:0x15 orphaned"). The "never written" half is confirmed; the reason is different from what was guessed. Update on next session.
