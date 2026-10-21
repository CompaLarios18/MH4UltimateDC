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
| N0-02 | Cloud development environment | S | native-worker | - | none | `done` | PR #5 |
| N0-03 | Native CI | M | native-worker | - | ci | `done` | PR #5; ci green (run 38880121118; its artifact was downloaded and run.sh --help checked in a cloud session on 2026-10-16) |
| N0-04 | Old 3DS runs, SVC results and normalised SVC traces | M | native-worker-high | N0-03 | ci, dump-check | `done` | PR #5; ci green (run 38885552387); dump-check pending (G1: jit vs recomp traces compare equal; ran 20261017 on G1 to frame 800: they do not, 10 of 18 threads differ in call counts and the main thread diverges at call 393, while state_diff agrees; D30 and D31 (2026-10-20) settle what equal means: the item is rewritten (two recomp traces from the N0-29 runner identical, plus the cross-model check of N0-28) and reruns after N0-29 and N0-28) |
| N0-07 | Game-state trace and unmapped-access counter | S | native-worker | N0-04 | ci, dump-check | `done` | PR #5; ci green (run 38892421769); dump-check passed 20261017 and 20261021 (the unmapped report exists, header only; the item rewritten by D32 passes: with the scene observable of N0-27, scene_probe.py validate holds and the scene takes 6 distinct readable values in G2) |
| N0-08 | Frame-keyed scheduling in the runner | S | native-worker | N0-07 | ci, dump-check | `done` | PR #5; ci green (run 38923994278); dump-check pending (START at f600 then A at f900: ran 20261017, the jit and recomp state traces agreed only vacuously because the old scene observable never changed; D32 rewrote the item (G2 with the real scene observable and the runner of N0-29): the observable now exists (N0-27); it reruns after N0-29) |
| N0-21 | IPC trace and comparator | M | native-worker-high | N0-08 | ci, dump-check | `done` | PR #5; ci green (run 38954204863: the runner compiles the IPC section); dump-check pending (G1: jit vs recomp IPC traces compare equal; ran 20261017 on G1 to frame 800: the first request line shows bare session names, but jit and recomp differ at the main thread request 256 and thread 9 request 827; D30 and D31 settle what equal means: the item is rewritten (two recomp IPC traces from the N0-29 runner identical, plus the cross-model check of N0-28) and reruns after N0-29 and N0-28) |
| N0-06 | DSP and HID captures | M | native-worker | N0-21 | ci, dump-check | `done` | PR #6; ci: native and runner jobs green (run 39003477255: the runner job compiles the DSP and HID section); its tools job failed only for the THIRD_PARTY rows, added in the merge; dump-check pending (type-2 (IR) records appear: ran 20261017, G1 never opens ir:USER so there are none; the rest passed: HID records equal the frames run, and non-silent PCM with type-3 records exists in G1 game frames 300 to 1350 (1198 non-silent frames, 236 type-3 records) but not at the title, which is silent) |
| N0-22 | GPU capture format and `gcap.py` | S | native-worker | - | none | `done` | PR #5 |
| N0-05 | GPU capture recorder | M | native-worker-high | N0-06, N0-22 | ci, dump-check | `done` | PR #6; ci green (run 39006180963: the runner job compiles and links the recorder and its patch); dump-check passed 20261017 (a 3-frame capture of G1 at frame 1200, before the title at about frame 1360, passes gcap.py check: 6 SUBMIT_CMDLIST, 6 PICA_REG, 3 FRAME, 11 FLUSH, 6 PPMs, 144 MB) |
| N0-25 | Comparator rules: polling runs, cross-model calls, nothing compared | M | native-worker-high | N0-04, N0-07 | dump-check | `done` | PR #17 merged; PR #17; batch 20261021-n0-oracles; the (with the dump) items ran on the saved 800-frame traces of G1: the pair n018/A1-A2 passes --collapse-polling; the other two recomp pairs differ only in 5 SleepThread durations of 91546 collapsed main-thread records (reported, no exception added); jit against recomp with --cross-model passes (111 calls compared); state_diff --min-changes 1 exits 3 on the five pairs measured (no real scene change with the old observable); see docs/native/comparators.md |
| N0-26 | Reference runner: fixed per-run values, timing trace, frame-keyed shots | M | native-worker-high | N0-15 | ci, dump-check | `done` | PR #17 merged; PR #17; batch 20261021-n0-oracles; ci green (Native CI run 39539337994 and windows.yml run 39539339042 on 6fa80b1; the later commits change only port/README.md); the (with the dump) items measured 20261021 on the maintainer PC: two G1 runs to frame 800 give identical shared pages, state, SVC and timing traces (the previous runner differs in all of them); the IPC trace differs in 3 reply words (APT:A, cfg:u), left to N0-29; see port/README.md, section Determinism |
| N0-29 | Reference runner repeats itself exactly | L | native-worker-high | N0-26 | ci, dump, pc | `ready` |  |
| N0-27 | The game's own scene observable | M | native-worker-high | N0-07, N0-08 | dump | `done` | PR #17 merged; PR #17; batch 20261021-n0-oracles; the scene is the class (vtable) of the topmost area of the sArea table at 0x01057298: scene_probe.py validate passes on G2 (recomp x2 at tolerance 0, and jit), G1 and G6 with 6 distinct values in G2; jit changes come 0 to 3 frames after recomp (two are 3 apart, over the 2-frame threshold of D33: N0-28 will meet it); the orchestrator edited config/native/observable.txt (scene, applet_state) and symbols.txt (D32); see docs/research/scene-observable.md |
| N0-09 | Golden sessions G1, G2, G6 | M | native-worker | N0-04, N0-05, N0-06, N0-07, N0-08, N0-21, N0-25, N0-27, N0-29 | dump | `todo` | WIP on origin/wp/N0-09-golden-sessions (base e6cd840): G1, G2 and G6 were recorded on the maintainer PC with the runner of CI run 39075253705; decisions D30 to D36 (2026-10-20) settle what blocked it; N0-25 (comparator rules), N0-26 (fixed per-run values) and N0-27 (the real scene observable) are done in batch 20261021-n0-oracles; it now waits for N0-29 (a runner that repeats itself at 1700 and 9000 frames) and then re-records once; see docs/native/golden.md on that branch |
| N0-28 | Cross-model calibration: jit against recomp | M | native-worker | N0-09, N0-25 | dump | `todo` |  |
| N0-10 | Golden sessions G3, G4, G7 | M | native-worker | N0-09 | dump, save | `todo` |  |
| N0-30 | Cross-model calibration on the quest sessions G3, G4, G7 | S | native-worker | N0-10, N0-28 | dump, save | `todo` |  |
| N0-11 | Golden session G5 (two players) | M | native-worker | N0-10 | dump, save | `todo` |  |
| N0-12 | Kernel census | S | native-worker | N0-04, N0-21, N0-10 | dump, save | `todo` |  |
| N0-13 | PICA census | M | native-worker | N0-05, N0-10 | dump, save | `todo` |  |
| N0-14 | Flat-memory coherence check | M | native-worker-high | N0-15, N0-10 | ci, dump, save | `todo` |  |
| N0-15 | Performance baseline | M | native-worker | N0-18 | ci, pc, mac | `done` | PR #6; ci green (Native CI run 39011509987; windows.yml 39011525115 and macos.yml 39011526794 on its branch); the --perf-log item ran on the CI runner with io_test (149 frames, report ok); maintainer: numbers on the reference PC and the M2 following docs/native/perf-procedure.md (the orchestrator writes docs/research/perf-baseline.md) |
| N0-16 | Azahar dependency inventory | S | native-worker | - | none | `done` | PR #5 |
| N0-17 | `RC_NATIVE` mode in the recompiler runtime | M | native-worker-high | N0-03 | ci | `done` | PR #5; ci green (run 38885556028) |
| N0-18 | Hooks on a platform interface | M | native-worker-high | N0-16, N0-17, N0-05 | ci, dump-check | `done` | PR #6; ci green (run 39008761854, windows.yml 39008783719 and macos.yml 39008785589 on its branch); dump-check passed 20261017 (G1 to frame 800 with --set 0:fps=60 --set f600:hud_edges=1 on the runner of batch/20261016-n0, from before this change, and on the current one: state_diff agrees, every compared value is equal in 801 of 801 records and the ticks in 798, and the SVC traces differ only in the main thread tick polling, as two runs of one runner do) |
| N0-19 | Game-data guard for agents | S | native-worker | - | ci | `done` | PR #5; ci green (native-guard runs 38884722260, 38903953519 and 38944855740); guard hardened in four rounds, the last two from the batch audits |
| N0-23 | Service command census from the code | S | native-worker | - | dump | `done` | PR #11; batch 20261017-n0-dump (run on the maintainer PC, Windows) |
| N0-20 | `IRuntime` seam in the app: lifecycle, options, paths | M | native-worker-high | N0-16, N0-15 | ci, pc | `done` | PR #7; ci green (linux.yml 39040404737, windows.yml 39040406726, macos.yml 39040409014 on its branch; Native CI native and runner jobs green, tools job failed only for the THIRD_PARTY rows added in the merge); maintainer smoke test pending (launcher -> play -> menu -> change an option -> quit) |
| N0-24 | `IRuntime` seam in the app: input, multiplayer, keyboard | M | native-worker-high | N0-20 | ci, pc | `done` | PR #7; ci green (linux.yml 39048239100, windows.yml 39048241157, macos.yml 39048243336 dispatched on the batch branch with the window.cpp change; the app code is unchanged since); maintainer smoke test pending (launcher -> play -> host a room and join it from a second instance -> name a character with the keyboard -> quit) |
<!-- wp-status:end N0 -->

