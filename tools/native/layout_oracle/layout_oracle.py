#!/usr/bin/env python3
"""The oracle of the native screen layouts (N2b-04): what Azahar's own code prints.

    layout_oracle.py --azahar ~/azahar --out native/tests/screen_layout_expected.inc

copies Azahar's framebuffer_layout.{cpp,h}, emu_window.{cpp,h} and the files they need into a
temporary folder, applies the hunks of port/azahar/azahar.patch for the files in
src/core/frontend/ (git apply --include), compiles them with clang and the stand-ins of
standin/azahar_standin.h, runs every case of cases.txt through driver.cpp and writes the answers as
the C++ table native/tests/screen_layout_expected.inc. Nothing in the table is computed or written
by hand: the native code (native/src/screen_layout/) and its test are compared with it.

Cases (cases.txt), one per line, `#` starts a comment:

    case <name> key=value ...      one case
    sweep <name> key=a,b key=c,d   the cases of every combination of the lists (the names are
                                   <name>_<value>_<value>...)

Keys (all optional; a case is the settings of one window, then touches on it):
    mode=overlay|stacked|single|side     window=WxH         top_width=N (0: 400)
    aspect=default|stretch|4:3|16:9|16:10|21:9
    corner=tl|tr|bl|br   height=F   opacity=F   crop=L,T,R,B   at=X,Y
    view=H,X,Y,L,T,R,B   (up to two)     ops=P:x,y/M:x,y/R   (press, move, release)
Floats are float32 values, passed to the driver exactly (hex floats).

Driver input, one line per case:
    case <id> <mode> <w> <h> <top_width> <aspect> <corner 0-3> <height> <opacity> <cl> <ct> <cr> <cb>
         <x> <y> <view0: height x y cl ct cr cb> <view1: ...> <ops: P,x,y;M,x,y;R or ->
Driver output:
    layout <id> <w> <h> <top_enabled> <bottom_enabled> <top l t r b> <bottom l t r b>
           <top_opacity> <bottom_opacity> <crop l t r b> <view_count> {<view l t r b> <crop l t r b>}
    step <id> <index> <P|M|R> <handled -1|0|1> <pressed> <x> <y> <pixel_x> <pixel_y>
"""
from __future__ import annotations

import argparse
import dataclasses
import hashlib
import itertools
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
DEFAULT_CASES = os.path.join(HERE, "cases.txt")
PATCH = os.path.join(ROOT, "port", "azahar", "azahar.patch")
PINNED = os.path.join(ROOT, "port", "azahar", "COMMIT")

MODES = ("overlay", "stacked", "single", "side")
ASPECTS = ("default", "stretch", "4:3", "16:9", "16:10", "21:9")
CORNERS = {"tl": 0, "tr": 1, "bl": 2, "br": 3}

# Azahar files copied as they are (relative to src/): the frontend ones are patched afterwards
COPIED = (
    "common/common_types.h",
    "common/math_util.h",
    "core/3ds.h",
    "core/frontend/emu_window.cpp",
    "core/frontend/emu_window.h",
    "core/frontend/framebuffer_layout.cpp",
    "core/frontend/framebuffer_layout.h",
)
# the Azahar headers that the stand-in header replaces: each is a forwarding header
FORWARDED = ("common/settings.h", "common/assert.h", "core/frontend/input.h")


class CaseError(ValueError):
    pass


def f32(value: float) -> float:
    """The float32 nearest to value (what the C++ code gets)."""
    return struct.unpack("<f", struct.pack("<f", value))[0]


def hexfloat(value: float) -> str:
    return f32(value).hex()


@dataclasses.dataclass(frozen=True)
class View:
    height: float = 0.0
    x: float = 1.0
    y: float = 0.0
    crop: tuple[float, float, float, float] = (0.0, 0.0, 1.0, 1.0)


@dataclasses.dataclass(frozen=True)
class Case:
    name: str
    mode: str = "stacked"
    width: int = 800
    height: int = 480
    top_width: int = 400
    aspect: str = "default"
    corner: str = "br"
    overlay_height: float = 0.0
    opacity: float = 1.0
    crop: tuple[float, float, float, float] = (0.0, 0.0, 1.0, 1.0)
    at: tuple[float, float] = (-1.0, -1.0)
    views: tuple[View, ...] = ()
    ops: tuple[tuple[str, int, int], ...] = ()


