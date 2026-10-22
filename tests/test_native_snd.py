"""Our own DSP test programs (port/tests/snd_*.c, N3-01) and their expected output, which is the
reference runner's (Azahar's HLE DSP): see docs/native/snd-tests.md. No game code, no game data.

- Always: every program builds, recompiles and links into a code library (paged and native); the
  expected files are well formed (the format of snd-tests.md, 300 to 400 frames), and snd_pcm's
  expected output shows the interpolation modes at work (none differs from linear; polyphase is
  Azahar's linear).
- With MH4U_RUNNER (the reference runner, `--cpu jit --new-3ds off`): the output equals the expected
  file, twice in a row.
- With MH4N_RUNNER (the native runner, started with --deterministic and a deterministic native
  library): the `s<i>` lines equal the expected file's; the `mix` lines too when MH4N_SND_REQUIRE
  names `pcm`.
- MH4N_SND_REQUIRE is a comma-separated list of `reference`, `native` and `pcm`: a runner it names
  that is not set is a failure (otherwise the test is skipped).

To rewrite the expected files from the reference runner (they must come out the same twice):
    MH4U_RUNNER=path/to/run.sh python3 tests/test_native_snd.py --regenerate [PROGRAM ...]
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_conformance as tc  # noqa: E402  (the helpers: building, running, reading the output)

PROGRAMS = ["snd_pcm", "snd_adpcm", "snd_mix"]
EXPECTED_DIR = os.path.join(tc.EXPECTED, "snd")
KNOWN_REQUIRE = {"reference", "native", "pcm"}
MAX_FRAMES = 400
MIN_FRAME_LINES = 300

SOURCE_LINE = re.compile(r"f(\d+): s(\d+) en=([01]) pos=(\d+) buf=(\d+) last=(\d+) sync=(\d+)")
MIX_LINE = re.compile(r"f(\d+): mix crc=([0-9a-f]{8}) peak=(\d+)")


def required() -> set[str]:
    return {t.strip() for t in os.environ.get("MH4N_SND_REQUIRE", "").split(",") if t.strip()}


def runner_or_skip(variable: str, name: str) -> str:
    """The runner's path; a skip when it is not set, a failure when MH4N_SND_REQUIRE names it."""
    path = os.environ.get(variable)
    if path:
        return path
    if name in required():
        pytest.fail(f"MH4N_SND_REQUIRE names {name}, but {variable} is not set")
    pytest.skip(f"{variable} not set")


# ── reading the output ─────────────────────────────────────────────────────────────

def expected_lines(program: str) -> list[str]:
    with open(os.path.join(EXPECTED_DIR, program + ".txt"), encoding="utf-8") as f:
        return f.read().splitlines()


def frame_lines(lines: list[str]) -> list[str]:
    return [line for line in lines if re.match(r"f\d+: ", line)]


def problems(program: str, lines: list[str]) -> list[str]:
    """Everything that is wrong with a program's output, as a list of messages (empty: well formed)."""
    found: list[str] = []
    if not lines or lines[0] != f"{program}: start":
        found.append("the first line is not '<prog>: start'")
    if not lines or lines[-1] != f"{program}: done":
        found.append("the last line is not '<prog>: done'")
    frame = -1          # the frame being read
    last_source = -1    # the last source line's index in it
    mixed = True        # the frame's mix line has been read
    for number, line in enumerate(lines[1:-1], start=2):
        s, m = SOURCE_LINE.fullmatch(line), MIX_LINE.fullmatch(line)
        if not s and not m:
            found.append(f"line {number}: not a source or mix line: {line!r}")
            continue
        k = int((s or m).group(1))
        if k != frame:
            if not mixed:
                found.append(f"line {number}: frame {frame} has no mix line")
            if k != frame + 1:
                found.append(f"line {number}: frame {k} follows frame {frame}")
            frame, last_source, mixed = k, -1, False
        if mixed:
            found.append(f"line {number}: a line after frame {k}'s mix line")
        if s:
            if int(s.group(2)) <= last_source or int(s.group(2)) >= 24:
                found.append(f"line {number}: sources out of order")
            last_source = int(s.group(2))
            if frame == 0 and last_source == 0 and s.group(3) != "1":
                found.append("source 0 is not enabled in frame 0")
        else:
            mixed = True
            if int(m.group(3)) > 32768:
                found.append(f"line {number}: peak above 32768")
    if not mixed:
        found.append(f"frame {frame} has no mix line")
    if frame + 1 > MAX_FRAMES:
        found.append(f"{frame + 1} frames (at most {MAX_FRAMES})")
    if len(frame_lines(lines)) < MIN_FRAME_LINES:
        found.append(f"{len(frame_lines(lines))} frame lines (at least {MIN_FRAME_LINES})")
    for line in lines:
        if line.startswith(("missed", "FAIL")) or "OutputDebugString" in line:
            found.append(f"failure line: {line!r}")
    return found


