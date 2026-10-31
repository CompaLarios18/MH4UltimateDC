#!/usr/bin/env python3
"""The 60/120 fps audit of the native runtime (N3-22, ROADMAP section 6's high-frame-rate rule, D70, D71).

The same golden session is played twice by the native runner, headless: at 60 fps with the session as it is, and at
120 fps with the session scaled by 2 (every frame key, hold and checkpoint frame doubled, milliseconds unchanged;
`observable_probe.py scale`, N3-21). The two runs must give the same game, within the tolerances of D70:

  1 scenes       the sequence of real scene changes is equal (values and order)
  2 times        each scene change falls at the same game time (frame / fps) within 4 frames at 60 fps (66.7 ms)
  3 checkpoints  at each checkpoint of the session the `eq` observables are equal (and the scene is the checkpoint's)
  4 observables  at matched game times (every second, in the hunt only: the chains name whatever sits there in other
                 scenes, N3-21) the hunter's position is within 2 % of the distance travelled in the last second
                 plus 0.05 units, and the quest's clock within 1 second (a counter that never moves in either run
                 compares nothing, n/a, unless its two constants are further apart than that); the table also has
                 the distance with 4 frames of time slack, as information
  5 dsp          `capfmt.py diff-dsp --align --max-shift 48` of the two DSP captures: the same sounds start with the
                 same buffers (D51)
  6 writes       the `fs:USER` file Writes are equal by D52's rule: per thread, in order, in full, the masked byte
                 ranges of config/native/fs_write_masks.txt zeroed first (tools/compare_ipc_traces.py)

The `dsp` item also prints the game clock offset between the two runs at the capture's first game frame, as
information beside the measured shift: it is not the shift (the measured shifts of N3-22's runs were 11 to 85 % of it).
Before the six, `runs` checks that both runs finished (exit status 0, the trace reaches the session's last frame): a
run that hangs must not read as a pass. The numbers above are D70's first guesses; they are options with those
defaults and are never retuned to make a session pass (N3 rule 7). An item with nothing to compare (a session without a
DSP capture, no readable position) is `n/a`: reported, not a failure, unless `--require` names it (D34).

  fps_audit.py scale SESSION --factor 2 -o OUT
      observable_probe.py's `scale` (N3-21).
  fps_audit.py run S --runner EXE --program DIR --code LIB --user-before DIR --io-latency azahar|none --out OUT
                [--factor 2] [--observable FILE] [--dsp START:FRAMES] [--path DIR] [--timeout SECONDS]
                [--lock DIR] [--force] [--deterministic [--seed N]] [--runner-arg=ARG ...]
      S is a session name of tools/native/golden/ or a session file. Runs the native runner at `--set 0:fps=60`
      with S and at `--set 0:fps=120` with S scaled, each from a fresh copy of USER-BEFORE, with `--state-trace`
      (config/native/observable.txt plus N3-21's four lines, as `ignore`), `--trace-ipc --trace-ipc-full` and
      `--capture-dsp` (the session's DSP captures, their start scaled). Writes OUT/r60/, OUT/r120/ and OUT/meta.json.
      `--lock DIR` is the emulator's lock folder: created before the runs (waiting while it exists), removed after.
      `--deterministic` (and `--seed N`) asks for the runner's deterministic mode; meta.json records the mode and
      compare prints it, so a deterministic and a real-time verdict cannot be confused. `--force` deletes OUT first
      only when it holds nothing but what `run` writes.
  fps_audit.py compare OUT [--require position,quest_time,dsp,writes] [--position-until FRAME] [--hunt-scene V|any]
                 [--fs-write-mask FILE|none] [tolerance options: --time-frames, --position-relative, --position-absolute,
                 --quest-seconds, --quest-units, --max-shift]
      The six items, one line each, then a verdict; writes OUT/compare.txt and OUT/compare.json (every number of
      the tables). Exit 0 when no item fails, 1 when one does, 2 for a usage problem or an unreadable run.

Traces and captures are game data: they live under build/ and only counts, frames, times and CRCs are printed.
"""
from __future__ import annotations

import argparse
import bisect
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional, Sequence

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import capfmt  # noqa: E402
import compare_ipc_traces  # noqa: E402
import golden  # noqa: E402
import observable_probe  # noqa: E402
import state_diff  # noqa: E402
import tracefmt  # noqa: E402

BASE_FPS = 60
DEFAULT_FACTOR = 2
DEFAULT_TIMEOUT = 3600           # per run: the slowest run of N3-22 took 455 s (a hang must not cost hours)
DEFAULT_WRITE_MASKS = ROOT / "config" / "native" / "fs_write_masks.txt"
DEFAULT_OBSERVABLE = state_diff.DEFAULT_OBSERVABLE
META = "meta.json"

# D70's first guesses (docs/native/DECISIONS.md): options with these defaults, never retuned to make a run pass.
TIME_TOLERANCE_FRAMES = 4        # a scene change: frames at 60 fps (66.7 ms)
POSITION_RELATIVE = 0.02         # of the distance travelled in the last second
POSITION_ABSOLUTE = 0.05         # units
QUEST_SECONDS = 1.0
QUEST_UNITS_PER_SECOND = 60      # the quest countdown's rate (N3-21, docs/research/fps-observables.md)
HUNT_SCENE = 0x00e655c8          # aQuest: the scene the two observables are valid in (N3-21: "in the hunt only")
MAX_SHIFT = capfmt.DEFAULT_MAX_SHIFT

# N3-21's lines for config/native/observable.txt, proposed as `ignore`: added at run time, to a copy of the file.
N321_LINES = (
    "[[[0x00fb83e8]+0xa98]+0xe48]  4 hunter_x    ignore",
    "[[[0x00fb83e8]+0xa98]+0xe4c]  4 hunter_y    ignore",
    "[[[0x00fb83e8]+0xa98]+0xe50]  4 hunter_z    ignore",
    "[[0x00fbeffc]+0x803c]         4 quest_time  ignore",
)
POSITION = ("hunter_x", "hunter_y", "hunter_z")
QUEST_TIME = "quest_time"
TICKS_PER_SECOND = observable_probe.TICKS_PER_SECOND
POSITION_LIMIT = observable_probe.MAGNITUDE_LIMIT

