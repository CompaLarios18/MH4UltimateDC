"""PC port tests (no game data).

The test program port/tests/svc_test.c must recompile cleanly, and its code must build
into a code library with tools/recomp/build.py. If a port build is available
(MH4U_RUNNER=path/to/mh4u_runner; see port/README.md), the program also runs under Azahar's kernel with the recompiled code and
with Azahar's JIT, and both must print the same thing.
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys

import pytest

HERE = os.path.dirname(__file__)
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "port", "tests"))

needed = ["clang", "ld.lld", "llvm-objcopy", "llvm-readelf"]
_TOOLCHAIN_MISSING = any(shutil.which(t) is None for t in needed)


def source_only(test):
    """Marks a test that reads this repository's sources and needs no toolchain (N0-29): the other
    tests of this file are skipped, as before, when the clang/lld toolchain is not installed."""
    test.source_only = True
    return test


@pytest.fixture(autouse=True)
def _toolchain_installed(request):
    if _TOOLCHAIN_MISSING and not getattr(request.function, "source_only", False):
        pytest.skip("clang/lld toolchain not installed")


def test_svc_test_program_recompiles(tmp_path):
    import build_svc_test
    elf = build_svc_test.build(str(tmp_path))
    assert os.path.exists(elf)
    src = os.listdir(tmp_path / "src")
    assert "table.c" in src and any(f.startswith("f") and f.endswith(".c") for f in src)
    # the short-vector routine was found and expanded
    code = "".join((tmp_path / "src" / f).read_text() for f in src if f.endswith(".c"))
    assert "ctx->f.sw[11] = vadd32" in code


def test_code_library_builds(tmp_path):
    import build_svc_test
    from recomp import build as rbuild
    build_svc_test.build(str(tmp_path))
    lib = rbuild.build(str(tmp_path), 2, "O1", ["RC_PAGED"], "code.so", [rbuild.DLL_GLUE],
                       extra_flags=["-fvisibility=hidden"])
    syms = subprocess.run(["llvm-nm", "-D", "--defined-only", lib], capture_output=True, text=True, check=True).stdout
    exported = {line.split()[-1] for line in syms.splitlines()}
    assert "rc_code_init" in exported and "rc_call" not in exported


def emulated_output(binary: str, code: str, elf: str, cpu: str) -> list[str]:
    r = subprocess.run([binary, elf, "--code", code, "--cpu", cpu, "--ms", "5000"], capture_output=True,
                       text=True, timeout=600)
    assert r.returncode == 0, (cpu, r.returncode, r.stdout[-2000:], r.stderr[-2000:])
    lines = []
    for line in (r.stdout + r.stderr).splitlines():
        m = re.search(r"OutputDebugString:\d+: (.*)$", line)
        if m:
            lines.append(m.group(1))
    return lines


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
def test_recompiled_backend_matches_jit(tmp_path):
    binary = os.environ["MH4U_RUNNER"]
    code = os.environ.get("MH4U_TEST_CODE")
    if code:
        elf = os.environ.get("MH4U_TEST_ELF", os.path.join(ROOT, "build", "port-test", "svc_test.elf"))
    else:   # build program and code library here (tools/recomp/build.py --code-library)
        import build_svc_test
        from recomp import build as rbuild
        elf = build_svc_test.build(str(tmp_path))
        code = rbuild.build(str(tmp_path), 2, "O1", ["RC_PAGED"], "code.so", [rbuild.DLL_GLUE],
                            extra_flags=["-fvisibility=hidden"])
    jit = emulated_output(binary, code, elf, "jit")
    recomp = emulated_output(binary, code, elf, "recomp")
    assert jit and jit[0] == "svc_test: start" and jit[-1] == "svc_test: done", jit
    assert recomp == jit


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
def test_interpreter_fallback_matches_jit(tmp_path):
    """Functions missing from the code library (here: whole worker threads and functions
    called through pointers) run in Azahar's interpreter; the output must not change."""
    import build_svc_test
    from recomp import build as rbuild
    binary = os.environ["MH4U_RUNNER"]
    elf = build_svc_test.build(str(tmp_path))
    syms = {}
    for line in subprocess.run(["llvm-readelf", "-s", elf], capture_output=True, text=True, check=True).stdout.splitlines():
        p = line.split()
        if len(p) >= 8 and p[3] == "FUNC":
            syms[p[7]] = int(p[1], 16) & ~1
    drop = {syms[n] for n in ("worker", "op_add", "op_xor", "op_mul")}
    table = tmp_path / "src" / "table.c"
    lines = [ln for ln in table.read_text().splitlines() if not any(f"{{0x{a:08x}u," in ln for a in drop)]
    count = sum(1 for ln in lines if ln.strip().startswith("{0x"))
    table.write_text("\n".join(re.sub(r"rc_func_count = \d+;", f"rc_func_count = {count};", ln) for ln in lines) + "\n")
    code = rbuild.build(str(tmp_path), 2, "O1", ["RC_PAGED"], "code.so", [rbuild.DLL_GLUE],
                        extra_flags=["-fvisibility=hidden"])
    r = subprocess.run([binary, elf, "--code", code, "--cpu", "recomp", "--ms", "5000"], capture_output=True,
                       text=True, timeout=600)
    assert r.returncode == 0, (r.stdout[-2000:], r.stderr[-2000:])
    interpreted = set(re.findall(r"interpreting ([0-9A-F]{8})", r.stdout + r.stderr))
    assert interpreted == {f"{a:08X}" for a in drop}
    assert emulated_output(binary, code, elf, "recomp") == emulated_output(binary, code, elf, "jit")


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
def test_call_counts(tmp_path):
    """A code library built with -D RC_CALL_COUNTS counts every function's calls, and
    --call-counts writes them at an emulated time: the entry point once, each worker thread's entry
    once per thread, the operations as often as the program calls them."""
    import build_svc_test
    from recomp import build as rbuild
    binary = os.environ["MH4U_RUNNER"]
    elf = build_svc_test.build(str(tmp_path))
    code = rbuild.build(str(tmp_path), 2, "O1", ["RC_PAGED", "RC_CALL_COUNTS"], "code.so", [rbuild.DLL_GLUE],
                        extra_flags=["-fvisibility=hidden"])
    syms = {}
    for line in subprocess.run(["llvm-readelf", "-s", elf], capture_output=True, text=True, check=True).stdout.splitlines():
        p = line.split()
        if len(p) >= 8 and p[3] == "FUNC":
            syms[p[7]] = int(p[1], 16) & ~1
    shots = tmp_path / "shots"
    # the program is done after a few emulated milliseconds: the snapshot comes at its end
    r = subprocess.run([binary, elf, "--code", code, "--ms", "5000", "--shots", str(shots),
                        "--call-counts", "4000"], capture_output=True, text=True, timeout=600)
    assert r.returncode == 0, (r.stdout[-2000:], r.stderr[-2000:])
    counts = {int(a, 16): int(n) for a, n in (line.split() for line in (shots / "calls_0004000.txt").read_text().splitlines())}
    assert counts[syms["_start"]] == 1
    assert counts[syms["worker"]] == 3                 # three worker threads
    assert counts[syms["op_add"]] >= 1 and counts[syms["op_mul"]] >= 1
    assert all(n > 0 for n in counts.values())
    # a library without the counters can't give them
    plain = rbuild.build(str(tmp_path), 2, "O1", ["RC_PAGED"], "plain.so", [rbuild.DLL_GLUE],
                         extra_flags=["-fvisibility=hidden"])
    r = subprocess.run([binary, elf, "--code", plain, "--ms", "100", "--call-counts", "50"],
                       capture_output=True, text=True, timeout=600)
    assert r.returncode == 1 and "RC_CALL_COUNTS" in r.stderr


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
@pytest.mark.parametrize("extra", [[], ["--draw-window", "50"], ["--async-io"],
                                   ["--shots-from", "500", "--shot-every", "500"],
                                   ["--dump", "500:100000:40"]])