def _floats(text: str, count: int, key: str) -> tuple[float, ...]:
    parts = text.split(",")
    if len(parts) != count:
        raise CaseError(f"{key}= needs {count} numbers: {text!r}")
    try:
        return tuple(float(p) for p in parts)
    except ValueError:
        raise CaseError(f"{key}= has a bad number: {text!r}") from None


def _ops(text: str) -> tuple[tuple[str, int, int], ...]:
    ops = []
    for item in text.split("/"):
        if item == "R":
            ops.append(("R", 0, 0))
            continue
        m = re.fullmatch(r"([PM]):(\d+),(\d+)", item)
        if not m:
            raise CaseError(f"bad touch op {item!r} (P:x,y M:x,y R)")
        ops.append((m.group(1), int(m.group(2)), int(m.group(3))))
    return tuple(ops)


def build_case(name: str, settings: dict[str, list[str]]) -> Case:
    """A Case from key -> values (a list: `view` may repeat)."""
    values: dict[str, object] = {}
    views: list[View] = []
    for key, vals in settings.items():
        if key == "view":
            for v in vals:
                h, x, y, cl, ct, cr, cb = _floats(v, 7, "view")
                views.append(View(h, x, y, (cl, ct, cr, cb)))
            continue
        if len(vals) != 1:
            raise CaseError(f"{name}: {key}= given more than once")
        v = vals[0]
        if key == "mode":
            if v not in MODES:
                raise CaseError(f"{name}: mode={v}: one of {', '.join(MODES)}")
            values["mode"] = v
        elif key == "window":
            m = re.fullmatch(r"(\d+)x(\d+)", v)
            if not m:
                raise CaseError(f"{name}: window={v}: WxH")
            values["width"], values["height"] = int(m.group(1)), int(m.group(2))
        elif key == "top_width":
            values["top_width"] = int(v)
        elif key == "aspect":
            if v not in ASPECTS:
                raise CaseError(f"{name}: aspect={v}: one of {', '.join(ASPECTS)}")
            values["aspect"] = v
        elif key == "corner":
            if v not in CORNERS:
                raise CaseError(f"{name}: corner={v}: one of tl tr bl br")
            values["corner"] = v
        elif key == "height":
            values["overlay_height"] = float(v)
        elif key == "opacity":
            values["opacity"] = float(v)
        elif key == "crop":
            values["crop"] = _floats(v, 4, "crop")
        elif key == "at":
            values["at"] = _floats(v, 2, "at")
        elif key == "ops":
            values["ops"] = _ops(v)
        else:
            raise CaseError(f"{name}: unknown key {key}=")
    if len(views) > 2:
        raise CaseError(f"{name}: at most two views")
    return Case(name=name, views=tuple(views), **values)  # type: ignore[arg-type]


def parse_cases(text: str) -> list[Case]:
    cases: list[Case] = []
    seen: set[str] = set()
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        words = line.split()
        kind = words[0]
        if kind not in ("case", "sweep") or len(words) < 2:
            raise CaseError(f"line {number}: expected `case <name> ...` or `sweep <name> ...`")
        name = words[1]
        if not re.fullmatch(r"[A-Za-z0-9_]+", name):
            raise CaseError(f"line {number}: bad name {name!r}")
        settings: dict[str, list[str]] = {}
        for word in words[2:]:
            if "=" not in word:
                raise CaseError(f"line {number}: expected key=value, got {word!r}")
            key, value = word.split("=", 1)
            settings.setdefault(key, []).append(value)
        try:
            if kind == "case":
                expanded = [(name, settings)]
            else:
                axes = [(k, v[0].split(",")) for k, v in settings.items() if len(v) == 1 and "," in v[0]
                        and k not in ("crop", "at", "view", "ops")]
                fixed = {k: v for k, v in settings.items() if k not in dict(axes)}
                expanded = []
                for combo in itertools.product(*(vals for _, vals in axes)):
                    one = dict(fixed)
                    for (k, _), v in zip(axes, combo):
                        one[k] = [v]
                    label = "_".join(re.sub(r"[^A-Za-z0-9]", "", v) for v in combo)
                    expanded.append((f"{name}_{label}" if label else name, one))
            for case_name, one in expanded:
                if case_name in seen:
                    raise CaseError(f"line {number}: two cases named {case_name}")
                seen.add(case_name)
                cases.append(build_case(case_name, one))
        except CaseError as error:
            raise CaseError(f"line {number}: {error}") from None
    return cases


