---
name: native-wp
description: How a worker implements one work package of the MH4U native port (docs/native/wp/) in the worktree it was given - read the spec, test first, stay in the allowlist, run every check, commit locally, report. Use whenever a message gives you a native-port WP ID to implement.
---

# Implementing one native-port work package

You are a worker. The orchestrator gave you a **WP ID**, a **worktree path**, the **base** commit your worktree starts from, the **commit identity and trailer lines** to use, and, when the WP needs them, the **absolute paths** of staged game files, recorded sessions, the reference runner and where shared outputs go. You implement that one WP and report. The full manual is `docs/native/AGENTS.md`; this file is what you need from it.

## Where you work

- **Your shell's working directory is reset before every command.** Start every Bash command with `cd <worktree> && ` (or use `git -C <worktree>`), every time.
- **Long commands:** a command is cut off after 2 minutes unless you give a longer timeout, and after 10 minutes in any case. Run anything longer (a first build, `setup_env.sh`, a long test run) in the background, `cd <worktree> && mkdir -p build/logs && nohup bash -c '<command>; echo "<name> exit=$?"' > build/logs/<name>.log 2>&1 &`, then check the end of the log with short commands until the `exit=` line appears.
- Give Read, Edit and Write only paths that start with `<worktree>/`.
- Never change anything under the main checkout (`/home/claude/mh4ultimatedc`) outside `.claude/worktrees/<ID>`, except the shared output folders the orchestrator named (for example `/home/claude/mh4ultimatedc/build/golden/`): outputs that later WPs need go there, because removing a worktree deletes its own `build/`.

## Steps

1. **Check the worktree.**
   ```
   cd <worktree> && git status --short && git branch --show-current
   cd <worktree> && git merge-base --is-ancestor <base> HEAD && git merge-base --is-ancestor origin/native HEAD && echo ok
   ```
   The status must be clean, the branch `wp/<ID>-<slug>`, and `ok` printed. If not, report `blocked` at once. Your changes are `git diff <base>...HEAD`: the base may already contain other WPs of this session's batch. If the orchestrator later asks you to bring your branch up to date, `git merge --no-edit <the sha it gives>`; that sha is your new base.

2. **Read, in this order, and nothing else unless the spec sends you there:**
   - your WP's section in `docs/native/wp/<phase>.md` (find it with `grep -n "^### <ID> " docs/native/wp/*.md`);
   - the rules at the top of that file and, for `native/` code, the rules at the top of `wp/N1.md`;
   - `docs/native/AGENTS.md` §8 (rules) and §9 (report format);
   - the WP's **Read first** list, and the decisions it mentions in `docs/native/DECISIONS.md`.

   Azahar's sources at the pinned commit are in `~/azahar` (the orchestrator fetches them). Read them; never build or run code from that tree unless the spec says so. If they are missing, say so in the report instead of fetching them yourself.

3. **Plan in a few lines:** the files you will create or change (all inside the allowlist), and how each **Done when** item will be checked. If the spec is ambiguous, contradicts the code, or needs a choice that neither it nor DECISIONS.md makes, stop and report `needs-decision` with the options you see.

4. **Write the tests first** (they encode **Done when**), then the code.
   - Python tools: standard library first, type hints, a test in `tests/` with synthetic data only.
   - C++ in `native/`: C++20, clang, a module in its own directory under `native/src/` with its own `CMakeLists.txt`, doctest tests in `native/tests/<name>_test.cpp` (picked up automatically). Options, services, GPUs and checks register themselves from your module (`mh4n/registry.h`, `mh4n/options.h`): don't edit the runner, the capture player or another module to add yours.
   - Follow the style of the neighbouring code.

5. **Run every check**, and read the real output:
   - every **Done when** command this session can run;
   - `cd <worktree> && python3 -m pytest -q -rfE` (the whole suite; the orchestrator tells you which failures already exist at the base);
   - if `native/` changed (the first build in the background):
     ```
     cd <worktree> && cmake -S native -B build/native -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
       -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMH4N_WERROR=ON && cmake --build build/native && ctest --test-dir build/native --output-on-failure
     ```
   - `cd <worktree> && python3 tools/native/wp_status.py check`;
   - once it exists: `cd <worktree> && git add <your files> && python3 tools/native/check_no_game_data.py --staged`.

   Items marked *(with the dump)* need staged game files (and, when they use a golden session, its recordings): if you weren't given their paths, report them as `pending (dump)`. Items marked *(CI)* run in GitHub Actions: report them as `pending (ci)`; the orchestrator runs them.

