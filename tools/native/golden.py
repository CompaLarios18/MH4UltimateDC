#!/usr/bin/env python3
"""Golden sessions: record them with the reference runner, check a candidate runtime against them.

    python3 tools/native/golden.py list [--out DIR]
    python3 tools/native/golden.py record S --runner RUN --game CXI --update CIA --user DIR
                                      [--code LIB] [--cpu jit|recomp] [--save DIR]
    python3 tools/native/golden.py check S --runner RUN --game CXI --update CIA --user DIR [--code LIB]
                                      [--candidate-runner RUN2] [--candidate-program PATH]
                                      [--candidate-code LIB] [--candidate-args "..."] [--repeat N]

A session is a frame-keyed input script (tools/native/golden/<S>.txt, the runner's --input-file
format) with header comment lines, see parse_session.  `record` runs the reference runner on a
copy of the user folder with every recorder on and writes build/golden/<S>/ (docs/native/golden.md
lists the files); `check` runs again on a copy of user-before/ and compares.  Standard library only;
no shell, no bash: the runner and the code library (.dll or .so) come from the command line, so it
works the same on Windows and Linux.  The recordings are game data: they stay in build/.
"""
from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Sequence

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import capfmt  # noqa: E402
import gcap  # noqa: E402
import state_diff  # noqa: E402

SESSIONS_DIR = HERE / "golden"
DEFAULT_OUT = ROOT / "build" / "golden"
OBSERVABLE = ROOT / "config" / "native" / "observable.txt"
IPC_TIME_DEPENDENT = ROOT / "config" / "native" / "ipc_time_dependent.txt"
TZ = "UTC0"                      # the time zone the runner is started with (see docs/native/golden.md)
MANIFEST_FORMAT = 1
MAX_GPU_FRAMES = 30
BUTTONS = {"A", "B", "X", "Y", "UP", "DOWN", "LEFT", "RIGHT", "L", "R", "START", "SELECT", "ZL", "ZR", "HOME"}
# Folders of a recording that the manifest's file inventory leaves out.
NOT_RECORDED_DIRS = {"user-before", "user-after", "work", "check"}


class GoldenError(Exception):
    """A usage or environment problem (exit status 2)."""


# --------------------------------------------------------------------------
# Session files

@dataclasses.dataclass
class Capture:
    kind: str                    # "gpu" or "dsp"
    start: int
    frames: int

    @property
    def filename(self) -> str:
        return f"{self.kind}-{self.start}.{'mgpu' if self.kind == 'gpu' else 'mdsp'}"


@dataclasses.dataclass
class Session:
    name: str
    needs: str
    frames: int
    checkpoints: list[tuple[int, int]]       # (frame, scene value)
    captures: list[Capture]
    draft: bool
    path: Path
    events: int                              # number of input events

    def sha256(self) -> str:
        return sha256_file(self.path)


_TIME = re.compile(r"^(?:f\d+|\d+)$")


def _frame_of(token: str) -> int | None:
    return int(token[1:]) if token.startswith("f") else None


def _check_event(item: str, frames: int, where: str) -> None:
    fields = item.split(":")
    if len(fields) < 2 or not _TIME.match(fields[0]):
        raise GoldenError(f"{where}: bad event {item!r}")
    frame = _frame_of(fields[0])
    if frame is None:
        raise GoldenError(f"{where}: event times must be frames (f<N>), got {fields[0]!r}")
    if frames and frame > frames:
        raise GoldenError(f"{where}: event at frame {frame} is after the end (frames: {frames})")
    kind, rest = fields[1].upper(), fields[2:]
    if kind in BUTTONS:
        arity = (0, 1)
    elif kind == "TOUCH" or kind == "CIRCLE":
        arity = (2, 3)
    else:
        raise GoldenError(f"{where}: unknown button {fields[1]!r}")
    if not arity[0] <= len(rest) <= arity[1]:
        raise GoldenError(f"{where}: wrong number of fields in {item!r}")
    if kind in BUTTONS and rest and not _TIME.match(rest[0]):
        raise GoldenError(f"{where}: bad hold {rest[0]!r}")
    if kind == "TOUCH" and not all(re.match(r"^\d+$", x) for x in rest[:2]):
        raise GoldenError(f"{where}: touch coordinates are pixels, got {item!r}")
    if kind == "CIRCLE":
        for x in rest[:2]:
            try:
                float(x)
            except ValueError:
                raise GoldenError(f"{where}: bad circle pad value {x!r}") from None
    if kind in ("TOUCH", "CIRCLE") and len(rest) == 3 and not _TIME.match(rest[2]):
        raise GoldenError(f"{where}: bad hold {rest[2]!r}")


