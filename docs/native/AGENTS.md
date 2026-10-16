# How agents work on the native port

The operating manual for branch `native`. It extends [`../AGENT_WORKFLOW.md`](../AGENT_WORKFLOW.md) (written for decompilation) to the whole native port. It is written so that the day-to-day work runs on **Sonnet**, with many subagents at once: the hard thinking is in the work-package specs ([`wp/`](wp/)), the decisions ([`DECISIONS.md`](DECISIONS.md)) and the checks, not in the session.

The runbooks agents follow are skills: `.claude/skills/native-orchestrate/` (the main session), `native-wp/` (workers) and `native-review/` (reviewers). This file explains the system they implement; where a command is given in both, the skill's is the one to run.

## 0. In one minute

- **The one rule** (unchanged): agents only get work that has an automatic pass/fail check. Every work package (WP) has a *Done when* list of commands with expected results. No check, no WP.
- **The unit of work** is one WP: one worker, one branch (`wp/<ID>-<slug>`), one spec section in `wp/`, and a list of the files it may touch.
- **The orchestrator** (the main session, with the maintainer):
  - picks the ready WPs from [`STATUS.md`](STATUS.md) that don't share files, and starts a worker for each;
  - has each result reviewed by a reviewer that didn't write it;
  - merges approved WPs into a batch branch (`batch/<date>-<topic>`), starts what they unblock on top of it, and opens **one PR per batch into `native`**.
- **The maintainer** merges batch PRs, settles decisions, and runs what only his machines can: performance, the AMD driver, the Mac, the real 3DS.
- **Models** (D17): the main session on Sonnet 5.5 in ultracode, so it can run many subagents at once; workers on Sonnet (medium, or high for packages marked so), reviewers on Sonnet high, and the planner on Opus high, only at phase boundaries and for open decisions.

## 1. Roles

| Role | Who | Does | Never does |
|---|---|---|---|
| Maintainer | Luis | Merges batch PRs; settles `Open` decisions; runs perf/GPU/3DS jobs on his machines; keeps his PC on when a session stages the dump | — |
| Orchestrator | The main session (Sonnet 5.5 in ultracode; `native-orchestrate` skill) | Reads STATUS; picks ready WPs; creates worktrees; stages game files; starts workers and reviewers; merges approved WPs into the batch; runs integration checks; updates STATUS and the project's `ESTADO.md`; opens the batch PR | Writes large features itself; merges into `native` or `main`; weakens a check |
| Worker | Subagent `native-worker` / `native-worker-high` (`native-wp` skill) | Implements one WP in its worktree, inside the WP's file allowlist; runs the *Done when* checks; commits locally; reports | Edits shared files; pushes; works on two WPs; makes design decisions |
| Reviewer | Subagent `native-reviewer` (`native-review` skill) | Reviews one WP's diff against its spec; re-runs its checks; approves or lists concrete changes | Reviews its own work; changes the code |
| Planner | Subagent `native-planner` (Opus high) | Details the next phase's WPs from [`wp/LATER.md`](wp/LATER.md); analyses open decisions with the evidence; redesigns WPs blocked twice for design reasons | Implements code |

## 2. Models and effort

**Where the setting lives:**
- **Main session:** the model and mode picked when the session starts in the Claude app. For an orchestrator day, the maintainer picks Sonnet 5.5 in ultracode, so the session can run many subagents at once.
- **Subagents:** the definitions in `.claude/agents/` (frontmatter `model`, `effort`, `skills`). The orchestrator starts them by name and does not pass `model` or `effort`, so the definition decides.
- **When those agent types aren't available** (a session that started on another branch, or that cloned the repository mid-session, may not have them): the orchestrator starts a `general-purpose` agent with `model: sonnet` and the effort below, and tells it which definition and which skill to read, by absolute path; a reviewer is also told it may only read and run commands. The `native-orchestrate` skill (§6) has the exact lines.
- Effort levels are `low`, `medium`, `high`, `xhigh` and `max`.

