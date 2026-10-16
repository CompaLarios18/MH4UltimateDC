# Native port status

The live state of every work package (WP). The specs are in [`wp/`](wp/); how the work runs is [`AGENTS.md`](AGENTS.md).

**Updating this file** (the orchestrator, in every batch):
- change a status with `python3 tools/native/wp_status.py set <ID> <status> [--note "…" | --add-note "…"]`;
- after editing any spec in `wp/`, run `python3 tools/native/wp_status.py sync`;
- `python3 tools/native/wp_status.py check` must pass before committing (pytest runs it too);
- `python3 tools/native/wp_status.py ready` lists what can start, the WPs that unblock the most first (`--no-dump` when the game files can't be staged; `--assume-done <IDs>` for WPs merged into this session's batch);
- `python3 tools/native/wp_status.py overlaps <IDs>` must print nothing for the WPs in flight at once.

**Statuses:** `todo` → `ready` (shown when every dependency is `done`) → `active` (a worker has it, this session) → `review` (merged into the batch, then in its open PR) → `done` (merged into `native`). Also `blocked` and `needs-decision`, with the reason in the note. No row stays `active` after a session.

**Notes** carry the PR (`PR #<n>`) and what is still owed, until it's settled: `dump-check pending`, `ci pending (run <id>)`, `waiting for D<n>`, `WIP on origin/wp/<ID>-<slug> (base <sha>): …`, `restart: …`. `python3 tools/native/wp_status.py pending` lists the owed ones.

Tables between the `wp-status` markers are generated: edit the specs or use `set`, not the tables.

## N0 · Foundations, oracles, baseline

<!-- wp-status:begin N0 -->
| ID | Title | Size | Agent | Depends on | Needs | Status | Note |
|---|---|---|---|---|---|---|---|
| N0-01 | Bootstrap the native branch | S | orchestrator | - | none | `done` | PR #3 |
| N0-02 | Cloud development environment | S | native-worker | - | none | `review` | batch 20261016 |
| N0-03 | Native CI | M | native-worker | - | ci | `review` | batch 20261016; ci pending (run 38880121118: green, artifact published; the download from a cloud session is blocked by the network policy, host productionresultssa2.blob.core.windows.net) |
| N0-04 | Old 3DS runs, SVC results and normalised SVC traces | M | native-worker-high | N0-03 | ci, dump-check | `review` | batch 20261016; dump-check pending (G1: jit vs recomp traces compare equal); ci green (run 38885552387) |
| N0-07 | Game-state trace and unmapped-access counter | S | native-worker | N0-04 | ci, dump-check | `review` | batch 20261016; dump-check pending (G1: the scene byte changes in the state trace; the unmapped report exists); ci green (run 38892421769) |
| N0-08 | Frame-keyed scheduling in the runner | S | native-worker | N0-07 | ci, dump-check | `todo` |  |
| N0-21 | IPC trace and comparator | M | native-worker-high | N0-08 | ci, dump-check | `todo` |  |
| N0-06 | DSP and HID captures | M | native-worker | N0-21 | ci, dump-check | `todo` |  |
| N0-22 | GPU capture format and `gcap.py` | S | native-worker | - | none | `review` | batch 20261016 |
| N0-05 | GPU capture recorder | M | native-worker-high | N0-06, N0-22 | ci, dump-check | `todo` |  |
| N0-09 | Golden sessions G1, G2, G6 | M | native-worker | N0-04, N0-05, N0-06, N0-07, N0-08, N0-21 | dump | `todo` |  |
| N0-10 | Golden sessions G3, G4, G7 | M | native-worker | N0-09 | dump, save | `todo` |  |
| N0-11 | Golden session G5 (two players) | M | native-worker | N0-10 | dump, save | `todo` |  |
| N0-12 | Kernel census | S | native-worker | N0-04, N0-21, N0-10 | dump, save | `todo` |  |
| N0-13 | PICA census | M | native-worker | N0-05, N0-10 | dump, save | `todo` |  |
| N0-14 | Flat-memory coherence check | M | native-worker-high | N0-15, N0-10 | ci, dump, save | `todo` |  |
| N0-15 | Performance baseline | M | native-worker | N0-18 | ci, pc, mac | `todo` |  |
| N0-16 | Azahar dependency inventory | S | native-worker | - | none | `review` | batch 20261016 |
| N0-17 | `RC_NATIVE` mode in the recompiler runtime | M | native-worker-high | N0-03 | ci | `review` | batch 20261016; ci green (run 38885556028) |
| N0-18 | Hooks on a platform interface | M | native-worker-high | N0-16, N0-17, N0-05 | ci, dump-check | `todo` |  |
| N0-19 | Game-data guard for agents | S | native-worker | - | ci | `review` | batch 20261016; ci green (runs 38884722260 and, for the follow-up, 38903953519) |
| N0-23 | Service command census from the code | S | native-worker | - | dump | `ready` |  |
| N0-20 | `IRuntime` seam in the app: lifecycle, options, paths | M | native-worker-high | N0-16, N0-15 | ci, pc | `todo` |  |
| N0-24 | `IRuntime` seam in the app: input, multiplayer, keyboard | M | native-worker-high | N0-20 | ci, pc | `todo` |  |
<!-- wp-status:end N0 -->