def parse_session(path: str | Path) -> Session:
    """Read a session file.

    Header lines (`# key: value`): name, needs (none|save), frames (total), checkpoints
    (`<frame>:<scene>, ...`, the scene being the observable `scene` value), captures
    (`gpu:<start>:<frames>, dsp:<start>:<frames>, ...`) and optionally `draft: yes` (the numbers
    are placeholders; `record` refuses it).  Every other line is an input event line for the
    runner's --input-file (comma separated `f<frame>:BUTTON[:hold]`, `f<frame>:TOUCH:x:y[:hold]`,
    `f<frame>:CIRCLE:x:y[:hold]`).
    """
    path = Path(path)
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as e:
        raise GoldenError(f"cannot read session file {path}: {e}") from None
    headers: dict[str, str] = {}
    events: list[tuple[int, str]] = []
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#"):
            m = re.match(r"^#\s*(name|needs|frames|checkpoints|captures|draft)\s*:\s*(.*)$", line)
            if m:
                if m.group(1) in headers:
                    raise GoldenError(f"{path.name}:{number}: header {m.group(1)!r} given twice")
                headers[m.group(1)] = m.group(2).strip()
            continue
        line = line.split("#", 1)[0].strip()
        events += [(number, item.strip()) for item in line.split(",") if item.strip()]
    where = path.name
    for key in ("name", "needs", "frames", "checkpoints", "captures"):
        if key not in headers:
            raise GoldenError(f"{where}: missing header '# {key}:'")
    name = headers["name"]
    if not re.match(r"^[A-Za-z0-9_-]+$", name):
        raise GoldenError(f"{where}: bad session name {name!r}")
    if name != path.stem:
        raise GoldenError(f"{where}: header name {name!r} differs from the file name")
    if headers["needs"] not in ("none", "save"):
        raise GoldenError(f"{where}: needs must be 'none' or 'save'")
    if not re.match(r"^\d+$", headers["frames"]) or int(headers["frames"]) <= 0:
        raise GoldenError(f"{where}: frames must be a positive number")
    frames = int(headers["frames"])
    draft = headers.get("draft", "no").lower() in ("yes", "true", "1")
    checkpoints: list[tuple[int, int]] = []
    for item in [x.strip() for x in headers["checkpoints"].split(",") if x.strip()]:
        m = re.match(r"^(\d+):(0x[0-9a-fA-F]+|\d+)$", item)
        if not m:
            raise GoldenError(f"{where}: bad checkpoint {item!r} (want <frame>:<scene value>)")
        checkpoints.append((int(m.group(1)), int(m.group(2), 0)))
    if [c[0] for c in checkpoints] != sorted(c[0] for c in checkpoints):
        raise GoldenError(f"{where}: checkpoints must be in increasing frame order")
    if any(f > frames for f, _ in checkpoints):
        raise GoldenError(f"{where}: a checkpoint is after the last frame ({frames})")
    if not checkpoints and not draft:
        raise GoldenError(f"{where}: no checkpoints (only a draft may have none)")
    captures: list[Capture] = []
    for item in [x.strip() for x in headers["captures"].split(",") if x.strip()]:
        m = re.match(r"^(gpu|dsp):(\d+):(\d+)$", item)
        if not m:
            raise GoldenError(f"{where}: bad capture {item!r} (want gpu|dsp:<start>:<frames>)")
        cap = Capture(m.group(1), int(m.group(2)), int(m.group(3)))
        if cap.frames < 1 or (cap.kind == "gpu" and cap.frames > MAX_GPU_FRAMES):
            raise GoldenError(f"{where}: {item!r}: frames must be 1..{MAX_GPU_FRAMES if cap.kind == 'gpu' else 'N'}")
        if cap.start > frames:
            raise GoldenError(f"{where}: {item!r} starts after the last frame")
        if any(c.filename == cap.filename for c in captures):
            raise GoldenError(f"{where}: capture {item!r} given twice")
        captures.append(cap)
    for number, item in events:
        _check_event(item, frames, f"{where}:{number}")
    return Session(name, headers["needs"], frames, checkpoints, captures, draft, path, len(events))