| Agent (`.claude/agents/`) | Model | Effort | Use for |
|---|---|---|---|
| `native-worker` | `sonnet` | medium | WPs whose agent in STATUS.md is `native-worker`: tools, docs, services, ports of known algorithms, tests |
| `native-worker-high` | `sonnet` | high | WPs whose agent is `native-worker-high`: the scheduler, guest memory, the IPC core, the PICA command processor, shader translation, the fragment pipeline, recorders inside Azahar, anything concurrent |
| `native-reviewer` | `sonnet` | high | Every WP before it enters a batch |
| `native-planner` | `opus` | high | Phase-boundary planning, `Open` decisions, a WP `blocked` twice for design reasons |

**Escalation:** a WP that fails its checks after two fix rounds goes back to the orchestrator as `blocked`.
- If the cause is a missing fact or tool, the orchestrator splits off or adds a WP.
- If it is a design question, the WP is marked `needs-decision` and goes to the planner, or to the maintainer.
- Nobody keeps retrying.

## 3. Branches, worktrees, commits and PRs

**Branches (D16):**
- `main` is the shipping v0.x port. Nothing native goes there.
- `native` is the integration branch, and only receives merged batch PRs.
- `batch/<yyyymmdd>-<topic>` is the orchestrator's branch for one session, started from `origin/native`. Approved WPs are merged into it one at a time; it is pushed at the end and becomes the PR.
- `wp/<ID>-<slug>` is one WP (the slug: two to four words of its title, e.g. `wp/N0-02-cloud-env`), created from the batch's current tip in its own worktree. The orchestrator pushes it when its CI checks must run (after the reviewer found no game data in it: the repository is public), and at the end of a session when the WP is unfinished (a WIP branch the next session resumes, after the checks of the skill's §9); otherwise it stays local and is deleted at the end of the session.
- Git can't have a branch named `native` and branches under `native/` at the same time; that is why work branches are `wp/…` and `batch/…`.
- Create branches with `--no-track`: a branch that tracks `origin/native` can't be deleted with `git branch -d` after it is merged.
- The cloud clone maps only `main`; the orchestrator adds fetch refspecs for `native`, `wp/*` and `batch/*` and unshallows it before anything else.

**Worktrees and stacking:**
- The orchestrator creates one worktree per worker, under `.claude/worktrees/` (git-ignored), from the batch's tip: `git worktree add --no-track -b wp/<ID>-<slug> .claude/worktrees/<ID> HEAD`, run in the main checkout on the batch branch. That commit is the WP's **base**: the worker and the reviewer diff against it.
- So a WP whose dependencies were merged into the batch earlier in the session can start at once, on top of them, and a WP that shares files with one already merged starts after it with its changes in place. `wp_status.py ready --assume-done <IDs merged this session>` lists what can start; nothing reaches `native` until the maintainer merges the batch.
- Don't use a subagent's automatic worktree isolation: by default it branches from the default branch (`main`), not from the batch.
- A subagent's shell starts in the main checkout before every command, so workers and reviewers prefix each command with `cd <worktree> && `. Outputs later WPs need (golden sessions, captures, code libraries) go to the main checkout's `build/`, since removing a worktree deletes its own.

**Commits:**
- Author: the same identity as the rest of the history, read from `git log -1 --format='%an <%ae>' origin/main` and passed with `git -c user.name=… -c user.email=… commit`.
- Message: `<ID>: <imperative summary>`, an optional body, then the attribution trailer lines the session's instructions specify. Workers don't see those instructions; the orchestrator passes the lines in the dispatch message.
- One WP may have several commits; keep them in the WP's scope.
- `STATUS.md` changes stay uncommitted in the main checkout during the session and are committed once at the end, after every row is settled, so no pushed commit shows a WP as `active`.

**Batch PRs:**
- Push the batch branch, then open the PR with the REST API (GraphQL, and so `gh pr create`, is not available from these sessions):
  ```
  gh api -X POST repos/CompaLarios18/MH4UltimateDC/pulls \
     -f title="Batch <date>: <IDs>" -f head="batch/<date>-<topic>" -f base="native" -F body=@<file>
  ```