## N1 · Headless runtime

<!-- wp-status:begin N1 -->
| ID | Title | Size | Agent | Depends on | Needs | Status | Note |
|---|---|---|---|---|---|---|---|
| N1-01 | `native/` skeleton, build and CI | M | native-worker-high | N0-02, N0-03 | ci | `review` | batch 20261016; ci green (run 38885559352) |
| N1-02 | Guest memory | M | native-worker-high | N1-01 | none | `review` | batch 20261016; ci green (run 38892424838); Windows/macOS paths untested (N1-22) |
| N1-03 | Loader and system pages | M | native-worker | N1-02 | dump-check | `todo` |  |
| N1-04 | Code host and guest fibers | M | native-worker-high | N1-02, N1-03, N0-17 | none | `todo` |  |
| N1-05 | Kernel objects and handle table | M | native-worker-high | N1-02 | none | `todo` |  |
| N1-06 | Scheduler, game clock and timers | L | native-worker-high | N1-04, N1-05 | none | `todo` |  |
| N1-27 | Threads: creation, TLS and the core-1 time limit | M | native-worker-high | N1-06 | none | `todo` |  |
| N1-07 | The SVC table | M | native-worker-high | N1-02, N1-27 | none | `todo` |  |
| N1-08 | Kernel conformance programs | M | native-worker | N0-04 | none | `todo` |  |
| N1-09 | The runtime, the native runner and the first boot | L | native-worker-high | N1-03, N1-07, N1-08, N0-08 | ci | `todo` |  |
| N1-10 | IPC core, ports, sessions and srv | M | native-worker-high | N1-07, N1-09, N0-21 | none | `todo` |  |
| N1-11 | IPC replay harness | M | native-worker | N1-10, N0-21 | dump-check | `todo` |  |
| N1-12 | fs: archives, RomFS and system archives | M | native-worker | N1-10, N1-11, N0-23 | dump-check | `todo` |  |
| N1-13 | fs: save data and extdata | M | native-worker | N1-12 | dump-check, save | `todo` |  |
| N1-15 | cfg, ptm, ndm, ac and the console's identity | M | native-worker | N1-10, N1-11, N0-23 | dump-check | `todo` |  |
| N1-14 | APT and the software keyboard | M | native-worker-high | N1-10, N1-11, N1-12, N1-15, N0-23 | dump-check | `todo` |  |
| N1-16 | hid and ir:USER from scripts | M | native-worker | N1-10, N0-08, N0-06, N0-23 | none | `todo` |  |
| N1-17 | gsp::Gpu with a null GPU | M | native-worker-high | N1-10, N1-06, N1-11, N0-23 | dump-check | `todo` |  |
| N1-18 | dsp::DSP without sound | M | native-worker-high | N1-10, N1-06, N1-11, N0-06, N0-23 | dump-check | `todo` |  |
| N1-19 | The remaining services | M | native-worker | N1-10, N1-11, N0-23 | dump-check | `todo` |  |
| N1-29 | nwm::UDS without a room | M | native-worker | N1-10, N1-11, N0-23 | dump-check | `todo` |  |
| N1-20 | Hooks on the native platform | S | native-worker | N0-18, N1-04, N1-17 | none | `todo` |  |
| N1-21 | First game boot: G1 and G2 headless | M | native-worker-high | N1-09, N1-12, N1-13, N1-14, N1-15, N1-16, N1-17, N1-18, N1-19, N1-20, N1-22, N1-29, N0-09 | dump | `todo` |  |
| N1-26 | G3 headless: from the save to the Arena hunt | L | native-worker-high | N1-21, N1-13, N0-10 | dump, save | `todo` |  |
| N1-22 | Windows and macOS builds | M | native-worker | N1-09 | ci | `todo` |  |
| N1-23 | CPU budget measurement | S | native-worker | N1-26, N1-22, N0-15 | ci, pc | `todo` |  |
| N1-24 | Fallback interpreter: spike | S | native-worker-high | N1-04, N1-09, N1-22 | none | `todo` |  |
| N1-28 | Fallback interpreter | L | native-worker-high | N1-24 | none | `todo` |  |
| N1-25 | Deterministic mode and the shuffling scheduler | M | native-worker-high | N1-06, N1-26, N0-17 | dump-check | `todo` |  |
<!-- wp-status:end N1 -->