def session_path(name: str, sessions_dir: Path = SESSIONS_DIR) -> Path:
    if not re.match(r"^[A-Za-z0-9_-]+$", name):
        raise GoldenError(f"bad session name {name!r}")
    p = sessions_dir / f"{name}.txt"
    if not p.is_file():
        raise GoldenError(f"no session file {p}")
    return p


# --------------------------------------------------------------------------
# Files, manifests

def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def inventory(folder: Path) -> dict[str, dict]:
    """Every file under `folder` (not the user folders, the check runs or the manifest itself)."""
    files: dict[str, dict] = {}
    for dirpath, dirnames, filenames in os.walk(folder):
        here = Path(dirpath)
        if here == folder:
            dirnames[:] = sorted(d for d in dirnames if d not in NOT_RECORDED_DIRS)
        else:
            dirnames.sort()
        for fn in sorted(filenames):
            p = here / fn
            rel = p.relative_to(folder).as_posix()
            if rel == "manifest.json":
                continue
            files[rel] = {"size": p.stat().st_size, "sha256": sha256_file(p)}
    return files


def evaluate_checkpoints(state_path: Path, checkpoints: Sequence[tuple[int, int]]) -> list[dict]:
    """For each (frame, scene): the scene in the state trace at the first record at or after the frame."""
    results = []
    try:
        records = state_diff.parse_trace(state_path.read_text(encoding="utf-8"))
    except (OSError, state_diff.FormatError) as e:
        return [{"frame": f, "scene": s, "actual": None, "result": f"unreadable state trace: {e}"}
                for f, s in checkpoints]
    for frame, scene in checkpoints:
        rec = next((r for r in records if r.frame >= frame), None)
        actual = None if rec is None else rec.values.get("scene")
        if rec is None:
            result = "missing"
        elif actual == scene:
            result = "ok"
        else:
            result = "mismatch"
        results.append({"frame": frame, "scene": scene, "actual": actual, "result": result})
    return results


# --------------------------------------------------------------------------
# Running the runner

def runner_argv(runner: str | Path) -> list[str]:
    """The command that starts a runner: the file itself, or the Python interpreter for a .py script."""
    p = Path(runner)
    if not p.is_file():
        raise GoldenError(f"runner not found: {runner}")
    if p.suffix.lower() == ".py":
        return [sys.executable, str(p)]
    return [str(p)]


def child_env() -> dict[str, str]:
    env = dict(os.environ)
    env["TZ"] = TZ
    return env


@dataclasses.dataclass
class RunSpec:
    runner: str
    program: str
    code: str | None
    cpu: str | None            # None: not passed (candidate runners)
    updates: list[str]
    user: Path
    outdir: Path
    session: Session
    sets: list[str]
    extra: list[str]
    full: bool                 # reference options (new-3ds, install, unmapped, gpu, shots) or the shared ones
    shot_every: int = 1000
    gpu: bool = True


def build_args(spec: RunSpec) -> list[str]:
    """The runner's arguments (without the runner itself), file names as in docs/native/golden.md."""
    o = spec.outdir
    s = spec.session
    args = [spec.program]
    if spec.full:
        args += ["--new-3ds", "off"]
        if spec.cpu:
            args += ["--cpu", spec.cpu]
    if spec.code:
        args += ["--code", spec.code]
    if spec.full:
        for u in spec.updates:
            args += ["--install", u]
    args += ["--user", str(spec.user), "--frames", str(s.frames), "--input-file", str(s.path)]
    for item in spec.sets:
        args += ["--set", item]
    args += ["--trace-svc", str(o / "svc.txt"), "--trace-ipc", str(o / "ipc.txt"), "--trace-ipc-full",
             "--state-trace", str(o / "state.txt"), "--capture-hid", str(o / "hid.mhid")]
    for cap in s.captures:
        if cap.kind == "dsp":
            args += ["--capture-dsp", f"{cap.start}:{cap.frames}:{o / cap.filename}"]
        elif cap.kind == "gpu" and spec.full and spec.gpu:
            args += ["--capture-gpu", f"{cap.start}:{cap.frames}:{o / cap.filename}"]
    if spec.full:
        args += ["--count-unmapped", str(o / "unmapped.txt")]
        if spec.shot_every > 0:
            args += ["--shots", str(o / "shots"), "--shot-every", str(spec.shot_every)]
    return args + spec.extra