6. **Fix and re-run.** You get **two fix rounds** after the first failure. If a check still fails, report `blocked` with what you learned. Never weaken a check: no deleted or loosened asserts, no special-cased inputs, no raised tolerances, no skipped tests.

7. **Commit** your WP's files only (`git add <paths>`, never `git add -A`), and check nothing is left over:
   ```
   cd <worktree> && git -c user.name="<name>" -c user.email="<email>" commit -m "<ID>: <imperative summary>" -m "<optional body>" -m "<trailer lines>"
   cd <worktree> && git status --porcelain      # must print nothing
   cd <worktree> && git diff --stat <base>...HEAD   # only allowlisted files
   ```
   Several commits are fine. **Don't push**, don't open PRs, don't merge.

8. **Report** in this format, and nothing else:
   ```
   wp: <ID>
   status: done | partial | blocked | needs-decision
   branch: wp/<ID>-<slug> @ <short sha>
   files: <created/changed, one line each>
   checks:
     - <command> → <result, summarised from the real output>
     - (with the dump) <command> → pending (dump) | <result>
     - (CI) <what> → pending (ci)
   derived-from-azahar: <your file ← Azahar path, one per line> | -
   proposed-shared-changes: <lines for STATUS, DECISIONS, contracts, config, THIRD_PARTY> | -
   notes: <blocker, open question, surprise, what is left for `partial`; at most 5 lines>
   ```
   `partial` means you ran out of budget or time: commit what works (`<ID>: WIP …`, allowlisted files only), and say exactly what is left.

## Rules that are never bent

- **No game data anywhere you write:** not in a commit, a test fixture, a log you paste, or the report. That includes traces, captures, dumps, screenshots, disassembly, file paths and text from the game, and anything else derived from it. Quote only service names, headers, result codes, sizes, CRCs and counts. Outputs go to `build/` (git-ignored). Test fixtures are synthetic.
- **Stay inside the allowlist.** If another file needs a change, do the minimum inside the allowlist and put the rest in `proposed-shared-changes`.
- **Shared files are the orchestrator's:** `docs/native/{ROADMAP,AGENTS,STATUS,DECISIONS}.md`, `docs/native/wp/*`, `CLAUDE.md`, `.claude/**`, `native/include/mh4n/**` (once merged), `native/THIRD_PARTY.md`, `config/**` and `include/**` (unless your allowlist names the file), and `tools/recomp/runtime/recomp.h` (outside N0-17). Entries for `config/native/ipc_time_dependent.txt` go in `proposed-shared-changes`, each with its reason.
- **Copying from Azahar is allowed** (D6) for individual functions and tables, never its architecture (kernel, memory system, timing). Every copied file starts with:
  `// Derived from Azahar <path> at <commit> (GPL-2.0-or-later or GPL-3.0-or-later, as the file says)`
  followed by the source file's own `// Copyright …` line(s), and goes in `derived-from-azahar`. A file that takes
  its names or layouts one for one from an Azahar header (enumerations, structures) counts as derived too.
- **No design decisions.** `needs-decision` is a good outcome when the spec doesn't decide.
- **Token budget:** S ~150 k, M ~400 k, L ~1 M. Past it, stop and report `partial`.

## When something goes wrong

| Situation | Do |
|---|---|
| The build takes long | Build once in the background, then incrementally; the machine has 2 cores. Don't start a second heavy build |
| A command was cut off at 2 or 10 minutes | Run it again in the background with `nohup … &` and poll the log |
| A tool or package is missing | Install it with apt or pip (`--break-system-packages`) only if your spec lists it; otherwise report it |
| A network request is refused | The sandbox allows package registries and GitHub only. Don't retry other ways; report it |
| The spec cites a file or function that doesn't exist | `needs-decision`, with what you found instead |
| A permission prompt blocks an action | Report it; don't work around it |
