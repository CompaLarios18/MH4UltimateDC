---
name: native-review
description: How a reviewer checks one finished work package of the MH4U native port against its spec - re-run the checks, read the diff for scope, gamed tests, game data and provenance, and give a verdict. Use whenever a message asks you to review a native-port WP branch.
---

# Reviewing one native-port work package

You are a reviewer. You get a **WP ID**, its **branch** (`wp/<ID>-<slug>`), its **worktree**, its **base** (the commit the worktree started from; it may already contain other WPs of the session's batch), the worker's report, the results of its CI run if there was one, and the **baseline**: which tests already fail at the base. You did not write this code, and you don't change it: you check it and give a verdict.

**Your shell's working directory is reset before every command:** start every Bash command with `cd <worktree> && ` (or use `git -C <worktree>`), and give Read only absolute paths. A command is cut off after 2 minutes unless you give a longer timeout, and after 10 minutes in any case: run a long build or test in the background (`cd <worktree> && mkdir -p build/logs && nohup bash -c '<command>; echo "<name> exit=$?"' > build/logs/<name>.log 2>&1 &`) and poll its log.

## Steps

1. **Read the spec:** the WP's section in `docs/native/wp/<phase>.md`, the rules at the top of that file (and of `wp/N1.md` for `native/` code), and `docs/native/AGENTS.md` §8.

2. **Check the worktree and read the whole diff:**
   ```
   cd <worktree> && git status --porcelain          # must print nothing, or the verdict is `changes`
   cd <worktree> && git diff --stat <base>...HEAD
   cd <worktree> && git diff <base>...HEAD
   ```
   A file the worker created but didn't commit would pass the checks here and be missing from the merge.

3. **Re-run the checks yourself.** Don't trust the worker's log.
   - every **Done when** command this session can run;
   - `cd <worktree> && python3 -m pytest -q -rfE`, comparing failures with the baseline;
   - if `native/` changed: the configure line, build and `ctest` of the native-wp skill;
   - `cd <worktree> && python3 tools/native/wp_status.py check`;
   - once it exists: `cd <worktree> && python3 tools/native/check_no_game_data.py --range <base>..HEAD`.

   Items marked *(with the dump)*: run them if the orchestrator gave you the staged paths; otherwise note `pending (dump)`. Items marked *(CI)*: use the CI results you were given; otherwise note `pending (ci)`.

4. **Check, in this order:**
   - **Scope:** every changed file is in the allowlist; nothing outside the WP's goal.
   - **The checks are real:** tests encode the spec's **Done when**; no assert removed or loosened; no input special-cased; no tolerance or threshold changed; no test skipped without the spec's reason (for example a missing `MH4U_RUNNER`).
   - **No game data:** no traces, captures, dumps, screenshots, disassembly, game text or paths, or other derived data in the diff or the report; fixtures are synthetic and small.
   - **Provenance:** code copied from Azahar has the header line and is listed in the report's `derived-from-azahar`; nothing of Azahar's architecture (kernel, memory system, timing) was copied.
   - **Decisions:** the code doesn't make a design choice the spec and DECISIONS.md don't make.
   - **Registration:** options, services, GPUs and checks register from the WP's own module; nothing edits the runner, the capture player or another module to add them.
   - **Style:** like the neighbouring code; names and comments say what things are; no dead code.
   - **Behaviour:** read the core logic against the spec and against the Azahar code it cites, where the spec says Azahar is the reference.

5. **Verdict**, in this format and nothing else:
   ```
   wp: <ID>
   verdict: approve | changes | reject
   checks-rerun:
     - <command> → <result>
     - (with the dump) <command> → pending (dump) | <result>
     - (CI) <what> → pending (ci) | <result>
   problems:
     1. <file:line> — <what is wrong, why, and the fix you expect>
   scope: ok | <files outside the allowlist>
   game-data: none found | <what kind and where, without quoting it>
   ```
   - `approve`: every check you could run passes and there are no problems (nits can be listed after `approve` as "optional").
   - `changes`: concrete, fixable problems. Each must say what to do.
   - `reject`: the approach can't meet the spec, or game data is in the commits. Say why.

## Never

- Edit, commit or push. You only read and run.
- Approve with a failing check, or with one this session could have run and you didn't.
- Approve a weakened check because "it was flaky".
