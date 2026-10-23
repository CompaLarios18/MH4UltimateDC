#!/usr/bin/env python3
"""Golden sessions: record them with the reference runner, check a candidate runtime against them.

    python3 tools/native/golden.py list [--out DIR]
    python3 tools/native/golden.py record S --runner RUN --game CXI --update CIA --user DIR
                                      [--code LIB] [--cpu jit|recomp] [--save DIR] [--shot-every-frames N]
    python3 tools/native/golden.py check S [--runner RUN --game CXI] [--update CIA] [--code LIB]
                                      [--candidate-runner RUN2] [--candidate-program PATH]
                                      [--candidate-code LIB] [--candidate-args "..."] [--repeat N]
                                      [--profile same|cross|native] [--fallback] [--require NAMES] [--ignore FILE]...
                                      [--tolerance N] [--min-margin N] [--max-surplus N] [--max-shift N]
                                      [--observable FILE]

A session is a frame-keyed input script (tools/native/golden/<S>.txt, the runner's --input-file
format) with header comment lines, see parse_session.  `record` runs the reference runner on a
copy of the user folder with every recorder on and writes build/golden/<S>/ (docs/native/golden.md
lists the files); `check` runs again on a copy of user-before/ and compares (D31: the `same` profile
for two runs of one backend, the `cross` profile for two timing models: the scene changes within 4 frames
and every input 60 frames after the last one (D50), the DSP capture aligned on its first buffer change
(D51), the `fs:` Reads as a multiset per thread and the Writes in full but for the byte ranges of
config/native/fs_write_masks.txt (D52); the `--tolerance`, `--min-margin`, `--max-shift` and `--max-surplus`
options change those numbers in the cross and native profiles only; the `native` profile (D76) is the cross profile
for the native runtime in real time: scene changes within 24 frames, the DSP shift up to 300 DSP frames, the
`fs:` Reads merged where their byte ranges touch before the multiset is built).  A session may carry `# env:`
lines (NAME=VALUE: the port's start-up options, G3-perf's widening), which `record` and `check` give every run of it.
Standard library only;
no shell, no bash: the runner and the code library (.dll or .so) come from the command line, so it
works the same on Windows and Linux.  The recordings are game data: they stay in build/.

Exit status of `check` (D34): 0 every gated comparison passed and at least one ran; 1 a comparison
failed; 2 a usage problem; 3 no gated comparison ran, or a comparison named in --require was skipped.
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
import cross_model_report  # noqa: E402
import gcap  # noqa: E402
import state_diff  # noqa: E402

SESSIONS_DIR = HERE / "golden"
DEFAULT_OUT = ROOT / "build" / "golden"
OBSERVABLE = ROOT / "config" / "native" / "observable.txt"
IPC_TIME_DEPENDENT = ROOT / "config" / "native" / "ipc_time_dependent.txt"
FS_WRITE_MASKS = ROOT / "config" / "native" / "fs_write_masks.txt"
# The cross profile's numbers (D50, D51, D52); `check` takes them as options in the cross and native profiles only.
CROSS_TOLERANCE = cross_model_report.DEFAULT_TOLERANCE      # frames a real scene change may move by (D50)
CROSS_MIN_MARGIN = cross_model_report.DEFAULT_MIN_MARGIN    # frames an input must follow the last scene change by (D50)
CROSS_MAX_SHIFT = capfmt.DEFAULT_MAX_SHIFT                  # DSP frames the candidate's audio may be shifted by (D51)
CROSS_MAX_SURPLUS = 16                                      # fs:USER/file reads a thread may make beyond the other (D52)
# The native profile (D76): the cross profile for the native runtime in real time, whose game clock follows the host
# while the reference charges the modelled 268 MHz.  Only these differ from the cross profile's numbers; the margin
# guard (CROSS_MIN_MARGIN), the surplus of reads (CROSS_MAX_SURPLUS), the write masks (D52) and every other rule are
# the cross profile's.  A tighter profile follows from the deterministic mode of N1-25.
NATIVE_TOLERANCE = 24                                       # frames a real scene change may move by (D76)
NATIVE_MAX_SHIFT = 300                                      # DSP frames the candidate's audio may be shifted by (D76)
NATIVE_FS_READS_COALESCE = True                             # fs:USER/file Reads that touch are merged first (D76)
NATIVE_FS_READS_PREFIX = True                               # a run may go on further on one side (N0-33, see comparators.md)
SVC_CROSS_MODEL = ROOT / "config" / "native" / "svc_cross_model.txt"
SVC_POLLING = ROOT / "config" / "native" / "svc_polling.txt"
TZ = "UTC0"                      # the time zone the runner is started with (see docs/native/golden.md)
MANIFEST_FORMAT = 2
MAX_GPU_FRAMES = 30
DEFAULT_SHOT_EVERY_FRAMES = 60   # --shot-every-frames: a screenshot every N game frames
SHOTS_FROM_FRAME = 300           # the first screenshot (the runner's --shots-from f300), and the shared-page dump
SHARED_PAGE_ADDR = "1FF81000"    # the shared page the runner dumps at SHOTS_FROM_FRAME (--dump), size SHARED_PAGE_SIZE
SHARED_PAGE_SIZE = "0x1000"
BUTTONS = {"A", "B", "X", "Y", "UP", "DOWN", "LEFT", "RIGHT", "L", "R", "START", "SELECT", "ZL", "ZR", "HOME"}
# Folders of a recording that the manifest's file inventory leaves out.
NOT_RECORDED_DIRS = {"user-before", "user-after", "work", "check"}
# What a recording of a session must show: real scene changes in state.txt and distinct scene values among the
# checkpoints (N0-09), and fog mode 7, gas, in a GPU capture (N0-10: G4's cutscene).  G3 and G3-perf pass through
# the intro, the continue screen, the village, the Arena's equipment screen and the hunt.
MINIMUMS = {
    "G2": {"scene_changes": 2, "checkpoint_scenes": 3},
    "G3": {"checkpoint_scenes": 5},
    "G3-perf": {"checkpoint_scenes": 5},
    "G4": {"gas_frames": 1},
}
# The comparisons `--require` can name (D34).
REQUIRABLE = ("svc", "ipc", "state", "checkpoints", "hid", "dsp", "images", "unmapped")
PROFILES = ("same", "cross", "native")
# The config savegame one default-mode reference run makes (docs/native/golden.md, "The user folder").
CONFIG_SAVEGAME = re.compile(r"^nand/data/[^/]+/sysdata/00010017/00000000/config$")


class GoldenError(Exception):
    """A usage or environment problem (exit status 2)."""


def split_args(text: str | None) -> list[str]:
    """Split a --runner-args / --candidate-args string.  POSIX mode treats a backslash as an escape,
    which would mangle Windows paths (C:\\a\\b.dll), so it is used only off Windows."""
    posix = os.name != "nt"
    parts = shlex.split(text or "", posix=posix)
    if not posix:     # non-POSIX mode keeps the quotes around a token: drop them
        parts = [p[1:-1] if len(p) >= 2 and p[0] == p[-1] and p[0] in "\"'" else p for p in parts]
    return parts


def option_value(args: Sequence[str], name: str) -> str | None:
    """The value after `name` in a list of runner options, or None."""
    for i, token in enumerate(args[:-1]):
        if token == name:
            return args[i + 1]
    return None


# --------------------------------------------------------------------------
# Session files

@dataclasses.dataclass
class Capture:
    kind: str                    # "gpu" or "dsp"
    start: int
    frames: int
    silent: bool = False         # a DSP capture that is expected to be silent

    @property
    def filename(self) -> str:
        return f"{self.kind}-{self.start}.{'mgpu' if self.kind == 'gpu' else 'mdsp'}"

    def text(self) -> str:
        return f"{self.kind}:{self.start}:{self.frames}" + (":silent" if self.silent else "")


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
    env: list[str] = dataclasses.field(default_factory=list)     # NAME=VALUE for the runner (`# env:` headers)
    init_time: str | None = None             # canonical `# init-time:` (the runners' --init-time), None: the runner's default

    def sha256(self) -> str:
        return sha256_file(self.path)


_TIME = re.compile(r"^(?:f\d+|\d+)$")
# `# env: NAME=VALUE`: an environment variable of the runner (the port's start-up options, MH4U_WIDESCREEN ...).
# Only the port's own MH4U_* variables: a session file cannot set PATH, PYTHONPATH, LD_PRELOAD or the like.
_ENV = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=[^\s\x00]+$")
ENV_PREFIX = "MH4U_"

# `# init-time: SECONDS[.FRACTION]` (N0-35, D83): the start date of the runners' clock, seconds since the Unix epoch, UTC.
# The grammar and the range are the runners' --init-time (the same on the reference and the native runner).
_INIT_TIME = re.compile(r"([0-9]{1,10})(?:\.([0-9]{1,6}))?", re.ASCII)
INIT_TIME_MIN_US = 946684800 * 1000000                  # 2000-01-01 00:00:00 UTC
INIT_TIME_MAX_US = 4102444799 * 1000000 + 999999        # 2099-12-31 23:59:59.999999 UTC


def parse_init_time(text: str) -> tuple[str, int]:
    """(canonical text, microseconds) of an init time; GoldenError when it is malformed or out of range.
    Canonical: the seconds without leading zeros, the fraction without trailing zeros, no dot for a zero fraction."""
    m = _INIT_TIME.fullmatch(text)
    if not m:
        raise GoldenError(f"bad init time {text!r} (want SECONDS[.FRACTION]: 1 to 10 digits, then a dot and 1 to 6 "
                          f"digits, nothing else)")
    micros = int(m.group(1)) * 1000000 + int((m.group(2) or "").ljust(6, "0"))
    if not INIT_TIME_MIN_US <= micros <= INIT_TIME_MAX_US:
        raise GoldenError(f"init time {text!r} is outside 946684800 to 4102444799.999999 (2000-01-01 to 2099-12-31 UTC)")
    seconds, fraction = divmod(micros, 1000000)
    return (str(seconds) + (f".{fraction:06d}".rstrip("0") if fraction else "")), micros


def init_time_args(session: Session, args: Sequence[str], what: str) -> list[str]:
    """`args` (the user's --runner-args or --candidate-args) for a session with an init time: a `--init-time` that
    disagrees with the session's header is a usage error; an equal one is dropped (build_args gives it once).
    Without a header the args are returned untouched."""
    out = list(args)
    if session.init_time is None:
        return out
    i = 0
    while i < len(out):
        token = out[i]
        if token == "--init-time" or token.startswith("--init-time="):
            inline = token.startswith("--init-time=")
            if not inline and i + 1 >= len(out):
                raise GoldenError(f"{what}: --init-time without a value")
            value = token.split("=", 1)[1] if inline else out[i + 1]
            try:
                same = parse_init_time(value)[0] == session.init_time
            except GoldenError:
                same = False
            if not same:
                raise GoldenError(f"{what} has --init-time {value}, but {session.name} says '# init-time: "
                                  f"{session.init_time}': the session decides its date")
            del out[i:i + (1 if inline else 2)]
            continue
        i += 1
    return out


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
    (`<frame>:<scene>, ...`, the scene being the observable `scene` value: the class of the topmost game
    area, N0-27), captures (`gpu:<start>:<frames>, dsp:<start>:<frames>[:silent], ...`; `:silent` marks a
    DSP capture that is expected to be silent), optionally `draft: yes` (the numbers are placeholders;
    `record` refuses it) and any number of `env: NAME=VALUE` lines (an environment variable that `record` and
    `check` give the runner on every run of the session, so that the session does not depend on what the
    command line says: G3-perf's MH4U_WIDESCREEN and MH4U_WIDE_RENDER, which the port reads once at start-up
    and the runner's --set cannot change; only MH4U_* names, each once, and never TZ) and at most one
    `init-time: SECONDS[.FRACTION]` (the start date the runners' clock begins at, `--init-time`, N0-35).  Every other line is an input event line for the runner's
    --input-file (comma separated `f<frame>:BUTTON[:hold]`, `f<frame>:TOUCH:x:y[:hold]`,
    `f<frame>:CIRCLE:x:y[:hold]`).
    """
    path = Path(path)
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as e:
        raise GoldenError(f"cannot read session file {path}: {e}") from None
    headers: dict[str, str] = {}
    env: list[str] = []
    init_time: str | None = None
    events: list[tuple[int, str]] = []
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#"):
            m = re.match(r"^#\s*(name|needs|frames|checkpoints|captures|draft|env|init-time)\s*:\s*(.*)$", line)
            if m and m.group(1) == "env":
                value = m.group(2).strip()
                if not _ENV.match(value):
                    raise GoldenError(f"{path.name}:{number}: bad env {value!r} (want NAME=VALUE)")
                var = value.split("=", 1)[0]
                if var.upper() == "TZ":
                    raise GoldenError(f"{path.name}:{number}: TZ is fixed by golden.py (UTC0)")
                if not var.startswith(ENV_PREFIX):
                    raise GoldenError(f"{path.name}:{number}: env {var!r}: only {ENV_PREFIX}* variables (the port's "
                                      f"options) can be set by a session")
                if var in (x.split("=", 1)[0] for x in env):
                    raise GoldenError(f"{path.name}:{number}: env {var!r} given twice")
                env.append(value)
            elif m and m.group(1) == "init-time":
                if init_time is not None:
                    raise GoldenError(f"{path.name}:{number}: header 'init-time' given twice")
                try:
                    init_time = parse_init_time(m.group(2).strip())[0]
                except GoldenError as e:
                    raise GoldenError(f"{path.name}:{number}: {e}") from None
            elif m:
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
        m = re.match(r"^(gpu|dsp):(\d+):(\d+)(?::(silent))?$", item)
        if not m:
            raise GoldenError(f"{where}: bad capture {item!r} (want gpu:<start>:<frames> or "
                              f"dsp:<start>:<frames>[:silent])")
        if m.group(4) and m.group(1) != "dsp":
            raise GoldenError(f"{where}: {item!r}: only a DSP capture can be marked silent")
        cap = Capture(m.group(1), int(m.group(2)), int(m.group(3)), bool(m.group(4)))
        if cap.frames < 1 or (cap.kind == "gpu" and cap.frames > MAX_GPU_FRAMES):
            raise GoldenError(f"{where}: {item!r}: frames must be 1..{MAX_GPU_FRAMES if cap.kind == 'gpu' else 'N'}")
        if cap.start > frames:
            raise GoldenError(f"{where}: {item!r} starts after the last frame")
        if any(c.filename == cap.filename for c in captures):
            raise GoldenError(f"{where}: capture {item!r} given twice")
        captures.append(cap)
    for number, item in events:
        _check_event(item, frames, f"{where}:{number}")
    return Session(name, headers["needs"], frames, checkpoints, captures, draft, path, len(events), env, init_time)


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


def read_state(state_path: Path) -> list[state_diff.Record]:
    return state_diff.parse_trace(state_path.read_text(encoding="utf-8"))


def evaluate_checkpoints(state_path: Path, checkpoints: Sequence[tuple[int, int]]) -> list[dict]:
    """For each (frame, scene): the scene in the state trace at the first record at or after the frame."""
    results = []
    try:
        records = read_state(state_path)
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


def real_scene_changes(state_path: Path) -> int:
    """The number of real scene changes in a state trace (N0-25's definition: between two readable values)."""
    try:
        return len(state_diff.real_scene_changes(read_state(state_path)))
    except (OSError, state_diff.FormatError):
        return 0


def has_config_savegame(user: Path) -> bool:
    for dirpath, _dirs, filenames in os.walk(user):
        for fn in filenames:
            if CONFIG_SAVEGAME.match((Path(dirpath) / fn).relative_to(user).as_posix()):
                return True
    return False


def shared_page_dump() -> str:
    """The runner's --dump value for the shared page, and the file it writes in the shots folder."""
    return f"f{SHOTS_FROM_FRAME}:{SHARED_PAGE_ADDR}:{SHARED_PAGE_SIZE}"


def shared_page_file() -> str:
    return f"shots/mem_f{SHOTS_FROM_FRAME:07d}_{SHARED_PAGE_ADDR.lower()}.bin"


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


def child_env(extra: Sequence[str] = ()) -> dict[str, str]:
    """The runner's environment: ours, `TZ=UTC0`, and the session's `# env:` variables (NAME=VALUE)."""
    env = dict(os.environ)
    env["TZ"] = TZ
    for item in extra:
        name, value = item.split("=", 1)
        env[name] = value
    return env


@dataclasses.dataclass
class RunSpec:
    runner: str
    program: str
    code: str | None
    cpu: str | None            # None: not passed
    updates: list[str]
    user: Path
    outdir: Path
    session: Session
    sets: list[str]
    extra: list[str]
    full: bool                 # the reference's whole option list (new-3ds, install, unmapped, timing, gpu, shots,
                               # shared page), or only the options both runners share (--observable goes to both,
                               # when `observable` is set)
    shot_every: int = DEFAULT_SHOT_EVERY_FRAMES      # frames between screenshots, 0: none
    observable: str | None = None


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
    if s.init_time is not None:       # every runner, the reference and the candidate (N0-35); none: the runner's default
        args += ["--init-time", s.init_time]
    for item in spec.sets:
        args += ["--set", item]
    args += ["--trace-svc", str(o / "svc.txt"), "--trace-ipc", str(o / "ipc.txt"), "--trace-ipc-full",
             "--state-trace", str(o / "state.txt"), "--capture-hid", str(o / "hid.mhid")]
    if spec.observable:       # both runners take it (the native runner's runner.md table): one list on both sides
        args += ["--observable", spec.observable]
    for cap in s.captures:
        if cap.kind == "dsp":
            args += ["--capture-dsp", f"{cap.start}:{cap.frames}:{o / cap.filename}"]
        elif cap.kind == "gpu" and spec.full:
            args += ["--capture-gpu", f"{cap.start}:{cap.frames}:{o / cap.filename}"]
    if spec.full:
        args += ["--trace-timing", str(o / "timing.txt"), "--count-unmapped", str(o / "unmapped.txt")]
        if spec.shot_every > 0:
            args += ["--shots", str(o / "shots"), "--shots-from", f"f{SHOTS_FROM_FRAME}",
                     "--shot-every", f"f{spec.shot_every}", "--dump", shared_page_dump()]
    return args + spec.extra


def run_runner(spec: RunSpec, timeout: float | None) -> tuple[int, list[str]]:
    """Start the runner (no shell), log to <outdir>/runner.log; returns (exit status, arguments)."""
    spec.outdir.mkdir(parents=True, exist_ok=True)
    if spec.full and spec.shot_every > 0:
        (spec.outdir / "shots").mkdir(exist_ok=True)
    args = build_args(spec)
    cmd = runner_argv(spec.runner) + args
    with open(spec.outdir / "runner.log", "wb") as log:
        try:
            done = subprocess.run(cmd, cwd=ROOT, env=child_env(spec.session.env), stdout=log, stderr=subprocess.STDOUT,
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


# --------------------------------------------------------------------------
# Capture problems (what makes `record` exit 1)

def _dsp_problems(path: Path, cap: Capture) -> list[str]:
    """Structure of a DSP capture, and, unless it is marked :silent, sound in it: a non-silent PCM frame
    and a type-3 (SAMPLES) record."""
    pcm_sound = samples = False
    for rec in capfmt.read(str(path)):
        capfmt.check_record("dsp", rec)
        if rec.type == capfmt.DSP_PCM and any(rec.payload):
            pcm_sound = True
        elif rec.type == capfmt.DSP_SAMPLES:
            samples = True
    problems: list[str] = []
    if not cap.silent and not pcm_sound:
        problems.append(f"{path.name}: no non-silent PCM frame (mark the capture :silent if it should be silent)")
    if not cap.silent and not samples:
        problems.append(f"{path.name}: no type-3 (SAMPLES) record (mark the capture :silent if it should be silent)")
    return problems


def _ir_requests(ipc_path: Path) -> list[int]:
    """The ticks (t=) of the `ir:USER` requests of an IPC trace."""
    ticks: list[int] = []
    with open(ipc_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#") or "session=ir:USER" not in line:
                continue
            m = re.search(r"\bt=(\d+)", line)
            if m:
                ticks.append(int(m.group(1)))
    return ticks


def _ir_problems(outdir: Path) -> list[str]:
    """An `ir:USER` request needs a type-2 (IR) record in the HID capture from that request's frame on."""
    ipc, hid = outdir / "ipc.txt", outdir / "hid.mhid"
    if not ipc.is_file() or not hid.is_file():
        return []
    requests = _ir_requests(ipc)
    if not requests:
        return []
    records = capfmt.read_all(str(hid))
    shared = [(r.ticks, r.frame) for r in records if r.type == capfmt.HID_SHARED]
    ir_frames = [r.frame for r in records if r.type == capfmt.HID_IR]
    problems: list[str] = []
    last = max(requests)           # the strictest: no IR record after it means none after the earlier ones either
    frame = next((fr for t, fr in shared if t >= last), (shared[-1][1] + 1) if shared else 0)
    if not any(fr >= frame for fr in ir_frames):
        problems.append(f"ipc.txt has {len(requests)} ir:USER request(s) (the last at frame {frame}) but "
                        f"hid.mhid has no type-2 (IR) record from that frame on")
    return problems


def validate_captures(outdir: Path, session: Session) -> list[str]:
    """Problems of the binary captures a run wrote (empty list: fine)."""
    problems: list[str] = []
    paths = [(outdir / "hid.mhid", "hid", None)] + [(outdir / c.filename, c.kind, c) for c in session.captures]
    for path, kind, cap in paths:
        if not path.is_file():
            problems.append(f"{path.name}: not written")
            continue
        try:
            if kind == "gpu":
                problems += [f"{path.name}: {p}" for p in gcap.check_file(str(path))]
            elif kind == "dsp":
                problems += _dsp_problems(path, cap)
            else:
                for rec in capfmt.read(str(path)):
                    capfmt.check_record(kind, rec)
        except (capfmt.CapError, gcap.GcapError, OSError) as e:
            problems.append(f"{path.name}: {e}")
    try:
        problems += _ir_problems(outdir)
    except (capfmt.CapError, OSError) as e:
        problems.append(f"ir:USER check: {e}")
    return problems


def gas_frames(outdir: Path, session: Session) -> int:
    """How many times the session's GPU captures set fog mode 7 (gas): the count `gcap.py summary` prints."""
    total = 0
    for cap in session.captures:
        path = outdir / cap.filename
        if cap.kind != "gpu" or not path.is_file():
            continue
        try:
            total += gcap.summarize(gcap.read_all(str(path)))["fog_modes"].get("gas", 0)
        except (gcap.GcapError, OSError):
            pass            # the capture problem itself is reported by validate_captures
    return total


def minimum_problems(session: Session, outdir: Path) -> list[str]:
    """What the session's recording must show at least (MINIMUMS)."""
    want = MINIMUMS.get(session.name)
    if not want:
        return []
    problems: list[str] = []
    if "scene_changes" in want:
        state = outdir / "state.txt"
        changes = real_scene_changes(state) if state.is_file() else 0
        if changes < want["scene_changes"]:
            problems.append(f"state.txt has {changes} real scene change(s), {session.name} needs at least "
                            f"{want['scene_changes']}")
    if "checkpoint_scenes" in want:
        distinct = len({s for _, s in session.checkpoints})
        if distinct < want["checkpoint_scenes"]:
            problems.append(f"{session.name}'s checkpoints hold {distinct} distinct scene value(s), at least "
                            f"{want['checkpoint_scenes']} wanted")
    if "gas_frames" in want and gas_frames(outdir, session) < want["gas_frames"]:
        problems.append(f"{session.name}'s GPU capture(s) never set fog mode 7 (gas): keep a cutscene where "
                        f"`gcap.py summary` shows it")
    return problems


# --------------------------------------------------------------------------
# record

def record(session: Session, a: argparse.Namespace) -> int:
    if session.draft and not a.allow_draft:
        raise GoldenError(f"{session.name} is a draft (placeholder numbers, no real checkpoints); "
                          f"finish it first, or pass --allow-draft to try it")
    if session.needs == "save" and not a.save:
        raise GoldenError(f"{session.name} needs a save: pass --save DIR")
    if a.shot_every_frames < 0:
        raise GoldenError("--shot-every-frames must not be negative")
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
    config_savegame = has_config_savegame(before)
    if not config_savegame:
        print(f"{session.name}: warning: user-before/ has no config savegame (nand/data/<id>/sysdata/00010017/"
              f"00000000/config): a native replay would draw its own console ID (docs/native/golden.md)")
    copy_tree(before, work)
    spec = RunSpec(a.runner, a.game, a.code if cpu == "recomp" else None, cpu, a.update or [], work, out, session,
                   a.set or [], init_time_args(session, split_args(a.runner_args), "--runner-args"), True, a.shot_every_frames, str(OBSERVABLE))
    started = time.time()
    status, args = run_runner(spec, a.timeout)
    seconds = round(time.time() - started, 1)
    after = out / "user-after"
    if after.exists():
        shutil.rmtree(after)
    work.rename(after)
    state = out / "state.txt"
    checkpoints = evaluate_checkpoints(state, session.checkpoints) if state.is_file() else [
        {"frame": f, "scene": s, "actual": None, "result": "missing"} for f, s in session.checkpoints]
    problems = validate_captures(out, session) + minimum_problems(session, out)
    manifest = {
        "format": MANIFEST_FORMAT,
        "tool": "golden.py",
        "session": {"name": session.name, "needs": session.needs, "frames": session.frames,
                    "checkpoints": [list(c) for c in session.checkpoints],
                    "captures": [dataclasses.asdict(c) for c in session.captures],
                    "env": list(session.env), "init_time": session.init_time,
                    "events": session.events, "sha256": session.sha256()},
        "runner": {"path": str(a.runner), "sha256": sha256_file(Path(a.runner)), "cpu": cpu,
                   "new_3ds": "off", "exit": status, "seconds": seconds},
        "code_library": {"path": a.code, "sha256": sha256_file(Path(a.code))} if a.code and cpu == "recomp" else None,
        "game": {"name": Path(a.game).name, "size": Path(a.game).stat().st_size},
        "updates": [{"name": Path(u).name, "size": Path(u).stat().st_size} for u in (a.update or [])],
        "save": Path(a.save).name if a.save else None,
        "user_config_savegame": config_savegame,
        "tz": TZ,
        "host": platform.system(),
        "observable_sha256": sha256_file(OBSERVABLE) if OBSERVABLE.is_file() else None,
        "shots": {"from_frame": SHOTS_FROM_FRAME, "every_frames": a.shot_every_frames},
        "real_scene_changes": real_scene_changes(state) if state.is_file() else 0,
        "args": [Path(args[0]).name] + args[1:],
        "checkpoint_results": checkpoints,
        "capture_problems": problems,
        "files": inventory(out),
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    ok = status == 0 and not problems and all(c["result"] == "ok" for c in checkpoints)
    print(f"{session.name}: runner exit {status}, {len(manifest['files'])} files, "
          f"checkpoints {sum(c['result'] == 'ok' for c in checkpoints)}/{len(checkpoints)} ok, "
          f"{manifest['real_scene_changes']} real scene changes, {len(problems)} capture problems -> {out}")
    for p in problems:
        print(f"  capture: {p}")
    for c in checkpoints:
        if c["result"] != "ok":
            print(f"  checkpoint {c['frame']}:{c['scene']:#x} {c['result']} (scene "
                  f"{'-' if c['actual'] is None else format(c['actual'], '#x')})")
    return 0 if ok else 1


# --------------------------------------------------------------------------
# check: comparisons

@dataclasses.dataclass
class Result:
    name: str
    status: str          # pass | fail | skipped | info
    detail: str = ""
    run: str = ""        # "run2: " ..., the run a repeated check's result belongs to
    note: str = ""       # what the cross profile measured (the DSP shift, the reads' surplus ...), printed with the status

    @property
    def base(self) -> str:
        """The comparison a --require names: `dsp-1240` is `dsp`."""
        return self.name.split("-")[0]

    @property
    def label(self) -> str:
        return self.run + self.name


@dataclasses.dataclass
class Compare:
    profile: str                        # same | cross | native
    fallback: bool = False              # D30's fallback of the same profile
    strict: bool = True                 # a file the run did not write is a failure (reference runner), not a skip
    ref_is_recording: bool = True       # a file the reference side lacks is a failure (a recording) or a skip
    ignore: Sequence[Path] = ()         # IPC ignore files (cross)
    observable: Path = OBSERVABLE
    real_changes: int = 0               # the recording's real scene changes
    checkpoints: bool = True
    # the cross profile's rules (D50 to D52, D76 for native); a `same` comparison has none of them
    tolerance: int | None = None        # frames a real scene change may move by (default CROSS_TOLERANCE)
    min_margin: int | None = None       # frames an input follows the last scene change by (CROSS_MIN_MARGIN)
    max_surplus: int | None = None      # reads a thread may make beyond the other (CROSS_MAX_SURPLUS)
    max_shift: int | None = None        # DSP frames of shift (CROSS_MAX_SHIFT)
    fs_write_masks: Path | None = None  # the write-mask file (default FS_WRITE_MASKS)

    def rule(self, name: str) -> int:
        """The value of one of the cross profile's numbers: the option, else the profile's own (D50 to D52; the
        native profile's tolerance and shift are D76's)."""
        mine = getattr(self, name)
        if mine is not None:
            return mine
        native = self.profile == "native"
        return {"tolerance": NATIVE_TOLERANCE if native else CROSS_TOLERANCE, "min_margin": CROSS_MIN_MARGIN,
                "max_surplus": CROSS_MAX_SURPLUS, "max_shift": NATIVE_MAX_SHIFT if native else CROSS_MAX_SHIFT}[name]


def _tool(script: Path, *args: str) -> tuple[int, str]:
    done = subprocess.run([sys.executable, str(script), *args], cwd=ROOT, capture_output=True, text=True,
                          check=False)
    return done.returncode, (done.stdout + done.stderr).strip()


def _first_lines(text: str, n: int = 8) -> str:
    return "\n".join(text.splitlines()[:n])


def compare_after_header(ref: Path, cand: Path, block: int = 1 << 23) -> tuple[bool, str]:
    """Two text traces are identical apart from their first line (the header; `#` lines after it are content).
    Reads in blocks; prints sizes and the line of the first difference, never a line."""
    with open(ref, "rb") as f1, open(cand, "rb") as f2:
        f1.readline()
        f2.readline()
        lines = 0
        total = 0
        while True:
            b1, b2 = f1.read(block), f2.read(block)
            if b1 == b2:
                if not b1:
                    return True, f"identical apart from the header ({total} bytes, {lines} lines)"
                lines += b1.count(b"\n")
                total += len(b1)
                continue
            n = min(len(b1), len(b2))
            at = next((i for i in range(n) if b1[i] != b2[i]), n)
            line = 1 + lines + b1.count(b"\n", 0, at)
            return False, (f"differs at line {line} after the header (file sizes {ref.stat().st_size} and "
                           f"{cand.stat().st_size} bytes)")


def _files_equal(ref: Path, cand: Path, names: Sequence[str]) -> tuple[bool, str]:
    """The files `names` (relative to both folders, those the reference has) have equal SHA-256."""
    differ = [n for n in names if not (cand / n).is_file() or sha256_file(cand / n) != sha256_file(ref / n)]
    if differ:
        return False, f"{len(differ)} of {len(names)} differ: " + ", ".join(differ[:5])
    return True, f"{len(names)} file(s) equal"


def _group(ref: Path, name: str) -> list[str]:
    """A capture and what goes with it: the file, its .json manifest and its .frames folder."""
    names = [name]
    if (ref / (name + ".json")).is_file():
        names.append(name + ".json")
    frames = ref / (name + ".frames")
    if frames.is_dir():
        names += sorted(p.relative_to(ref).as_posix() for p in frames.rglob("*") if p.is_file())
    return names


def margin_result(session: Session, ref: Path, cand: Path, cfg: Compare, add) -> None:
    """D50's guard of the cross profile, with `cross_model_report.py`'s logic: every input event of the session
    that follows a real scene change comes at least `min_margin` frames after the last one on both sides, so that
    the shift of a scene change can never move an input into another scene.  Adds `state-margin` (the
    `--require state` of `check` covers it).  The scene sequence and the shift itself are `state`'s."""
    try:
        ref_changes = cross_model_report.read_scene_changes(ref / "state.txt")
        cand_changes = cross_model_report.read_scene_changes(cand / "state.txt")
        events = cross_model_report.parse_events(session.path.read_text(encoding="utf-8"))
    except (cross_model_report.ReportError, OSError, UnicodeError) as e:
        add("state-margin", "fail", str(e))
        return
    minimum = cfg.rule("min_margin")
    problems = cross_model_report.margin_problems(events, ref_changes, cand_changes, minimum)
    parts = []
    if ref_changes and cand_changes:
        worst = max((abs(c.frame - r.frame) for r, c in zip(ref_changes, cand_changes)), default=0)
        parts.append(f"largest scene-change shift {worst} frames (at most {cfg.rule('tolerance')})")
    for label, changes in (("reference", ref_changes), ("candidate", cand_changes)):
        margins = [e.frame - c.frame for e in events
                   if (c := cross_model_report.last_change_at_or_before(changes, e.frame))]
        parts.append(f"smallest input margin on the {label} {min(margins)} frames (at least {minimum})"
                     if margins else f"no input follows a real scene change on the {label}")
    add("state-margin", "fail" if problems else "pass", "\n".join(problems[:8]), "; ".join(parts))


def compare_outputs(session: Session, ref: Path, cand: Path, cfg: Compare) -> list[Result]:
    """Compare a run's outputs (`cand`) with a recording or another run (`ref`) in a profile.

    `same` (D31): the text traces identical apart from their first line, the captures and the screenshots
    identical by SHA-256, the checkpoints.  D30's fallback (cfg.fallback) swaps the svc, ipc and state
    comparisons for the tolerant ones of N0-25.  `cross`: what a different clock cannot change, with the rules
    of D50 (state: scene changes within `cfg.tolerance` frames, `state-margin`: inputs at least `cfg.min_margin`
    frames after the last scene change), D51 (dsp: aligned) and D52 (ipc: `fs:` Reads as a multiset, masked Writes).
    `native` (D76) is `cross` with the larger tolerance and DSP shift, and the touching `fs:` Reads merged.
    A comparison whose input the run did not write is skipped for a candidate runner and a failure
    for the reference runner."""
    results: list[Result] = []
    same = cfg.profile == "same"
    tools = ROOT / "tools"

    def add(name: str, status: str, detail: str = "", note: str = "") -> None:
        results.append(Result(name, status, detail, note=note))

    def need(name: str, rel: str, info: bool = False) -> bool:
        if not (ref / rel).exists():
            if info:
                return False
            if cfg.ref_is_recording:
                add(name, "fail", f"the recording has no {rel}")
            else:
                add(name, "skipped", f"the first run wrote no {rel}")
            return False
        if not (cand / rel).exists():
            if info:
                return False
            add(name, "fail" if cfg.strict else "skipped", f"the run wrote no {rel}")
            return False
        return True

    def tool_result(name: str, code: int, text: str, note: str = "") -> None:
        if code == 0:
            add(name, "pass", _first_lines(text, 2), note)
        elif code == 3:
            add(name, "skipped", _first_lines(text))
        else:
            add(name, "fail", _first_lines(text))

    # -- svc
    if need("svc", "svc.txt"):
        if same and not cfg.fallback:
            ok, detail = compare_after_header(ref / "svc.txt", cand / "svc.txt")
            add("svc", "pass" if ok else "fail", detail)
        elif same:
            code, text = _tool(tools / "compare_svc_traces.py", "--normalize", "--outputs", "--collapse-polling",
                               str(SVC_POLLING), str(ref / "svc.txt"), str(cand / "svc.txt"))
            tool_result("svc", code, text)
        else:
            code, text = _tool(tools / "compare_svc_traces.py", "--normalize", "--outputs", "--cross-model",
                               str(SVC_CROSS_MODEL), str(ref / "svc.txt"), str(cand / "svc.txt"))
            tool_result("svc", code, text)
            code, text = _tool(tools / "compare_svc_traces.py", "--normalize", "--outputs",
                               str(ref / "svc.txt"), str(cand / "svc.txt"))
            add("svc-full", "info", f"the full SVC comparison (never gated) exits {code}: " + _first_lines(text, 3))
    # -- ipc
    if need("ipc", "ipc.txt"):
        if same and not cfg.fallback:
            ok, detail = compare_after_header(ref / "ipc.txt", cand / "ipc.txt")
            add("ipc", "pass" if ok else "fail", detail)
        else:
            extra: list[str] = []
            if not same:
                for f in cfg.ignore:
                    extra += ["--ignore", str(f)]
                # D52: the Reads of fs:USER/file as a multiset per thread, the Writes in full except the byte
                # ranges proved to depend on the clock (this session's lines of the mask file)
                extra += ["--fs-reads-multiset", "--max-surplus", str(cfg.rule("max_surplus")),
                          "--fs-write-mask", str(cfg.fs_write_masks or FS_WRITE_MASKS),
                          "--fs-write-mask-session", session.name]
                if cfg.profile == "native":      # D76: the Reads that touch are merged before the multiset is built
                    if NATIVE_FS_READS_COALESCE:
                        extra.append("--fs-reads-coalesce")
                        if NATIVE_FS_READS_PREFIX:
                            extra.append("--fs-reads-prefix")
            code, text = _tool(tools / "compare_ipc_traces.py", *extra, str(ref / "ipc.txt"), str(cand / "ipc.txt"))
            if code == 1 and "\n\n" in text:
                text = text.split("\n\n", 1)[1]            # past the per-thread counts: the first difference
            tool_result("ipc", code, text, " ; ".join(ln for ln in text.splitlines() if ln.startswith("fs ")))
    # -- state, checkpoints
    state_ok = need("state", "state.txt")
    if state_ok:
        if same and not cfg.fallback:
            ok, detail = compare_after_header(ref / "state.txt", cand / "state.txt")
            add("state", "pass" if ok else "fail", detail)
        elif cfg.real_changes <= 0:
            add("state", "skipped", "the recording has no real scene change: nothing to align")
        else:
            # same: exact. cross: D50's tolerance (4 frames), native: D76's (24), and D50's margin guard below
            tol = ["--tolerance", "0" if same else str(cfg.rule("tolerance"))]
            code, text = _tool(HERE / "state_diff.py", "--observable", str(cfg.observable), *tol,
                               "--min-changes", str(cfg.real_changes), str(ref / "state.txt"), str(cand / "state.txt"))
            tool_result("state", code, text, "" if same else _first_lines(text, 1))
            if not same:
                margin_result(session, ref, cand, cfg, add)
    if cfg.checkpoints:
        if (cand / "state.txt").is_file():
            if session.checkpoints:
                bad = [c for c in evaluate_checkpoints(cand / "state.txt", session.checkpoints) if c["result"] != "ok"]
                add("checkpoints", "fail" if bad else "pass",
                    "; ".join(f"{c['frame']}:{c['scene']:#x} {c['result']}" for c in bad[:5]))
            else:
                add("checkpoints", "skipped", "the session has no checkpoints")
        elif (ref / "state.txt").is_file():
            add("checkpoints", "fail" if cfg.strict else "skipped", "the run wrote no state.txt")
    # -- unmapped (same profile only)
    if same and need("unmapped", "unmapped.txt"):
        ok, detail = compare_after_header(ref / "unmapped.txt", cand / "unmapped.txt")
        add("unmapped", "pass" if ok else "fail", detail)
    # -- hid, dsp
    if need("hid", "hid.mhid"):
        if same:
            ok, detail = _files_equal(ref, cand, _group(ref, "hid.mhid"))
            add("hid", "pass" if ok else "fail", detail)
        else:
            try:
                lines = capfmt.diff_hid(str(ref / "hid.mhid"), str(cand / "hid.mhid"))
            except (capfmt.CapError, OSError) as e:
                lines = [str(e)]
            add("hid", "fail" if lines else "pass", "\n".join(lines[:8]))
    for cap in session.captures:
        if cap.kind != "dsp" or not need(f"dsp-{cap.start}", cap.filename):
            continue
        if same:
            ok, detail = _files_equal(ref, cand, _group(ref, cap.filename))
            add(f"dsp-{cap.start}", "pass" if ok else "fail", detail)
        else:
            # D51: the candidate aligned on the first buffer change, the shift constant and bounded, then every
            # source status block equal; the shift is part of the result
            try:
                aligned = capfmt.diff_dsp_aligned(str(ref / cap.filename), str(cand / cap.filename),
                                                  cfg.rule("max_shift"))
                lines, summary = aligned.problems, aligned.summary
            except (capfmt.CapError, OSError) as e:
                lines, summary = [str(e)], ""
            add(f"dsp-{cap.start}", "fail" if lines else "pass", "\n".join(lines[:8]), summary)
    # -- gpu captures (same profile only: the capture player compares them across runtimes, N2)
    if same:
        for cap in session.captures:
            if cap.kind == "gpu" and need(f"gpu-{cap.start}", cap.filename):
                ok, detail = _files_equal(ref, cand, _group(ref, cap.filename))
                add(f"gpu-{cap.start}", "pass" if ok else "fail", detail)
    # -- screenshots: gated in the same profile, information in cross
    shots = sorted(p.relative_to(ref).as_posix() for p in (ref / "shots").rglob("*.ppm")) \
        if (ref / "shots").is_dir() else []
    cand_shots = [p for p in (cand / "shots").rglob("*.ppm")] if (cand / "shots").is_dir() else []
    if not shots:
        if same:
            add("images", "skipped", "there are no screenshots to compare with")
    elif not cand_shots:
        if same:
            add("images", "fail" if cfg.strict else "skipped", "the run wrote no screenshots")
    else:
        ok, detail = _files_equal(ref, cand, shots)
        add("images", ("pass" if ok else "fail") if same else "info", detail)
    # -- the shared page and the timing trace of the reference runner
    if same and (ref / shared_page_file()).is_file() and (cfg.strict or (cand / shared_page_file()).is_file()):
        if need("shared-page", shared_page_file()):
            ok, detail = _files_equal(ref, cand, [shared_page_file()])
            add("shared-page", "pass" if ok else "fail", detail)
    if same and need("timing", "timing.txt", info=True):
        ok, detail = compare_after_header(ref / "timing.txt", cand / "timing.txt")
        add("timing", "info", ("identical apart from the header" if ok else detail) + " (not gated)")
    return results


def exit_status(results: Sequence[Result], require: Sequence[str]) -> int:
    """D34: 1 a failure; 3 nothing gated ran or a required comparison was skipped (or did not run); else 0."""
    gated = [r for r in results if r.status != "info"]
    if any(r.status == "fail" for r in gated):
        return 1
    if not any(r.status == "pass" for r in gated):
        return 3
    for name in require:
        mine = [r for r in gated if r.base == name]
        if not mine or any(r.status == "skipped" for r in mine):
            return 3
    return 0


def resolve_profile(forced: str | None, candidate_runner: str | None, args_cpu: str | None,
                    recording_cpu: str) -> str:
    """D31: `same` when there is no candidate runner and --candidate-args does not change --cpu, else `cross`."""
    if forced:
        return forced
    if candidate_runner or (args_cpu is not None and args_cpu != recording_cpu):
        return "cross"
    return "same"


def check(session: Session, a: argparse.Namespace) -> int:
    ref = Path(a.out) / session.name
    manifest_path = ref / "manifest.json"
    if not manifest_path.is_file():
        raise GoldenError(f"{session.name} is not recorded ({manifest_path} missing)")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    # usage problems first
    require = [x.strip() for x in (a.require or "").split(",") if x.strip()]
    for name in require:
        if name not in REQUIRABLE:
            raise GoldenError(f"--require: unknown comparison {name!r} (one of {', '.join(REQUIRABLE)})")
    if a.repeat < 1:
        raise GoldenError("--repeat must be at least 1")
    if (a.candidate_program or a.candidate_code) and not a.candidate_runner:
        raise GoldenError("--candidate-program and --candidate-code need --candidate-runner")
    if not a.candidate_runner and not (a.runner and a.game):
        raise GoldenError("--runner and --game are needed unless --candidate-runner runs the check")
    if a.candidate_runner and not (a.candidate_program or a.game):
        raise GoldenError("--candidate-runner needs --candidate-program (or --game, the program the reference ran)")
    cand_args = init_time_args(session, split_args(a.candidate_args), "--candidate-args")
    recording_cpu = manifest["runner"]["cpu"]
    args_cpu = option_value(cand_args, "--cpu")
    profile = resolve_profile(a.profile, a.candidate_runner, args_cpu, recording_cpu)
    if a.fallback and (profile != "same" or a.candidate_runner):
        raise GoldenError("--fallback is for the same profile and the reference runner only")
    if a.ignore and profile == "same":
        raise GoldenError("--ignore adds IPC ignore files to the cross and native profiles only: "
                          "a same-model check is never loosened by a list")
    rules = {"tolerance": a.tolerance, "min_margin": a.min_margin, "max_surplus": a.max_surplus,
             "max_shift": a.max_shift}
    if profile == "same" and any(v is not None for v in rules.values()):
        raise GoldenError("--tolerance, --min-margin, --max-surplus and --max-shift set the rules of the cross and "
                          "native profiles (D50 to D52, D76): a same-model check is exact and is never loosened")
    for flag, value in rules.items():
        if value is not None and value < 0:
            raise GoldenError(f"--{flag.replace('_', '-')} must not be negative")
    across = profile in ("cross", "native")
    if (across or a.fallback) and not Path(SVC_CROSS_MODEL if across else SVC_POLLING).is_file():
        raise GoldenError(f"missing {SVC_CROSS_MODEL if across else SVC_POLLING}")
    if across and not FS_WRITE_MASKS.is_file():
        raise GoldenError(f"missing {FS_WRITE_MASKS} (the write masks of D52)")
    broken = [rel for rel, meta in manifest["files"].items()
              if not (ref / rel).is_file() or sha256_file(ref / rel) != meta["sha256"]]
    if broken:
        raise GoldenError(f"the recording was changed since it was made: {', '.join(broken[:5])}")
    # the environment of the recording's run (the `# env:` lines of the session, N0-10) is part of what is checked:
    # a session file edited since `record` would run the candidate under other variables.  A manifest made before
    # the key existed was recorded without any.
    recorded_env = list(manifest.get("session", {}).get("env", []))
    if recorded_env != session.env:
        raise GoldenError(f"the '# env:' lines of {session.name} ({session.env or 'none'}) differ from the ones the "
                          f"recording was made with ({recorded_env or 'none'}): record it again")
    # the start date of the recording's run (`# init-time:`, N0-35) is checked the same way; no key counts as none
    recorded_init = manifest.get("session", {}).get("init_time")
    if recorded_init != session.init_time:
        raise GoldenError(f"the '# init-time:' of {session.name} ({session.init_time or 'none'}) differs from the one "
                          f"the recording was made with ({recorded_init or 'none'}): record it again")

    foreign = bool(a.candidate_runner)
    if not foreign and manifest.get("updates") and not a.update:
        raise GoldenError("the recording was made with --install: pass --update CIA so the run starts the same way")
    shot_every =int(manifest.get("shots", {}).get("every_frames", DEFAULT_SHOT_EVERY_FRAMES))
    if foreign:
        runner, program, code, cpu, full = a.candidate_runner, a.candidate_program or a.game, a.candidate_code, None, False
        extra = cand_args
    else:
        runner, program, full = a.runner, a.game, True
        extra = init_time_args(session, split_args(a.runner_args), "--runner-args") + cand_args
        cpu_value = args_cpu or a.cpu or recording_cpu
        cpu = None if "--cpu" in extra else cpu_value
        code = None
        if cpu_value == "recomp" and "--code" not in extra:
            code = a.code
            if not code:
                raise GoldenError("--cpu recomp needs --code LIB")
    ignore = ([IPC_TIME_DEPENDENT] if IPC_TIME_DEPENDENT.is_file() else []) + [Path(p) for p in a.ignore or []]
    real = int(manifest.get("real_scene_changes", real_scene_changes(ref / "state.txt")))
    cfg = Compare(profile, a.fallback, strict=not foreign, ignore=ignore, observable=Path(a.observable),
                  real_changes=real, **rules)
    pair = Compare("same", a.fallback, strict=not foreign, ref_is_recording=False, observable=Path(a.observable),
                   real_changes=real, checkpoints=False)
    checks_dir = ref / "check"
    if checks_dir.exists():
        shutil.rmtree(checks_dir)
    results: list[Result] = []
    for n in range(1, a.repeat + 1):
        outdir = checks_dir / f"run{n}"
        work = checks_dir / f"work{n}"
        copy_tree(ref / "user-before", work)
        spec = RunSpec(runner, program, code, cpu, a.update or [], work, outdir, session, a.set or [], extra, full,
                       shot_every, str(a.observable))
        status, _ = run_runner(spec, a.timeout)
        shutil.rmtree(work, ignore_errors=True)
        prefix = f"run{n}: " if a.repeat > 1 else ""
        if status != 0:
            results.append(Result("runner", "fail", f"exit status {status}", prefix))
        for r in compare_outputs(session, ref, outdir, cfg):
            r.run = prefix
            results.append(r)
    for n in range(2, a.repeat + 1):          # the runs against each other, in the same profile
        for r in compare_outputs(session, checks_dir / "run1", checks_dir / f"run{n}", pair):
            r.run = f"run{n} against run1: "
            results.append(r)
    for r in results:
        detail = r.detail.splitlines()[0] if r.detail and r.status != "pass" else ""
        shown = "; ".join(x for x in (detail, r.note) if x)
        print(f"{session.name}: {r.label}: {r.status.upper()}" + (f" ({shown})" if shown else ""))
    count = {s: sum(r.status == s for r in results) for s in ("pass", "fail", "skipped", "info")}
    print(f"{session.name}: {count['pass']} passed, {count['fail']} failed, {count['skipped']} skipped"
          + (f", {count['info']} info" if count["info"] else "") + f" (profile {profile}"
          + (", D30 fallback" if a.fallback else "") + ")")
    status = exit_status(results, require)
    if status == 3:
        gated = [r for r in results if r.status != "info"]
        if not any(r.status == "pass" for r in gated):
            print(f"{session.name}: nothing was compared: no gated comparison passed (exit 3)")
        for name in require:
            if not any(r.base == name and r.status != "skipped" for r in gated) or \
                    any(r.base == name and r.status == "skipped" for r in gated):
                print(f"{session.name}: --require {name}: skipped or not run (exit 3)")
    checks_dir.mkdir(parents=True, exist_ok=True)
    (checks_dir / "result.json").write_text(
        json.dumps([dataclasses.asdict(r) | {"label": r.label} for r in results], indent=2) + "\n", encoding="utf-8")
    return status


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
        caps = ",".join(c.text() for c in s.captures) or "-"
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
        s.add_argument("--runner", required=(name == "record"),
                       help="the reference runner (mh4u_runner, or a .py script); a check with "
                            "--candidate-runner and --candidate-program does not need it")
        s.add_argument("--game", required=(name == "record"),
                       help="the CXI; a check with --candidate-runner and --candidate-program does not need it")
        s.add_argument("--update", action="append", help="the update CIA (repeatable)")
        s.add_argument("--user", required=(name == "record"), help="the user folder")
        s.add_argument("--code", help="the code library (.dll or .so) for --cpu recomp")
        s.add_argument("--cpu", choices=["jit", "recomp"])
        s.add_argument("--set", action="append", help="MS:KEY=VALUE for the runner (repeatable)")
        s.add_argument("--runner-args", help="more reference-runner options, one string")
        s.add_argument("--timeout", type=float, default=None, help="seconds before the runner is killed")
        if name == "record":
            s.add_argument("--shot-every-frames", type=int, default=DEFAULT_SHOT_EVERY_FRAMES, metavar="N",
                           help=f"a screenshot every N game frames from frame {SHOTS_FROM_FRAME} "
                                f"(default {DEFAULT_SHOT_EVERY_FRAMES}, 0: none)")
            s.add_argument("--save", help="a save folder, merged into user-before/ (sessions that need one)")
            s.add_argument("--force", action="store_true", help="record over an existing recording")
            s.add_argument("--allow-draft", action="store_true", help="record a draft session")
        else:
            s.add_argument("--candidate-runner")
            s.add_argument("--candidate-program", help="its program (the native runner: a prepare_game.py folder)")
            s.add_argument("--candidate-code", help="its code library (a --native library)")
            s.add_argument("--candidate-args", help="more options for the candidate, one string")
            s.add_argument("--repeat", type=int, default=1,
                           help="runs; besides each against the recording, the runs against each other (same profile)")
            s.add_argument("--profile", choices=PROFILES,
                           help="same (two runs of one backend), cross (two timing models) or native (D76: the native "
                                "runtime in real time against the reference); the default is same or cross, following "
                                "--candidate-runner and --candidate-args, never native")
            s.add_argument("--fallback", action="store_true",
                           help="D30's fallback of the same profile (only if N0-29 ended needs-decision)")
            s.add_argument("--require", help="comma-separated comparisons that must not be skipped: "
                                             + ", ".join(REQUIRABLE))
            s.add_argument("--ignore", action="append",
                           help="more IPC ignore files, cross and native profiles only (repeatable)")
            s.add_argument("--tolerance", type=int, metavar="N",
                           help=f"cross and native profiles only: frames a real scene change may move by (default "
                                f"{CROSS_TOLERANCE} in cross, D50; {NATIVE_TOLERANCE} in native, D76)")
            s.add_argument("--min-margin", type=int, metavar="N",
                           help=f"cross and native profiles only: frames every input must follow the last real scene "
                                f"change by, on both sides (default {CROSS_MIN_MARGIN}, D50)")
            s.add_argument("--max-surplus", type=int, metavar="N",
                           help=f"cross and native profiles only: fs:USER/file reads one side's thread may make "
                                f"beyond the other's (default {CROSS_MAX_SURPLUS}, D52)")
            s.add_argument("--max-shift", type=int, metavar="N",
                           help=f"cross and native profiles only: DSP frames the candidate's audio may be shifted by "
                                f"(default {CROSS_MAX_SHIFT} in cross, D51; {NATIVE_MAX_SHIFT} in native, D76)")
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
