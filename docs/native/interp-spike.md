# N1-24: can the fallback interpreter (D13) be generated from the recompiler?

**Answer: yes.** `tools/recomp/interp_gen.py` takes the recompiler's own decoders (`ArmTranslator`, `ThumbTranslator`) as a black box and writes, per instruction form, a C function `ih_<form>(RCtx *ctx, u32 insn)` whose body is the C the recompiler emits for that form, with the registers, immediates, condition and short-vector length read from `insn` (and `ctx->fpscr`) at run time. Nothing in `arm.py`, `thumb.py`, `insn.py`, `emit.py` or `recomp.h` is changed.

- **All 20 forms of the spike generate and match** the recompiled code on 1,000 random states each (`tests/test_recomp_interp_gen.py`; threshold 16 of 20). The default campaign, a 20,000-state campaign per form with two other seeds, and the 28 extra forms are in the tables below.
- **Of 29 further forms probed to size N1-28, 28 generate and match; one does not** (`VLDM`/`VSTM`: its C depends on a register *count* held in an 8-bit field).
- Generating all 49 forms takes about 50 s of Python on one core; compiling the result (about 630 KB of C, no warning of its own under `-Wall -Wextra`) takes 6 s at `-O2` and gives a 320 KB code section.

## How it works

Run `cd tools && python3 -m recomp.interp_gen [--extra] [--form NAME] [--out FILE.c]`.

1. A **form** (`Form` in `interp_gen.py`) is a set of encodings: the fixed bits plus named bit `Field`s (register numbers, immediates, selectors, a register-list field, and for VFP short vectors the LEN and STRIDE bits of FPSCR). Writing one is 3 to 6 lines.
2. The generator decodes many encodings of the form and turns each result into text: `Insn.kind`, `body`, `pcexpr`, branch target and link value. Every number in that text is replaced by a marker, which leaves a *skeleton* and a list of values.
3. **Switch.** Where the skeletons of a region differ, the field that separates them is split into classes (values that decode alike) and the generator recurses: this is "where the emitter specialises on a field, the generator emits a switch over its values". Classes are found by comparing what each value decodes to in contexts where it is valid; values that never decode in the same context (must-be-zero fields in conflict) stay apart.
4. **Numbers.** In a uniform region each varying number is fitted exactly: as an affine function of the fields (`R(rd)`, `32 - imm5`, `16*hi + lo`), of a sign-extended field (branch offsets) and of the instruction address or the word-aligned PC (PC-relative values), or, when that fails, as a lookup table over the fields it depends on (at most 12 index bits). Anything else is a `GenError` that names the number and the fields.
5. **Register lists** (`LDM`/`STM`/`PUSH`/`POP`): the body is split into a head, a template per element (first element, later elements) and a tail whose numbers are fitted in the list size, the register number and the element rank; the handler loops over the set bits. A line whose shape differs for an empty list (`s` against `s + 4`) becomes a conditional. Thumb `LDMIA` additionally splits on whether the base register is in the list.
6. **Verification.** The finished tree is instantiated for 2,000 random valid encodings (fields at their smallest or largest value now and then, every list size alike, addresses 4-aligned for ARM and 2-aligned for Thumb); the instantiated text must give exactly the decoder's skeleton and numbers. Then the test builds the handler next to the recompiler's own C (`emit.straight_line`) for 100 random encodings of the form and runs both from 1,000 random states made by `fuzz.random_state`: registers, flags, Q, GE, VFP registers, FPSCR, the next address and a 64 KiB memory buffer must agree, or both must fault at the same address (and the run goes on until at least 500 states ran to completion).