## N1 · Headless runtime

<!-- wp-status:begin N1 -->
| ID | Title | Size | Agent | Depends on | Needs | Status | Note |
|---|---|---|---|---|---|---|---|
| N1-01 | `native/` skeleton, build and CI | M | native-worker-high | N0-02, N0-03 | ci | `done` | PR #5; ci green (runs 38885559352 and, for the follow-up, 38956325317) |
| N1-02 | Guest memory | M | native-worker-high | N1-01 | none | `done` | PR #5; ci green (run 38892424838); Windows/macOS paths untested (N1-22) |
| N1-03 | Loader and system pages | M | native-worker | N1-02 | dump-check | `done` | PR #5; ci green (run 38926727430); dump-check passed 20261017 (prepare_game.py writes the folder; mh4u_native_runner --print-process agrees with tools/exheader.py on program id, stack, priority, ideal processor, memory mode, kernel version, handle table, SVC mask, address mappings and segments; the linear base 0x30000000 is the result of the first linear ControlMemory in the jit and recomp SVC traces) |
| N1-04 | Code host and guest fibers | M | native-worker-high | N1-02, N1-03, N0-17 | none | `done` | PR #5; ci green (run 38958762393: native builds the cpu module and the test program); no dump items |
| N1-05 | Kernel objects and handle table | M | native-worker-high | N1-02 | none | `done` | PR #5; ci green (run 38926731888) |
| N1-06 | Scheduler, game clock and timers | L | native-worker-high | N1-04, N1-05 | none | `done` | PR #6; ci green (Native CI run 39018443575 on PR #6's head) |
| N1-27 | Threads: creation, TLS and the core-1 time limit | M | native-worker-high | N1-06 | none | `done` | PR #6; ci green (Native CI run 39018443575 on PR #6's head) |
| N1-07 | The SVC table | M | native-worker-high | N1-02, N1-27 | none | `done` | PR #6; ci green (Native CI run 39018443575 on PR #6's head) |
| N1-08 | Kernel conformance programs | M | native-worker | N0-04 | none | `done` | PR #5; ci green (runs 38923999146 and, for the follow-up, 38944850589); the 10 programs match the reference runner twice in a row; tests/test_port.py and tests/test_conformance.py pass with the tip's runner (148 passed, 11 skipped for the game and the native runner, at the third audit) |
| N1-09 | The runtime, the native runner and the first boot | L | native-worker-high | N1-03, N1-07, N1-08, N0-08 | ci | `done` | PR #6; ci green on its branch (Native CI run 39012897629: native job ctest 22/22 and the conformance step 51 passed; the tools job failed only for the THIRD_PARTY row added in the merge); no dump items |
| N1-10 | IPC core, ports, sessions and srv | M | native-worker-high | N1-07, N1-09, N0-21 | none | `done` | PR #7; ci green (PR #7: Native CI, CI and native-guard); conf_srv stays in NATIVE_NOT_YET until N1-15 (cfg:u, ptm:u) |
| N1-11 | IPC replay harness | M | native-worker | N1-10, N0-21 | dump-check | `review` | PR #7 (ci green); N1-11b in PR #19: the replay moves the game clock to each record's recorded tick before it sends the request, forward only (--no-time keeps the old behaviour), so G2's APT requests should now match; dump-check pending (replaying G1's srv: requests, build/golden/G1/ipc.txt and svc.txt, finds no differences; G2's APT replay with and without --no-time). A reply that waits on a timer an earlier request armed (gsp 0x19/0x1A SaveVramSysArea and RestoreVramSysArea wait on the next VBlank) is reported as a difference after --reply-timeout and never delivered by guessing: lower it (--reply-timeout 500) or use --no-time for a trace that has them; a harness hint that lets such a reply be delivered deterministically is a follow-up |
| N1-12 | fs: archives, RomFS and system archives | M | native-worker | N1-10, N1-11, N0-23 | dump-check | `done` | PR #16; dump-check pending (mh4n_ipc_replay build/golden/G1/ipc.txt --service fs:USER --game build/native-game --io-latency none, then G2); ci pending for the Windows branch of positioned_file.cpp; licences of the open-source archives read (PROVENANCE.md) |
| N1-13 | fs: save data and extdata | M | native-worker | N1-12 | dump-check, save | `review` | PR #16 and PR #19; dump-check pending (G3: replay fs:USER on a copy of build/golden/G3/user-before, then diff -r of the save data and extdata folders against user-after; the 3 padding bytes of the 16-byte metadata files may differ). N1-13d and N1-13e in PR #19 must go together (D45): UTF-8 paths in the fs module and the runner, in the loader, runtime, cfg, cecd and platform, a clean extdata file never syncs (D42), stale .deleted files of a crash are removed when an archive opens; ci pending: native-windows is the only check of every Windows branch (non-ASCII user and game folder tests, the UTF-16 command line, the 8.3 short name for mh4u.ini). Writes still run inline on the scheduler thread (as Azahar's disk files); a save during play is a case for N3's 60/120 fps audit |
| N1-15 | cfg, ptm, ndm, ac and the console's identity | M | native-worker | N1-10, N1-11, N0-23 | dump-check | `review` | PR #16 and PR #19 (N1-15b: a random console ID is made once when --user DIR exists and holds no config, D37; N1-15c: prepare_game.py writes icon.bin so --region auto finds the game's region, D38); dump-check pending (G1 and G2 replay of cfg:u, ptm:u, ndm:u, ac:u; re-run prepare_game.py on the dump and check that icon.bin is 0x36C0 bytes or more and that its lockout matches the game's region; a game folder without icon.bin still needs --region) |
| N1-14 | APT and the software keyboard | M | native-worker-high | N1-10, N1-11, N1-12, N1-15, N0-23 | dump-check | `review` | PR #16 (with N1-14b, the APT:SharedFont block; conf_info passes natively) and PR #19 (N1-14c: the Error, Mint and Mii selector applets close without a HOME menu, Preload commands, D43); dump-check pending (mh4n_ipc_replay <trace> --service APT:U --service APT:A --service APT:S; with N1-11b the replay advances game time, so G2's keyboard tick should now fire); StartSystemApplet and every other applet id answer 0xD8C0CFFA as Azahar does when it cannot make them, and CloseApplication/DoApplicationJump with no HOME menu end the run with status 0 (D43) |
| N1-16 | hid and ir:USER from scripts | M | native-worker | N1-10, N0-08, N0-06, N0-23 | none | `done` | PR #16; ir:USER connects no Circle Pad Pro (differs from Azahar's emulated device 1, see the PR) |
| N1-17 | gsp::Gpu with a null GPU | M | native-worker-high | N1-10, N1-06, N1-11, N0-23 | dump-check | `review` | PR #16 and PR #19 (N1-17b, D41: interrupts reach the sink at once, the null GPU hands Azahar's delays over and GetPerfLog records them, a DMA between any two mapped ranges copies as Azahar's CopyBlock: zero fill for an unmapped source page); dump-check pending (build/native/bin/mh4n_ipc_replay build/golden/G1/ipc.txt --svc-trace build/golden/G1/svc.txt --service gsp::Gpu --continue); open: GxEngine::Dma in native/src/gpu/gx/engine.cpp (the capture player) keeps the old copies-nothing rule, and the N2 renderer's DMA path must call FlushRegion on the source and InvalidateRegion on the destination |
| N1-18 | dsp::DSP without sound | M | native-worker-high | N1-10, N1-06, N1-11, N0-06, N0-23 | dump-check | `done` | PR #16; dump-check pending (mh4n_ipc_replay build/golden/G1/ipc.txt --svc-trace build/golden/G1/svc.txt --service dsp::DSP --continue; then --capture-dsp 300:300:FILE against the reference with capfmt.py diff-dsp, needed by N1-21); Reply::Push of u8/u16 zero-extends a word where Azahar leaves the bytes (framework, N1-10): the replay will show if it matters |
| N1-19 | The remaining services | M | native-worker | N1-10, N1-11, N0-23 | dump-check | `review` | PR #19: 10 services (act:u, am:app, boss:U/P, cam:u, cecd:u, frd:u, http:C, nim:aoc, soc:U, y2r:u) after six fix rounds and independent reviews, the last found nothing open; dump-check pending (G1 to G3: build/native/bin/mh4n_ipc_replay build/golden/G1/ipc.txt --svc-trace build/golden/G1/svc.txt --game build/native-game --continue --service am:app --service nim:aoc --service act:u --service frd:u --service cecd:u --service boss:U --service boss:P --service soc:U --service http:C --service cam:u --service y2r:u, then G2 and G3; no time-dependent command was found among the ones the game sends, so config/native/ipc_time_dependent.txt stays unchanged until the replay shows a difference); cecd ProgramId reads exheader.bin from the game folder because Process carries no program id (a shared-change proposal for later) |
| N1-29 | nwm::UDS without a room | M | native-worker | N1-10, N1-11, N0-23 | dump-check | `review` | PR #16 and PR #19 (N1-29b, D40: the nwm start-up puts cfg's console MAC in the shared page at offset 0x60 before the game runs); dump-check pending (mh4n_ipc_replay build/golden/G1/ipc.txt --service nwm::UDS, then G2 and G3; a golden that compares the shared page needs a --user folder with a fixed mac.txt, since without --user the MAC is random per run) |
| N1-20 | Hooks on the native platform | S | native-worker | N0-18, N1-04, N1-17 | none | `review` | PR #16 and PR #19 (N1-20b, D44: a refresh rate set from guest code, the hooks' path, applies at once; without --user the hooks get a placeholder folder and read no mh4u.ini); dump-check not needed; the game run with fps=120 is N1-21 (golden.py check G1 ... --candidate-args "--set 0:fps=120", always with --user); hooks.cpp is compiled with -Wno-unused-const-variable for its unused PANEL_TOUCH_ALLOWED; --set changes and calls from other threads still apply at the next safe point, in game ticks |
| N1-21 | First game boot: G1 and G2 headless | M | native-worker-high | N1-09, N1-12, N1-13, N1-14, N1-15, N1-16, N1-17, N1-18, N1-19, N1-20, N1-22, N1-29, N0-09, N0-28 | dump | `todo` |  |
| N1-26 | G3 headless: from the save to the Arena hunt | L | native-worker-high | N1-21, N1-13, N0-10, N0-30 | dump, save | `todo` |  |
| N1-22 | Windows and macOS builds | M | native-worker | N1-09 | ci | `done` | PR #7; ci green (Native CI run 39047113161 on its branch: native-windows and native-macos jobs green, ctest 22/22 and conformance 51 passed on both, artifact mh4n-runner-windows uploaded) |
| N1-23 | CPU budget measurement | S | native-worker | N1-26, N1-22, N0-15 | ci, pc | `todo` |  |
| N1-24 | Fallback interpreter: spike | S | native-worker-high | N1-04, N1-09, N1-22 | none | `done` | PR #7; ci green (Native CI run 39065116199 on the N1-28 branch ran the tools job with tests/test_recomp_interp_gen.py); the spike's verdict: generator feasible (20/20 spike forms, 28/29 extra) |
| N1-28 | Fallback interpreter | L | native-worker-high | N1-24 | none | `done` | PR #7; ci green (Native CI run 39065116199 on its branch: native, native-windows, native-macos and runner jobs green) |
| N1-25 | Deterministic mode and the shuffling scheduler | M | native-worker-high | N1-06, N1-26, N0-17 | dump-check | `todo` |  |
<!-- wp-status:end N1 -->

## N2a · Native renderer from captures

<!-- wp-status:begin N2a -->
| ID | Title | Size | Agent | Depends on | Needs | Status | Note |
|---|---|---|---|---|---|---|---|
| N2-01 | RHI spike | M | native-worker-high | N0-13, N1-01 | ci, pc, mac | `todo` |  |
| N2-02 | Capture player and comparison harness | M | native-worker-high | N0-22, N1-01 | dump-check | `done` | PR #5; ci green (run 38926737594); dump-check pending (--gpu replay passes on G1's capture; the timing table prints) |
| N2-03 | GX engine | M | native-worker | N2-02 | dump-check, save | `done` | PR #5; ci green (run 38954207839); dump-check pending (--gpu cpu-gx --isolated --judge fill,transfer,copy,dma passes on the G1-G4 captures) |
| N2-04 | PICA command processor | M | native-worker-high | N2-02 | dump-check, save | `done` | PR #5; ci green (run 38954211634); dump-check pending (--gpu pica-only --isolated --judge none --check registers passes on every command list of G1-G4) |
| N2-05 | Vertex loading | M | native-worker | N2-04, N2-01 | dump-check, save | `todo` |  |
| N2-06 | Vertex shader translation | L | native-worker-high | N2-04, N2-21 | dump-check, save | `todo` |  |
| N2-07 | Fragment pipeline design | M | native-worker-high | N0-13, N2-01 | none | `todo` |  |
| N2-21 | Backend foundation | M | native-worker-high | N2-01, N2-02 | none | `todo` |  |
| N2-22 | Renderer thread and synchronisation | M | native-worker-high | N2-21, N2-03, N2-04 | dump-check, save | `todo` |  |
| N2-13 | Framebuffer operations | M | native-worker-high | N2-07, N2-21 | none | `todo` |  |
| N2-14 | Textures: decoding, hashes, cache | M | native-worker | N2-02 | dump-check, save, pc | `done` | PR #6; dump-check pending (every texture in the G1-G4 captures decodes equal to the CPU reference); maintainer check (hd_hash_check.py with the HD pack on G3's capture); ci green (Native CI run 39018443575 on PR #6's head) |
| N2-08 | Fragment generator, TEV and alpha test | L | native-worker-high | N2-03, N2-05, N2-06, N2-13, N2-14, N2-21 | none | `todo` |  |
| N2-23 | Texture sampling and the first captured frames | M | native-worker-high | N2-08 | dump-check | `todo` |  |
| N2-09 | Fragment lighting | L | native-worker-high | N2-08, N2-23 | none | `todo` |  |
| N2-10 | Fog and gas | M | native-worker-high | N2-08 | none | `todo` |  |
| N2-11 | Procedural textures | M | native-worker | N2-08, N2-23 | none | `todo` |  |
| N2-12 | Shadows | M | native-worker-high | N2-08, N2-13, N2-23 | none | `todo` |  |
| N2-20 | Fragment integration on the captures | M | native-worker-high | N2-09, N2-10, N2-11, N2-12, N2-23 | dump, save | `todo` |  |
| N2-15 | Surface cache and resolution scaling | L | native-worker-high | N2-20, N2-22, N0-14 | dump-check, save | `todo` |  |
| N2-16 | Pipeline cache, ubershader and background compilation | L | native-worker-high | N2-07, N2-20, N2-15 | dump-check, save, pc | `todo` |  |
| N2-17 | HD pack pre-processor | M | native-worker | N2-14 | pc | `done` | PR #6; ci green (Native CI run 39018443575 on PR #6's head); maintainer check (the real pack converts on the reference PC; time and archive size go into docs/native/hdpack.md) |
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
| M-5 | Let sessions download the reference runner: the cloud environment's Network access must be Full (or allow `*.blob.core.windows.net`, seen as `productionresultssa2.blob.core.windows.net`, under Allowed domains) | done (2026-10-16: set to Full; the download worked) |

## Open decisions

D4 (the RHI; spike N2-01) and D5 (the fragment pipeline; N2-07). The full list is [`DECISIONS.md`](DECISIONS.md).