REQUIRABLE = ("position", "quest_time", "dsp", "writes")


class AuditError(Exception):
    """A usage problem or an input that cannot be read (exit status 2)."""


# -- the session and the observable file --------------------------------------------------------------------------

def resolve_session(name: str) -> Path:
    path = Path(name)
    if path.is_file():
        return path.resolve()
    try:
        return golden.session_path(name).resolve()
    except golden.GoldenError as error:
        raise AuditError(str(error)) from None


def load_session(path: Path) -> golden.Session:
    try:
        return golden.parse_session(path)
    except golden.GoldenError as error:
        raise AuditError(str(error)) from None


def base_name(name: str) -> str:
    """`G3-x2` -> `G3`."""
    head, sep, tail = name.rpartition("-x")
    return head if sep and tail.isdigit() and head else name


def observable_text(base: str) -> str:
    """The observable file the runs use: the base file plus N3-21's four lines (the ones whose name it does not
    have already), so config/** is never edited."""
    try:
        parsed = state_diff.parse_observable(base)
    except state_diff.FormatError as error:
        raise AuditError(f"observable file: {error}") from None
    have = {o.name for o in parsed}
    text = base if base.endswith("\n") or not base else base + "\n"
    added = [line for line in N321_LINES if line.split()[2] not in have]
    return text + "".join(line + "\n" for line in added)


def parse_dsp_capture(text: str) -> golden.Capture:
    parts = text.split(":")
    if len(parts) != 2 or not all(p.isdigit() for p in parts) or int(parts[1]) < 1:
        raise AuditError(f"--dsp {text!r}: want START:FRAMES (game frames at 60 fps, DSP frames)")
    return golden.Capture("dsp", int(parts[0]), int(parts[1]))


def dsp_captures(session: golden.Session, extra: Sequence[golden.Capture]) -> list[golden.Capture]:
    caps = [c for c in session.captures if c.kind == "dsp"]
    for cap in extra:
        if any(c.filename == cap.filename for c in caps):
            raise AuditError(f"the DSP capture starting at frame {cap.start} is given twice")
        caps.append(cap)
    return caps


# -- run ----------------------------------------------------------------------------------------------------------

def run_dir(rate: int) -> str:
    return f"r{rate}"


def runner_args(*, program: str, code: str, user: str, session: golden.Session, session_file: str, rate: int,
                factor_of_rate: int, io_latency: str, observable: str, out: Path,
                captures: Sequence[golden.Capture], extra: Sequence[str] = ()) -> list[str]:
    """The native runner's arguments (without the runner itself) for one rate. `captures` are the 60 fps captures:
    the start of each is multiplied by `factor_of_rate` (1 at 60 fps, K at the scaled rate)."""
    args = [program, "--code", code, "--user", user, "--frames", str(session.frames), "--input-file", session_file]
    if session.init_time is not None:
        args += ["--init-time", session.init_time]
    args += ["--set", f"0:fps={rate}", "--io-latency", io_latency,
             "--trace-ipc", str(out / "ipc.txt"), "--trace-ipc-full",
             "--state-trace", str(out / "state.txt"), "--observable", observable]
    for cap in captures:
        args += ["--capture-dsp", f"{cap.start * factor_of_rate}:{cap.frames}:{out / cap.filename}"]
    return args + list(extra)


def child_env(session: golden.Session, paths: Sequence[str]) -> dict[str, str]:
    env = golden.child_env(session.env)
    if paths:
        env["PATH"] = os.pathsep.join([*paths, env.get("PATH", "")])
    return env


class Lock:
    """The emulator's lock folder (AGENTS: exclusive resource): created atomically, waited for while it exists."""

    def __init__(self, folder: Optional[str], poll: float = 30.0, wait: float = 6 * 3600) -> None:
        self.folder = Path(folder) if folder else None
        self.poll, self.wait = poll, wait
        self.held = False

    def __enter__(self) -> "Lock":
        if self.folder is None:
            return self
        self.folder.parent.mkdir(parents=True, exist_ok=True)
        deadline = time.monotonic() + self.wait
        while True:
            try:
                self.folder.mkdir()
                self.held = True
                return self
            except FileExistsError:
                if time.monotonic() >= deadline:
                    raise AuditError(f"the emulator lock {self.folder} is still taken after {self.wait:.0f} s") from None
                time.sleep(self.poll)

    def __exit__(self, *exc) -> None:
        if self.held:
            shutil.rmtree(self.folder, ignore_errors=True)
            self.held = False