def crcs_by_sync(lines: list[str]) -> dict[int, list[str]]:
    """The final-mix CRCs of the frames, grouped by the sync count that source 0 reports in them."""
    groups: dict[int, list[str]] = {}
    sync = -1
    for line in lines:
        s, m = SOURCE_LINE.fullmatch(line), MIX_LINE.fullmatch(line)
        if s and s.group(2) == "0":
            sync = int(s.group(7))
        elif m:
            groups.setdefault(sync, []).append(m.group(2))
    return groups


def native_view(lines: list[str], pcm: bool) -> list[str]:
    """What the native runner is held to: the source lines, and the mix lines when `pcm`."""
    return [line for line in lines
            if line.endswith((": start", ": done")) or SOURCE_LINE.fullmatch(line) or (pcm and MIX_LINE.fullmatch(line))]


# ── synthetic output: the checks above on lines made here ──────────────────────────────

def synthetic(frames: int, extra: list[str] | None = None) -> list[str]:
    lines = ["snd_x: start"]
    for k in range(frames):
        lines.append(f"f{k}: s0 en=1 pos={k} buf=1 last=0 sync=1")
        lines.append(f"f{k}: mix crc={k:08x} peak=7")
    return lines + (extra or []) + ["snd_x: done"]


def test_well_formed_output_has_no_problems():
    assert problems("snd_x", synthetic(MIN_FRAME_LINES)) == []


@pytest.mark.parametrize("damage, expect", [
    (lambda l: l[:1] + l[3:], "frame 1 follows"),                    # frame 0 dropped
    (lambda l: l[:-2] + l[-1:], "has no mix line"),                  # the last mix line dropped
    (lambda l: l[:-1] + ["missed 3", l[-1]], "failure line"),
    (lambda l: l[:-1] + ["FAIL LoadComponent 00000001", l[-1]], "failure line"),
    (lambda l: l[:1] + ["junk"] + l[1:], "not a source or mix line"),
    (lambda l: l[:2] + [l[2].replace("peak=7", "peak=40000")] + l[3:], "peak above"),
    (lambda l: l[:1] + [l[1].replace("en=1", "en=0")] + l[2:], "not enabled in frame 0"),
    (lambda l: l[:-1], "last line"),
])
def test_malformed_output_is_found(damage, expect):
    found = problems("snd_x", damage(synthetic(MIN_FRAME_LINES)))
    assert any(expect in p for p in found), found


def test_too_short_and_too_long_are_found():
    assert any("frame lines" in p for p in problems("snd_x", synthetic(10)))
    assert any("at most" in p for p in problems("snd_x", synthetic(MAX_FRAMES + 1)))


def test_crcs_are_grouped_by_the_sync_count():
    lines = ["p: start",
             "f0: s0 en=1 pos=0 buf=1 last=0 sync=1", "f0: mix crc=00000001 peak=0",
             "f1: s0 en=1 pos=9 buf=1 last=0 sync=1", "f1: mix crc=00000002 peak=1",
             "f2: s0 en=1 pos=0 buf=2 last=0 sync=2", "f2: s1 en=1 pos=0 buf=2 last=0 sync=7",
             "f2: mix crc=00000003 peak=1", "p: done"]
    assert crcs_by_sync(lines) == {1: ["00000001", "00000002"], 2: ["00000003"]}


def test_native_view_keeps_source_lines_and_optionally_mix_lines():
    lines = synthetic(2)
    assert native_view(lines, pcm=False) == [l for l in lines if "mix" not in l]
    assert native_view(lines, pcm=True) == lines


def test_require_names_are_known():
    assert required() <= KNOWN_REQUIRE, required() - KNOWN_REQUIRE


# ── building and running ───────────────────────────────────────────────────────────

pytestmark_toolchain = pytest.mark.skipif(not tc.have_toolchain(), reason="clang/lld toolchain not installed")


def build_native_deterministic(out: str) -> str:
    """The deterministic native code library of a program whose recompilation is in `out`."""
    from recomp import build as rbuild
    return rbuild.build(out, 2, "O1", rbuild.native_defines(True, True), "mh4u_code_native_det.so",
                        [rbuild.DLL_GLUE], extra_flags=["-fvisibility=hidden"])


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    cache: dict[tuple[str, str], tuple[str, str]] = {}

    def get(program: str, kind: str = "paged") -> tuple[str, str]:
        """(elf, library) of a program; kind is paged, native or native-det. All share one directory
        (one recompilation, as it does not depend on the mode)."""
        if (program, kind) not in cache:
            sibling = next((v for (p, _), v in cache.items() if p == program), None)
            if kind == "native-det":
                if sibling is None:
                    sibling = get(program, "paged")
                cache[(program, kind)] = (sibling[0], build_native_deterministic(os.path.dirname(sibling[0])))
            else:
                out = os.path.dirname(sibling[0]) if sibling else str(tmp_path_factory.mktemp(program))
                cache[(program, kind)] = tc.build_program(out, program, native=(kind == "native"))
        return cache[(program, kind)]
    return get


