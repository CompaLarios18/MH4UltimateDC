# Static recompiler

`tools/recompile.py` turns all ARM and Thumb code in the game's `code.bin` into C that runs natively on PC. The C is generated on the user's machine from their own dump. It's derived from game code, so it is **never committed** (`build/` is git-ignored).

```
python3 tools/recompile.py --extra build/export/missing2.txt -o build/recomp     # ~80 s
cd tools && python3 -m recomp.build ../build/recomp --harness --opt O0 -D RC_CHECK_RETURNS -o game.so
python3 -m recomp.gametest --sample 2000 --cases 3 --report ../build/recomp/sample.tsv
```

## Pipeline

1. **Entry points.** `config/functions.tsv` (Ghidra plus the second discovery pass), extra lists (`--extra`), and words in `.rodata`/`.data` that point at code (vtables, callback tables). Numbers that merely look like code addresses are weeded out: the target must decode into clean code, must not be reached by falling through from the instruction before it, must not overlap code of the other instruction set, and Thumb targets must sit near known Thumb code. Entries outside `.text` are dropped.
2. **Decoding** (`recomp/arm.py`, `recomp/thumb.py`). Each instruction becomes an `Insn`: a kind (plain op, branch, call, return, indirect jump, jump table, SVC, trap) and a list of C statements over the register file. Covered: ARMv6K (data processing, multiplies, saturating and parallel arithmetic, media instructions, LDREX/STREX family, CP15 TLS registers), VFPv2, Thumb-1. UNPREDICTABLE encodings become traps.
3. **Function analysis** (`recomp/analysis.py`). Follows the control flow from each entry:
   - branches to another entry are tail calls, others are local labels;
   - jump tables: `add pc, pc, rI, lsl #2`, `ldr pc, [pc, rI, lsl #2]`, `adr` + `ldr pc, [rB, rI, lsl #2]`, with the bound found from the guarding `cmp` (or `and` + `subs` + `bhs`);
   - `mov lr, pc` / `add lr, pc, #n` followed by a jump are calls that return to that constant;
   - offset tables (`ldr rX, [rB, rI, lsl #2]` + `add pc, rX, rB`);
   - FPSCR vector length and stride are tracked through `vmsr`, so VFP short-vector code (LEN=3 in two audio mixers) is expanded per element;
   - RVCT floating-point helpers that read a descriptor word after their call site and continue at one of several places in the caller are handled as multi-return calls;
   - code that falls into a function of the other instruction set stops there (the call before it doesn't return).
   New call targets are analysed too, until the set is closed.
4. **Emission** (`recomp/emit.py`). One C function per guest function, `goto` for local control flow, `switch` for jump tables, a cycle charge per basic block (`RC_TICK`). Direct calls become direct C calls; anything else goes through `rc_call`.
5. **Build** (`recomp/build.py`). Parallel, incremental clang build into a shared library (or object files for the port).

## Runtime contract (`tools/recomp/runtime/recomp.h`)

Generated code only touches `RCtx` (core registers, split NZCVQ/GE flags, VFP registers and FPSCR, TLS registers, exclusive monitor, cycle budget) and guest memory. Memory is either one flat mapping (`ctx->mem + addr`, used by the tests) or, with `RC_PAGED`, a table of host pointers per 4 KiB page, as used by Azahar (`PageTable::pointers`), with `rc_slow_read`/`rc_slow_write` for pages that need special handling.

The host provides:

| Callback | Called for |
|---|---|
| `rc_call(ctx, target)` | indirect calls and jumps (function pointers, virtual calls, unknown targets) |
| `rc_svc(ctx, imm, next_pc)` | `svc` (system calls) |
| `rc_trap(ctx, addr, kind)` | undefined/unsupported instructions, bad jump-table indices, jumps into the middle of nowhere |
| `rc_bad_return(ctx, got, expected)` | with `RC_CHECK_RETURNS`: a function returned somewhere other than its caller |
| `rc_budget(ctx)` | the cycle budget ran out (yield point for the scheduler) |

## Semantics worth knowing

- **Flags** are kept as separate 0/1 words; `rc_adds` implements ADC/SBC/CMP/CMN carry and overflow exactly.
- **VFP** results are computed on raw bits, with ARM rules where C's differ: the game runs in RunFast mode (FPSCR.FZ and FPSCR.DN set at startup), so denormal inputs and outputs are flushed to signed zero and every NaN result is the default NaN. Without DN, NaN propagation follows ARM operand priority. Rounding is always to nearest. Cumulative exception flags are not modelled.
- **`vmsr fpscr`** keeps the bits QEMU and dynarmic keep (`0xFFF7009F`).
- **Short vectors**: destination in bank 0 means scalar; second operand in bank 0 means a scalar broadcast; element registers wrap within their bank; elements are computed in order. Functions are assumed to start with LEN=0, as the procedure call standard requires; the analysis reports any call, return or fallthrough made in vector mode (there are none).

## Verification

There are three levels, all against Unicorn's ARM11 MPCore model:

1. **Instruction fuzzing** (`python3 -m recomp.fuzz --only {vfp,vfpvec,media,mul} | --mode thumb`): random encodings the translator accepts, each run from random states (special float values, denormals, all FZ/DN combinations, random vector lengths), comparing all registers, flags, VFP state and memory. Zero mismatches. Known reference bug: Unicorn gets double-precision VMOV/VABS/VNEG/VSQRT short vectors wrong, so those are fuzzed as scalars only.
2. **Synthetic programs** (`tests/test_recomp.py`): a C program built with clang (recursion, switch tables, function pointers, VFP, 64-bit division, ARM↔Thumb), plus hand-written assembly for short-vector code and a multi-return helper, recompiled whole and compared function by function.
3. **Whole-game differential test** (`python3 -m recomp.gametest`): every recompiled function, with everything it calls, from random states (registers, VFP, flags, pointer-rich scratch memory) on both sides. A case passes when both exit the same way (return, same system call, or fault at the same address) with identical registers, flags, VFP state and identical writable memory (`.data`, `.bss`, stack, buffers, TLS). Indirect targets that the native side couldn't find are written to `missing_targets.txt` and can be fed back with `--extra`.

Random states can't reach everything (most game functions fault early on random pointers), so the final check is running the game itself with the dynarmic JIT as reference (see [PORT_ARCHITECTURE.md](PORT_ARCHITECTURE.md)).

## Current numbers (USA v1.3.0)

| | |
|---|---|
| Functions | 39,697 (7,820 found through data pointers) |
| Instructions | 3,230,400 |
| Jump tables | 3,475, all resolved |
| Traps | 1 (data after a call that never returns) |
| Analysis problems | 0 |
| Generate / build | ~100 s / ~10 min at `-O0` on 2 cores (rebuilds only touch changed 64 KiB buckets) |