Handler contract (for the dispatcher of N1-28): on entry `ctx->r[15]` is the address of the instruction; on exit it is the address of the next instruction, with bit 0 set when that is Thumb code. The handler tests the ARM condition itself (`ih_cond`, generated from `COND_NAMES` and the `C_xx` macros of `recomp.h`; Thumb's conditional branch carries its condition as a field). A read of r15 gives address + 8 (ARM) or + 4 (Thumb) through the macro `RR`; no handler writes r15 except to say where control goes next (branches, returns, loads into pc, `bx`/`blx`). `SVC` calls `rc_svc` with the next address as the recompiler does; an instruction the decoder refuses (unsupported, not UNPREDICTABLE) calls `rc_trap(ctx, pc, code)`.

What "the recompiled code for the same instruction" is in the comparison: for straight-line instructions, `emit.straight_line`, byte for byte what the function emitter writes. For control flow (`B`, `BL`, `BX`, `BLX`, loads into pc, `mov pc, lr`) the emitter's output depends on function analysis (gotos, calls, return checks), so `reference_c` writes it from the same `Insn` fields exactly as `FunctionEmitter.insn` does and ends in the same exit convention. That comparison is weaker than the others (the branch target, link value and `pcexpr` come from the same decoder on both sides); the check that matters there is that the handler computes them from the instruction word and the address.

## The 20 forms

"leaves" is the number of cases of the generated decision tree; "tables" is lookup tables (entries).

| # | form | covers | generates | matches | leaves | C bytes | tables | what it needed |
|---|---|---|---|---|---|---|---|---|
| 1 | `arm_dp_imm_shift` | the 16 data-processing opcodes, S, register shifted by an immediate | yes | yes | 204 | 53,623 | 1 (32) | switch on opcode, S, shift type, `imm5 == 0`; `32 - imm5` affine; a 32-entry table for the `ASR #32` / `ROR #0` special cases |
| 2 | `arm_dp_reg_shift` | same, shifted by a register | yes | yes | 112 | 29,293 | 0 | switch and register numbers only |
| 3 | `arm_dp_imm` | same, rotated 8-bit immediate | yes | yes | 37 | 119,821 | 3 (12,288) | switch on opcode, S, `rot == 0`; the rotated value and its carry are tables over (imm8, rot) |
| 4 | `arm_mul` | MUL, MLA, UMAAL, UMULL, UMLAL, SMULL, SMLAL, S | yes | yes | 14 | 3,236 | 0 | switch |
| 5 | `arm_ldst_imm` | LDR/STR/LDRB/STRB, 12-bit offset, pre/post index, writeback | yes | yes | 24 | 4,818 | 0 | switch on P U B W L; offset is the field |
| 6 | `arm_ldst_reg` | same, shifted register offset | yes | yes | 168 | 43,332 | 1 (32) | like 1 |
| 7 | `arm_ldst_extra` | LDRH, STRH, LDRSB, LDRSH, LDRD, STRD, immediate or register | yes | yes | 85 | 20,169 | 0 | offset `16*hi + lo` is affine in two fields |
| 8 | `arm_ldm_stm` | LDM/STM, four modes, writeback, with or without pc in the list | yes | yes | 28 | 23,203 | 0 | register-list recipe; the pc element is a tail line that is a conditional on an empty list |
| 9 | `arm_b_bl` | B, BL | yes | yes | 2 | 514 | 0 | target = `ih_pc + 8 + 4*sext24(imm24)`; link = `ih_pc + 4` |
| 10 | `arm_bx_blx` | BX, BXJ, BLX (register) | yes | yes | 3 | 571 | 0 | kinds return, indirect jump and indirect call |
| 11 | `arm_media_ext` | SXTB/UXTB/SXTH/UXTH, SXTAB/UXTAH..., 16-bit pairs | yes | yes | 24 | 5,575 | 0 | switch on rotation and on `rn == 15` (no accumulate) |
| 12 | `arm_qadd` | QADD, QSUB, QDADD, QDSUB | yes | yes | 4 | 911 | 0 | switch |
| 13 | `vfp_arith` | VMLA..VDIV, F32 and F64, scalar and short vectors | yes | yes | 237 | 182,984 | 42 (2,160) | switch on opcode, size, LEN (read from `ctx->fpscr`), bank of the destination and of the second operand; the wrapped register numbers of vector elements are small tables over (register, D, stride) |
| 14 | `vfp_ext` | VMOV, VABS, VNEG, VSQRT (vectors), VCMP, VCVT | yes | yes | 157 | 65,898 | 28 (1,440) | like 13 |
| 15 | `vfp_ldst` | VLDR, VSTR | yes | yes | 12 | 2,993 | 0 | affine register index (`2*vd + D`) and offset (`4*imm8`) |
| 16 | `vfp_xfer` | VMOV core <-> single / half of a double, VMRS, VMSR | yes | yes | 20 | 3,265 | 0 | switch; `rt == 15` (APSR flags) is a separate case |
| 17 | `t_shift_imm` | Thumb LSL, LSR, ASR by immediate | yes | yes | 6 | 1,630 | 0 | switch |
| 18 | `t_alu` | Thumb ALU group | yes | yes | 16 | 2,645 | 0 | switch |
| 19 | `t_ldst_imm` | Thumb LDR/STR/LDRB/STRB, 5-bit offset | yes | yes | 4 | 938 | 0 | switch |
| 20 | `t_push_pop` | Thumb PUSH, POP, with LR / PC | yes | yes | 4 | 2,732 | 0 | register-list recipe; the extra register is a tail line |

20 of 20 generate and match (the bar was 16). 1,000 states per form is the committed test; "matches" below the table says what was run beyond it.

## The extra forms (probed for the estimate of N1-28)

All of these are in `EXTRA_FORMS` and run by `test_extra_forms_match_recompiled_code` with the same comparison.

| form | covers | generates | matches | leaves | notes |
|---|---|---|---|---|---|
| `arm_media_par` | parallel add/subtract (SADD16, QADD8, UHSUB16, ... GE flags) | yes | yes | 1 | the decoder passes the opcode to a helper, so there is nothing to split |
| `arm_media_sat` | SSAT, USAT | yes | yes | 4 | one 32-entry table for the `ASR #32` case |
| `arm_halfmul` | SMLAxy, SMLAWy, SMULWy, SMLALxy, SMULxy | yes | yes | 16 | |
| `arm_swp` | SWP, SWPB | yes | yes | 2 | |
| `arm_excl` | LDREX, STREX and the B/H/D variants | yes | yes | 8 | |
| `arm_vldm` | VLDM, VSTM | **no** | | | `GenError: the C depends on the wide field imm8 (256 values)`: one statement per register, so the register count (`imm8`, or `imm8/2` doublewords) shapes the C |
| `arm_vmov2` | VMOV two core registers <-> double or two singles | yes | yes | 6 | |
| `arm_mrs`, `arm_msr` | MRS, MSR (flags) | yes | yes | 1, 2 | |
| `arm_cp15` | MRC/MCR to CP15 (TLS registers, barriers) | yes | yes | 17 | every other CP15 register is an `rc_trap` leaf |
| `arm_ldrpc` | LDR/LDRB immediate, including loads into pc | yes | yes | 20 | kinds op, return, indirect jump |
| `arm_dp_rd15` | data processing with pc as destination | yes | yes | 140 | `mov pc, lr` and `add pc, pc, rm, lsl #2` are ordinary indirect jumps here (the interpreter needs no jump-table special case) |
| `arm_svc` | SVC | yes | yes | 1 | compared on the `rc_svc` arguments |
| `t_addsub`, `t_imm8`, `t_hireg`, `t_ldr_lit`, `t_ldst_reg`, `t_ldst_half`, `t_ldst_sp`, `t_addr`, `t_misc_sp`, `t_ext`, `t_rev` | the rest of the Thumb-1 data and memory forms | yes | yes | 1 to 22 | `t_ldr_lit` and `t_addr` need the word-aligned PC (`(pc & ~3)`), which is why Thumb addresses are sampled 2-aligned and `PCA` is a fit variable |
| `t_ldm` | Thumb LDMIA, STMIA | yes | yes | 4 | split on "base register in the list" (writeback depends on it) |
| `t_bcond`, `t_b` | Thumb conditional and unconditional branch | yes | yes | 1 | the condition is a field of the word |
| `t_bl`, `t_blx_imm` | the two-halfword BL and BLX | yes | yes | 1 | the word handed to the handler holds both halfwords; BLX aligns the target to 4 (`PCA`) |

## Beyond the committed test

| campaign | result |
|---|---|
| default (CI): 1,000 states per form (and more until 500 ran to the end), 100 words per form, seed 1: 20 forms + 28 extras | 48 of 48 pass |
| 20 forms + 28 extras, 20,000 states per form, 2,000 words per form, seed 2 | 48 of 48 pass (9 min 14 s) |
| the same, seed 3 | 48 of 48 pass (9 min 9 s) |

`INTERP_GEN_STATES=<n> INTERP_GEN_SEED=<n> python3 -m pytest tests/test_recomp_interp_gen.py` runs a different campaign.

## What does not work, and what to watch

- **A register count in a field** (`VLDM`, `VSTM`). Options for N1-28: a "count" field kind next to the register list (the element period is 1 register for F32 and 2 words for F64, which the list recipe does not model yet), or a hand-written loop of about 30 lines over `ctx->f.sw` tested against the recompiler for both sizes and the four addressing modes. The second is cheaper.
- **UNPREDICTABLE encodings** (`Unpredictable` in the decoders) are wildcards in the generator, so a handler runs them as the nearest defined form where the recompiler emits a trap. That is allowed by the architecture; N1-28 should log them, not trap.
- **The inference samples; it does not prove.** A relation between two or three fields that sampling never hits would produce a handler that is wrong for exactly those encodings. Two things catch that: the 2,000-encoding check of the template against the decoder, and the run comparison. Neither is exhaustive, and the large campaign did find one miss: with seed 3, `STMDB rn!, {pc}` (an empty list plus pc) stored a constant for pc, because that shape has a single list and only one sample address had been used for it. The list samples now cover every address and the check draws every list size alike; with that fix the same campaign is clean. N1-28 should (a) check the whole Thumb space (65,536 halfwords) against the decoder, (b) check 20,000 or more random ARM words per form, and (c) fuzz every word the dispatcher accepts against the recompiler as `fuzz.py` does, so a form that is missing shows up as a mismatch or an unhandled word.
- **Code size.** `arm_dp_imm` (rotated immediates: three 4,096-entry tables) and `vfp_arith` (every short-vector length unrolled) are 120 KB and 183 KB. A rotate-right family for the fit and a loop with a bank-advance helper for vectors would shrink both to a few KB; neither is needed for correctness (the whole set compiles to a 320 KB code section).
- **Cycle accounting.** Handlers do not charge `RC_TICK`; the deterministic build (N1-25) needs the loop in N1-28 to charge one tick per instruction.
- **Generation time.** 45 s on one core for all 49 forms, dominated by `vfp_arith` (12 s) and `arm_dp_imm_shift` (8 s). The output is deterministic (seeded by form name and seed), so the build can cache it by a hash of `arm.py`, `thumb.py`, `insn.py` and `interp_gen.py`.
- **Not part of the spike:** the dispatcher, the interpret loop, the `started` boot step, the log and `missing_entries.txt`.

## Estimate for N1-28

The spec sizes N1-28 as L. With the generator in hand it is closer to M (about 400 to 500 k tokens), in four pieces:

| piece | work | size |
|---|---|---|
| form catalogue | add the ten or so decoder paths no form covers yet: `CLZ`, `MSR` immediate and the hint NOPs, `SSAT16`/`USAT16`, `PKH`, `SEL`, `REV`/`REV16`/`REVSH`, `USAD8`/`USADA8`, the media multiplies (`SMLAD`, `SMLSD`, `SMLALD`, `SMMLA`, `SMMLS`), `BLX` immediate, `PLD`, `CLREX`, `BKPT`/`UDF`, Thumb `SVC`/`BKPT`; plus `VLDM`/`VSTM` by hand | S |
| dispatcher | `ih_dispatch_arm` and `ih_dispatch_thumb` mapping a word to a handler (generated from the forms' fixed bits, with the decoder settling overlaps such as data processing against the miscellaneous space), Thumb 16/32-bit detection, an exhaustive Thumb test and a random-word ARM test against the decoder | S to M |
| native module | `native/src/cpu/interp`: the interpret loop in the shape of the Azahar backend's `Interpret` (expected return address with sp at or above the original), `SetMissingFunctionHandler`, the `started` boot step, `interpreting <addr>` log once per entry, `build/missing_entries.txt` in the format of `config/extra_entries.txt`; build-time generation of the C (Python is already required by `cpu_test`) | M |
| tests | ctest: every function of `svc_test` interpreted against recompiled; random states per form through the built handlers; `svc_test` natively with its worker functions forced into the interpreter | S to M |

Risks: the `RCtx` layout and `recomp.h` macros are an interface (a change to either regenerates and rebuilds the handlers, which is the point of generating them); mixed ARM/Thumb returns across the interpreted/recompiled boundary use the bit-0 convention above and need the tests of `Interpret`'s return-address logic; short-vector code (`docs/RECOMPILER.md` says LEN = 3 in two audio mixers) is covered by the `vfp_arith` and `vfp_ext` leaves for every LEN.

## Reproduce

```
python3 -m pytest tests/test_recomp_interp_gen.py          # needs clang and unicorn (requirements.txt); skipped without clang
cd tools && python3 -m recomp.interp_gen --extra --out /tmp/handlers.c
clang -O2 -Wall -Wextra -c -ffp-contract=off -frounding-math -fno-strict-aliasing -fwrapv -Irecomp/runtime -x c /tmp/handlers.c -o /tmp/handlers.o
```
