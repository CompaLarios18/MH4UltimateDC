# The game thread's CPU budget on the native runtime (N1-23, N1-32)

This page is about N1's exit question ([`ROADMAP.md`](../native/ROADMAP.md) section 5; the budget is in section 7):
at 120 fps on the reference CPU, does the game thread of the native runtime, without Azahar, fit its 4 ms? If it
does not, one host thread per core (N6) moves forward to N3. The page holds two measurements made with the procedure
in [`perf-procedure.md`](../native/perf-procedure.md) (*The native runtime: does the game thread fit 4 ms?*):
- N1-23's first measurement, which was UNDECIDED;
- N1-32's re-run, with the game thread's own system calls and services (D99, option 1), which decides it.

**Verdict: WITHIN** (N1-32's re-run, 2026-11-01). Over its three counting runs, the largest p99s are:
- **3.027 ms** for the upper bound, `game_us` (the main thread's guest code, system calls and services);
- **1.709 ms** for the lower bound, `main_us`.

So the game thread fits its 4 ms at p99 on the reference CPU, with about 1 ms to spare on the upper bound. That meets
N1's exit item "in G3 the game thread fits its budget on the reference CPU" (ROADMAP section 5), so one host thread
per core (N6) does not move forward to N3.

Two things to read with it:
- **The per-thread counts barely moved the bound.**
  - Taking the other threads' calls and services out lowers the p99 by about 0.03 ms: `game_all_us`, N1-23's bound,
    is 3.014 to 3.057 ms where `game_us` is 2.983 to 3.027 ms.
  - What made N1-23's measurement UNDECIDED was its run03, which was heavier in every work column (an all-thread
    bound of 4.245 ms). To bring that p99 down to 4 ms, the other threads' calls and services would have had to take
    about 0.25 ms a frame near its p99, where the re-run shows 0.05 ms at p99. So that run would most likely have
    stayed above 4 ms on the own-thread bound too (`unverified`: it has no per-thread counts).
  - Across both measurements, 5 of the 6 counting runs are within 4 ms even on the all-thread bound.
- **The verdict rests on three runs on an idle PC.** The load was 2.45 to 4.5 % before each. The idle check covers
  only the 20 s before a run, not the run itself. N1-23's run03 passed the same check and was heavier for a reason
  not known; a run like it among these three would most likely have made the verdict UNDECIDED (`unverified`).

## The re-run with per-thread counts (N1-32)

### Setup

Everything is as in N1-23's measurement (below), except:
- **The runner** is from `batch/20261101-11` at a10fec6 (N1-32 merged), built the same way.
  - Its `--version` says `a10fec6756-dirty`. The only uncommitted file at build time was `docs/native/STATUS.md`,
    which is not compiled.
  - The code library is the same; `tools/recomp` is still unchanged at a10fec6.
- **The logs** are `# perf v2`: the driver checked the first line of every attempt's `perf.csv`. They hold
  `main_svc_us` and `main_service_us`, the main thread's own share of `svc_us` and `service_us`.
  - `game_us` = `main_us` + `main_svc_us` + `main_service_us`.
  - `game_all_us` = `main_us` + `svc_us` + `service_us`, N1-23's upper bound, is printed for comparison.
- **The folders** are `build\n1-32\run01` to `run07`.

### Attempts

Seven run folders were made, run01 to run07. None was held (the load before each was 2.45 to 4.5 %), so all seven
are attempts:
- **3 counted:** run03, run05 and run07.
- **4 were discarded:** run01, run02, run04 and run06. Each ran 304 s and exited with status 0, but its state trace
  never shows the hunt's scene: the script fell out of step and stayed in the village.

G3-ms reached the hunt in 3 of 7 attempts here, against 3 of 3 in N1-23's measurement and 2 of 5 at N1-26's base.

| Run | Load before | Hunt rows | Frames counted | `game_us` p99 | `game_all_us` p99 | `main_us` p99 | `busy_us` p99 | Run verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| run03 | 3.65 % | 7,913 | 7,313 | 3.007 ms | 3.040 ms | 1.709 ms | 3.102 ms | WITHIN |
| run05 | 4.5 % | 7,920 | 7,320 | 3.027 ms | 3.057 ms | 1.703 ms | 3.116 ms | WITHIN |
| run07 | 2.45 % | 7,912 | 7,312 | 2.983 ms | 3.014 ms | 1.662 ms | 3.062 ms | WITHIN |

The full distributions (milliseconds; p50 / p95 / p99 / max):

| Column | run03 | run05 | run07 |
|---|---|---|---|
| `present` | 8.015 / 10.005 / 10.051 / 70.769 | 8.033 / 10.002 / 10.041 / 68.076 | 8.146 / 9.995 / 10.020 / 72.022 |
| `main` | 1.224 / 1.513 / 1.709 / 2.707 | 1.237 / 1.536 / 1.703 / 2.892 | 1.230 / 1.514 / 1.662 / 3.411 |
| `guest` | 1.262 / 1.564 / 1.766 / 7.486 | 1.277 / 1.588 / 1.764 / 7.054 | 1.267 / 1.560 / 1.717 / 11.824 |
| `svc` | 1.062 / 1.312 / 1.350 / 2.366 | 1.063 / 1.325 / 1.366 / 1.839 | 0.979 / 1.312 / 1.357 / 4.130 |
| `main_svc` | 1.036 / 1.288 / 1.323 / 2.328 | 1.039 / 1.300 / 1.337 / 1.658 | 0.953 / 1.286 / 1.331 / 1.795 |
| `service` | 0.044 / 0.053 / 0.068 / 0.166 | 0.044 / 0.052 / 0.070 / 0.556 | 0.043 / 0.051 / 0.069 / 0.122 |
| `main_service` | 0.044 / 0.050 / 0.067 / 0.165 | 0.043 / 0.049 / 0.067 / 0.156 | 0.042 / 0.048 / 0.066 / 0.117 |
| `idle` | 5.820 / 7.381 / 7.596 / 61.308 | 5.836 / 7.345 / 7.577 / 59.472 | 5.919 / 7.304 / 7.414 / 62.809 |
| `busy` | 2.339 / 2.893 / 3.102 / 8.996 | 2.373 / 2.942 / 3.116 / 8.268 | 2.291 / 2.894 / 3.062 / 16.031 |
| `game` | 2.277 / 2.818 / 3.007 / 4.235 | 2.310 / 2.862 / 3.027 / 4.205 | 2.228 / 2.823 / 2.983 / 4.529 |
| `game_all` | 2.300 / 2.846 / 3.040 / 4.266 | 2.335 / 2.892 / 3.057 / 4.261 | 2.255 / 2.852 / 3.014 / 7.618 |

These figures were taken over the same rows that `perf_report.py` keeps; they are not in its report:

| Run | `game_us` > 4 ms | `game_all_us` > 4 ms | `main_us` > 4 ms | `game_us` p99.9 | `game_all_us` − `game_us`, p50 / p99 | `present_us` > 8.833 ms |
|---|---:|---:|---:|---:|---:|---:|
| run03 | 3 | 4 | 0 | 3.742 ms | 0.025 / 0.051 ms | 995 (13.61 %) |
| run05 | 1 | 1 | 0 | 3.355 ms | 0.025 / 0.049 ms | 916 (12.51 %) |
| run07 | 4 | 4 | 0 | 3.522 ms | 0.025 / 0.051 ms | 805 (11.01 %) |

The reports' `switches` and `preempts` rows give 29 to 30 switches a frame at p50, and 5 to 6 preemptions. As in
N1-23's runs (below), every run has one frame over 50 ms (68 to 72 ms).

### Reading it

- **The main thread makes almost all the system calls.** Its share of `svc_us` is 97.5 to 97.8 % at p50. The other
  threads' calls and services add 0.025 ms a frame at p50 and about 0.05 ms at p99.
- **The margin.**
  - The upper bound's p99 is at most 3.03 ms and its p99.9 at most 3.74 ms.
  - At most 4 frames in about 7,300 have a `game_us` over 4 ms (0.05 %).
  - No frame has a `main_us` over 4 ms.
- **The bound is still an upper bound.** `game_us` still holds the scheduler's host work run at the main thread's
  preemption checks and system calls (N1-32's spec, D99). Counting that work apart could only lower `game_us`, so it
  cannot change a WITHIN verdict, whatever its size. Its size is not measured: the procedure puts the part at
  preemption checks at probably tens of microseconds a frame, and the part at system calls has no estimate.
- **Every guest thread together** fits the one host thread at p99, as in N1-23's runs: `busy_us` has a p99 of 3.06
  to 3.12 ms, and `present_us` minus `idle_us` (which holds the host loop too) 3.11 to 3.17 ms, of the 8.333 ms
  frame.
- **The frame pacing differs from N1-23's runs.**
  - `present_us` has a p50 of 8.015 to 8.146 ms and a p95 of about 10.0 ms, where N1-23's had 8.33 ms and 8.4 to
    10.0 ms.
  - 9.4 to 13.5 % of frames take 9.5 to 10.5 ms, and 51 to 68 % take under 8.2 ms.
  - Frames late by more than half a millisecond are 11.0 to 13.6 %, against 3.9 to 8.6 %.
  - They are late in the wait, not in the work: their `present_us` minus `idle_us` is 2.68 to 2.70 ms at p50, and
    the work columns are close to those of N1-23's run02 and run05.
  - N1-32's change touches no wait: it adds one addition where the scheduler closes a stretch of system call or
    service time, and three look-ups and two fields to each row of the log. The cause is not known (`unverified`).
    It concerns the frame limiter, not the game thread's budget, which is this page's question.

## The first measurement (N1-23)

This first measurement gave **UNDECIDED**. Its verdict over the three runs takes the largest p99 of each bound;
both are run03's, and 4 ms lies between them:
- the upper bound (at N1-23, `main_us` + every thread's `svc_us` + `service_us`) has a p99 of **4.245 ms**;
- the lower bound, `main_us`, has a p99 of **2.675 ms**.

In the other two runs, even the upper bound's p99 is within 4 ms (3.132 and 3.182 ms). The log's two bounds could
not tell whether the p99 was within 4 ms, which N1-23's Done-when asks; *What settled it*, below, says how it was
settled.

### Setup

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

### Attempts

Five run folders were made, `build\n1-23\run01` to `run05`:

- **Two were held** (run01 and run04). The load before them was 26.9 % and 28.05 %, so the runner was not started.
  Held runs are not attempts.
- **Three attempts were made, and all three counted** (run02, run03 and run05). Each took 304 s of host time,
  exited with status 0, and reached the hunt.
- **Discarded:** none.

G3-ms reached the hunt in 3 of 3 attempts, against 2 of 5 at N1-26's base. Three runs counted within the ten
attempts allowed, so no other script was needed.

In this section, `game_us` is the v1 upper bound: `main_us` + every thread's `svc_us` + `service_us`
(`game_all_us` in v2).

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

### Reading it

- **The bounds.** At N1-23 the scheduler counted system calls by number and services by name, not by thread.
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

### What settled it

N1-32 (D99, option 1, decided 2026-11-01) made the scheduler charge system call and service time to the thread
that made the call. `--perf-log` writes the main thread's own share (`# perf v2`), and `perf_report.py` builds
`game_us` and its verdict from it. The procedure then ran again, and its three counting runs are the re-run above.

`game_us` now holds the game thread's own calls and services, and no other thread's. It still holds the scheduler's
host work run at the game thread's preemption checks and at the start and end of its system calls: timer events,
posted work, the hooks (this log's row and the state trace's line among them) and picking the next thread. The
procedure puts the part at preemption checks at probably tens of microseconds a frame; the part at system calls has
no estimate. So `game_us` stays an upper bound, and the re-run does not measure how close it is. To make it exact,
the scheduler would also have to count that host work apart. A WITHIN verdict does not need that: counting the work
apart could only lower `game_us`.