## N2a · Native renderer from captures

<!-- wp-status:begin N2a -->
| ID | Title | Size | Agent | Depends on | Needs | Status | Note |
|---|---|---|---|---|---|---|---|
| N2-01 | RHI spike | M | native-worker-high | N0-13, N1-01 | ci, pc, mac | `todo` |  |
| N2-02 | Capture player and comparison harness | M | native-worker-high | N0-22, N1-01 | dump-check | `todo` |  |
| N2-03 | GX engine | M | native-worker | N2-02 | dump-check, save | `todo` |  |
| N2-04 | PICA command processor | M | native-worker-high | N2-02 | dump-check, save | `todo` |  |
| N2-05 | Vertex loading | M | native-worker | N2-04, N2-01 | dump-check, save | `todo` |  |
| N2-06 | Vertex shader translation | L | native-worker-high | N2-04, N2-21 | dump-check, save | `todo` |  |
| N2-07 | Fragment pipeline design | M | native-worker-high | N0-13, N2-01 | none | `todo` |  |
| N2-21 | Backend foundation | M | native-worker-high | N2-01, N2-02 | none | `todo` |  |
| N2-22 | Renderer thread and synchronisation | M | native-worker-high | N2-21, N2-03, N2-04 | dump-check, save | `todo` |  |
| N2-13 | Framebuffer operations | M | native-worker-high | N2-07, N2-21 | none | `todo` |  |
| N2-14 | Textures: decoding, hashes, cache | M | native-worker | N2-02 | dump-check, save, pc | `todo` |  |
| N2-08 | Fragment generator, TEV and alpha test | L | native-worker-high | N2-03, N2-05, N2-06, N2-13, N2-14, N2-21 | none | `todo` |  |
| N2-23 | Texture sampling and the first captured frames | M | native-worker-high | N2-08 | dump-check | `todo` |  |
| N2-09 | Fragment lighting | L | native-worker-high | N2-08, N2-23 | none | `todo` |  |
| N2-10 | Fog and gas | M | native-worker-high | N2-08 | none | `todo` |  |
| N2-11 | Procedural textures | M | native-worker | N2-08, N2-23 | none | `todo` |  |
| N2-12 | Shadows | M | native-worker-high | N2-08, N2-13, N2-23 | none | `todo` |  |
| N2-20 | Fragment integration on the captures | M | native-worker-high | N2-09, N2-10, N2-11, N2-12, N2-23 | dump, save | `todo` |  |
| N2-15 | Surface cache and resolution scaling | L | native-worker-high | N2-20, N2-22, N0-14 | dump-check, save | `todo` |  |
| N2-16 | Pipeline cache, ubershader and background compilation | L | native-worker-high | N2-07, N2-20, N2-15 | dump-check, save, pc | `todo` |  |
| N2-17 | HD pack pre-processor | M | native-worker | N2-14 | pc | `todo` |  |
| N2-18 | HD texture streaming and the VRAM budget | M | native-worker-high | N2-17, N2-15 | dump-check, save, pc | `todo` |  |
| N2-19 | GPU budget measurement | S | native-worker | N2-15, N2-16, N2-18, N1-22, N1-23 | ci, pc, mac | `todo` |  |
<!-- wp-status:end N2a -->

## Later phases

N2b to N7 and Track D are outlined in [`wp/LATER.md`](wp/LATER.md); the planner details each one before it starts.

## Maintainer tasks

| ID | Task | Status |
|---|---|---|
| M-1 | Merge the bootstrap PR (#3) into `native` | done (2026-10-16) |
| M-2 | Confirm the new monitor's resolution (expected 3440×1440, 21:9) so D15 and the budgets drop "to confirm" | open (the monitor arrives Monday 2026-10-19) |
| M-3 | Contact the HD pack's author about mirrors before heavy traffic (Dropbox pauses shared links for 24 h past the daily limit) | open |
| M-4 | Keep the PC on with the desktop app open in sessions that stage game files | when asked |
| M-5 | Allow the artifact host (`*.blob.core.windows.net`, seen as `productionresultssa2.blob.core.windows.net`) under Allowed domains in the cloud environment's network settings, so sessions can download the reference runner (`gh run download` fails with `Forbidden` today); blocks N0-03's last check and N1-08 | open |

## Open decisions

D4 (the RHI; spike N2-01) and D5 (the fragment pipeline; N2-07). The full list is [`DECISIONS.md`](DECISIONS.md).