def driver_line(case: Case) -> str:
    views = list(case.views) + [View()] * (2 - len(case.views))
    fields = [
        "case", case.name, case.mode, str(case.width), str(case.height), str(case.top_width),
        case.aspect, str(CORNERS[case.corner]), hexfloat(case.overlay_height), hexfloat(case.opacity),
        *(hexfloat(c) for c in case.crop), *(hexfloat(c) for c in case.at),
    ]
    for view in views:
        fields += [hexfloat(view.height), hexfloat(view.x), hexfloat(view.y), *(hexfloat(c) for c in view.crop)]
    fields.append(";".join(op if op == "R" else f"{op},{x},{y}" for op, x, y in case.ops) if case.ops else "-")
    return " ".join(fields)


def driver_input(cases: list[Case]) -> str:
    return "".join(driver_line(c) + "\n" for c in cases)


@dataclasses.dataclass
class Result:
    layout: list[str]                 # the tokens after `layout <id>`
    steps: list[list[str]]            # the tokens after `step <id>`


def parse_driver_output(text: str, cases: list[Case]) -> dict[str, Result]:
    results: dict[str, Result] = {}
    for raw in text.splitlines():
        words = raw.split()
        if not words:
            continue
        if words[0] == "layout":
            results[words[1]] = Result(words[2:], [])
        elif words[0] == "step":
            results[words[1]].steps.append(words[2:])
        else:
            raise CaseError(f"unexpected driver output: {raw!r}")
    missing = [c.name for c in cases if c.name not in results]
    if missing:
        raise CaseError(f"no answer for {missing[:3]}")
    for case in cases:
        if len(results[case.name].steps) != len(case.ops):
            raise CaseError(f"{case.name}: {len(results[case.name].steps)} answers for {len(case.ops)} touches")
    return results


def _int_list(tokens: list[str]) -> str:
    return ", ".join(tokens)


def render_inc(cases: list[Case], results: dict[str, Result], commit: str, cases_sha: str) -> str:
    """The table: one MH4N_CASE per case. The inputs are echoed from the cases, the answers are the
    driver's, byte for byte (floats as exact hex floats)."""
    out = [
        "// Generated by tools/native/layout_oracle/layout_oracle.py. DO NOT EDIT.",
        f"// Azahar {commit} with port/azahar/azahar.patch; cases.txt sha256 {cases_sha}.",
        "// MH4N_CASE(name, mode, window_w, window_h, top_width, aspect, corner, overlay_height, opacity,",
        "//           crop{l,t,r,b}, at{x,y}, views{{height,x,y,crop{l,t,r,b}}...},",
        "//           layout{w, h, top_shown, bottom_shown, top{l,t,r,b}, bottom{l,t,r,b}, top_opacity,",
        "//                  bottom_opacity, bottom_crop{l,t,r,b}, view_count, views{{rect, crop}...}},",
        "//           steps{{op, x, y, handled, pressed, fx, fy, px, py}...})",
    ]
    for case in cases:
        r = results[case.name].layout
        w, h, top_on, bot_on = r[0], r[1], r[2], r[3]
        top, bottom = r[4:8], r[8:12]
        top_op, bot_op = r[12], r[13]
        crop = r[14:18]
        count = int(r[18])
        views = [r[19 + 8 * i: 27 + 8 * i] for i in range(count)]
        views_in = ", ".join(
            "{" + f"{hexfloat(v.height)}, {hexfloat(v.x)}, {hexfloat(v.y)}, "
            f"{{{', '.join(hexfloat(c) for c in v.crop)}}}" + "}" for v in case.views)
        views_out = ", ".join(f"{{{{{_int_list(v[:4])}}}, {{{_int_list(v[4:])}}}}}" for v in views)
        steps = []
        for (op, x, y), s in zip(case.ops, results[case.name].steps):
            # s: index op handled pressed fx fy px py
            steps.append(f"{{'{op}', {x}, {y}, {s[2]}, {s[3]}, {s[4]}, {s[5]}, {s[6]}, {s[7]}}}")
        out.append(
            f'MH4N_CASE("{case.name}", "{case.mode}", {case.width}, {case.height}, {case.top_width}, '
            f'"{case.aspect}", {CORNERS[case.corner]}, {hexfloat(case.overlay_height)}, {hexfloat(case.opacity)}, '
            f"{{{', '.join(hexfloat(c) for c in case.crop)}}}, "
            f"{{{', '.join(hexfloat(c) for c in case.at)}}}, "
            f"{{{views_in}}},\n"
            f"    {{{w}, {h}, {top_on}, {bot_on}, {{{_int_list(top)}}}, {{{_int_list(bottom)}}}, {top_op}, {bot_op}, "
            f"{{{_int_list(crop)}}}, {count}, {{{views_out}}}}},\n"
            f"    {{{', '.join(steps)}}})")
    return "\n".join(out) + "\n"


