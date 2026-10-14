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

## Phase 1 — Mapping the binary
*How work is split across parallel agents: [`AGENT_WORKFLOW.md`](AGENT_WORKFLOW.md).*
- [x] Ghidra export script (`tools/ghidra/`): function list, call graph, strings, per-function pseudo-C → `build/export/` (git-ignored)
- [x] Second discovery pass for vtable-only code (`tools/find_missing_functions.py`): 25,872 → 31,409 functions, 80% → 89.7% of .text
- [x] `tools/difftest.py`: compile candidate C for armv6k, link against original addresses, compare with original in Unicorn; first verified function: `crc32_string`
- [x] Status table `config/functions.tsv` (`tools/build_function_table.py`, `tools/fnstatus.py`)
- [x] Worker prompt template `docs/prompts/worker.md`
- [x] Pilot batch of ~10 leaf functions; measure pass rate and cost
- [x] Ghidra project setup guide; shared function/symbol list in text form (`config/symbols.txt`)
- [ ] Identify 3DS SDK/system library code (svc wrappers, `nn::` services, libc, math) — these get replaced, not decompiled
- [x] Recover the MT Framework class registry: 992 classes with hierarchy, sizes, ids (`tools/mt_dti.py`, `config/mt_classes.tsv`)
- [ ] Identify engine layers (Capcom's MT Framework Mobile): memory, file/archive loading, rendering, sound, task/scheduling
- [ ] Document RomFS formats used by the game (`.arc` archives and their contents) — lots of public community knowledge exists from modding
- [x] **Decision point:** hybrid. Static recompilation first (route B) to get a running port, hand-decompiled functions (route A) where we need to change behaviour (widescreen, 60 fps, HUD). See [PORT_ARCHITECTURE.md](PORT_ARCHITECTURE.md).
- [x] Static recompiler for the whole game (`tools/recompile.py`, [RECOMPILER.md](RECOMPILER.md)): 39,697 functions, 3.2 M instructions, every jump table resolved; verified per instruction (fuzzing) and per function (random-state differential tests) against an ARM11 model

## Phase 2 — Running *something* on PC
- [x] Licence: GPL-3.0-or-later for the whole repository (the port reuses Azahar's GPL-3.0 core)
- [x] Recompiled-code CPU backend for Azahar's core (`port/`, `Core::ARM_Interface`): paged memory, SVCs into Azahar's kernel, cycle accounting, one host fiber per guest thread. A multi-threaded test program prints the same results as with Azahar's JIT
- [x] The game's own code boots like it does on Azahar's JIT (SDK start-up, services, static constructors) until it opens its RomFS
- [x] Frontend: Azahar's own Qt interface loads the code library for the game it opens (no separate SDL frontend needed); Windows build in CI, run under Wine with a code library built by llvm-mingw (into gameplay, OpenGL, widescreen)
- [x] `tools/build_game.py`: the user's dump to an installed code library in one command (fetches the compiler: llvm-mingw on Windows, Zig's C compiler on Linux and macOS when clang isn't installed)
- [x] Hook points: native code around chosen game functions (`config/hooks.txt`, `port/src/hooks.cpp`); headless runner with input scripts and screenshots for automated runs of the real game
- [x] Run the game with dynarmic and with the recompiled code side by side and diff them: same screens through first boot, intro movie (the hand-written video decoder needed new analysis patterns), title, character creation and the opening on the sand ship
- [x] Boot to title screen, and into gameplay, at 60 fps; in-game save and load work

## Phase 3 — Playable
- [x] Two-screen layout handling (side by side, swappable, touch screen via mouse): Azahar's layouts, plus the port's touch-screen overlay
- [x] Audio (DSP): Azahar's HLE DSP; sounds right in play on Windows
- [ ] Save data compatibility with real 3DS saves
- [x] Gameplay loop: village → quest → hunt (played on Windows)

## Phase 4 — PC niceties *(current)*
- [x] Research: frame pacing (the engine is delta-time based; frame limiter in `sMain`), widescreen (projection, render targets, culling, HUD anchoring), second screen (viewport 7, layout views, touch panels) — [`research/`](research/README.md)
- [x] 60 fps: frame-rate cap hook, verified on the real game (frame delta 1/60 s, game speed unchanged); find what holds it at 30 on hardware
- [x] Above 60: 120 fps (delta 1/120 s, movies stay at their own 30; the quest-start loading race it exposes is worked around), played on Windows in hunts
- [ ] Widescreen: anamorphic (projection hook + Azahar single-screen aspect) and true wide rendering (render targets, transfer, tunable 400s; up to 512x240) work in gameplay; the hunting HUD's sides go to the screen's edges (`hud_edges`, by layout object); next, the quest HUD
- [ ] Second screen: Azahar layouts (available), the custom layout as a touch-screen overlay in a corner of a full-window game (`touch_overlay`), kick and target camera on buttons, and in quests the map alone in that corner (`quest_overlay`, by the player's panel layout) (done); next, the item shortcuts and the other panels on buttons
- [x] Resolution scaling, texture packs, rebindable controls and controllers: Azahar's
- [ ] Multiplayer: Azahar's network rooms carry MH4U's local play; two runners (`--room-host`, `--room-join`, `--lockstep`) meet in a Gathering Hall, and one joins the quest the other posts and both go on it together; next a hunt with a monster, then 4 players

## Prior art to study
- N64Recomp + Zelda64Recomp, XenonRecomp + UnleashedRecomp — static recompilation to PC
- zeldaret's decomp projects and decomp.me — workflow, tooling, conventions
- 3DS emulators (Citra lineage: Azahar, etc.) — reference for OS service HLE and PICA200 behavior (mind their licenses if reusing code)
- 3dbrew wiki — file formats, system services, hardware docs