def run_runner(spec: RunSpec, timeout: float | None) -> tuple[int, list[str]]:
    """Start the runner (no shell), log to <outdir>/runner.log; returns (exit status, arguments)."""
    spec.outdir.mkdir(parents=True, exist_ok=True)
    (spec.outdir / "shots").mkdir(exist_ok=True)
    args = build_args(spec)
    cmd = runner_argv(spec.runner) + args
    with open(spec.outdir / "runner.log", "wb") as log:
        try:
            done = subprocess.run(cmd, cwd=ROOT, env=child_env(), stdout=log, stderr=subprocess.STDOUT,
                                  timeout=timeout, check=False)
            status = done.returncode
        except subprocess.TimeoutExpired:
            log.write(b"\n[golden.py] runner timed out\n")
            status = 124
        except OSError as e:
            raise GoldenError(f"cannot start the runner {spec.runner}: {e}") from None
    return status, args


def copy_tree(src: Path, dst: Path) -> None:
    if not src.is_dir():
        raise GoldenError(f"not a folder: {src}")
    shutil.copytree(src, dst, dirs_exist_ok=True)


def validate_captures(outdir: Path, session: Session) -> list[str]:
    """Structural problems of the binary captures a run wrote (empty list: fine)."""
    problems: list[str] = []
    paths = [(outdir / "hid.mhid", "hid")] + [(outdir / c.filename, c.kind) for c in session.captures]
    for path, kind in paths:
        if not path.is_file():
            problems.append(f"{path.name}: not written")
            continue
        try:
            if kind == "gpu":
                problems += [f"{path.name}: {p}" for p in gcap.check_file(str(path))]
            else:
                for rec in capfmt.read(str(path)):
                    capfmt.check_record(kind, rec)
        except (capfmt.CapError, gcap.GcapError, OSError) as e:
            problems.append(f"{path.name}: {e}")
    return problems


# --------------------------------------------------------------------------
# record