def run_one(runner: str, args: list[str], rdir: Path, env: dict[str, str], timeout: float) -> dict:
    rdir.mkdir(parents=True, exist_ok=True)
    cmd = golden.runner_argv(runner) + args
    started = time.monotonic()
    timed_out = False
    with open(rdir / "runner.log", "wb") as log:
        try:
            done = subprocess.run(cmd, cwd=rdir, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=timeout,
                                  check=False)
            status = done.returncode
        except subprocess.TimeoutExpired:
            log.write(b"\n[fps_audit] the runner timed out\n")
            status, timed_out = 124, True
        except OSError as error:
            raise AuditError(f"cannot start the runner {runner}: {error}") from None
    result = {"exit": status, "seconds": round(time.monotonic() - started, 1), "timed_out": timed_out}
    (rdir / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    return result


def cmd_run(args: argparse.Namespace) -> int:
    if args.io_latency not in ("azahar", "none"):
        raise AuditError("--io-latency is azahar or none")
    factor = args.factor
    if factor < 2:
        raise AuditError("--factor must be at least 2")
    rates = [BASE_FPS, BASE_FPS * factor]
    source = resolve_session(args.session)
    base_text = source.read_text(encoding="utf-8")
    session = load_session(source)
    out = Path(args.out).resolve()
    scaled_name = f"{session.name}-x{factor}"
    scaled_file = out / f"{scaled_name}.txt"
    try:
        scaled_text = observable_probe.scale_session(base_text, factor)
    except observable_probe.UsageError as error:
        raise AuditError(str(error)) from None
    for what, path in (("runner", args.runner), ("program", args.program), ("code library", args.code),
                       ("user-before", args.user_before)):
        if not Path(path).exists():
            raise AuditError(f"{what} not found: {path}")
    base_observable = Path(args.observable).read_text(encoding="utf-8") if args.observable else \
        DEFAULT_OBSERVABLE.read_text(encoding="utf-8")
    captures = dsp_captures(session, [parse_dsp_capture(d) for d in args.dsp])
    if args.seed is not None and not args.deterministic:
        raise AuditError("--seed is an option of the deterministic mode: pass --deterministic too")
    mode = "deterministic" if args.deterministic or "--deterministic" in args.runner_arg else "real-time"
    mode_args = (["--deterministic"] if args.deterministic else []) + \
        (["--seed", str(args.seed)] if args.seed is not None else [])
    if args.force and out.exists():
        # only a folder `run` wrote is deleted: its own files and nothing else (a killed run has no meta.json yet)
        own = {META, "observable.txt", "compare.txt", "compare.json", source.name, scaled_file.name,
               *(run_dir(r) for r in rates)}
        others = sorted(p.name for p in out.iterdir() if p.name not in own)
        if others:
            raise AuditError(f"--force deletes only a folder of `fps_audit.py run`: {out} also holds "
                             f"{', '.join(others[:5])}{' ...' if len(others) > 5 else ''}")
        shutil.rmtree(out)
    if any((out / run_dir(r)).exists() for r in rates):
        raise AuditError(f"{out} already holds runs: choose another --out or pass --force")
    out.mkdir(parents=True, exist_ok=True)
    (out / source.name).write_text(base_text, encoding="utf-8", newline="\n")
    scaled_file.write_text(scaled_text, encoding="utf-8", newline="\n")
    scaled = load_session(scaled_file)
    observable = out / "observable.txt"
    observable.write_text(observable_text(base_observable), encoding="utf-8", newline="\n")
    meta = {
        "version": 1, "session": session.name, "session_file": source.name, "scaled_file": scaled_file.name,
        "factor": factor, "io_latency": args.io_latency, "observable": observable.name,
        "frames": session.frames, "init_time": session.init_time,
        "checkpoints": [[f, s] for f, s in session.checkpoints],
        "mode": mode, "seed": args.seed,
        "runner_args": mode_args + list(args.runner_arg),
        "dsp": [c.filename for c in captures], "dsp_starts": {c.filename: c.start for c in captures},
        "runs": {},
    }
    paths = [str(Path(p)) for p in args.path]
    status = 0
    with Lock(args.lock, poll=args.lock_poll):
        for rate in rates:
            k = rate // BASE_FPS
            rdir = out / run_dir(rate)
            user = rdir / "user"
            rdir.mkdir(parents=True, exist_ok=True)
            shutil.copytree(args.user_before, user)
            this = session if k == 1 else scaled
            file = source if k == 1 else scaled_file
            runner_argv = runner_args(
                program=str(Path(args.program).resolve()), code=str(Path(args.code).resolve()), user=str(user),
                session=this, session_file=str(file), rate=rate, factor_of_rate=k, io_latency=args.io_latency,
                observable=str(observable), out=rdir, captures=captures,
                extra=mode_args + [a.replace("{out}", str(rdir)) for a in args.runner_arg])
            result = run_one(str(Path(args.runner).resolve()), runner_argv, rdir, child_env(session, paths),
                             args.timeout)
            result.update({"rate": rate, "frames": this.frames, "dir": rdir.name})
            meta["runs"][str(rate)] = result
            (out / META).write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8", newline="\n")
            print(f"fps_audit: {session.name} at {rate} fps: exit {result['exit']} in {result['seconds']} s")
            if result["exit"] != 0:
                status = 1
    return status


# -- reading a pair of runs ---------------------------------------------------------------------------------------

@dataclass
class Pair:
    """Two runs of one session as `compare` reads them."""
    root: Path
    meta: dict
    factor: int
    session: str
    observables: list[state_diff.Observable]
    states: dict[int, list[state_diff.Record]] = field(default_factory=dict)

    def path(self, rate: int, name: str) -> Path:
        return self.root / self.meta["runs"][str(rate)]["dir"] / name


def read_pair(root: Path) -> Pair:
    try:
        meta = json.loads((root / META).read_text(encoding="utf-8"))
    except OSError as error:
        raise AuditError(f"cannot read {root / META}: {error.strerror}") from None
    except ValueError:
        raise AuditError(f"{root / META} is not JSON") from None
    try:
        factor = int(meta["factor"])
        runs = meta["runs"]
        if sorted(runs) != sorted({str(BASE_FPS), str(BASE_FPS * factor)}):
            raise KeyError("runs")
        name = meta["session"]
        observables = state_diff.parse_observable((root / meta["observable"]).read_text(encoding="utf-8"))
    except (KeyError, TypeError, ValueError):
        raise AuditError(f"{root / META}: not the meta file of two runs (run `fps_audit.py run` first)") from None
    except OSError as error:
        raise AuditError(f"cannot read the observable file: {error.strerror}") from None
    except state_diff.FormatError as error:
        raise AuditError(f"observable file: {error}") from None
    return Pair(root, meta, factor, name, observables)


def load_states(pair: Pair) -> None:
    for rate in (BASE_FPS, BASE_FPS * pair.factor):
        path = pair.path(rate, "state.txt")
        try:
            pair.states[rate] = state_diff.parse_trace(path.read_text(encoding="utf-8"))
        except OSError:
            pair.states[rate] = []
        except state_diff.FormatError as error:
            raise AuditError(f"{path}: {error}") from None


# -- the items ----------------------------------------------------------------------------------------------------

PASS, FAIL, NA = "pass", "fail", "n/a"


@dataclass
class Item:
    name: str
    status: str
    summary: str
    details: list[str] = field(default_factory=list)
    data: dict = field(default_factory=dict)


def check_runs(pair: Pair) -> Item:
    problems: list[str] = []
    notes: list[str] = []
    for rate in (BASE_FPS, BASE_FPS * pair.factor):
        run = pair.meta["runs"][str(rate)]
        records = pair.states.get(rate, [])
        last = records[-1].frame if records else 0
        notes.append(f"{rate} fps: exit {run['exit']}, {run.get('seconds', '?')} s, {len(records)} records, "
                     f"last frame {last} of {run['frames']}")
        if run["exit"] != 0:
            problems.append(f"the run at {rate} fps ended with exit status {run['exit']}"
                            + (" (timed out)" if run.get("timed_out") else ""))
        if not records:
            problems.append(f"the run at {rate} fps wrote no state trace")
        elif last < run["frames"]:
            problems.append(f"the run at {rate} fps stopped at frame {last}, before the session's {run['frames']}")
    return Item("runs", FAIL if problems else PASS,
                "; ".join(problems) if problems else "both runs finished: " + "; ".join(notes), notes)


def scene_list(records: list[state_diff.Record]) -> list[state_diff.Record]:
    return state_diff.real_scene_changes(records)


def _hex(value: Optional[int]) -> str:
    return "-" if value is None else f"0x{value:08x}"


def check_scenes(pair: Pair) -> Item:
    a = scene_list(pair.states[BASE_FPS])
    b = scene_list(pair.states[BASE_FPS * pair.factor])
    rows = []
    for k in range(max(len(a), len(b))):
        x = a[k] if k < len(a) else None
        y = b[k] if k < len(b) else None
        rows.append(f"#{k}: {_hex(x.values[state_diff.SCENE]) if x else '(none)'} at frame "
                    f"{x.frame if x else '-'} | {_hex(y.values[state_diff.SCENE]) if y else '(none)'} at frame "
                    f"{y.frame if y else '-'}")
    data = {"changes": [[x.frame, x.values[state_diff.SCENE]] for x in a],
            "changes_scaled": [[y.frame, y.values[state_diff.SCENE]] for y in b]}
    if not a and not b:
        return Item("scenes", FAIL, "no real scene change in either run: nothing to compare", rows, data)
    for k in range(max(len(a), len(b))):
        if k >= len(a) or k >= len(b) or a[k].values[state_diff.SCENE] != b[k].values[state_diff.SCENE]:
            return Item("scenes", FAIL, f"the scene sequences differ at change #{k} ({len(a)} changes at "
                        f"{BASE_FPS} fps, {len(b)} at {BASE_FPS * pair.factor} fps)", rows, data)
    return Item("scenes", PASS, f"{len(a)} real scene changes, the same scenes in the same order", rows, data)


def check_times(pair: Pair, scenes: Item, tolerance_frames: int) -> Item:
    if scenes.status != PASS:
        return Item("times", NA, "the scene sequences differ (item scenes): no change to match", [], {})
    k = pair.factor
    a = scene_list(pair.states[BASE_FPS])
    b = scene_list(pair.states[BASE_FPS * k])
    rows, worst, bad = [], 0, []
    deltas = []
    for i, (x, y) in enumerate(zip(a, b)):
        diff = abs(x.frame * k - y.frame)               # frames at the scaled rate
        ms = diff * 1000 / (BASE_FPS * k)
        clock = (y.ticks - x.ticks) / TICKS_PER_SECOND  # the game clock (the trace's t=) at the two records
        deltas.append({"index": i, "frame": x.frame, "frame_scaled": y.frame, "ms": round(ms, 2),
                       "frames_at_60": round(diff / k, 2), "clock_offset_s": round(clock, 3)})
        mark = "" if diff <= tolerance_frames * k else "  OVER"
        rows.append(f"#{i} to {_hex(x.values[state_diff.SCENE])}: frame {x.frame} ({x.frame * 1000 / BASE_FPS:.1f} ms) | "
                    f"frame {y.frame} ({y.frame * 1000 / (BASE_FPS * k):.1f} ms): {ms:.1f} ms, "
                    f"{diff / k:.2f} frames at {BASE_FPS} fps{mark}; game clock {clock:+.3f} s")
        worst = max(worst, diff)
        if diff > tolerance_frames * k:
            bad.append(i)
    limit_ms = tolerance_frames * 1000 / BASE_FPS
    data = {"changes": deltas, "worst_ms": round(worst * 1000 / (BASE_FPS * k), 2),
            "limit_ms": round(limit_ms, 2), "limit_frames": tolerance_frames}
    summary = (f"largest difference {worst * 1000 / (BASE_FPS * k):.1f} ms ({worst / k:.2f} frames at {BASE_FPS} fps) over "
               f"{len(deltas)} changes, limit {tolerance_frames} frames ({limit_ms:.1f} ms)")
    if bad:
        return Item("times", FAIL, f"{len(bad)} of {len(deltas)} scene changes are over the limit (changes "
                    f"{', '.join('#' + str(i) for i in bad)}): " + summary, rows, data)
    return Item("times", PASS, summary, rows, data)


def first_at_or_after(records: list[state_diff.Record], frame: int) -> Optional[state_diff.Record]:
    frames = [r.frame for r in records]
    i = bisect.bisect_left(frames, frame)
    return records[i] if i < len(records) else None


def check_checkpoints(pair: Pair) -> Item:
    k = pair.factor
    names = [o.name for o in pair.observables if o.compare and o.name != state_diff.FRAME_COUNTER]
    a, b = pair.states[BASE_FPS], pair.states[BASE_FPS * k]
    checkpoints = pair.meta.get("checkpoints", [])
    if not checkpoints:
        return Item("checkpoints", NA, "the session has no checkpoint", [], {})
    rows, bad = [], []
    for frame, scene in checkpoints:
        x, y = first_at_or_after(a, frame), first_at_or_after(b, frame * k)
        if x is None or y is None:
            bad.append(frame)
            rows.append(f"frame {frame} ({frame * k}): no record in the "
                        f"{'60 fps' if x is None else str(BASE_FPS * k) + ' fps'} run")
            continue
        problems = []
        for n in names:
            if n not in x.values or n not in y.values:
                problems.append(f"{n} is not in the trace")
            elif x.values[n] != y.values[n]:
                problems.append(f"{n} {_hex(x.values[n])} | {_hex(y.values[n])}")
        for label, r in (("60 fps", x), (f"{BASE_FPS * k} fps", y)):
            if r.values.get(state_diff.SCENE) != scene:
                problems.append(f"scene is {_hex(r.values.get(state_diff.SCENE))} at {label}, the checkpoint wants "
                                f"{_hex(scene)}")
        rows.append(f"frame {frame} ({frame * k}): " + ("equal " + ", ".join(names) if not problems else
                                                        "; ".join(problems)))
        if problems:
            bad.append(frame)
    data = {"observables": names, "frames": [f for f, _ in checkpoints], "failed": bad}
    if bad:
        return Item("checkpoints", FAIL, f"{len(bad)} of {len(checkpoints)} checkpoints differ (frames "
                    f"{', '.join(map(str, bad))})", rows, data)
    return Item("checkpoints", PASS, f"{len(checkpoints)} checkpoints: {', '.join(names)} equal", rows, data)


def _f32(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]


class Track:
    """The readable positions of one trace, with the length of the path between them."""

    def __init__(self, records: list[state_diff.Record]) -> None:
        self.frames = [r.frame for r in records]
        self.points: list[Optional[tuple[float, float, float]]] = []
        self.cumulative = [0.0]
        last: Optional[tuple[float, float, float]] = None
        for r in records:
            vals = [r.values.get(n) for n in POSITION]
            point = None if any(v is None for v in vals) else tuple(_f32(v) for v in vals)
            self.points.append(point)
            total = self.cumulative[-1]
            if point is not None and last is not None:
                total += math.dist(point, last)
            if point is not None:
                last = point
            else:
                last = None
            self.cumulative.append(total)

    def index(self, frame: int) -> int:
        return bisect.bisect_right(self.frames, frame) - 1

    def at(self, frame: int) -> Optional[tuple[float, float, float]]:
        i = self.index(frame)
        return self.points[i] if i >= 0 else None

    def travelled(self, start: int, end: int) -> float:
        """The length of the path over the records with frames in (start, end]."""
        i, j = self.index(start), self.index(end)
        return max(0.0, self.cumulative[j + 1] - self.cumulative[i + 1])


def check_observables(pair: Pair, relative: float, absolute: float, quest_seconds: float, units: float,
                      require: set[str], position_until: Optional[int], scene: Optional[int] = HUNT_SCENE,
                      slack_frames: int = TIME_TOLERANCE_FRAMES) -> Item:
    k = pair.factor
    a, b = pair.states[BASE_FPS], pair.states[BASE_FPS * k]
    if not a or not b:
        return Item("observables", NA, "a run has no state trace", [], {})
    for n in (*POSITION, QUEST_TIME):
        for label, recs in (("60 fps", a), (f"{BASE_FPS * k} fps", b)):
            if n not in recs[0].values:
                raise AuditError(f"the {label} state trace has no {n}= value (record it with the observable file "
                                 f"`run` wrote)")
    seconds = int(min(a[-1].frame / BASE_FPS, b[-1].frame / (BASE_FPS * k)))
    ta, tb = Track(a), Track(b)
    pos_rows: list[dict] = []
    quest_rows: list[dict] = []
    quest_values: tuple[set, set] = (set(), set())
    rows: list[str] = []
    for s in range(1, seconds + 1):
        fa, fb = s * BASE_FPS, s * BASE_FPS * k
        i, j = ta.index(fa), tb.index(fb)
        if i < 0 or j < 0:
            continue
        if scene is not None and (a[i].values.get(state_diff.SCENE) != scene or b[j].values.get(state_diff.SCENE) != scene):
            continue                       # the chains name whatever sits there outside the hunt (N3-21)
        pa, pb = ta.points[i], tb.points[j]
        if pa is not None and pb is not None:
            bad_value = [p for p in (pa, pb) if not all(math.isfinite(v) and abs(v) < POSITION_LIMIT for v in p)]
            travelled = ta.travelled(fa - BASE_FPS, fa)
            limit = relative * travelled + absolute
            dist = math.dist(pa, pb)
            # information, not the rule: the closest the 60 fps hunter comes to the 120 fps hunter within the scene
            # changes' time tolerance either side of the second (a jump that comes a frame earlier is one sample off)
            near = [math.dist(p, pb) for p in ta.points[max(0, ta.index(fa - slack_frames)):ta.index(fa + slack_frames) + 1]
                    if p is not None]
            slack = min([dist, *near])
            pos_rows.append({"second": s, "distance": round(dist, 4), "travelled": round(travelled, 4),
                             "limit": round(limit, 4), "ratio": round(dist / limit, 4),
                             "slack_distance": round(slack, 4), "slack_ratio": round(slack / limit, 4),
                             "over": bool(bad_value) or dist > limit})
        qa, qb = a[i].values.get(QUEST_TIME), b[j].values.get(QUEST_TIME)
        if qa is not None and qb is not None:
            quest_values[0].add(qa)
            quest_values[1].add(qb)
            diff = abs(qa - qb)
            quest_rows.append({"second": s, "units": diff, "seconds": round(diff / units, 4),
                               "over": diff / units > quest_seconds})
    parts, status = {}, []
    for label, table, over_text in (("position", pos_rows, "over D70's position tolerance"),
                                    (QUEST_TIME, quest_rows, "over the quest-clock tolerance")):
        over = [r for r in table if r["over"]]
        if label == QUEST_TIME and table and len(quest_values[0]) == 1 and len(quest_values[1]) == 1:
            # a counter that never moves is not a running clock: it compares nothing (D34), unless the two constants
            # are further apart than the tolerance, which is a difference like any other
            gap = abs(next(iter(quest_values[0])) - next(iter(quest_values[1])))
            text = (f"the counter is constant in both runs over the {len(table)} seconds compared, the two constants "
                    f"{gap} units = {gap / units:.4g} s apart (limit {quest_seconds} s)")
            if gap / units > quest_seconds:
                text += ": over the quest-clock tolerance"
                state = FAIL
            else:
                text += ": not a running clock"
                state = FAIL if QUEST_TIME in require else NA
                if state == FAIL:
                    text += " (required)"
        elif table:
            if label == "position":
                worst = max(table, key=lambda r: r["ratio"])
                text = (f"{len(table)} seconds compared (game time {table[0]['second']} s to {table[-1]['second']} s), "
                        f"{len(over)} {over_text}; worst {worst['distance']} units against a limit of {worst['limit']} "
                        f"at {worst['second']} s; largest distance {max(r['distance'] for r in table)} units; with "
                        f"{slack_frames} frames of time slack the largest is {max(r['slack_distance'] for r in table)} units "
                        f"and {sum(1 for r in table if r['slack_ratio'] > 1)} seconds are over")
            else:
                worst = max(table, key=lambda r: r["units"])
                text = (f"{len(table)} seconds compared (game time {table[0]['second']} s to {table[-1]['second']} s), "
                        f"{len(over)} {over_text}; largest difference {worst['units']} units = {worst['seconds']} s "
                        f"at {worst['second']} s (limit {quest_seconds} s)")
            if over:
                text += f"; first over at {over[0]['second']} s"
            state = FAIL if over else PASS
        else:
            text = "no second with a readable value in both runs"
            state = FAIL if label in require else NA
            if state == FAIL:
                text += " (required)"
        parts[label] = {"status": state, "summary": text, "rows": table}
        status.append(state)
        rows.append(f"{label}: {state}: {text}")
    if position_until is not None:
        early = [r for r in pos_rows if r["second"] * BASE_FPS <= position_until]
        if early:
            over = [r for r in early if r["over"]]
            worst = max(early, key=lambda r: r["ratio"])
            rows.append(f"position up to frame {position_until} at {BASE_FPS} fps: {len(early)} seconds, {len(over)} over; "
                        f"largest distance {max(r['distance'] for r in early)} units, worst ratio to the limit "
                        f"{worst['ratio']}")
            parts["position_until"] = {"frame": position_until, "seconds": len(early), "over": len(over),
                                       "max_distance": max(r["distance"] for r in early), "worst_ratio": worst["ratio"]}
        else:
            rows.append(f"position up to frame {position_until} at {BASE_FPS} fps: no second compared")
    overall = FAIL if FAIL in status else (NA if all(s == NA for s in status) else PASS)
    summary = "; ".join(f"{label} {parts[label]['status']}" for label in ("position", "quest_time"))
    return Item("observables", overall, summary, rows, parts)


def check_dsp(pair: Pair, max_shift: int, require: set[str]) -> Item:
    names = pair.meta.get("dsp", [])
    if not names:
        return Item("dsp", FAIL if "dsp" in require else NA,
                    "the session has no DSP capture (add one with `run --dsp START:FRAMES`)"
                    + (" (required)" if "dsp" in require else ""), [], {})
    k = pair.factor
    rows, bad, shifts = [], [], []
    for name in names:
        a, b = pair.path(BASE_FPS, name), pair.path(BASE_FPS * k, name)
        for path in (a, b):
            if not path.is_file():
                bad.append(name)
                rows.append(f"{name}: {path.parent.name}/{path.name} was not written")
                break
        else:
            try:
                result = capfmt.diff_dsp_aligned(str(a), str(b), max_shift)
            except capfmt.CapError as error:
                bad.append(name)
                rows.append(f"{name}: {error}")
                continue
            rows.append(f"{name}: {'equal after alignment' if result.equal else 'DIFFERENT'}: {result.summary}")
            rows.extend("    " + p for p in result.problems)
            start = pair.meta.get("dsp_starts", {}).get(name)
            if start is not None:
                x, y = first_at_or_after(pair.states[BASE_FPS], start), first_at_or_after(pair.states[BASE_FPS * k],
                                                                                           start * k)
                if x is not None and y is not None:
                    offset = (y.ticks - x.ticks) / TICKS_PER_SECOND
                    measured = ("no shift was measured" if result.shift is None
                                else f"the measured shift is {result.shift:+d} DSP frames")
                    rows.append(f"    game clock offset at the window's start (game frame {start}, {start * k}): "
                                f"{offset:+.3f} s = {offset * TICKS_PER_SECOND / capfmt.DSP_FRAME_TICKS:+.0f} DSP frames, "
                                f"as information ({measured})")
            shifts.append(result.shift)
            if not result.equal:
                bad.append(name)
    data = {"captures": names, "shifts": shifts, "max_shift": max_shift}
    if bad:
        return Item("dsp", FAIL, f"{len(bad)} of {len(names)} DSP captures differ", rows, data)
    return Item("dsp", PASS, f"{len(names)} DSP captures equal after alignment, shift(s) "
                f"{', '.join(f'{s:+d}' for s in shifts if s is not None)} DSP frames (at most {max_shift})", rows, data)


def fs_writes(by_thread: dict[str, list]) -> dict[str, list[tuple]]:
    """Per thread, in order: (offset, size, result, CRC-32 token of the data) of the file Writes of fs:USER."""
    out: dict[str, list[tuple]] = {}
    for thread, calls in by_thread.items():
        for call in calls:
            if tracefmt.fs_file_command(call) != "write":
                continue
            request = tracefmt.fs_file_request(call)
            if request is None:
                out.setdefault(thread, []).append(("malformed", call.cmd))
                continue
            crc = next((d.token for d in call.desc if d.kind in tracefmt.IPC_READ_BUFFERS and d.size == request[1]),
                       tracefmt.IPC_NO_NAME)
            out.setdefault(thread, []).append((request[0], request[1], call.result, crc))
    return out


def check_writes(pair: Pair, masks_file: Optional[str], require: set[str]) -> Item:
    k = pair.factor
    paths = [pair.path(r, "ipc.txt") for r in (BASE_FPS, BASE_FPS * k)]
    for p in paths:
        if not p.is_file():
            return Item("writes", FAIL, f"{p.parent.name}/{p.name} was not written", [], {})
    session = base_name(pair.session)
    try:
        masks = []
        if masks_file:
            masks = compare_ipc_traces.load_write_masks(masks_file, session)
        used = [compare_ipc_traces.MaskUse(), compare_ipc_traces.MaskUse()]
        runs = [compare_ipc_traces.load(str(p), set(), masks, u) for p, u in zip(paths, used)]
        for m in masks:
            if not used[0].lines[m.number] and not used[1].lines[m.number]:
                raise compare_ipc_traces.MaskError(
                    f"the mask of session {m.session} (Write of {m.write_size} bytes at offset {m.write_offset}) "
                    f"matches no Write of either run")
    except (compare_ipc_traces.TraceError, compare_ipc_traces.MaskError) as error:
        return Item("writes", FAIL, str(error), [], {})
    a, b = fs_writes(runs[0]), fs_writes(runs[1])
    count_a, count_b = sum(map(len, a.values())), sum(map(len, b.values()))
    rows = [f"thread {t}: {len(a.get(t, []))} Writes at {BASE_FPS} fps, {len(b.get(t, []))} at {BASE_FPS * k} fps"
            for t in sorted(set(a) | set(b), key=compare_ipc_traces._thread_order)]
    rows.append(f"write masks: {len(masks)} line(s) of session {session}, applied to {used[0].writes} and "
                f"{used[1].writes} Write(s)")
    data = {"writes": [count_a, count_b], "masks": len(masks), "masked": [used[0].writes, used[1].writes]}
    for t in sorted(set(a) | set(b), key=compare_ipc_traces._thread_order):
        x, y = a.get(t, []), b.get(t, [])
        n = min(len(x), len(y))
        first = next((i for i in range(n) if x[i] != y[i]), None)
        if first is None and len(x) != len(y):
            first = n
        if first is not None:
            lx = x[first] if first < len(x) else None
            ly = y[first] if first < len(y) else None
            rows.append(f"thread {t}: Write #{first} differs: {lx} | {ly}")
            return Item("writes", FAIL, f"the Writes of thread {t} differ at Write #{first} ({len(x)} against "
                        f"{len(y)} Writes)", rows, data)
    if count_a == 0 and count_b == 0:
        return Item("writes", FAIL if "writes" in require else NA,
                    "no fs:USER file Write in either run" + (" (required)" if "writes" in require else ""), rows, data)
    return Item("writes", PASS, f"{count_a} Writes on {len(a)} thread(s), equal in order and in full "
                f"({used[0].writes} masked)", rows, data)


# -- compare ------------------------------------------------------------------------------------------------------

def compare_pair(pair: Pair, *, time_frames: int = TIME_TOLERANCE_FRAMES, position_relative: float = POSITION_RELATIVE,
                 position_absolute: float = POSITION_ABSOLUTE, quest_seconds: float = QUEST_SECONDS,
                 quest_units: float = QUEST_UNITS_PER_SECOND, max_shift: int = MAX_SHIFT,
                 masks_file: Optional[str] = None, require: Sequence[str] = (),
                 position_until: Optional[int] = None, hunt_scene: Optional[int] = HUNT_SCENE) -> list[Item]:
    load_states(pair)
    need = set(require)
    runs = check_runs(pair)
    if not pair.states[BASE_FPS] or not pair.states[BASE_FPS * pair.factor]:
        return [runs]
    scenes = check_scenes(pair)
    items = [runs, scenes, check_times(pair, scenes, time_frames), check_checkpoints(pair),
             check_observables(pair, position_relative, position_absolute, quest_seconds, quest_units, need,
                               position_until, hunt_scene, time_frames),
             check_dsp(pair, max_shift, need), check_writes(pair, masks_file, need)]
    return items


def run_mode(meta: dict) -> str:
    """`deterministic` or `real-time`: the meta file's `mode`, or, for a meta file written before it had one, whether
    the runner was given `--deterministic`."""
    mode = meta.get("mode")
    if mode in ("deterministic", "real-time"):
        return mode
    return "deterministic" if "--deterministic" in meta.get("runner_args", []) else "real-time"


def run_seed(meta: dict) -> Optional[str]:
    if meta.get("seed") is not None:
        return str(meta["seed"])
    args = meta.get("runner_args", [])
    return args[args.index("--seed") + 1] if "--seed" in args[:-1] else None


def format_report(pair: Pair, items: list[Item], verbose: bool = True) -> list[str]:
    k = pair.factor
    seed = run_seed(pair.meta)
    lines = [f"fps_audit: {pair.session}, {BASE_FPS} fps against {BASE_FPS * k} fps (x{k}), "
             f"--io-latency {pair.meta.get('io_latency', '?')}, {run_mode(pair.meta)} mode"
             + (f" (--seed {seed})" if seed is not None else "")]
    for it in items:
        lines.append(f"{it.name:<12} {it.status.upper() if it.status == FAIL else it.status:<5} {it.summary}")
        if verbose:
            lines.extend("    " + d for d in it.details)
    failed = [it.name for it in items if it.status == FAIL]
    na = [it.name for it in items if it.status == NA]
    lines.append("verdict: " + ("FAIL (" + ", ".join(failed) + ")" if failed else "PASS")
                 + (f"; not applicable: {', '.join(na)}" if na else ""))
    return lines


def cmd_compare(args: argparse.Namespace) -> int:
    pair = read_pair(Path(args.dir))
    require = [r for r in (x.strip() for x in args.require.split(",")) if r]
    for r in require:
        if r not in REQUIRABLE:
            raise AuditError(f"--require {r!r}: want one of {', '.join(REQUIRABLE)}")
    try:
        hunt_scene = None if args.hunt_scene.lower() == "any" else int(args.hunt_scene, 0)
    except ValueError:
        raise AuditError(f"--hunt-scene {args.hunt_scene!r}: want a scene value (0x...) or `any`") from None
    masks = args.fs_write_mask
    if masks is None:
        masks = str(DEFAULT_WRITE_MASKS)
    elif masks.lower() == "none":
        masks = None
    items = compare_pair(pair, time_frames=args.time_frames, position_relative=args.position_relative,
                         position_absolute=args.position_absolute, quest_seconds=args.quest_seconds,
                         quest_units=args.quest_units, max_shift=args.max_shift, masks_file=masks, require=require,
                         position_until=args.position_until, hunt_scene=hunt_scene)
    lines = format_report(pair, items, verbose=not args.brief)
    print("\n".join(lines))
    root = Path(args.dir)
    (root / "compare.txt").write_text("\n".join(format_report(pair, items, True)) + "\n", encoding="utf-8", newline="\n")
    (root / "compare.json").write_text(json.dumps({
        "session": pair.session, "factor": pair.factor, "io_latency": pair.meta.get("io_latency"),
        "mode": run_mode(pair.meta), "seed": run_seed(pair.meta),
        "tolerances": {"time_frames": args.time_frames, "position_relative": args.position_relative,
                       "position_absolute": args.position_absolute, "quest_seconds": args.quest_seconds,
                       "quest_units_per_second": args.quest_units, "max_shift": args.max_shift},
        "items": [{"name": i.name, "status": i.status, "summary": i.summary, "data": i.data} for i in items],
    }, indent=1) + "\n", encoding="utf-8", newline="\n")
    return 1 if any(i.status == FAIL for i in items) else 0


# -- scale and the command line -----------------------------------------------------------------------------------

def cmd_scale(args: argparse.Namespace) -> int:
    namespace = argparse.Namespace(session=args.session, factor=args.factor, output=args.output)
    try:
        return observable_probe.cmd_scale(namespace)
    except observable_probe.UsageError as error:
        raise AuditError(str(error)) from None


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description="The 60/120 fps audit of the native runtime (N3-22).")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("scale", help="a session with every frame count multiplied (observable_probe.py scale)")
    p.add_argument("session")
    p.add_argument("--factor", type=int, default=DEFAULT_FACTOR)
    p.add_argument("-o", "--output", required=True)

    p = sub.add_parser("run", help="play a session at 60 fps and, scaled, at 120 fps on the native runner")
    p.add_argument("session", help="a session name of tools/native/golden/ or a session file")
    p.add_argument("--runner", required=True, help="mh4u_native_runner")
    p.add_argument("--program", required=True, help="the game folder")
    p.add_argument("--code", required=True, help="the native code library")
    p.add_argument("--user-before", required=True, help="the recording's user-before folder (copied for each run)")
    p.add_argument("--io-latency", required=True, choices=["azahar", "none"])
    p.add_argument("--out", required=True)
    p.add_argument("--factor", type=int, default=DEFAULT_FACTOR)
    p.add_argument("--observable", help=f"the base observable file (default {DEFAULT_OBSERVABLE.name} of the repository)")
    p.add_argument("--dsp", action="append", default=[], metavar="START:FRAMES",
                   help="a DSP capture besides the session's (START in game frames at 60 fps); repeatable")
    p.add_argument("--deterministic", action="store_true",
                   help="the runner's deterministic mode (give it the _native_det library); recorded in meta.json and "
                        "printed in compare's first line")
    p.add_argument("--seed", type=int, help="the deterministic mode's seed (needs --deterministic)")
    p.add_argument("--runner-arg", action="append", default=[], metavar="ARG",
                   help="an extra option for the runner, as it is (`{out}` is the run's folder), for what the audit "
                        "does not name itself (diagnostics such as --capture-hid); repeatable")
    p.add_argument("--path", action="append", default=[], metavar="DIR", help="a folder put first on the runner's PATH")
    p.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                   help=f"seconds per run (default {DEFAULT_TIMEOUT:.0f}: about 8 times the slowest real-time run of "
                        f"N3-22, G3 at 120 fps on a busy PC)")
    p.add_argument("--lock", metavar="DIR", help="the emulator's lock folder: waited for, created, removed at the end")
    p.add_argument("--lock-poll", type=float, default=30.0, help=argparse.SUPPRESS)
    p.add_argument("--force", action="store_true", help="delete OUT first when it holds runs")

    p = sub.add_parser("compare", help="the six items for two runs made by `run`")
    p.add_argument("dir")
    p.add_argument("--require", default="", metavar="ITEMS",
                   help=f"items that must have something to compare (a n/a becomes a failure): {', '.join(REQUIRABLE)}")
    p.add_argument("--position-until", type=int, metavar="FRAME",
                   help="also report the position up to this frame at 60 fps (the part before the first attack)")
    p.add_argument("--hunt-scene", default=hex(HUNT_SCENE), metavar="VALUE|any",
                   help="the scene the position and the quest clock are compared in (default aQuest, the hunt: the "
                        "chains are valid there only, N3-21); `any` compares them in every scene")
    p.add_argument("--time-frames", type=int, default=TIME_TOLERANCE_FRAMES, help="D70's first guess: 4")
    p.add_argument("--position-relative", type=float, default=POSITION_RELATIVE, help="D70's first guess: 0.02")
    p.add_argument("--position-absolute", type=float, default=POSITION_ABSOLUTE, help="D70's first guess: 0.05")
    p.add_argument("--quest-seconds", type=float, default=QUEST_SECONDS, help="D70's first guess: 1")
    p.add_argument("--quest-units", type=float, default=QUEST_UNITS_PER_SECOND,
                   help="units of the quest counter per second of game time (N3-21: 60)")
    p.add_argument("--max-shift", type=int, default=MAX_SHIFT, help="DSP frames the 120 fps capture may be shifted by")
    p.add_argument("--fs-write-mask", metavar="FILE|none", help="default: config/native/fs_write_masks.txt")
    p.add_argument("--brief", action="store_true", help="only the item lines on the terminal")
    return ap


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return {"scale": cmd_scale, "run": cmd_run, "compare": cmd_compare}[args.cmd](args)
    except AuditError as error:
        print(f"fps_audit: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