def test_runner_scripted_input_and_screenshots(tmp_path, extra):
    """port/tests/io_test.c reads HID shared memory and draws into the top framebuffer:
    the runner's --input script must reach it and --shots must save what it drew. With
    --draw-window the GPU skips triangles between screenshots; the program writes its
    framebuffer with the CPU, so nothing it does may change. --async-io only changes how
    file reads finish, and the program reads no files. --shots-from moves the first shot;
    --dump writes guest memory (here the program's first instructions) next to the shots."""
    import build_svc_test
    from recomp import build as rbuild
    binary = os.environ["MH4U_RUNNER"]
    elf = build_svc_test.build(str(tmp_path), "io_test")
    code = rbuild.build(str(tmp_path), 2, "O1", ["RC_PAGED"], "code.so", [rbuild.DLL_GLUE],
                        extra_flags=["-fvisibility=hidden"])
    shots = tmp_path / "shots"
    r = subprocess.run([binary, elf, "--code", code, "--ms", "3000", "--user", str(tmp_path / "user"),
                        "--shots", str(shots), "--input",
                        "300:A,600:TOUCH:10:20:100,900:CIRCLE:1:-0.5:200,1200:START"] + extra,
                       capture_output=True, text=True, timeout=600)
    assert r.returncode == 0, (r.stdout[-2000:], r.stderr[-2000:])
    out = [m.group(1) for m in re.finditer(r"OutputDebugString:\d+: (.*)", r.stdout + r.stderr)]
    events = {}
    for line in out:
        m = re.fullmatch(r"(.*) ([0-9a-f]{8})", line)
        if m:
            events[m.group(1)] = int(m.group(2), 16)
    assert 15 <= events["A pressed, frame"] <= 21          # 300 ms at ~60 polls per second
    assert 69 <= events["START pressed, frame"] <= 75      # 1200 ms
    assert events["touch x"] == 10 and events["touch y"] == 20
    assert "circle right" in events and "circle down" in events
    assert (tmp_path / "user" / "nand").is_dir()
    assert (shots / "0000500_top.ppm").exists() == ("--shots-from" in extra)
    if "--dump" in extra:
        dump = (shots / "mem_0000500_00100000.bin").read_bytes()
        assert len(dump) == 0x40 and any(dump)
    data = (shots / "0001000_top.ppm").read_bytes()
    magic, size, _, pixels = data.split(b"\n", 3)
    width, height = map(int, size.split())
    assert (magic, width, height) == (b"P6", 400, 240)

    def pixel(x, y):
        i = (y * width + x) * 3
        return tuple(pixels[i:i + 3])
    assert pixel(100, 5) == (0, 255, 0) and pixel(100, 120) == (255, 0, 0) and pixel(300, 120) == (0, 0, 255)


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
def test_runner_replaces_memory(tmp_path):
    """--replace MS:FROM:TO changes every FROM in the program's writable memory at an emulated
    time: io_test prints its marker variable once it changes. Read-only memory (the code and its
    constants) is left alone, so the program still runs."""
    import build_svc_test
    from recomp import build as rbuild
    binary = os.environ["MH4U_RUNNER"]
    elf = build_svc_test.build(str(tmp_path), "io_test")
    code = rbuild.build(str(tmp_path), 2, "O1", ["RC_PAGED"], "code.so", [rbuild.DLL_GLUE],
                        extra_flags=["-fvisibility=hidden"])
    run = [binary, elf, "--code", code, "--ms", "1500", "--user", str(tmp_path / "user")]
    r = subprocess.run(run + ["--replace", "500:5534484d:04030201"], capture_output=True, text=True, timeout=600)
    assert r.returncode == 0, (r.stdout[-2000:], r.stderr[-2000:])
    assert re.search(r"replace at 5\d\d ms: 1 found", r.stdout)
    assert "marker 01020304" in r.stdout + r.stderr
    r = subprocess.run(run + ["--replace", "500:55:0102"], capture_output=True, text=True, timeout=600)
    assert r.returncode == 1 and "bad --replace" in r.stderr


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
def test_runner_sets_options_while_running(tmp_path):
    """--set MS:KEY=VALUE changes one of the port's options at an emulated time (one player at
    120 fps once a session is together, say); options that only apply at start-up are refused."""
    import build_svc_test
    from recomp import build as rbuild
    binary = os.environ["MH4U_RUNNER"]
    elf = build_svc_test.build(str(tmp_path), "io_test")
    code = rbuild.build(str(tmp_path), 2, "O1", ["RC_PAGED"], "code.so", [rbuild.DLL_GLUE],
                        extra_flags=["-fvisibility=hidden"])
    run = [binary, elf, "--code", code, "--ms", "1200", "--user", str(tmp_path / "user")]
    r = subprocess.run(run + ["--set", "800:refresh_rate=120", "--set", "500:quest_overlay=map 40",
                              "--set", "600:bloom=off", "--set", "700:depth_of_field=off"],
                       capture_output=True, text=True, timeout=600)
    assert r.returncode == 0, (r.stdout[-2000:], r.stderr[-2000:])
    first = re.search(r"set quest_overlay = map 40 at 5\d\d ms", r.stdout)
    second = re.search(r"set refresh_rate = 120 at 8\d\d ms", r.stdout)
    assert first and second and first.start() < second.start(), r.stdout[-2000:]
    # the screen filters are skipped or drawn as the game runs
    assert re.search(r"set bloom = off at 6\d\d ms", r.stdout), r.stdout[-2000:]
    assert re.search(r"set depth_of_field = off at 7\d\d ms", r.stdout), r.stdout[-2000:]
    r = subprocess.run(run + ["--set", "500:widescreen=16:9"], capture_output=True, text=True, timeout=600)
    assert r.returncode == 1 and "widescreen is not an option that can change" in r.stderr
    for bad in ["fps=120", "500:=120", "x:fps=120"]:
        r = subprocess.run([binary, elf, "--set", bad], capture_output=True, text=True, timeout=60)
        assert r.returncode == 1 and "bad --set" in r.stderr, bad


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
def test_runner_installs_without_a_game(tmp_path):
    """--install CIA with no game file installs into the --user folder and exits (the Windows
    package's script installs the update this way), logging to its own file so that it doesn't
    replace Azahar's log in a shared user folder. A missing CIA fails with a message."""
    binary = os.environ["MH4U_RUNNER"]
    user = tmp_path / "user"
    r = subprocess.run([binary, "--install", str(tmp_path / "missing.cia"), "--user", str(user)],
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 1 and re.search(r"installing .*missing\.cia failed", r.stderr), r.stderr
    assert (user / "log" / "mh4u_runner_log.txt").exists()
    assert not (user / "log" / "azahar_log.txt").exists()
    r = subprocess.run([binary, "--user", str(user)], capture_output=True, text=True, timeout=60)
    assert r.returncode != 0 and "usage" in r.stdout + r.stderr


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
def test_runners_in_lockstep(tmp_path):
    """--lockstep keeps the emulated clocks of runners on one machine together: a runner that
    could go as fast as it likes waits for one that keeps real time, and both finish. --stalls
    reports the steps of emulation that took longer than a given host time."""
    import time
    import build_svc_test
    from recomp import build as rbuild
    binary = os.environ["MH4U_RUNNER"]
    elf = build_svc_test.build(str(tmp_path), "io_test")
    code = rbuild.build(str(tmp_path), 2, "O1", ["RC_PAGED"], "code.so", [rbuild.DLL_GLUE],
                        extra_flags=["-fvisibility=hidden"])
    shared = tmp_path / "clocks"

    def run(speed, index, *extra):
        return subprocess.Popen([binary, elf, "--code", code, "--ms", "1500", "--speed", speed,
                                 "--user", str(tmp_path / f"user{index}"),
                                 "--lockstep", f"{shared}:2:{index}", *extra],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    start = time.monotonic()
    slow = run("100", 0)
    fast = run("0", 1, "--stalls", "0.001")
    out, err = fast.communicate(timeout=600)
    fast_s = time.monotonic() - start
    slow.communicate(timeout=600)
    assert slow.returncode == 0 and fast.returncode == 0, err[-2000:]
    assert fast_s > 1.2                     # held back to the real-time runner's pace
    assert "stall: " in out and "lockstep" not in out
    # alone, the same runner is done long before that
    start = time.monotonic()
    r = subprocess.run([binary, elf, "--code", code, "--ms", "1500", "--user", str(tmp_path / "user9")],
                       capture_output=True, text=True, timeout=600)
    assert r.returncode == 0 and time.monotonic() - start < fast_s
    r = subprocess.run([binary, elf, "--lockstep", f"{shared}:1:0"], capture_output=True, text=True, timeout=600)
    assert r.returncode == 1 and "bad --lockstep" in r.stderr


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
def test_runners_meet_in_a_room(tmp_path):
    """--room-host makes a network room (Azahar's multiplayer) inside the runner and joins
    it; other runners join with --room-join, which is how two runners play the game's local
    wireless modes together. A room turns away a console it already has, as with two copies
    of one user folder, until one of them takes --new-console-id."""
    import socket
    import build_svc_test
    from recomp import build as rbuild
    binary = os.environ["MH4U_RUNNER"]
    elf = build_svc_test.build(str(tmp_path), "io_test")
    code = rbuild.build(str(tmp_path), 2, "O1", ["RC_PAGED"], "code.so", [rbuild.DLL_GLUE],
                        extra_flags=["-fvisibility=hidden"])
    run = [binary, elf, "--code", code, "--speed", "100"]
    first = subprocess.run(run + ["--ms", "100", "--user", str(tmp_path / "a")], capture_output=True, timeout=600)
    assert first.returncode == 0
    shutil.copytree(tmp_path / "a", tmp_path / "b")
    shutil.copytree(tmp_path / "a", tmp_path / "c")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]

    # the program runs for 150 frames, ~2.5 s at real-time speed: the room lives that long
    host = subprocess.Popen(run + ["--ms", "5000", "--user", str(tmp_path / "a"), "--room-host", str(port),
                                   "--nick", "alice"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    host_out = []
    for line in host.stdout:
        host_out.append(line)
        if "room: joined" in line or "room: could not" in line:
            break
    assert any(f"room: joined 127.0.0.1:{port} as alice" in line for line in host_out), host_out
    join = ["--room-join", f"127.0.0.1:{port}"]
    copy = subprocess.run(run + ["--ms", "300", "--user", str(tmp_path / "c"), "--nick", "carol"] + join,
                          capture_output=True, text=True, timeout=120)
    guest = subprocess.run(run + ["--ms", "500", "--user", str(tmp_path / "b"), "--nick", "bobby",
                                  "--new-console-id", "--input", "200:A"] + join,
                           capture_output=True, text=True, timeout=120)
    rest, _ = host.communicate(timeout=120)
    assert host.returncode == 0, "".join(host_out) + rest
    assert copy.returncode == 5 and "room: ConsoleIdCollision" in copy.stdout, copy.stdout
    assert guest.returncode == 0, guest.stdout[-2000:] + guest.stderr[-2000:]
    assert re.search(rf"room: joined 127\.0\.0\.1:{port} as bobby, MAC 00:1F:32(:[0-9A-F]{{2}}){{3}}\n", guest.stdout)
    assert "room: members: alice, bobby\n" in guest.stdout
    assert re.search(r"console id [0-9a-f]{16}\n", guest.stdout)
    assert "A pressed, frame" in guest.stdout + guest.stderr      # and the program ran as usual


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
@pytest.mark.parametrize("mode, waits", [("all", True), ("on", False)])
def test_net_sync_waits_for_a_frozen_console(tmp_path, mode, waits):
    """Net sync: the port's consoles in a network room tell each other their emulated clocks,
    and one that has run ahead of a peer it plays with waits for it. A runner frozen for 2 s
    (--freeze, as a PC stalling on shader compilation would) is waited for, about 2 s less the
    200 ms of slack, instead of being left behind. `on` (the default) only syncs with consoles
    that send game data over local wireless, which these test programs don't."""
    import socket
    import build_svc_test
    from recomp import build as rbuild
    binary = os.environ["MH4U_RUNNER"]
    elf = build_svc_test.build(str(tmp_path), "io_test")
    code = rbuild.build(str(tmp_path), 2, "O1", ["RC_PAGED"], "code.so", [rbuild.DLL_GLUE],
                        extra_flags=["-fvisibility=hidden"])
    run = [binary, elf, "--code", code, "--speed", "100", "--log", "*:Warning Frontend:Info Core.ARM11:Info"]
    first = subprocess.run(run + ["--ms", "100", "--user", str(tmp_path / "a")], capture_output=True, timeout=600)
    assert first.returncode == 0
    shutil.copytree(tmp_path / "a", tmp_path / "b")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    env = dict(os.environ, MH4U_NET_SYNC=mode)
    host = subprocess.Popen(run + ["--ms", "4000", "--user", str(tmp_path / "a"), "--room-host", str(port),
                                   "--nick", "alice", "--freeze", "1500:2000"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    host_out = []
    for line in host.stdout:
        host_out.append(line)
        if "room: joined" in line or "room: could not" in line:
            break
    guest = subprocess.run(run + ["--ms", "4000", "--user", str(tmp_path / "b"), "--nick", "bobby",
                                  "--new-console-id", "--room-join", f"127.0.0.1:{port}"],
                           capture_output=True, text=True, timeout=600, env=env)
    rest, _ = host.communicate(timeout=600)
    out = "".join(host_out) + rest
    assert host.returncode == 0 and guest.returncode == 0, out[-2000:] + guest.stdout[-2000:]
    assert re.search(r"freeze at 1[56]\d\d ms for 2000 ms of host time", out)
    guest_out = guest.stdout + guest.stderr
    waited = re.search(r"net sync: waited (\d+) ms at \d+ ms for 00:1F:32", guest_out)
    if waits:
        assert waited and 1200 <= int(waited.group(1)) <= 2500, guest_out[-3000:]
    else:
        assert not waited, guest_out[-3000:]
    r = subprocess.run([binary, elf, "--freeze", "1500"], capture_output=True, text=True, timeout=60)
    assert r.returncode == 1 and "bad --freeze" in r.stderr


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
@pytest.mark.parametrize("spec, size, expected", [
    # 16:9 top screen (424 wide) filling a 1080p window, the touch screen 30% tall in a corner
    ("top-right 30 85", "1920x1080:424", "top 6,0 1914,1080 opacity 1.00\nbottom 1488,0 1920,324 opacity 0.85\n"),
    ("Bottom_Left 25", "1920x1080:424", "top 6,0 1914,1080 opacity 1.00\nbottom 0,810 360,1080 opacity 0.85\n"),
    ("on", "1920x1080:424", "top 6,0 1914,1080 opacity 1.00\nbottom 1488,0 1920,324 opacity 0.85\n"),
    # a 21:9 window: the game centred, the touch screen in the window's corner (over the bar)
    ("top-left 30 100", "2560x1080:424", "top 326,0 2234,1080 opacity 1.00\nbottom 0,0 432,324 opacity 1.00\n"),
    # off: Azahar's own custom layout (its default positions)
    ("off", "1920x1080", "top 0,0 800,480 opacity 1.00\nbottom 80,500 720,980 opacity 1.00\n"),
])
def test_touch_overlay_layout(spec, size, expected):
    """The port's touch_overlay option turns Azahar's custom screen layout into the top
    screen filling the window and the touch screen drawn over it in a corner."""
    r = subprocess.run([os.environ["MH4U_RUNNER"], "--overlay-layout", spec, size], capture_output=True,
                       text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    assert r.stdout == expected


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
@pytest.mark.parametrize("quest, slot, expected", [
    # the map in the left column's top two slots, at the touch overlay's own scale
    ("map", "0", "top 6,0 1914,1080 opacity 1.00\nbottom 1704,0 1920,184 opacity 0.85\n"
                 "shows 0,36 160,172 of the touch screen\na click at 1812,92 touches 80,104\n"),
    # in the right column's bottom two slots, 40% of the window's height
    ("map 40", "4", "top 6,0 1914,1080 opacity 1.00\nbottom 1412,0 1920,432 opacity 0.85\n"
                    "shows 160,104 320,240 of the touch screen\na click at 1666,216 touches 240,172\n"),
    # off: the whole touch screen
    ("off", "0", "top 6,0 1914,1080 opacity 1.00\nbottom 1488,0 1920,324 opacity 0.85\n"
                 "shows 0,0 320,240 of the touch screen\na click at 1704,162 touches 160,120\n"),
])
def test_quest_map_overlay(quest, slot, expected):
    """With quest_overlay, the touch overlay shows the quest's map panel alone (the touch
    screen's slot grid: two columns of 160 pixels, rows of 68 under a 36-pixel bar), and a
    click on it touches the map where it is on the touch screen."""
    r = subprocess.run([os.environ["MH4U_RUNNER"], "--overlay-layout", "top-right 30 85", "1920x1080:424",
                        quest, slot], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    assert r.stdout == expected


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
@pytest.mark.parametrize("quest, slot, target, tslot, expected", [
    # the map placed at the bottom of the window, in the middle
    ("map 40 at 50 100", "0", None, None,
     "top 6,0 1914,1080 opacity 1.00\nbottom 706,648 1214,1080 opacity 0.85\n"
     "shows 0,36 160,172 of the touch screen\na click at 960,864 touches 80,104\n"),
    # the target camera (slot 2 of the default layout) right under the map, at the same scale;
    # a click on it touches the panel, and a drag that started on it stays on it
    ("map", "0", "on", "2",
     "top 6,0 1914,1080 opacity 1.00\nbottom 1704,0 1920,184 opacity 0.85\n"
     "shows 0,36 160,172 of the touch screen\na click at 1812,92 touches 80,104\n"
     "view 1704,184 1920,276 shows 0,172 160,240 of the touch screen\n"
     "a click at 1812,230 touches 80,206\ndragged to 0,1079 it touches 0,239\n"),
    # the map in the bottom-right corner: the target camera goes over it
    ("map 40 at 100 100", "3", "on", "5",
     "top 6,0 1914,1080 opacity 1.00\nbottom 1412,648 1920,1080 opacity 0.85\n"
     "shows 160,36 320,172 of the touch screen\na click at 1666,864 touches 240,104\n"
     "view 1704,556 1920,648 shows 160,172 320,240 of the touch screen\n"
     "a click at 1812,602 touches 240,206\ndragged to 0,1079 it touches 160,239\n"),
    # placed on its own: the top-left corner, 10% of the window's height
    ("map", "0", "on 10 at 0 0", "2",
     "top 6,0 1914,1080 opacity 1.00\nbottom 1704,0 1920,184 opacity 0.85\n"
     "shows 0,36 160,172 of the touch screen\na click at 1812,92 touches 80,104\n"
     "view 0,0 254,108 shows 0,172 160,240 of the touch screen\n"
     "a click at 127,54 touches 80,206\ndragged to 0,1079 it touches 0,239\n"),
    # off, or no target camera panel in the layout: the map alone
    ("map", "0", "off", "2",
     "top 6,0 1914,1080 opacity 1.00\nbottom 1704,0 1920,184 opacity 0.85\n"
     "shows 0,36 160,172 of the touch screen\na click at 1812,92 touches 80,104\n"),
    ("map", "0", "on", "-1",
     "top 6,0 1914,1080 opacity 1.00\nbottom 1704,0 1920,184 opacity 0.85\n"
     "shows 0,36 160,172 of the touch screen\na click at 1812,92 touches 80,104\n"),
])
def test_quest_panel_views(quest, slot, target, tslot, expected):
    """The quest's map placed anywhere in the window (quest_overlay = map SIZE at X Y), and the
    target camera panel in a view of its own (target_overlay): drawn where the options say,
    showing its slot of the touch screen, and a click on it is a tap on the panel."""
    args = [quest, slot] + ([target, tslot] if target else [])
    r = subprocess.run([os.environ["MH4U_RUNNER"], "--overlay-layout", "top-right 30 85", "1920x1080:424", *args],
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    assert r.stdout == expected


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
@pytest.mark.parametrize("key, value, expected", [
    ("quest_overlay", "map", "map"),
    ("quest_overlay", "on", "map"),
    ("quest_overlay", "MAP 40", "map 40"),
    ("quest_overlay", "map 40 at 98 2", "map 40 at 98 2"),
    ("quest_overlay", "map at 50 100", "map at 50 100"),
    ("quest_overlay", "map 17.53 at 33.333 0.04", "map 17.5 at 33.3 0"),
    ("quest_overlay", "map 40 at 150 -5", "map 40 at 100 0"),
    ("quest_overlay", "map 40 at", "map 40"),
    ("quest_overlay", "off", "off"),
    ("target_overlay", "on", "on"),
    ("target_overlay", "target 10", "on 10"),
    ("target_overlay", "on 12 at 100 45", "on 12 at 100 45"),
    ("target_overlay", "off", "off"),
    ("target_overlay", "", "off"),
])
def test_panel_view_option(key, value, expected):
    """quest_overlay and target_overlay as the hooks read them and the in-game menu writes them
    back to mh4u.ini (to 0.1%, inside the window)."""
    r = subprocess.run([os.environ["MH4U_RUNNER"], "--panel-view", key, value], capture_output=True, text=True,
                       timeout=60)
    assert r.returncode == 0, r.stderr
    assert r.stdout == expected + "\n"
    again = subprocess.run([os.environ["MH4U_RUNNER"], "--panel-view", key, expected], capture_output=True,
                           text=True, timeout=60)
    assert again.stdout == expected + "\n"


QUEST_FLICKER = ",".join(["map0:2"] + ["none:0.3,map0:0.3"] * 6 + ["map0:1"])


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
@pytest.mark.parametrize("fps", [30, 60, 120, 180])
@pytest.mark.parametrize("spans, expected", [
    # each change of area stops the small panels for a second or two: the map stays
    ("map0:3,none:1.5,map0:3,none:1.8,map0:2", [(0.1, "map in slot 0")]),
    # panels that come and go many times in a few seconds: the map stays
    (QUEST_FLICKER, [(0.1, "map in slot 0")]),
    # the quest menu (or a big panel) has buttons on the touch screen: all of it, at once
    ("map0:3,menu:2,map0:2", [(0.1, "map in slot 0"), (3.1, "whole touch screen"), (5.1, "map in slot 0")]),
    # someone talks: the panels don't run, but the touch screen still shows them
    ("map0:1,drawn:20,map0:1", [(0.1, "map in slot 0")]),
    # a long break with no panels on the touch screen (the quest is over): the whole touch
    # screen after 5 s
    ("map3:2,none:7,map3:2", [(0.1, "map in slot 3"), (7.1, "whole touch screen"), (9.1, "map in slot 3")]),
    ("map0:3,none:8", [(0.1, "map in slot 0"), (8.1, "whole touch screen")]),
    # F12, and a panel layout without the map
    ("map4:2,off:2,map4:2,nomap:1", [(0.1, "map in slot 4"), (2.1, "whole touch screen"),
                                     (4.1, "map in slot 4"), (6.1, "whole touch screen")]),
])
def test_quest_overlay_choice(fps, spans, expected):
    """What the touch overlay shows in a quest, frame by frame: the map while the small panels
    are up, kept through short breaks (a change of area, a cutscene) so that the corner doesn't
    flicker between the map and the whole touch screen; the whole touch screen for the quest
    menu and big panels, and once the panels have been gone for a while. The same at any frame
    rate: the times are emulated seconds."""
    r = subprocess.run([os.environ["MH4U_RUNNER"], "--quest-overlay-frames", str(fps), spans],
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    changes = [(float(t), what) for t, what in re.findall(r"^([0-9.]+) s: the (.+)$", r.stdout, re.M)]
    assert [what for _, what in changes] == [what for _, what in expected], r.stdout
    for (t, _), (want, _) in zip(changes, expected):
        assert abs(t - want) <= 1.5 / fps + 1e-6, r.stdout


def _normalized_log(text: str, user_dir: str) -> list[str]:
    """Azahar's log lines (written by its own thread, so the runner's prints can land
    anywhere between them), without timestamps, user paths and the backend's own lines."""
    out = []
    for line in text.splitlines():
        line = re.sub(r"\x1b\[[0-9;]*m", "", line)
        line = re.sub(r"^\[\s*[0-9.]+\]\s*", "", line).replace(user_dir, "<user>")
        line = re.sub(r"Datetime: [0-9/]+ [0-9:]+", "Datetime: <now>", line)
        if not re.match(r"\S+ <\w+> ", line) or line.startswith("Core.ARM11") or "as the user directory" in line:
            continue
        out.append(line)
    return out


@pytest.mark.skipif(not (os.environ.get("MH4U_RUNNER") and os.environ.get("MH4U_GAME")
                         and os.environ.get("MH4U_GAME_CODE")),
                    reason="needs MH4U_RUNNER, MH4U_GAME (your dump as .cxi) and MH4U_GAME_CODE")
def test_game_boot_matches_jit(tmp_path):
    """The game's own code (your dump, never in the repo) logs exactly the same things on
    the recompiled backend as on Azahar's JIT over the first MH4U_GAME_MS of emulated time
    (default 3000): same service calls, same files, same errors."""
    binary, game, code = os.environ["MH4U_RUNNER"], os.environ["MH4U_GAME"], os.environ["MH4U_GAME_CODE"]
    ms = os.environ.get("MH4U_GAME_MS", "3000")
    logs = {}
    for cpu in ("recomp", "jit"):
        user = str(tmp_path / cpu)
        cmd = [binary, game, "--cpu", cpu, "--ms", ms, "--user", user, "--log", "*:Info"]
        if cpu == "recomp":
            cmd += ["--code", code]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)
        logs[cpu] = _normalized_log(r.stdout + r.stderr, user)
    assert logs["recomp"] == logs["jit"]


def _svc_trace_by_thread(path) -> dict:
    """The calls of each thread, without their times (nor the fields after them: the
    registers at entry and the results, which hold the host's handle values and times)."""
    threads = {}
    for line in open(path):
        if line.startswith("#") or not line.strip():          # header, thread lines, notes
            continue
        tid, rest = line.split(" ", 1)
        threads.setdefault(tid, []).append(rest.split(" t=")[0])
    return threads


def test_svc_trace_by_thread_skips_comment_lines(tmp_path):
    """Header and `# thread` lines (N0-04) are not calls; the fields after `t=` are not compared."""
    trace = tmp_path / "svc.txt"
    trace.write_text(
        "# svctrace v1 2026-10-16 game=0x0004000000000000 cpu=jit new3ds=off\n"
        "# thread 1 parent=0 entry=00100000 arg=00000000 stack=0FFFF000 prio=48 core=0/0\n"
        "1 17 pc=00100100 lr=00100104 00000001 00000000 00000000 00000000 t=10 in4=00000000,00000000,00000000"
        " out=00000000,00008001,00000000,00000000\n"
        "# thread 2 parent=1 entry=00200000 arg=00000000 stack=0FFFE000 prio=48 core=0/0\n"
        "\n"
        "2 24 pc=00200100 lr=00200104 00008001 00000000 FFFFFFFF FFFFFFFF t=20 in4=00000000,00000000,00000000"
        " out=00000000,00000000,00000000,00000000\n"
        "1 18 pc=00100300 lr=00100104 00008001 00000000 00000000 00000000 t=30\n")
    assert _svc_trace_by_thread(trace) == {
        "1": ["17 pc=00100100 lr=00100104 00000001 00000000 00000000 00000000",
              "18 pc=00100300 lr=00100104 00008001 00000000 00000000 00000000"],
        "2": ["24 pc=00200100 lr=00200104 00008001 00000000 FFFFFFFF FFFFFFFF"],
    }


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
def test_svc_traces_match_jit_per_thread(tmp_path):
    """--trace-svc: every guest thread makes the same system calls, from the same places
    and with the same arguments, on the recompiled code as on Azahar's JIT."""
    import build_svc_test
    from recomp import build as rbuild
    binary = os.environ["MH4U_RUNNER"]
    elf = build_svc_test.build(str(tmp_path))
    code = rbuild.build(str(tmp_path), 2, "O1", ["RC_PAGED"], "code.so", [rbuild.DLL_GLUE],
                        extra_flags=["-fvisibility=hidden"])
    traces = {}
    for cpu in ("recomp", "jit"):
        trace = tmp_path / f"{cpu}.txt"
        r = subprocess.run([binary, elf, "--code", code, "--cpu", cpu, "--ms", "5000", "--trace-svc", str(trace)],
                           capture_output=True, text=True, timeout=600)
        assert r.returncode == 0, (cpu, r.stderr[-2000:])
        traces[cpu] = _svc_trace_by_thread(trace)
    assert len(traces["jit"]) >= 4                        # main thread + 3 workers
    assert traces["recomp"] == traces["jit"]


def _print_schedule(*args):
    return subprocess.run([os.environ["MH4U_RUNNER"], "--print-schedule", *args], capture_output=True,
                          text=True, timeout=60)


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
def test_schedule_prints_milliseconds_and_frames():
    """--print-schedule shows the parsed schedule and exits without a game; a time is emulated
    ms or f<N>, and a hold may be ms or f<N> frames (src/schedule.cpp, N0-08)."""
    r = _print_schedule(
        "--input", "f600:START,f900:A:f3,3000:A,3500:START:200,5000:TOUCH:160:120,6000:CIRCLE:0:1:f30",
        "--dump", "f600:100000:40", "--replace", "1500.5:55:aa", "--set", "f800:fps=120",
        "--freeze", "500:30", "--call-counts", "f77", "--shots-from", "f1200", "--frames", "3000")
    assert r.returncode == 0, r.stderr
    assert r.stdout.splitlines() == [
        "# schedule v1",
        "stop frames=3000",
        "press at=f600 hold=100ms kind=button button=START",
        "press at=f900 hold=f3 kind=button button=A",
        "press at=3000ms hold=100ms kind=button button=A",
        "press at=3500ms hold=200ms kind=button button=START",
        "press at=5000ms hold=100ms kind=touch x=160 y=120",
        "press at=6000ms hold=f30 kind=circle x=0 y=1",
        "dump at=f600 addr=0x00100000 size=0x00000040",
        "replace at=1500.5ms from=55 to=aa",
        "set at=f800 key=fps value=120",
        "freeze at=500ms host=30ms",
        "call-counts at=f77",
        "shots-from at=f1200",
    ]


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
def test_schedule_stop_options():
    """--ms takes ms or f<N>; --frames N is the same as --ms f<N>; --frames alone has no time limit."""
    assert "stop ms=10000" in _print_schedule().stdout.splitlines()
    assert _print_schedule("--ms", "1200").stdout.splitlines()[1:] == ["stop ms=1200"]
    assert _print_schedule("--ms", "f300").stdout.splitlines()[1:] == ["stop frames=300"]
    assert _print_schedule("--frames", "300").stdout.splitlines()[1:] == ["stop frames=300"]
    both = _print_schedule("--frames", "300", "--ms", "2000").stdout.splitlines()
    assert both[1:] == ["stop ms=2000", "stop frames=300"]
    # values of 1e6 ms or more (a 16 minute run) print exactly, not as 1.2e+06
    assert _print_schedule("--ms", "1200000").stdout.splitlines()[1:] == ["stop ms=1200000"]
    big = _print_schedule("--ms", "1234567.5", "--input", "1200000:A").stdout.splitlines()
    assert big[1:] == ["stop ms=1234567.5", "press at=1200000ms hold=100ms kind=button button=A"]


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
def test_schedule_input_file(tmp_path):
    """--input-file reads one event per line, with '#' comments and blank lines, and combines
    with --input."""
    script = tmp_path / "script.txt"
    script.write_text("# a script\n\nf600:START   # comment after an event\n  1000:A:f2\n"
                      "2000:TOUCH:10:20:50\n")
    r = _print_schedule("--input", "500:B", "--input-file", str(script))
    assert r.returncode == 0, r.stderr
    assert r.stdout.splitlines()[2:] == [
        "press at=500ms hold=100ms kind=button button=B",
        "press at=f600 hold=100ms kind=button button=START",
        "press at=1000ms hold=f2 kind=button button=A",
        "press at=2000ms hold=50ms kind=touch x=10 y=20",
    ]
    bad = tmp_path / "bad.txt"
    bad.write_text("100:A\n200:NOPE\n")
    r = _print_schedule("--input-file", str(bad))
    assert r.returncode == 1 and "line 2" in r.stderr and "NOPE" in r.stderr
    r = _print_schedule("--input-file", str(tmp_path / "missing.txt"))
    assert r.returncode == 1 and "bad --input-file" in r.stderr


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
@pytest.mark.parametrize("args", [
    ["--input", "f:A"], ["--input", "-5:A"], ["--input", "100:A:fx"], ["--input", "100:Q"],
    ["--input", "100:TOUCH:1"], ["--dump", "f600:zz:40"], ["--dump", "500:10:0"],
    ["--set", "f600:=1"], ["--set", "fps=120"], ["--freeze", "500:0"], ["--call-counts", "g5"],
    ["--shots-from", "-1"], ["--ms", "f"], ["--frames", "x"], ["--frames", "-3"],
    ["--shot-every", "f0"], ["--shot-every", "f"], ["--shot-every", "x"], ["--shot-every", "-5"],
])
def test_schedule_rejects_bad_times(args):
    r = _print_schedule(*args)
    assert r.returncode == 1 and r.stdout == "" and "bad --" in r.stderr, (r.stdout, r.stderr)


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
def test_schedule_prints_frame_keyed_shots():
    """--shot-every f<N> (N0-26): --print-schedule lists the interval in frames and the frame of the
    first shot (--shots-from, or frame N without it). The millisecond form prints nothing new."""
    r = _print_schedule("--shots-from", "f100", "--shot-every", "f60")
    assert r.returncode == 0, r.stderr
    assert r.stdout.splitlines()[2:] == ["shots-from at=f100", "shot-every every=f60 first=f100"]
    assert _print_schedule("--shot-every", "f60").stdout.splitlines()[2:] == ["shot-every every=f60 first=f60"]
    assert _print_schedule("--shots-from", "1500", "--shot-every", "f60").stdout.splitlines()[2:] == [
        "shots-from at=1500ms", "shot-every every=f60 first=1500ms"]
    assert _print_schedule("--shots-from", "f100", "--shot-every", "500").stdout.splitlines()[2:] == [
        "shots-from at=f100"]


SHOT_HARNESS = r'''
// Throwaway driver for port/src/schedule.cpp (standard library only): the screenshot schedule
// without Azahar. usage: shots describe EVERY FROM | shots run EVERY FROM FRAMES_PER_STEP MS_PER_STEP STEPS
// ("-" for an option that is not given). `run` takes a step of that many frames and milliseconds at a
// time and prints the name stem of each screenshot that is due.
#include <cstdio>
#include <cstdlib>
#include <string>
#include "schedule.h"

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    Schedule::Plan plan;
    std::string error;
    const std::string every = argv[2], from = argv[3];
    if (every != "-" && !Schedule::ParseShotEvery(every, plan, error)) {
        std::printf("bad --shot-every (%s)\n", error.c_str());
        return 1;
    }
    if (from != "-" && !Schedule::ParseShotsFrom(from, plan, error)) {
        std::printf("bad --shots-from (%s)\n", error.c_str());
        return 1;
    }
    if (std::string(argv[1]) == "describe") {
        std::fputs(Schedule::Describe(plan).c_str(), stdout);
        return 0;
    }
    if (argc != 7) return 2;
    Schedule::ShotSchedule shots(plan);
    const unsigned frames = std::atoi(argv[4]);
    const double ms = std::atof(argv[5]);
    const int steps = std::atoi(argv[6]);
    unsigned frame = 0;
    for (int i = 1; i <= steps; ++i) {
        frame += frames;
        std::string stem;
        if (shots.Due(i * ms, frame, stem)) std::printf("%s\n", stem.c_str());
    }
    return 0;
}
'''


@pytest.fixture(scope="module")
def shot_harness(tmp_path_factory):
    """port/src/schedule.cpp built with a throwaway driver, so the screenshot schedule is tested
    without the runner (and shows that the file includes only the standard library)."""
    cxx = shutil.which("clang++") or shutil.which("g++")
    if not cxx:
        pytest.skip("no C++ compiler")
    out = tmp_path_factory.mktemp("shots")
    (out / "shots.cpp").write_text(SHOT_HARNESS)
    exe = out / ("shots.exe" if sys.platform == "win32" else "shots")
    src = os.path.join(ROOT, "port", "src")
    flags = ["-static"] if sys.platform == "win32" else []
    r = subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", f"-I{src}", str(out / "shots.cpp"),
                        os.path.join(src, "schedule.cpp"), "-o", str(exe)] + flags,
                       capture_output=True, text=True, timeout=600)
    assert r.returncode == 0, r.stderr
    return str(exe)


def _shots(harness, every, start, frames=1, ms=16.7, steps=300):
    r = subprocess.run([harness, "run", every, start, str(frames), str(ms), str(steps)], capture_output=True,
                       text=True, timeout=60)
    assert r.returncode == 0, r.stdout + r.stderr
    return r.stdout.split()


def test_frame_keyed_shots_follow_the_frame_counter(shot_harness):
    """--shot-every f<N> (N0-26): a shot every N frames, the first at --shots-from f<M> (frame M), at
    frame N without it, or at the first frame after --shots-from in ms; named f<frame>."""
    assert _shots(shot_harness, "f60", "f100") == ["f0000100", "f0000160", "f0000220", "f0000280"]
    assert _shots(shot_harness, "f60", "-") == ["f0000060", "f0000120", "f0000180", "f0000240", "f0000300"]
    # 16.7 ms per frame: 1500 ms has come at frame 90
    assert _shots(shot_harness, "f60", "1500") == ["f0000090", "f0000150", "f0000210", "f0000270"]
    # a counter that skips frames (7 per step): no shot for a frame it skipped over, none twice
    assert _shots(shot_harness, "f4", "f0", frames=7, steps=4) == ["f0000000", "f0000008", "f0000016", "f0000024"]
    # the shot waits for its frame, whatever the time
    assert _shots(shot_harness, "f100", "f50", ms=1000, steps=60) == ["f0000050"]


def test_millisecond_shots_keep_their_names(shot_harness):
    """The millisecond forms behave as before N0-26: names are the time in ms, 7 digits."""
    assert _shots(shot_harness, "-", "-", steps=250) == ["0001000", "0002000", "0003000", "0004000"]
    assert _shots(shot_harness, "500", "-", steps=100) == ["0000500", "0001000", "0001500"]
    assert _shots(shot_harness, "500", "1500", steps=130) == ["0001500", "0002000"]
    # --shots-from f<M> with a millisecond interval: the first shot is f<M>, the next ones follow in ms
    assert _shots(shot_harness, "500", "f100", steps=200)[:3] == ["f0000100", "0002170", "0002670"]
    assert _shots(shot_harness, "0.2", "-", steps=3) == ["0000001", "0000002", "0000003"]   # below 1 ms counts as 1


@pytest.mark.parametrize("every", ["f0", "f", "x", "-5", "f1.5"])
def test_shot_every_rejects_what_is_not_a_time(shot_harness, every):
    r = subprocess.run([shot_harness, "describe", every, "-"], capture_output=True, text=True, timeout=60)
    assert r.returncode == 1 and r.stdout.startswith("bad --shot-every"), (r.stdout, r.stderr)


def test_shot_schedule_describes_frame_keyed_shots(shot_harness):
    def describe(every, start):
        return subprocess.run([shot_harness, "describe", every, start], capture_output=True, text=True,
                              timeout=60).stdout.splitlines()
    assert describe("f60", "f100") == ["# schedule v1", "stop ms=10000", "shots-from at=f100",
                                       "shot-every every=f60 first=f100"]
    assert describe("f60", "-")[2:] == ["shot-every every=f60 first=f60"]
    assert describe("500", "f1200")[2:] == ["shots-from at=f1200"]


FIXED_START_TICKS = 2000000000        # port/README.md, section Determinism (kFixedStartTicks in runner.cpp)
FIXED_CONSOLE_MAC = bytes.fromhex("020000000001")


def _shared_page_after(binary, elf, code, tmp_path, name, *extra):
    """The 4 KiB shared page (0x1FF81000) at 1000 ms of a run of io_test in a fresh user folder."""
    shots = tmp_path / f"shots_{name}"
    r = subprocess.run([binary, elf, "--code", code, "--ms", "1500", "--user", str(tmp_path / f"user_{name}"),
                        "--shots", str(shots), "--dump", "1000:1FF81000:0x1000", *extra],
                       capture_output=True, text=True, timeout=600)
    assert r.returncode == 0, (name, r.stdout[-2000:], r.stderr[-2000:])
    page = (shots / "mem_0001000_1ff81000.bin").read_bytes()
    assert len(page) == 0x1000
    return page


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
def test_shared_page_repeats_and_holds_the_fixed_values(tmp_path):
    """N0-26: without --async-io, two runs with the same arguments (each in a fresh user folder) give a
    byte-identical shared page: the Wi-Fi MAC address (offsets 0x60 to 0x65) is the fixed 02:00:00:00:00:01
    and the clock starts at the fixed tick, which the date records carry (update ticks at 0x28 and 0x48).
    With --async-io Azahar's own values stay (a random address from its list, in the folder's mac.txt)."""
    import build_svc_test
    from recomp import build as rbuild
    binary = os.environ["MH4U_RUNNER"]
    elf = build_svc_test.build(str(tmp_path), "io_test")
    code = rbuild.build(str(tmp_path), 2, "O1", ["RC_PAGED"], "code.so", [rbuild.DLL_GLUE],
                        extra_flags=["-fvisibility=hidden"])
    first = _shared_page_after(binary, elf, code, tmp_path, "a")
    second = _shared_page_after(binary, elf, code, tmp_path, "b")
    assert hashlib.sha256(first).digest() == hashlib.sha256(second).digest()
    assert first[0x60:0x66] == FIXED_CONSOLE_MAC
    # the first update of the date (at the first slice of emulation) is in the record that was written last
    update_ticks = [int.from_bytes(first[o:o + 8], "little") for o in (0x28, 0x48)]
    assert FIXED_START_TICKS <= max(update_ticks) < FIXED_START_TICKS + (1 << 28), update_ticks
    # the asynchronous mode is Azahar's: its address is not the fixed one
    assert _shared_page_after(binary, elf, code, tmp_path, "c", "--async-io")[0x60:0x66] != FIXED_CONSOLE_MAC


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
def test_timing_trace_lists_the_events_that_ran(tmp_path):
    """--trace-timing FILE (N0-26): a header in the shared format, then a line per CoreTiming event run:
    tick, event, core, cycles late, and where it was scheduled from. Each core's ticks never go back.
    A file that can't be written is an error."""
    import build_svc_test
    from recomp import build as rbuild
    binary = os.environ["MH4U_RUNNER"]
    elf = build_svc_test.build(str(tmp_path), "io_test")
    code = rbuild.build(str(tmp_path), 2, "O1", ["RC_PAGED"], "code.so", [rbuild.DLL_GLUE],
                        extra_flags=["-fvisibility=hidden"])
    trace = tmp_path / "timing.txt"
    r = subprocess.run([binary, elf, "--code", code, "--ms", "500", "--user", str(tmp_path / "user"),
                        "--trace-timing", str(trace)], capture_output=True, text=True, timeout=600)
    assert r.returncode == 0, (r.stdout[-2000:], r.stderr[-2000:])
    lines = trace.read_text().splitlines()
    assert re.fullmatch(r"# timingtrace v1 \d{4}-\d\d-\d\d game=0x[0-9a-f]{16} cpu=recomp new3ds=off", lines[0]), lines[0]
    events = [re.fullmatch(r"tick=(-?\d+) event=(\S+) core=(\d+) late=(\d+) from=(emu|host)", ln) for ln in lines[1:]]
    assert events and all(events), [ln for ln, m in zip(lines[1:], events) if not m][:3]
    names = {m.group(2) for m in events}
    assert "SharedPage::UpdateTimeCallback" in names, sorted(names)
    last = {}
    for m in events:
        core, tick = m.group(3), int(m.group(1))
        assert tick >= last.get(core, tick), (core, tick, last.get(core))
        last[core] = tick
    assert max(last.values()) > 400 * 268111           # ~500 ms of emulated time
    bad = subprocess.run([binary, elf, "--code", code, "--ms", "100", "--trace-timing",
                          str(tmp_path / "missing" / "t.txt")], capture_output=True, text=True, timeout=600)
    assert bad.returncode == 1 and "cannot write" in bad.stderr


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
def test_ipc_trace_is_written_parsed_and_compared(tmp_path):
    """--trace-ipc (N0-21): a conformance program that talks to services writes a trace that
    tracefmt parses (header, thread lines, request lines), that compare_ipc_traces finds equal
    for the JIT and the recompiled code, and that it refuses when it is not an IPC trace.
    --trace-ipc-full without --trace-ipc is a usage error."""
    import compare_ipc_traces
    import build_svc_test
    from recomp import build as rbuild
    sys.path.insert(0, os.path.join(ROOT, "tools", "native"))
    import tracefmt
    binary = os.environ["MH4U_RUNNER"]
    elf = build_svc_test.build(str(tmp_path), "conf_services")
    code = rbuild.build(str(tmp_path), 2, "O1", ["RC_PAGED"], "code.so", [rbuild.DLL_GLUE],
                        extra_flags=["-fvisibility=hidden"])
    traces = {}
    for cpu in ("jit", "recomp"):
        path = tmp_path / f"ipc_{cpu}.txt"
        r = subprocess.run([binary, elf, "--code", code, "--cpu", cpu, "--new-3ds", "off", "--ms", "5000",
                            "--user", str(tmp_path / f"user_{cpu}"), "--trace-ipc", str(path)],
                           capture_output=True, text=True, timeout=600)
        assert r.returncode == 0, (cpu, r.stderr[-2000:])
        assert tracefmt.is_ipc_trace(str(path))
        traces[cpu] = tracefmt.parse_ipc(str(path))
        assert traces[cpu].header["tool"] == "ipctrace" and traces[cpu].header["cpu"] == cpu
        assert traces[cpu].threads and len(traces[cpu].calls) >= 3
        assert all(c.session and c.seq >= 1 for c in traces[cpu].calls)
        assert {c.session for c in traces[cpu].calls} >= {"srv:"}
    ref, cand = str(tmp_path / "ipc_jit.txt"), str(tmp_path / "ipc_recomp.txt")
    assert compare_ipc_traces.main([ref, cand]) == 0
    assert compare_ipc_traces.main([ref, ref, "--strict"]) == 0
    # an SVC trace is not an IPC trace
    svc = tmp_path / "svc.txt"
    r = subprocess.run([binary, elf, "--code", code, "--cpu", "jit", "--new-3ds", "off", "--ms", "5000",
                        "--user", str(tmp_path / "user_svc"), "--trace-svc", str(svc)],
                       capture_output=True, text=True, timeout=600)
    assert r.returncode == 0, r.stderr[-2000:]
    assert compare_ipc_traces.main([str(svc), ref]) == 2
    # --trace-ipc-full needs --trace-ipc
    r = subprocess.run([binary, elf, "--code", code, "--cpu", "jit", "--ms", "1000", "--trace-ipc-full"],
                       capture_output=True, text=True, timeout=600)
    assert r.returncode == 1


# ── N0-29: what the sources alone show (no emulator, no game data) ─────────────────────

PATCH = os.path.join(ROOT, "port", "azahar", "azahar.patch")


def _text(*parts):
    with open(os.path.join(ROOT, *parts), encoding="utf-8", newline="") as f:
        return f.read()


def _patch_section(path):
    """The part of azahar.patch that changes `path`, from its 'diff --git' line to the next file's."""
    patch = _text("port", "azahar", "azahar.patch")
    start = patch.index(f"diff --git a/{path} b/{path}\n")
    end = patch.find("\ndiff --git ", start + 1)
    return patch[start:] if end < 0 else patch[start:end + 1]


def _console_id_in_azahars_shape(random_number, console_id):
    """What SetFixedConsoleUniqueId accepts (patch, cfg.cpp): GenerateConsoleUniqueId's shape."""
    return (random_number <= 0xFFFF and (console_id >> 48) == random_number
            and (console_id & 0x0000FFFC00000000) == 0)


@source_only
def test_patch_applies_to_the_pinned_azahar_sources():
    """port/azahar/azahar.patch applies, as the workflows apply it (git apply), to Azahar at the commit in
    port/azahar/COMMIT. Needs git and those sources (AZAHAR_SRC, default ~/azahar), unmodified."""
    git = shutil.which("git")
    src = os.environ.get("AZAHAR_SRC") or os.path.expanduser("~/azahar")
    if not git or not os.path.isdir(os.path.join(src, "src")):
        pytest.skip("git or the pinned Azahar sources (AZAHAR_SRC, default ~/azahar) are not available")

    def run(*args):
        return subprocess.run([git, "-C", src, *args], capture_output=True, text=True, timeout=300)
    head = run("rev-parse", "HEAD")
    if head.returncode != 0 or head.stdout.strip() != _text("port", "azahar", "COMMIT").strip():
        pytest.skip("the Azahar sources are not at the commit of port/azahar/COMMIT")
    if run("status", "--porcelain", "--untracked-files=no").stdout.strip():
        pytest.skip("the Azahar sources are modified (already patched?)")
    r = run("apply", "--check", PATCH)
    assert r.returncode == 0, r.stderr[-2000:]


@source_only
def test_patch_keeps_the_earlier_work_packages_hunks():
    """N0-29 adds to the patch; the MAC address switch (N0-26) and the timing observer stay."""
    cfg = _patch_section("src/core/hle/service/cfg/cfg.cpp")
    assert "+bool SetFixedConsoleMac(const std::string& mac) {" in cfg
    assert "+static std::string fixed_console_mac;" in cfg
    assert "bool SetFixedConsoleMac(const std::string& mac);" in _patch_section("src/core/hle/service/cfg/cfg.h")
    timing = _patch_section("src/core/core_timing.cpp")
    assert "+void SetTimingObserver(" in timing and "evt.from_host" in timing


@source_only
def test_patch_adds_the_fixed_console_unique_id_switch():
    """SetFixedConsoleUniqueId (cfg.cpp, cfg.h): refuses a pair that GenerateConsoleUniqueId could not have
    made, and GenerateConsoleUniqueId, which a config savegame that is made calls, returns it when it is set."""
    cpp = _patch_section("src/core/hle/service/cfg/cfg.cpp")
    assert "+bool SetFixedConsoleUniqueId(u32 random_number, u64 console_id) {" in cpp
    assert "+    if (random_number > 0xFFFF || (console_id >> 48) != random_number ||" in cpp
    assert "+        (console_id & 0x0000FFFC00000000ULL) != 0) {" in cpp
    assert "+        return false;" in cpp
    assert "ClearFixedConsoleUniqueId" not in cpp      # nothing would call it
    # the switch is checked before anything is drawn from the host's random source
    assert "+    if (fixed_console_unique_id_set) {" in cpp
    assert cpp.index("+    if (fixed_console_unique_id_set) {") < cpp.index("CryptoPP::AutoSeededRandomPool rng;")
    h = _patch_section("src/core/hle/service/cfg/cfg.h")
    assert "+bool SetFixedConsoleUniqueId(u32 random_number, u64 console_id);" in h
    assert "ClearFixedConsoleUniqueId" not in h


@source_only
def test_the_fixed_console_unique_id_is_in_azahars_shape_and_documented():
    """The runner's constants pass the patch's check (mirrored here), the check refuses what it should, and
    the README table has the ID and the GetTransferableId replies that follow from it."""
    import hashlib
    import struct
    runner = _text("port", "src", "runner.cpp")
    number = int(re.search(r"constexpr u32 kFixedConsoleRandomNumber = (0x[0-9A-Fa-f]+);", runner).group(1), 16)
    ident = int(re.search(r"constexpr u64 kFixedConsoleId = (0x[0-9A-Fa-f]+)ULL;", runner).group(1), 16)
    assert _console_id_in_azahars_shape(number, ident)
    assert ident & 0x3FFFFFFFF == 0x1A2B3C4D5 and ident >> 48 == number
    assert not _console_id_in_azahars_shape(number, ident ^ (1 << 48))       # another top 16 bits
    assert not _console_id_in_azahars_shape(number, ident | (1 << 34))       # a bit of 34 to 47
    assert not _console_id_in_azahars_shape(number, ident | (1 << 47))
    assert not _console_id_in_azahars_shape(0x10000, ident)                  # more than 16 bits
    readme = _text("port", "README.md")
    assert f"**0x{ident:016X}**" in readme and f"**0x{number:04X}**" in readme
    # cfg:u GetTransferableId: the last 8 bytes of SHA-256(ID as 8 bytes LE, salt & 0xFFFFF as 4 bytes LE)
    for salt, words in ((0, (0x61B8087C, 0xB904EFF1)), (0x1263, (0x9592177D, 0xFE13E102))):
        digest = hashlib.sha256(struct.pack("<QI", ident, salt & 0xFFFFF)).digest()
        assert struct.unpack("<II", digest[24:32]) == words, hex(salt)
        assert f"words 0x{words[0]:08X}, 0x{words[1]:08X}" in readme
    assert ident.to_bytes(8, "little").hex(" ").upper() == "D5 C4 B3 A2 01 00 48 4D"


@source_only
def test_the_runner_sets_the_n0_29_values_in_its_default_mode():
    """runner.cpp: the zeroed IPC replies and the time zone with the other default-mode settings (no
    --async-io), the console unique ID under the MAC address's condition (no room, no --new-console-id),
    all before the system loads."""
    runner = _text("port", "src", "runner.cpp")
    load = runner.index("system.Load(window, path)")
    block = runner[runner.index("    if (!async_io) {\n        // the game seeds"):]
    block = block[:block.index("    // the console's Wi-Fi MAC address")]
    assert "Kernel::SetZeroedIpcReplies(true);" in block
    assert '_putenv_s("TZ", "UTC0");' in block and 'setenv("TZ", "UTC0", 1);' in block
    assert runner.index("Kernel::SetZeroedIpcReplies(true);") < load
    assert '#include "core/hle/kernel/hle_ipc.h"' in runner
    mac = "const bool fix_console_mac = !async_io && !new_console_id && !room_host && room_join.empty();"
    assert mac in runner
    assert "const bool fix_console_id = fix_console_mac;" in runner
    call = "Service::CFG::SetFixedConsoleUniqueId(kFixedConsoleRandomNumber, kFixedConsoleId)"
    assert f"if (fix_console_id &&\n        !{call}) {{" in runner
    assert runner.index(mac) < runner.index(call) < load
    # --new-console-id keeps drawing its own: NewConsoleId asks Azahar, which is not told to fix it
    assert "cfg->GenerateConsoleUniqueId();" in runner and runner.count("SetFixedConsoleUniqueId(") == 1


@source_only
def test_patch_zeroes_the_command_buffer_behind_a_switch():
    """hle_ipc.cpp, hle_ipc.h: Kernel::SetZeroedIpcReplies; without it the constructor is Azahar's."""
    h = _patch_section("src/core/hle/kernel/hle_ipc.h")
    assert "+void SetZeroedIpcReplies(bool enable);" in h
    cpp = _patch_section("src/core/hle/kernel/hle_ipc.cpp")
    assert "+#include <atomic>" in cpp
    assert "+static std::atomic<bool> zeroed_ipc_replies{false};" in cpp
    assert "+void SetZeroedIpcReplies(bool enable) {" in cpp
    assert "+    if (zeroed_ipc_replies.load(std::memory_order_relaxed)) {" in cpp
    assert "+        cmd_buf.fill(0);" in cpp
    assert "+    } else {\n+        cmd_buf[0] = 0;\n+    }" in cpp
    assert "-    cmd_buf[0] = 0;" in cpp


@source_only
@pytest.mark.parametrize("path", ["src/core/file_sys/archive_extsavedata.cpp",
                                  "src/core/file_sys/archive_source_sd_savedata.cpp"])
def test_patch_writes_zero_padding_in_the_format_metadata(path):
    """Both writers of the 16-byte ArchiveFormatInfo write the 13 bytes of its fields and zeros after them
    when deterministic_async_operations is set, and the whole struct as before when it is not."""
    section = _patch_section(path)
    assert '+#include "common/settings.h"' in section and "+#include <cstring>" in section
    flat = " ".join(line[1:].strip() for line in section.splitlines() if line.startswith("+"))
    assert "if (Settings::values.deterministic_async_operations) {" in flat
    assert "std::array<u8, sizeof(format_info)> metadata{};" in flat
    assert ("std::memcpy(metadata.data(), &format_info, sizeof(format_info.total_size) + "
            "sizeof(format_info.number_directories) + sizeof(format_info.number_files) + "
            "sizeof(format_info.duplicate_data));") in flat
    assert "file.WriteBytes(metadata.data(), metadata.size()); } else { " \
           "file.WriteBytes(&format_info, sizeof(format_info)); }" in flat       # Azahar's branch kept


@source_only
def test_patch_writes_zero_padding_in_mcu_dat():
    """cfg.cpp SaveMCUConfig: the 20 bytes of MCUData and zeros for its 4 tail padding bytes when
    deterministic_async_operations is set, the whole struct as before when it is not."""
    cpp = _patch_section("src/core/hle/service/cfg/cfg.cpp")
    flat = " ".join(line[1:].strip() for line in cpp.splitlines() if line.startswith("+"))
    assert "if (Settings::values.deterministic_async_operations) {" in flat
    assert "std::array<u8, sizeof(MCUData)> raw{};" in flat
    assert "std::copy_n(reinterpret_cast<const u8*>(&mcu_data.header), sizeof(MCUData::Header), raw.begin());" in flat
    assert ("std::copy_n(reinterpret_cast<const u8*>(&mcu_data.clock_sequence), "
            "sizeof(mcu_data.clock_sequence), raw.begin() + sizeof(MCUData::Header));") in flat
    assert "mcu_data_file.WriteBytes(raw.data(), raw.size()); } else { " \
           "mcu_data_file.WriteBytes(&mcu_data, sizeof(MCUData)); }" in flat       # Azahar's branch kept


@source_only
def test_readme_documents_the_sources_n0_29_found():
    readme = _text("port", "README.md")
    determinism = readme[readme.index("## Determinism"):readme.index("## With Azahar's normal interface")]
    assert "### Sources found by N0-29, iteration 0" in determinism
    for term in ("SetFixedConsoleUniqueId", "kFixedConsoleId", "SetZeroedIpcReplies", "ArchiveFormatInfo",
                 "TZ", "UTC0", "GetTransferableId", "APT:A", "y2r:u", "cfg:u", "--perf-log",
                 "mh4u_runner_log.txt", "mcu.dat", "MH4U_*"):
        assert term in determinism, term
    # N0-26's measurement is kept as it was written
    assert "### Measurement with the game (N0-26)" in determinism
    assert "Where N0-29 starts: the three IPC reply words." in determinism
    # the README no longer tells the reader to set TZ by hand, and says what each mode keeps
    assert "run with `TZ=UTC0`" not in readme
    intro = determinism.split("### What the game sees")[0]
    assert "What each mode keeps of Azahar's behaviour:" in intro and "`--async-io` keeps all of it" in intro
    assert "log/" in intro