- The body has a table of WPs (ID, title, checks run with their results, reviewer verdict, status), the CI results, anything that needs the maintainer, what is blocked, what is ready next, and the attribution lines the session's instructions specify for PR descriptions. The template is in the `native-orchestrate` skill.
- Then each WP's note gets the PR number (`PR #<n>`): that is how the next session finds a PR's WPs.
- **Only the maintainer merges** (GitHub: **Create a merge commit**). The sessions' permission checks block merging without a human review, and that is intended.
- **One open batch PR at a time.** If the previous one is still open when a session starts, the orchestrator asks the maintainer to merge it; if he can't, the new batch is built on top of it (its PR says it includes the other). Two batches side by side would conflict in STATUS.md.

**Syncing `main` into `native`:** after each release from `main`, the orchestrator merges `origin/main` into a batch branch (`batch/<date>-sync-main`), resolves conflicts, runs the checks and opens the PR. `native` is never merged into `main` before v1.0. A seam useful for v0.x (say, a recorder) can go to `main` only as a separate PR the maintainer asks for.

**Never:**
- push to `main` or `native`;
- force-push a branch someone else uses;
- rewrite published history (a batch can be reset only before it is pushed);
- commit with `--no-verify`.

## 4. Work packages

**Where they are:**
- Specs: [`wp/N0.md`](wp/N0.md), [`wp/N1.md`](wp/N1.md) and [`wp/N2a.md`](wp/N2a.md), detailed. Each file starts with rules shared by all its WPs (for `native/` code, the rules at the top of `wp/N1.md` apply in every phase).
- [`wp/LATER.md`](wp/LATER.md): later phases, outlined; the planner details them at each phase boundary.
- [`STATUS.md`](STATUS.md): live status. `tools/native/wp_status.py` reads the specs and keeps it consistent:
  - `check` (specs and table agree, dependencies exist, no cycles), `sync` (regenerate the tables);
  - `ready [--no-dump] [--assume-done IDs]` (what can start, the WPs that unblock the most first);
  - `overlaps ID…` (pairs whose allowlists share files) and `overlaps --all` (every such pair that could be in flight at once; the specs are written so it prints nothing once the bootstrap, N0-01, is done);
  - `pending` (notes with something still owed), `set <ID> <status> [--note … | --add-note …]`.

**Spec format** (every WP has every field; `-` when empty). `wp_status.py` parses the first three lines and the allowlist, so keep their shape:

```
### <ID> · <title>
- **Size / agent:** S|M|L · native-worker | native-worker-high | orchestrator | maintainer
- **Depends on:** <IDs, or "A to B" for a range>  · **Needs:** <tags, comma-separated>
- **Goal:** one or two sentences.
- **Read first:** the only files and pages to read before starting.
- **Files (allowlist):** every path in backquotes: `dir/file.ext`, `dir/**`, `dir/{a,b}/**`, `name.ext` (next to the path before it); remarks in parentheses.
- **Spec:** what to build: names, formats, interfaces, behaviour.
- **Done when:** commands and expected results. All of them must pass.
- **Traps:** what earlier work learned the hard way.
- **Out of scope:** what not to do here.
```

**Sizes:**
- S: under half a session.
- M: one session.
- L: two or three sessions. Prefer splitting.
- Anything bigger is split before it starts.

**Needs:**
- `none`: nothing beyond the cloud session.
- `ci`: some checks run only in GitHub Actions (marked *(CI)*), or the C++ only builds in a long local build.
- `dump`: the game files staged into the session (section 6), for the whole WP.
- `dump-check`: only the last *Done when* items (marked *(with the dump)*) need the game files. The rest can be done without them, so the WP can always start; those items stay pending until a session that has them. An item that uses a golden session's recordings also waits for the WP that records it (N0-09, N0-10, N0-11).
- `save`: the maintainer's save and extdata. With `dump-check`, only for the *(with the dump)* items.
- `pc`: the reference PC (performance, the AMD driver).
- `mac`: the M2.
- `3ds`: the real console.

**Status lifecycle (in STATUS.md):**
- `todo` → `ready` (shown when every dependency is `done`) → `active` (a worker has it, this session) → `review` (merged into the batch, then in its open PR) → `done` (the batch PR was merged into `native`).
- `blocked` and `needs-decision`, with the reason in the note, can happen at any point.
- The note carries the PR (`PR #<n>`) and what is still owed, and keeps it until it's settled: `dump-check pending`, `ci pending (run <id>)`, `waiting for D<n>` (a WP whose *Done when* ends in a decision: it stays `review` until the decision is `Decided`, and doesn't unblock anything before), `WIP on origin/wp/<ID>-<slug> (base <sha>): <what is left>` (unfinished work pushed at the end of a session), `restart: <why>` (start again from scratch). While a WP is `active`, the note also has its base (`session <date>; base <sha>`). `wp_status.py pending` lists the owed ones.
- No row stays `active` when a session ends: unfinished WPs go back to `todo` or `blocked`, with their WIP branch in the note.

**Shared files.** Only the orchestrator edits these; workers propose changes in their report:
- `docs/native/{ROADMAP,AGENTS,STATUS,DECISIONS}.md` and `docs/native/wp/*`;
- `CLAUDE.md` and `.claude/**`;
- `native/include/mh4n/**` (the contracts, once each is merged);
- `native/THIRD_PARTY.md`;
- `config/**` and `include/**`, except a file the WP's own allowlist names;
- `tools/recomp/runtime/recomp.h`, outside N0-17.

A worker may create new files its WP lists, and change existing files its WP lists. In `native/`, a WP adds its options, services, GPUs and checks from its own module through the registries (`wp/N1.md`), never by editing the runner, the capture player or another module.

## 5. Session protocols

### 5.1 Orchestrator
The steps, with every command, are the `native-orchestrate` skill. In short:
1. **Repository with push access.** In a cloud session: `add_repo` (owner `CompaLarios18`, repo `MH4UltimateDC`, access `push`) clones it to `/home/claude/mh4ultimatedc`, shallow, with only `main` mapped. Add the `native`, `wp/*` and `batch/*` fetch refspecs, unshallow, fetch, check the bootstrap is merged, create the batch branch from `origin/native`, and call `register_repo_root` so this branch's `CLAUDE.md`, skills and agents load. Fetch Azahar's sources once, and run the test suite for a baseline.
2. **Read** `wp_status.py check` and `pending`; the merged, open and closed batch PRs (merged → `done`, unless waiting for a decision; open → their WPs are in flight; not mergeable → bring up to date; closed → `blocked`); the WIP branches left by earlier sessions; the project's `ESTADO.md` if the project is attached; the open decisions.
3. **Choose** with `ready` every WP whose needs the session can meet, with two limits: `overlaps` prints nothing for the set in flight (here or in open PRs), and one build-heavy WP at a time (2 cores, 7 GB). Tell the maintainer in a few lines, in Spanish.
4. **Builds:** `setup_env.sh` (once N0-02 exists) in the background; the reference runner artifact, or one built by CI from a WP's own branch when the WP changes the runner; code libraries from the same commit as the runner.
5. **Stage** game files if a WP needs them (section 6).
6. **Dispatch:** a worktree per WP from the batch's tip; the workers in one message, so they run in parallel; `wp_status.py set <ID> active`.
7. **Review and merge:** one `native-reviewer` per finished WP, in parallel. `approve` → a WP with *(CI)* items is pushed and merges once they pass; then merge into the batch, one at a time, each followed by quick integration checks; a failure undoes that merge. Then start what the merge unblocked. `changes` → back to the same worker (SendMessage), at most two rounds; then `blocked`. `reject` for game data → the branch is deleted, never pushed.
8. **Integrate:** the checks again on the final batch, `wp_status.py sync`; owed dump-checks and CI results of earlier WPs.
9. **Wrap up:** push unfinished WPs as WIP branches (checked first); commit STATUS; push the batch and open the PR; put its number in the notes; watch its CI and record the results; update `ESTADO.md`; tell the maintainer what needs his click or his machine. Push before ending the turn: a session-end check flags local branches with unpushed commits.

### 5.2 Worker (`native-wp` skill)
Every command starts with `cd <worktree> && `; file tools get paths inside the worktree; anything longer than two minutes runs in the background.
1. Check the worktree: clean, on `wp/<ID>-<slug>`, descending from the base and from `origin/native`.
2. Read the WP's spec section, the rules at the top of its file, then its *Read first* list, and nothing else unless the spec sends you there.
3. Write the tests that encode *Done when* first, then the code.
4. Run every *Done when* command the session can, plus `python3 -m pytest`, `ctest` when `native/` changed, and `wp_status.py check`. *(with the dump)* items without staged files and *(CI)* items are reported as pending.
5. Commit your WP's files only (`git add <paths>`), with the identity and trailers you were given; `git status --porcelain` must then print nothing, and `git diff --stat <base>...HEAD` show only allowlisted files. Don't push.
6. Report in the format of section 9.

### 5.3 Reviewer (`native-review` skill)
Every command starts with `cd <worktree> && `.
1. Read the spec and the rules at the top of its file.
2. Check the worktree is clean (an uncommitted file passes the checks but is missing from the merge), then read `git diff <base>...HEAD`.
3. Re-run *Done when* yourself, and pytest against the baseline; don't trust the worker's log.
4. Check scope, tests (not gamed, thresholds untouched), no game data, provenance headers on copied Azahar code, decisions the spec doesn't make, registrations from the WP's own module, the behaviour against the cited Azahar code, and the style of the neighbouring code.
5. Verdict in the format of section 9.

### 5.4 Planner (phase boundary or open decision)
- **Phase boundary:** the orchestrator starts `native-planner` with the phase to detail, the exit report of the previous phase (STATUS, research notes, measurements), and `wp/LATER.md`.
  - The planner writes the new `wp/<phase>.md` in the spec format, in a worktree, and checks it with `wp_status.py sync`, `check` and `overlaps --all` there, committing only the spec file (and `LATER.md` if it changes it).
  - It splits anything over L, gives every WP checks and its own files, and proposes decision changes.
  - The orchestrator merges it, runs `wp_status.py sync`, and puts the result in a batch PR, which the maintainer reads.
- **Open decision:** the planner gets the evidence (spike report, census) and writes the options, its recommendation and the consequences. The maintainer decides.

## 6. Getting game files into a cloud session

Game files never enter the repository, a PR, a log or a report, a file sent to the chat, or the project's file area (`/mnt/project-files/`). They live in `baserom/` and `build/`, which are git-ignored, inside the session's container, which is discarded afterwards.
- **Only the orchestrator stages them:** the device tools belong to the main session. It checks the PC is reachable (`get_device_info`) and, if the game folder isn't connected, asks for it once (`device_request_folder_access`).
- **Workers in the same session can use them:** the orchestrator passes the absolute paths (under the main checkout's `baserom/` and `build/`) in the dispatch message.
- **Staging checks:** each staged part must have the size the staging call reported before it is joined; the joined dump is checked against its own hashes (`tools/verify_ncch.py`); the parts are deleted after joining (disk space).

The game files are on the maintainer's PC (`C:\NoJales\mh4pcport`):

| What | Where | How to bring it |
|---|---|---|
| The game, decrypted CXI, in 8 parts of 350 MB | `partes\juego.cxi.parte00` … `parte07`, made from the decrypted `.3ds` by `scripts\partir_3ds.ps1` in that folder (on the PC, not in the repository) | One `device_stage_files` call per part (the limit is about 500 MB per call), then `cat /mnt/user-data/uploads/mh4pcport/partes/juego.cxi.parte0* > baserom/juego.cxi` |
| The v1.3.0 update | `0004000E00126300 MONSTER HUNTER 4 ULTIMATE (CTR-U-BFGE) (U) (v1.3.0).standard.cia` (48 MB) | One call; copy to `baserom/update_v130.cia` |
| The ELF for disassembly | `mh4u_v130.elf` (16 MB) | One call |
| Ghidra exports | `export\functions.tsv`, `callgraph.tsv`, `strings.tsv` | One call; copy to `build/export/` |
| The maintainer's save | `save\00000001\` (save data: `system`, `user1`) | One call; copied into a user folder for the runner, where Azahar keeps the game's save (run the runner once with `--user DIR`, then `find` the folder) |
| Extdata (guild cards, Palico, downloaded quests) | `save\user\` (`card1-3`, `cardbox`, `otomo1`, `quest1-5`) | One call; into the same user folder's extdata. The staged originals are never written to |
| Recorded sessions, captures and code libraries, if kept | `golden\` (parts of 25 MB) | Only if a previous session saved them there; stage the parts and join them with `cat` |

- If the PC is asleep or the desktop app is closed, the staging calls fail. Say so, and do the WPs that need no game files.
- If `verify_ncch.py` finds bad blocks in the joined dump, the dump or a part is damaged (it has happened): the maintainer dumps or splits it again.
- The code library is built in the session from the update (`tools/build_game.py`). It is game code too.
- **Keeping what a session recorded** (optional, and only in sessions not attached to a project): golden sessions, captures and code libraries can go back to the PC, split into parts of 25 MB (`device_commit_files` takes files up to 30 MB), into `golden\`, through `/mnt/user-data/outputs/`, which is emptied afterwards. That saves a later session from recording them again. In a project session the output area is the project's, so this is skipped.

## 7. Building and testing

- **Long commands:** the shell tool cuts a command off after 2 minutes unless it is given a longer timeout, and after 10 minutes in any case. Builds, `setup_env.sh` and waits for CI run in the background, `mkdir -p build/logs && nohup bash -c '<command>; echo "<name> exit=$?"' > build/logs/<name>.log 2>&1 &`, and are polled until the `exit=` line appears.
- **Cloud environment:** `tools/native/setup_env.sh` (N0-02) installs everything; with `--with-azahar` it also fetches Azahar's sources at the pinned commit, and `--azahar-only` does only that.
  - The container is Ubuntu 24.04 with 2 cores and 7 GB. Its default C and C++ compilers are GCC: the configure line names clang.
  - apt works: CMake 3.28, Ninja, clang 18, glslang, the Vulkan loader, and lavapipe (a CPU Vulkan driver, so renderer tests run without a GPU).
  - SDL3 is built from source.
  - Network access is limited to package registries and GitHub. GitHub Actions artifacts are served from `*.blob.core.windows.net` (seen: `productionresultssa2.blob.core.windows.net`), a host the default policy denies: `gh run download` fails with `Forbidden` until the maintainer adds it under Allowed domains in the environment's network settings (first met in the 2026-10-16 session).
- **Python tools:** stdlib first. Every tool change has a test in `tests/` with synthetic data. Run `python3 -m pytest` (at the start of the branch: 312 passed, 75 skipped because the reference runner, the app or the dump isn't there; the bootstrap adds `wp_status.py`'s tests).
- **`native/`** (from N1-01), with the same configure line everywhere (workers, reviewers, the orchestrator, Linux CI):
  ```
  cmake -S native -B build/native -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMH4N_WERROR=ON
  cmake --build build/native
  ctest --test-dir build/native --output-on-failure
  ```
- **CI** (`native.yml`, from N0-03): runs on pushes to `native` and `wp/**` and on PRs into `native`; it builds the reference runner when `port/`, `tools/` or `config/` changed. `ci.yml` (from `main`) runs pytest and the game-data guard on every PR, and `native-guard.yml` (N0-19) runs `check_no_game_data.py`. **Checks only CI can run** (marked *(CI)*): once the reviewer approves, the orchestrator pushes the WP's branch, which starts the runs, polls them, runs the items that need their artifact, and merges the WP when they pass. It never dispatches `native.yml` for a branch it just pushed; a dispatch (`gh workflow run native.yml --ref native`) is only for rebuilding an expired artifact. `windows.yml` and `macos.yml` live on `main` and can be dispatched with any branch as `ref`.
- **The reference runner** (Azahar + `port/`): from N0-03, CI builds it and publishes the artifact `mh4u-runner-linux`, fetched with `gh run download` (the commands are in `docs/native/CI.md` and the skill). A local build follows `port/README.md`, takes tens of minutes on the cloud machine, and is done once per session, in the background, with `-DMH4U_PORT_DIR` pointing at the worktree's `port/`.
- **What only the maintainer's machines can run:** performance numbers (reference PC), the AMD driver's Vulkan features (RX 6700 XT), Metal (M2), real-3DS captures. WPs with those needs leave an exact procedure and a script, and the orchestrator asks the maintainer to run it.

## 8. Rules for workers

1. One WP, inside its allowlist. If another file needs a change, say so in the report.
2. Never add game data or anything derived from it: no traces, captures, dumps, screenshots, disassembly listings, or the game's text and file paths. Not to the repository, a test fixture, a PR or a report: quote only service names, headers, result codes, sizes, CRCs and counts. Test fixtures are synthetic.
3. Never weaken a check: no deleted asserts, special-cased inputs, raised tolerances or skipped tests. If a check can't pass, report `blocked`.
4. No design decisions. If the spec doesn't say and DECISIONS.md doesn't settle it, stop and report `needs-decision` with the options you see.
5. Two fix rounds after the first failure, then report `blocked` with what you learned.
6. Copying from Azahar is allowed (D6). Keep a header in the file: `Derived from Azahar <path> at <commit> (GPL-2.0-or-later or GPL-3.0-or-later, as the file says)`, followed by the source file's own `Copyright …` line(s) (the GPL asks that copied code keeps its notices), and list it in your report so `native/THIRD_PARTY.md` gets updated. Don't copy Azahar's architecture (kernel, memory system, timing).
7. Follow the neighbours. Python: stdlib, type hints, a test per tool. C++: C++20, the rules at the top of [`wp/N1.md`](wp/N1.md).
8. Logs and outputs go to `build/` and never into the repository.
9. Don't push, open PRs, merge, or edit STATUS.md, DECISIONS.md or the other shared files; propose changes to them (entries for `config/native/ipc_time_dependent.txt` included) in the report.
10. Keep your report short and exact: commands run, with their real output summarised.

## 9. Report formats

**Worker:**
```
wp: <ID>
status: done | partial | blocked | needs-decision
branch: wp/<ID>-<slug> @ <short sha>
files: <created/changed, one line each>
checks:
  - <command> → <result>
  - (with the dump) <command> → pending (dump) | <result>
  - (CI) <what> → pending (ci)
derived-from-azahar: <files and source paths> | -
proposed-shared-changes: <STATUS/DECISIONS/contracts/config/THIRD_PARTY lines> | -
notes: <blocker, open question, surprise, what is left; ≤ 5 lines>
```
`partial`: the budget or the session ran out; what works is committed as `<ID>: WIP …`, and the notes say what is left.

**Reviewer:**
```
wp: <ID>
verdict: approve | changes | reject
checks-rerun:
  - <command> → <result>
  - (with the dump) <command> → pending (dump) | <result>
  - (CI) <what> → pending (ci) | <result>
problems:
  1. <file:line — what, why, the fix you expect>
scope: ok | <files outside the allowlist>
game-data: none found | <what kind and where, without quoting it>
```

**Orchestrator (end of session, also the batch PR body):** a table of `WP | title | checks | review | status`; the CI results; what needs the maintainer; what is blocked; what is next in STATUS.

## 10. Cost and time

| WP size | Worker token budget | If exceeded |
|---|---|---|
| S | ~150 k | Stop, report what is done and what is left |
| M | ~400 k | Same |
| L | ~1 M | Same; the orchestrator splits the rest |

- As many workers at a time as there are ready WPs that share no files; the machine's limit is one build-heavy WP at a time (a build of Azahar, of `native/` or of the game's code library). Doc, Python and analysis WPs can run alongside it.
- Within a session, work stacks: what a merged WP unblocks starts on top of the batch, so a chain of dependent WPs can advance several links before the maintainer merges. Keep a batch to about ten WPs, so its PR stays readable.
- Review once per WP; reviews of different WPs run in parallel.
- The planner is rare (a few runs per phase).
- Overnight batches can run as scheduled tasks if the maintainer sets them up. They only take WPs that need no game files, unless the PC stays on for staging.

## 11. When things go wrong

| Situation | What to do |
|---|---|
| A check fails after two fix rounds | `blocked` with the evidence; the orchestrator splits off or adds a WP |
| The spec is ambiguous or conflicts with the code | `needs-decision`; don't guess |
| A WP needs a file outside its allowlist | Do the minimum in the allowlist, report the rest; the orchestrator decides |
| `main` changed files a WP touches | The orchestrator syncs `main` into `native` first (section 3) |
| An integration check fails after a merge into the batch | Undo that merge (`git reset --keep HEAD^1`; the batch isn't pushed yet), send it back to the worker |
| CI fails on the batch PR | The orchestrator finds the WP responsible and sends it back or reverts its merge in the batch |
| A reviewer finds game data in a WP's commits | The branch is deleted without being pushed, and the WP restarts from scratch; if it had been pushed, it is deleted on GitHub too and the maintainer is told at once |
| A WP's CI fails | It goes back to its worker as a fix round; WPs with *(CI)* items merge into the batch only once their CI passes |
| Staging fails (PC off) | WPs without `dump`/`save` needs only (`ready --no-dump`); `dump-check` items stay pending |
| The `native-*` agent types aren't available | `general-purpose` with `model: sonnet` and the effort of section 2, told which definition and skill to read (section 2) |
| A worker wrote into the main checkout | Move the files into its worktree or delete them; remind it to start each command with `cd <worktree> &&` |
| A command is cut off at 2 or 10 minutes | Run it again in the background and poll its log |
| The session ends with WPs unfinished | Commit their allowlisted files as WIP, check and push their `wp/` branches, set them `todo` or `blocked` with the branch in the note |
| An open batch PR can no longer be merged | Merge `origin/native` into its branch in a worktree; in STATUS.md keep the batch's rows for its WPs and `native`'s for the rest; `sync`, `check`, pytest, push (the skill's §2) |
| A permission check blocks an action (merge, push to `main`) | Expected: report it to the maintainer; don't work around it |
| The game behaves differently natively and no oracle explains it | Record it in `docs/research/` with a reproduction; a new WP to build the missing oracle |

## 12. Maintainer-only tasks

- Merge batch PRs (GitHub: **Create a merge commit**, so the review merges inside the batch stay visible).
- Settle `Open` decisions (D4 after the spike, D5 after the design).
- Run the procedures the WPs leave for:
  - the reference PC: baseline, spike, budgets;
  - the M2: Metal;
  - the real 3DS: hardware captures.
- Keep the PC on, with the desktop app open, when a session needs to stage game files.
- Contact the HD pack's author about mirrors before heavy traffic.

The open ones are listed in [`STATUS.md`](STATUS.md#maintainer-tasks).

## 13. Keeping the documents current

- **STATUS.md** is the truth about the work: one row per WP, updated in every batch with `wp_status.py`.
- **DECISIONS.md** records every decision with its evidence.
- **The project's `ESTADO.md`** (outside the repo) gets one line per batch (the PR, its WPs, what is pending), so that a session without the repo knows where things are.
- **Lessons** that would have saved a worker time go into the *Traps* of the affected WPs, edited by the orchestrator, and into this file if they are general.