def pinned_commit() -> str:
    with open(PINNED, encoding="utf-8") as f:
        return f.read().strip()


def azahar_commit(azahar: str) -> str:
    return subprocess.run(["git", "-C", azahar, "rev-parse", "HEAD"], capture_output=True, text=True,
                          check=True).stdout.strip()


def prepare_tree(azahar: str, tree: str) -> None:
    """Copy Azahar's files into tree/src, patch the frontend ones, write the forwarding headers."""
    for rel in COPIED:
        dst = os.path.join(tree, "src", rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(os.path.join(azahar, "src", rel), dst)
    # the port's hunks for the frontend files (and top_screen.h, which the patch adds)
    # (git apply works on the repository around the current directory when there is one: the ceiling
    # keeps it to the temporary folder, which may be inside a checkout)
    stale = os.path.join(tree, "src", "core", "frontend", "top_screen.h")
    if os.path.exists(stale):
        os.remove(stale)                                     # a kept folder of an earlier run
    env = dict(os.environ, GIT_CEILING_DIRECTORIES=os.path.dirname(os.path.abspath(tree)))
    subprocess.run(["git", "apply", "--include=src/core/frontend/*", "-p1", PATCH], cwd=tree, check=True,
                   capture_output=True, text=True, env=env)
    if not os.path.isfile(os.path.join(tree, "src", "core", "frontend", "top_screen.h")):
        raise RuntimeError("the port's patch did not apply to the copied files")
    for rel in FORWARDED:
        dst = os.path.join(tree, "src", rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "w", encoding="utf-8") as f:
            f.write('#pragma once\n#include "azahar_standin.h"\n')
    shutil.copyfile(os.path.join(HERE, "standin", "azahar_standin.h"), os.path.join(tree, "src", "azahar_standin.h"))


def compile_driver(tree: str, cxx: str) -> str:
    exe = os.path.join(tree, "layout_driver")
    src = os.path.join(tree, "src")
    cmd = [cxx, "-std=c++20", "-O1", "-Wall", "-Wno-unused", "-I", src, "-I", os.path.join(src, "common"),
           os.path.join(src, "core", "frontend", "framebuffer_layout.cpp"),
           os.path.join(src, "core", "frontend", "emu_window.cpp"),
           os.path.join(HERE, "driver.cpp"), "-o", exe]
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode != 0:
        raise RuntimeError("compiling Azahar's layout code failed:\n" + done.stderr[-4000:])
    return exe


def render_from_run(azahar: str, cases: list[Case], cxx: str, keep: str | None, commit: str, cases_sha: str) -> str:
    tmp = keep or tempfile.mkdtemp(prefix="layout_oracle_")
    try:
        os.makedirs(tmp, exist_ok=True)
        prepare_tree(azahar, tmp)
        exe = compile_driver(tmp, cxx)
        done = subprocess.run([exe], input=driver_input(cases), capture_output=True, text=True)
        if done.returncode != 0:
            raise RuntimeError(f"the driver failed ({done.returncode}):\n{done.stderr[-2000:]}")
        return render_inc(cases, parse_driver_output(done.stdout, cases), commit, cases_sha)
    finally:
        if keep is None:
            shutil.rmtree(tmp, ignore_errors=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--azahar", required=True, help="Azahar's sources at the pinned commit (~/azahar)")
    parser.add_argument("--out", required=True, help="the table to write (native/tests/screen_layout_expected.inc)")
    parser.add_argument("--cases", default=DEFAULT_CASES)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--keep", help="build in this folder and keep it")
    args = parser.parse_args(argv)
    try:
        with open(args.cases, encoding="utf-8") as f:
            text = f.read()
        cases = parse_cases(text)
        azahar = os.path.abspath(os.path.expanduser(args.azahar))
        commit = azahar_commit(azahar)
        if commit != pinned_commit():
            raise RuntimeError(f"{azahar} is at {commit}, the pinned commit is {pinned_commit()}")
        table = render_from_run(azahar, cases, args.cxx, args.keep, commit,
                                hashlib.sha256(text.encode("utf-8")).hexdigest())
    except (CaseError, RuntimeError, subprocess.CalledProcessError, OSError) as error:
        detail = getattr(error, "stderr", "") or ""
        print(f"layout_oracle: {error}\n{detail}", file=sys.stderr)
        return 1
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(table)
    print(f"layout_oracle: {len(cases)} cases -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
