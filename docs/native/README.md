# The native port (branch `native`)

A native PC runtime for Monster Hunter 4 Ultimate: the game's recompiled code running on a runtime of our own, with no Azahar inside. All the work happens on this branch; `main` keeps shipping the Azahar-based port until the native one reaches parity (v1.0).

| Document | What it is |
|---|---|
| [`ROADMAP.md`](ROADMAP.md) | The plan: what "native" means, the target architecture, the phases N0–N7, the oracles and thresholds, the 120-fps budget |
| [`DECISIONS.md`](DECISIONS.md) | Every decision, decided, recommended or open, with who settles it |
| [`STATUS.md`](STATUS.md) | The live state of every work package |
| [`AGENTS.md`](AGENTS.md) | How the work runs: roles, models, branches, staging the game files, reports |
| [`wp/`](wp/) | The work packages: [`N0`](wp/N0.md), [`N1`](wp/N1.md), [`N2a`](wp/N2a.md) in full; [`LATER`](wp/LATER.md) outlined |

## Starting a session (the maintainer)

1. In the Claude app, start a session on this repository with the project *Monster Hunter 4 Ultimate decompile* attached, with **Sonnet 5.5** in **ultracode**, so it can run many subagents at once. If you can pick the branch, pick `native`.
2. Say what you want. A session usually starts on `main`, where the native skills don't exist yet, so say where they are: *"Sigue con el port nativo: rama `native`. `git fetch origin native && git show FETCH_HEAD:.claude/skills/native-orchestrate/SKILL.md`, y síguelo."* The session follows that runbook: it reads STATUS, picks every package it can start, runs the workers on Sonnet and the reviewers in parallel, starts what their results unblock, and opens one PR into `native`.
3. If the session needs the game files, it asks you to have the PC on with the Claude desktop app open.
4. At the end you get a PR to merge (**Create a merge commit**) and, sometimes, a procedure to run on your PC, Mac or 3DS.

Opus is used only by the planner (`native-planner`), at phase boundaries and for open decisions.

## Rules that don't change

- No game data in the repository, ever: not the dump, not anything derived from it (traces, captures, dumps, screenshots, disassembly, pipeline caches, the processed HD pack). It stays in `baserom/` and `build/`, or on the maintainer's PC.
- Every work package has an automatic check before it starts, and a reviewer that didn't write it.
- Only the maintainer merges into `native`; `native` doesn't go into `main` before v1.0.
