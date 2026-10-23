# Notes for AI-assisted sessions

Project: decompilation of Monster Hunter 4 Ultimate (3DS, ARM11 / ARMv6K) aiming for a native PC port. Read `docs/ROADMAP.md` for the current phase.

Hard rules:
- Never add game data (ROM, code.bin, exheader, RomFS files, keys) to the repo, and never help obtain unowned copies. Work only from the maintainer's own dump in `baserom/` (git-ignored).
- Tools are Python 3.10+, standard library first; every tool change needs a test in `tests/` using synthetic data.
- Run `python3 -m pytest` before committing.
- Multi-agent work follows `docs/AGENT_WORKFLOW.md`: only the orchestrator edits shared files (`config/*`, `include/*`); workers write their own new files and return results. A function counts as decompiled only when `difftest` passes; report `blocked` rather than weakening a test.
- Ghidra exports (`build/export/`) are derived from game code: read them, never commit them.
- The 100% native port (no Azahar) lives on branch `native`. On any other branch, for anything about it, read its runbook first, `git fetch origin native && git show FETCH_HEAD:.claude/skills/native-orchestrate/SKILL.md`, and follow it (it sets the branch up). Never commit native-port work to `main`.

Key facts:
- `code.bin` from ExeFS is usually BLZ (backward LZ77) compressed; exheader flag bit 0 at offset 0x0D says so. `tools/blz.py` handles it.
- Segment addresses come from the exheader SCI (text/ro/data/bss). `tools/codebin2elf.py` turns them into an ELF for Ghidra (`ARM:LE:32:v6`).
- Engine is Capcom's MT Framework Mobile (verify details against the binary before relying on them).

## Native port (this branch: `native`)

- All native-port work lives on branch `native` (D16 in `docs/native/DECISIONS.md`): batch branches `batch/<yyyymmdd>-<topic>` start from `origin/native` and work branches `wp/<ID>-<slug>` from the batch, PRs go into `native`, and only the maintainer merges them. Never commit native-port work to `main`; never push to `main` or `native`.
- The plan is in `docs/native/`; on this branch, `docs/native/README.md` is the starting point instead of `docs/ROADMAP.md`. The live status is `docs/native/STATUS.md`, kept with `python3 tools/native/wp_status.py check | sync | ready | overlaps | pending | set`.
- The main session follows `.claude/skills/native-orchestrate/SKILL.md` (read the file directly if the skill isn't listed). Workers follow `native-wp`, reviewers `native-review`; the agent definitions are `.claude/agents/native-*.md`. On this branch the shared files are those of `docs/native/AGENTS.md` §4; a native WP may change a file under `config/` that its own allowlist names.
- Code in `native/` never includes or links Azahar; the rules are at the top of `docs/native/wp/N1.md`.
- Talk to the maintainer in Spanish; write the repository in English.

### Verification policy (the maintainer's, 2026-10-23)

Builds, CI waits and full suites cost more time than they save when run on every change. This applies to every session on this branch: orchestrators, workers and reviewers.

- **Level 1, on every change (fast, always):** build only the target you touched (incremental; never rebuild `native/` from scratch) and run only the tests related to what you changed (`ctest -R <pattern>`, `pytest <file>` or `-k`). If they pass, go on to the next task.
- **Level 2, when a batch closes or before a PR is declared ready:** the full `pytest` and the full `ctest`, once. Group independent packages (several N0-xx, for example) in one PR instead of one PR per task.
- **Level 3, only when the maintainer asks:** dispatching `native-app.yml` or any workflow that makes Windows artifacts, and rebuilds from scratch.
- **CI:** do not wait for it. Push and go straight on to the next task; look at its state when the next batch starts or just before the PR is declared ready, not before. Never wait or poll in a loop ("wait 9 minutes and look again"); if CI is still running and there is nothing else to do, end the turn and say what is pending. When Linux is green and the local tests pass, the PR is ready to merge; if Windows or macOS fail afterwards, fix it in the next PR instead of blocking. (Who merges is unchanged: the maintainer, with "Create a merge commit".)
- **Reporting:** do not narrate every step. Report once, at the end of a batch: what was done, what passed or failed, and what comes next.