def record(session: Session, a: argparse.Namespace) -> int:
    if session.draft and not a.allow_draft:
        raise GoldenError(f"{session.name} is a draft (placeholder numbers, no real checkpoints); "
                          f"finish it first, or pass --allow-draft to try it")
    if session.needs == "save" and not a.save:
        raise GoldenError(f"{session.name} needs a save: pass --save DIR")
    cpu = a.cpu or "recomp"
    if cpu == "recomp" and not a.code:
        raise GoldenError("--cpu recomp needs --code LIB")
    out = Path(a.out) / session.name
    if out.exists() and any(out.iterdir()):
        if not a.force:
            raise GoldenError(f"{out} is not empty (use --force to record again)")
        shutil.rmtree(out)
    out.mkdir(parents=True, exist_ok=True)
    before, work = out / "user-before", out / "work"
    copy_tree(Path(a.user), before)
    if a.save:
        copy_tree(Path(a.save), before)
    copy_tree(before, work)
    spec = RunSpec(a.runner, a.game, a.code, cpu, a.update or [], work, out, session, a.set or [],
                   shlex.split(a.runner_args or ""), True, a.shot_every)
    started = time.time()
    status, args = run_runner(spec, a.timeout)
    seconds = round(time.time() - started, 1)
    after = out / "user-after"
    if after.exists():
        shutil.rmtree(after)
    work.rename(after)
    checkpoints = evaluate_checkpoints(out / "state.txt", session.checkpoints) if (out / "state.txt").is_file() else [
        {"frame": f, "scene": s, "actual": None, "result": "missing"} for f, s in session.checkpoints]
    problems = validate_captures(out, session)
    manifest = {
        "format": MANIFEST_FORMAT,
        "tool": "golden.py",
        "session": {"name": session.name, "needs": session.needs, "frames": session.frames,
                    "checkpoints": [list(c) for c in session.checkpoints],
                    "captures": [dataclasses.asdict(c) for c in session.captures],
                    "events": session.events, "sha256": session.sha256()},
        "runner": {"path": str(a.runner), "sha256": sha256_file(Path(a.runner)), "cpu": cpu,
                   "new_3ds": "off", "exit": status, "seconds": seconds},
        "code_library": {"path": a.code, "sha256": sha256_file(Path(a.code))} if a.code else None,
        "game": {"name": Path(a.game).name, "size": Path(a.game).stat().st_size},
        "updates": [{"name": Path(u).name, "size": Path(u).stat().st_size} for u in (a.update or [])],
        "save": Path(a.save).name if a.save else None,
        "tz": TZ,
        "host": platform.system(),
        "observable_sha256": sha256_file(OBSERVABLE) if OBSERVABLE.is_file() else None,
        "args": [Path(args[0]).name] + args[1:],
        "checkpoint_results": checkpoints,
        "capture_problems": problems,
        "files": inventory(out),
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    ok = status == 0 and not problems and all(c["result"] == "ok" for c in checkpoints)
    print(f"{session.name}: runner exit {status}, {len(manifest['files'])} files, "
          f"checkpoints {sum(c['result'] == 'ok' for c in checkpoints)}/{len(checkpoints)} ok, "
          f"{len(problems)} capture problems -> {out}")
    for p in problems:
        print(f"  capture: {p}")
    for c in checkpoints:
        if c["result"] != "ok":
            print(f"  checkpoint {c['frame']}:{c['scene']:#x} {c['result']} (scene {c['actual']})")
    return 0 if ok else 1


# --------------------------------------------------------------------------
# check

@dataclasses.dataclass
class Result:
    name: str
    status: str          # pass | fail | skipped
    detail: str = ""


def _tool(script: Path, *args: str) -> tuple[int, str]:
    done = subprocess.run([sys.executable, str(script), *args], cwd=ROOT, capture_output=True, text=True,
                          check=False)
    return done.returncode, (done.stdout + done.stderr).strip()


def _first_lines(text: str, n: int = 8) -> str:
    return "\n".join(text.splitlines()[:n])


def _strip_header(path: Path) -> list[str]:
    return [ln for ln in path.read_text(encoding="utf-8").splitlines() if not ln.startswith("#")]


def compare_outputs(session: Session, ref: Path, cand: Path, candidate: bool,
                    ignore: Sequence[Path], observable: Path) -> list[Result]:
    """Compare a run's outputs with the recording; a comparison whose candidate file is missing is
    skipped for a candidate runner and a failure for the reference runner."""
    results: list[Result] = []

    def need(name: str, rel: str) -> bool:
        if not (ref / rel).exists():
            results.append(Result(name, "fail", f"the recording has no {rel}"))
            return False
        if not (cand / rel).exists():
            results.append(Result(name, "skipped" if candidate else "fail", f"the run wrote no {rel}"))
            return False
        return True

    tools = ROOT / "tools"
    if need("svc", "svc.txt"):
        code, text = _tool(tools / "compare_svc_traces.py", "--normalize", "--outputs",
                           str(ref / "svc.txt"), str(cand / "svc.txt"))
        results.append(Result("svc", "pass" if code == 0 else "fail", _first_lines(text)))
    if need("ipc", "ipc.txt"):
        extra: list[str] = []
        for f in ignore:
            extra += ["--ignore", str(f)]
        code, text = _tool(tools / "compare_ipc_traces.py", *extra, str(ref / "ipc.txt"), str(cand / "ipc.txt"))
        results.append(Result("ipc", "pass" if code == 0 else "fail", _first_lines(text)))
    if need("state", "state.txt"):
        code, text = _tool(HERE / "state_diff.py", "--observable", str(observable),
                           str(ref / "state.txt"), str(cand / "state.txt"))
        results.append(Result("state", "pass" if code == 0 else "fail", _first_lines(text)))
        bad = [c for c in evaluate_checkpoints(cand / "state.txt", session.checkpoints) if c["result"] != "ok"]
        results.append(Result("checkpoints", "fail" if bad else "pass",
                              "; ".join(f"{c['frame']}:{c['scene']:#x} {c['result']}" for c in bad[:5])))
    if need("hid", "hid.mhid"):
        try:
            lines = capfmt.diff_hid(str(ref / "hid.mhid"), str(cand / "hid.mhid"))
        except (capfmt.CapError, OSError) as e:
            lines = [str(e)]
        results.append(Result("hid", "fail" if lines else "pass", "\n".join(lines[:8])))
    for cap in session.captures:
        if cap.kind == "dsp" and need(f"dsp-{cap.start}", cap.filename):
            try:
                lines = capfmt.diff_dsp(str(ref / cap.filename), str(cand / cap.filename))
            except (capfmt.CapError, OSError) as e:
                lines = [str(e)]
            results.append(Result(f"dsp-{cap.start}", "fail" if lines else "pass", "\n".join(lines[:8])))
    # Image hashes of the frame-keyed screenshots.
    shots = sorted(p.relative_to(ref).as_posix() for p in (ref / "shots").rglob("*") if p.is_file()) \
        if (ref / "shots").is_dir() else []
    cand_shots = [p for p in (cand / "shots").rglob("*") if p.is_file()] if (cand / "shots").is_dir() else []
    if not shots:
        results.append(Result("images", "fail", "the recording has no screenshots"))
    elif not cand_shots:
        results.append(Result("images", "skipped" if candidate else "fail", "the run wrote no screenshots"))
    else:
        differ = [rel for rel in shots if not (cand / rel).is_file() or sha256_file(cand / rel) != sha256_file(ref / rel)]
        results.append(Result("images", "fail" if differ else "pass",
                              f"{len(differ)} of {len(shots)} differ: " + ", ".join(differ[:5]) if differ else
                              f"{len(shots)} images equal"))
    return results


def check(session: Session, a: argparse.Namespace) -> int:
    ref = Path(a.out) / session.name
    manifest_path = ref / "manifest.json"
    if not manifest_path.is_file():
        raise GoldenError(f"{session.name} is not recorded ({manifest_path} missing)")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    broken = [rel for rel, meta in manifest["files"].items()
              if not (ref / rel).is_file() or sha256_file(ref / rel) != meta["sha256"]]
    if broken:
        raise GoldenError(f"the recording was changed since it was made: {', '.join(broken[:5])}")
    candidate = any([a.candidate_runner, a.candidate_program, a.candidate_code, a.candidate_args])
    runner = (a.candidate_runner or a.runner) if candidate else a.runner
    program = (a.candidate_program or a.game) if candidate else a.game
    code = (a.candidate_code or (a.code if not a.candidate_runner else None)) if candidate else a.code
    cpu = None if candidate else (a.cpu or manifest["runner"]["cpu"])
    if not candidate and cpu == "recomp" and not code:
        raise GoldenError("--cpu recomp needs --code LIB")
    if a.repeat < 1:
        raise GoldenError("--repeat must be at least 1")
    ignore = ([IPC_TIME_DEPENDENT] if IPC_TIME_DEPENDENT.is_file() else []) + [Path(p) for p in a.ignore or []]
    extra = shlex.split(a.candidate_args or "") if candidate else shlex.split(a.runner_args or "")
    checks_dir = ref / "check"
    if checks_dir.exists():
        shutil.rmtree(checks_dir)
    all_results: list[Result] = []
    states: list[list[str]] = []
    failed_runs = 0
    for n in range(1, a.repeat + 1):
        outdir = checks_dir / f"run{n}"
        work = outdir.parent / f"work{n}"
        copy_tree(ref / "user-before", work)
        spec = RunSpec(runner, program, code, cpu, a.update or [], work, outdir, session, a.set or [], extra,
                       not candidate, a.shot_every, gpu=False)
        status, _ = run_runner(spec, a.timeout)
        shutil.rmtree(work, ignore_errors=True)
        prefix = f"run{n}: " if a.repeat > 1 else ""
        if status != 0:
            failed_runs += 1
            all_results.append(Result(prefix + "runner", "fail", f"exit status {status}"))
        for r in compare_outputs(session, ref, outdir, candidate, ignore, Path(a.observable)):
            all_results.append(Result(prefix + r.name, r.status, r.detail))
        if (outdir / "state.txt").is_file():
            states.append(_strip_header(outdir / "state.txt"))
    if a.repeat > 1:
        same = len(states) == a.repeat and all(s == states[0] for s in states[1:])
        all_results.append(Result("repeat", "pass" if same else "fail",
                                  f"state traces of {a.repeat} runs " + ("are identical" if same else "differ")))
    for r in all_results:
        print(f"{session.name}: {r.name}: {r.status.upper()}" + (f" ({r.detail.splitlines()[0]})" if r.detail and r.status != "pass" else ""))
    summary = {s: sum(r.status == s for r in all_results) for s in ("pass", "fail", "skipped")}
    print(f"{session.name}: {summary['pass']} passed, {summary['fail']} failed, {summary['skipped']} skipped")
    (checks_dir / "result.json").write_text(json.dumps([dataclasses.asdict(r) for r in all_results], indent=2) + "\n",
                                            encoding="utf-8")
    return 1 if summary["fail"] else 0


# --------------------------------------------------------------------------
# list, command line

def list_sessions(a: argparse.Namespace) -> int:
    paths = sorted(Path(a.sessions_dir).glob("*.txt"))
    if not paths:
        print("no sessions")
    for p in paths:
        try:
            s = parse_session(p)
        except GoldenError as e:
            print(f"{p.stem}: INVALID: {e}")
            continue
        recorded = (Path(a.out) / s.name / "manifest.json").is_file()
        caps = ",".join(f"{c.kind}:{c.start}:{c.frames}" for c in s.captures) or "-"
        print(f"{s.name}: needs={s.needs} frames={s.frames} events={s.events} checkpoints={len(s.checkpoints)} "
              f"captures={caps} {'DRAFT ' if s.draft else ''}{'recorded' if recorded else 'not recorded'}")
    return 0


def make_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Golden sessions: record and check.")
    sub = p.add_subparsers(dest="cmd", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--out", default=str(DEFAULT_OUT), help="folder of the recordings (default build/golden)")
    common.add_argument("--sessions-dir", default=str(SESSIONS_DIR))
    ls = sub.add_parser("list", parents=[common], help="the sessions and whether they are recorded")
    ls.set_defaults(func=None)
    for name, help_ in (("record", "record a session with the reference runner"),
                        ("check", "run again and compare with the recording")):
        s = sub.add_parser(name, parents=[common], help=help_)
        s.add_argument("session")
        s.add_argument("--runner", required=True, help="the reference runner (mh4u_runner, or a .py script)")
        s.add_argument("--game", required=True, help="the CXI")
        s.add_argument("--update", action="append", help="the update CIA (repeatable)")
        s.add_argument("--user", required=(name == "record"), help="the user folder")
        s.add_argument("--code", help="the code library (.dll or .so) for --cpu recomp")
        s.add_argument("--cpu", choices=["jit", "recomp"])
        s.add_argument("--set", action="append", help="MS:KEY=VALUE for the runner (repeatable)")
        s.add_argument("--runner-args", help="more reference-runner options, one string")
        s.add_argument("--shot-every", type=int, default=1000, help="screenshot interval in ms (0: none)")
        s.add_argument("--timeout", type=float, default=None, help="seconds before the runner is killed")
        if name == "record":
            s.add_argument("--save", help="a save folder, merged into user-before/ (sessions that need one)")
            s.add_argument("--force", action="store_true", help="record over an existing recording")
            s.add_argument("--allow-draft", action="store_true", help="record a draft session")
        else:
            s.add_argument("--candidate-runner")
            s.add_argument("--candidate-program", help="its program (the native runner: a prepare_game.py folder)")
            s.add_argument("--candidate-code", help="its code library (a --native library)")
            s.add_argument("--candidate-args", help="more options for the candidate, one string")
            s.add_argument("--repeat", type=int, default=1, help="runs; the state traces must be identical")
            s.add_argument("--ignore", action="append", help="more IPC ignore files (repeatable)")
            s.add_argument("--observable", default=str(OBSERVABLE))
        s.set_defaults(func=record if name == "record" else check)
    return p


def main(argv: Sequence[str] | None = None) -> int:
    a = make_parser().parse_args(argv)
    try:
        if a.cmd == "list":
            return list_sessions(a)
        session = parse_session(session_path(a.session, Path(a.sessions_dir)))
        return a.func(session, a)
    except GoldenError as e:
        print(f"golden.py: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
