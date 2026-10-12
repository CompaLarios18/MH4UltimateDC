# Roadmap

The end goal is a native PC executable built from decompiled source, running with assets from the user's own dump. There are two well-trodden routes to a PC port; this project starts on shared groundwork and decides the main route at the end of Phase 1.

| Route | How it works | Pros | Cons |
|---|---|---|---|
| **A. Source decompilation** (functionally equivalent C/C++) | Rewrite each function in C/C++ that behaves like the original; compile for x86-64 | Real, editable source; best for mods and long-term maintenance | Slow: a game this size is many thousands of functions |
| **B. Static recompilation** (N64Recomp / XenonRecomp style) | Mechanically translate ARM machine code to C, then compile for PC; hand-write only the OS/GPU layer | Playable much sooner | Output is not human-readable; harder to modify |

A hybrid is common: recompile everything first (B) to get a running port, then replace recompiled functions with hand-decompiled ones (A) over time.

> **Byte-matching** decompilation (rebuilding an identical `code.bin`) would require the exact proprietary compiler the game was built with (most likely ARM's RVCT/armcc for 3DS titles). It is **not** a goal for now; *functional equivalence* is.

---

## Phase 0 — Bootstrapping ✅
- [x] Repository, legal guidelines, `.gitignore` for dumps
- [x] BLZ (backward LZ77) decompressor for `code.bin`
- [x] Exheader parser and `code.bin → ELF` converter for disassemblers
- [x] Extractor for decrypted .3ds / .cia / .cxi (`tools/extract_ncch.py`)
- [x] Verify against a real dump (USA v1.3.0 update); record title ID + hashes in `config/versions.json`
- [x] Target version: **USA v1.3.0** (code from the update title)

## Phase 1 — Mapping the binary *(current)*
*How work is split across parallel agents: [`AGENT_WORKFLOW.md`](AGENT_WORKFLOW.md).*
- [ ] Ghidra export script (`tools/ghidra/`): function list, call graph, strings, per-function pseudo-C → `build/export/` (git-ignored)
- [ ] `tools/difftest.py`: compile candidate C for armv6k, link against original addresses, compare with original in Unicorn (proof of concept done)
- [ ] Status table `config/functions.tsv` + worker prompt template `docs/prompts/worker.md`
- [ ] Pilot batch of ~10 leaf functions; measure pass rate and cost
- [ ] Ghidra project setup guide; shared function/symbol list in text form (`config/symbols.txt`)
- [ ] Identify 3DS SDK/system library code (svc wrappers, `nn::` services, libc, math) — these get replaced, not decompiled
- [ ] Identify engine layers (Capcom's MT Framework Mobile): memory, file/archive loading, rendering, sound, task/scheduling
- [ ] Document RomFS formats used by the game (`.arc` archives and their contents) — lots of public community knowledge exists from modding
- [ ] **Decision point:** route A, B or hybrid

## Phase 2 — Running *something* on PC
- [ ] Platform layer in `port/`: file system over the user's RomFS, HID → keyboard/gamepad, timing, threads
- [ ] HLE of the 3DS OS services the game actually calls (fs, hid, apt, gsp, dsp, …)
- [ ] PICA200 GPU command translation → modern API (likely via SDL3 GPU / Vulkan / OpenGL)
- [ ] Boot to title screen

## Phase 3 — Playable
- [ ] Two-screen layout handling (side by side, swappable, touch screen via mouse)
- [ ] Audio (DSP)
- [ ] Save data compatibility with real 3DS saves
- [ ] Gameplay loop: village → quest → hunt

## Phase 4 — PC niceties
- [ ] Resolution scaling, widescreen, higher framerate (needs care: game logic is likely frame-locked)
- [ ] Rebindable controls, controller support
- [ ] Multiplayer: local/online replacement (original relied on 3DS wireless / Nintendo Network)

## Prior art to study
- N64Recomp + Zelda64Recomp, XenonRecomp + UnleashedRecomp — static recompilation to PC
- zeldaret's decomp projects and decomp.me — workflow, tooling, conventions
- 3DS emulators (Citra lineage: Azahar, etc.) — reference for OS service HLE and PICA200 behavior (mind their licenses if reusing code)
- 3dbrew wiki — file formats, system services, hardware docs