@pytestmark_toolchain
@pytest.mark.parametrize("program", PROGRAMS)
def test_program_builds_and_recompiles(built, program):
    elf, lib = built(program)
    assert os.path.exists(elf) and os.path.exists(lib)
    assert os.path.exists(built(program, "native")[1])
    assert os.path.exists(built(program, "native-det")[1])


@pytest.mark.parametrize("program", PROGRAMS)
def test_program_is_short(program):
    with open(os.path.join(tc.PORT_TESTS, program + ".c"), encoding="utf-8") as f:
        assert len(f.read().splitlines()) < tc.MAX_SOURCE_LINES


def test_programs_are_known_and_not_conformance_programs():
    import build_svc_test
    assert set(PROGRAMS) <= set(build_svc_test.programs())
    assert not set(PROGRAMS) & set(tc.PROGRAMS)      # never in test_conformance.py's PROGRAMS
    assert not any(p.startswith("conf_") for p in PROGRAMS)
    assert sorted(os.listdir(EXPECTED_DIR)) == sorted(p + ".txt" for p in PROGRAMS)


@pytest.mark.parametrize("program", PROGRAMS)
def test_expected_file_is_wellformed(program):
    assert problems(program, expected_lines(program)) == []


def test_snd_pcm_expected_shows_the_interpolation_modes():
    """Cases 1-12 of snd_pcm: PCM16 mono at the rates 1.0, 0.5, 1.37 and 2.9, each in the modes none,
    linear, polyphase (sync = 1 + 3 * rate + mode). Linear differs from none whenever the rate makes
    the position fractional (not at 1.0, where the fraction is always 0), and Azahar plays polyphase
    as linear."""
    groups = crcs_by_sync(expected_lines("snd_pcm"))
    for rate in range(4):
        none, linear, polyphase = (groups[1 + 3 * rate + mode] for mode in range(3))
        if rate:
            assert none != linear, f"rate {rate}: none and linear sound the same"
        else:
            assert none == linear
        assert polyphase == linear, f"rate {rate}: polyphase differs from linear"


@pytestmark_toolchain
@pytest.mark.parametrize("program", PROGRAMS)
def test_reference_runner_matches_expected_twice(built, tmp_path, program):
    runner = runner_or_skip("MH4U_RUNNER", "reference")
    elf, lib = built(program)
    first = tc.run_reference(runner, elf, lib, str(tmp_path / "user1"))
    second = tc.run_reference(runner, elf, lib, str(tmp_path / "user2"))
    assert first == expected_lines(program)
    assert second == first


def run_native(runner: str, elf: str, lib: str, user_dir: str) -> list[str]:
    r = subprocess.run([runner, elf, "--code", lib, "--deterministic", "--ms", str(tc.RUN_MS), "--user", user_dir],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=600)
    assert r.returncode == 0, (r.returncode, r.stdout[-2000:])
    return tc.debug_lines(r.stdout)


@pytestmark_toolchain
@pytest.mark.parametrize("program", PROGRAMS)
def test_native_runner_matches_expected(built, tmp_path, program):
    runner = runner_or_skip("MH4N_RUNNER", "native")
    elf, lib = built(program, "native-det")
    got = run_native(runner, elf, lib, str(tmp_path / "user"))
    assert not [l for l in got if l.startswith(("missed", "FAIL"))], got
    pcm = "pcm" in required()
    assert native_view(got, pcm) == native_view(expected_lines(program), pcm)


# ── regenerating the expected files ────────────────────────────────────────────────

def regenerate(programs: list[str]) -> int:
    runner = os.environ.get("MH4U_RUNNER")
    if not runner:
        print("set MH4U_RUNNER to the reference runner", file=sys.stderr)
        return 2
    os.makedirs(EXPECTED_DIR, exist_ok=True)
    status = 0
    for program in programs or PROGRAMS:
        if program not in PROGRAMS:
            print(f"{program}: not a DSP program ({', '.join(PROGRAMS)})", file=sys.stderr)
            return 2
        with tempfile.TemporaryDirectory() as tmp:
            elf, lib = tc.build_program(os.path.join(tmp, "build"), program)
            runs = [tc.run_reference(runner, elf, lib, os.path.join(tmp, f"user{i}")) for i in range(2)]
        bad = problems(program, runs[0])
        if runs[0] != runs[1] or bad:
            print(f"{program}: {'unstable output' if runs[0] != runs[1] else bad[:3]}; not written", file=sys.stderr)
            status = 1
            continue
        with open(os.path.join(EXPECTED_DIR, program + ".txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(runs[0]) + "\n")
        print(f"{program}: {len(runs[0])} lines, {len(frame_lines(runs[0]))} frame lines")
    return status


if __name__ == "__main__":
    if sys.argv[1:2] == ["--regenerate"]:
        sys.exit(regenerate(sys.argv[2:]))
    sys.exit(pytest.main([__file__, *sys.argv[1:]]))
