# The game thread's CPU budget on the native runtime (N1-23)

This page is about N1's exit question ([`ROADMAP.md`](../native/ROADMAP.md) section 5; the budget is in section 7):
at 120 fps on the reference CPU, does the game thread of the native runtime, without Azahar, fit its 4 ms? If it
does not, one host thread per core (N6) moves forward to N3. The page holds the measurement made with the procedure
in [`perf-procedure.md`](../native/perf-procedure.md) (*The native runtime: does the game thread fit 4 ms?*), and its
verdict.

**Verdict: UNDECIDED.** The verdict over the three runs takes the largest p99 of each bound. Both are run03's, and
4 ms lies between them:
- the upper bound, `game_us`, has a p99 of **4.245 ms**;
- the lower bound, `main_us`, has a p99 of **2.675 ms**.

In the other two runs, even the upper bound's p99 is within 4 ms (3.132 and 3.182 ms).

So the measurement does not answer N1's exit question either way. N1-23's Done-when asks whether the p99 is within
4 ms or not, and the log's two bounds cannot tell. *What settles it*, below, says what would.

## Setup

- **Machine:** the maintainer's PC: AMD Ryzen 5 5600X, Windows 11. Its RX 6700 XT is not used: the runner's default
  is `--gpu null`.
- **Date:** 2026-11-01.
- **Runner:** `mh4u_native_runner` from `batch/20261101-11` at 7ea454b (N1-23 merged).
  - It was built locally from that tree. The procedure allows this in place of the CI artifact the spec names.
  - The build is RelWithDebInfo with `-DMH4N_WERROR=ON` and llvm-mingw, as the native-windows job builds it.
  - Its `--version` says `7ea454b370-dirty`. The only uncommitted file at build time was `docs/native/STATUS.md`
    (N1-23's row), which is not compiled.
- **Code library:** the `--native` library of `fps-audit.md`'s *Runs on an idle PC*. It has `RC_HOST_VERSION` 6 and
  was built 2026-10-31 at `native` 4a6eaf4. `tools/recomp` is unchanged from 4a6eaf4 to 7ea454b.
- **Session:** G3 in real time, run as follows:
  - `G3-ms.txt` (N1-26, keyed in game milliseconds), run with `--ms 304100`;
  - `--set 0:fps=120`, `--io-latency azahar` and G3's `--init-time`;
  - a fresh copy of G3's save for every run;
  - `--state-trace` (with `--observable config\native\observable.txt`) for the hunt check;
  - `TZ=UTC0`.
- **Reading each run:** `perf_report.py perf.csv --state-trace state.txt --skip 600 --fps 120`. It keeps only the
  frames in which the state trace shows the hunt's scene, and drops the first 600 of them.
- **Idle rule (D70):** an attempt starts only when the total CPU load over the 20 s before it is at most 25 %.

## Attempts

Five run folders were made, run01 to run05:

- **Two were held** (run01 and run04). The load before them was 26.9 % and 28.05 %, so the runner was not started.
  Held runs are not attempts.
- **Three attempts were made, and all three counted** (run02, run03 and run05). Each took 304 s of host time,
  exited with status 0, and reached the hunt.
- **Discarded:** none.

G3-ms reached the hunt in 3 of 3 attempts, against 2 of 5 at N1-26's base. Three runs counted within the ten
attempts allowed, so no other script was needed.

| Run | Load before | Hunt rows | Frames counted | `game_us` p99 | `main_us` p99 | `busy_us` p99 | Run verdict |
|---|---:|---:|---:|---:|---:|---:|---|
| run02 | 21.5 % | 7,910 | 7,310 | 3.132 ms | 1.885 ms | 3.205 ms | WITHIN |
| run03 | 5.1 % | 7,912 | 7,312 | 4.245 ms | 2.675 ms | 4.389 ms | UNDECIDED |
| run05 | 23.85 % | 7,909 | 7,309 | 3.182 ms | 1.938 ms | 3.266 ms | WITHIN |

The full distributions (milliseconds; p50 / p95 / p99 / max):

| Column | run02 | run03 | run05 |
|---|---|---|---|
| `present` | 8.333 / 8.367 / 10.018 / 72.031 | 8.325 / 9.967 / 10.095 / 70.680 | 8.332 / 9.049 / 9.552 / 69.166 |
| `main` | 1.208 / 1.540 / 1.885 / 3.381 | 1.481 / 2.388 / 2.675 / 6.704 | 1.233 / 1.631 / 1.938 / 2.802 |
| `guest` | 1.245 / 1.596 / 1.965 / 7.455 | 1.537 / 2.514 / 2.836 / 10.597 | 1.271 / 1.689 / 2.026 / 7.138 |
| `svc` | 1.028 / 1.323 / 1.358 / 1.551 | 1.109 / 1.418 / 1.542 / 4.082 | 1.008 / 1.275 / 1.327 / 1.862 |
| `service` | 0.044 / 0.052 / 0.075 / 0.276 | 0.048 / 0.090 / 0.107 / 0.336 | 0.045 / 0.056 / 0.082 / 0.125 |
| `idle` | 5.948 / 6.622 / 7.523 / 62.739 | 5.624 / 6.666 / 7.216 / 57.177 | 5.933 / 7.445 / 7.671 / 60.553 |
| `busy` | 2.323 / 2.899 / 3.205 / 8.928 | 2.645 / 3.915 / 4.389 / 12.597 | 2.326 / 2.901 / 3.266 / 8.247 |
| `game` | 2.281 / 2.849 / 3.132 / 4.747 | 2.582 / 3.800 / 4.245 / 7.622 | 2.288 / 2.847 / 3.182 / 4.208 |

These figures were taken over the same rows that `perf_report.py` keeps; they are not in its report:

| Run | `game_us` > 4 ms | `main_us` > 4 ms | `game_us` p99.9 | `present_us` > 8.833 ms |
|---|---:|---:|---:|---:|
| run02 | 2 (0.03 %) | 0 | 3.782 ms | 288 (3.94 %) |
| run03 | 208 (2.84 %) | 5 (0.07 %) | 5.349 ms | 631 (8.63 %) |
| run05 | 2 (0.03 %) | 0 | 3.743 ms | 470 (6.43 %) |

The reports' `switches` and `preempts` rows give 29 to 30 switches a frame at p50, and 5 to 6 preemptions.

Every run has one frame over 50 ms: 69 to 72 ms, the `max` of `present` above. It falls at the same point of the hunt
in all three runs, about 630 rows after the skip.
- Most of it is `idle_us`, 57 to 63 ms: a wait, not work.
- It is also each run's largest `busy_us`, 8.2 to 12.6 ms. But its `game_us` is 2.0 to 3.2 ms, so most of that work
  (6.3 to 9.4 ms, its `guest_us` minus `main_us`) belongs to the other threads.

## Reading it

- **The bounds.** The scheduler counts system calls by number and services by name, not by thread.
  - `game_us` = `main_us` + `svc_us` + `service_us`, the last two summed over every thread. It is an upper bound of
    the game thread's time, because it also holds the other threads' calls and services.
  - `main_us` alone is a lower bound, up to the scheduler work done at the main thread's preemption checks.
  - The gap between them is `svc_us` + `service_us`. Per frame it is 1.06 to 1.17 ms at p50 and 1.32 to 1.48 ms at
    p95, and about 95 % of it is `svc_us`. Between the two p99s it is 1.24 to 1.57 ms.
- **Two runs are WITHIN even on the upper bound.** In run02 and run05, 2 frames of about 7,300 have a `game_us` over
  4 ms, and none has a `main_us` over it.
- **run03 is heavier in every work column.** Its `main_us`, `guest_us`, `svc_us`, `service_us`, `busy_us` and
  `game_us` are higher than in the other two runs at p50, p95, p99 and max.
  - Its `main_us` is 20 to 23 % higher at p50 and 46 to 55 % higher at p95.
  - Its load before the run was the lowest of the three (5.1 %). The idle check covers only the 20 s before a run,
    not the run itself, so whether something else ran during run03 is not known (`unverified`).
  - The verdict takes the largest p99s, so run03 decides it.
- **Every guest thread together fits the one host thread at p99.**
  - `busy_us`, every guest thread's counted work on the one host thread, has a p99 of 3.2 to 4.4 ms.
  - The host loop between guest runs is in no column, so `busy_us` is a lower bound of the host thread's work.
    `present_us` minus `idle_us` holds that loop too. Its p99 is 3.3 to 4.5 ms, and `idle_us` is at least 3.6 ms in
    99 % of frames (5.6 to 5.9 ms at p50).
  - This does not hold in every frame: in 1 or 2 frames a run, `present_us` minus `idle_us` is over 8.333 ms (the
    frame over 50 ms is one of them).
  - This is G3's hunt with `--gpu null`, and it says nothing about the game thread's 4 ms.
- **Frame pacing (`present_us`).** The p50 is 8.33 ms.
  - The report's line "Frames over the budget of 8.333 ms" (36 to 42 % of frames) counts every late frame, however
    little it is late. 61 to 89 % of those are late by 0.05 ms or less.
  - Frames late by more than half a millisecond are 3.9 to 8.6 %.
  - They are late in the wait, not in the work: their `present_us` minus `idle_us` is 2.6 to 2.9 ms at p50.

## What settles it

The proposal: make the scheduler (N1-06's counters, `native/src/kernel/scheduler.cpp`) charge system call and
service time to the thread that made the call. `--perf-log` (`native/src/perf/perf_log.cpp`, which today sums them
over every thread) would then write the main thread's own share, and `perf_report.py` would build `game_us` and its
verdict from it. Then run the procedure again (three counting runs). N1-32 holds it (D99, option 1, decided 2026-11-01).

`game_us` would then hold the game thread's own calls and services, and no other thread's. It would still hold the
scheduler's host work run at the game thread's preemption checks and at the start and end of its system calls:
timer events, posted work, the hooks (this log's row and the state trace's line among them) and picking the next
thread. The procedure puts the part at preemption checks at probably tens of microseconds a frame; the part at
system calls has no estimate.
- So `game_us` would stay an upper bound, probably a close one.
- The verdict would be WITHIN or OVER, unless the p99 falls within that margin of 4 ms.
- To make it exact, the scheduler would also have to count that host work apart.

Until then, N1's exit question stays open. UNDECIDED is not the "does not fit" that would move one host thread per
core (N6) forward to N3 (`wp/N3.md`), and it does not rule that out either.
