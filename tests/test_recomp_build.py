"""tools/recomp/build.py: incremental builds keep objects of different settings apart."""
import os
import shutil
import sys

import pytest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))

pytestmark = pytest.mark.skipif(shutil.which("clang") is None, reason="clang not installed")


def objects(tmp_path):
    return sorted(o for o in os.listdir(tmp_path / "obj") if o.endswith(".o"))


def test_objects_per_build_variant(tmp_path):
    from recomp import build as rbuild
    src = tmp_path / "src"
    src.mkdir()
    (src / "a.c").write_text("#ifdef VARIANT\nint variant = 1;\n#endif\nint a(void) { return 1; }\n")
    rbuild.build(str(tmp_path), 1, "O0", [], "one.so", [])
    rbuild.build(str(tmp_path), 1, "O0", [], "two.so", [], extra_flags=["-DVARIANT"])
    objs = objects(tmp_path)
    assert len(objs) == 2 and all(o.startswith("a-O0-") for o in objs)
    stamp = {o: os.path.getmtime(tmp_path / "obj" / o) for o in objs}
    rbuild.build(str(tmp_path), 1, "O0", [], "one.so", [])        # nothing to recompile
    assert {o: os.path.getmtime(tmp_path / "obj" / o) for o in objs} == stamp


def test_header_date_does_not_matter_its_contents_do(tmp_path):
    """Unpacking a new package gives recomp.h a new date: with the same contents nothing is
    recompiled; changed contents recompile everything, under the same object names."""
    from recomp import build as rbuild
    src = tmp_path / "src"
    src.mkdir()
    (src / "a.c").write_text("int a(void) { return 1; }\n")
    header = tmp_path / "recomp.h"
    header.write_text("/* v1 */\n")
    rbuild.build(str(tmp_path), 1, "O0", [], "one.so", [], runtime_header=str(header))
    objs = objects(tmp_path)
    assert len(objs) == 1
    stamp = os.path.getmtime(tmp_path / "obj" / objs[0])
    future = stamp + 3600
    os.utime(header, (future, future))                                   # a copy: new date, same text
    rbuild.build(str(tmp_path), 1, "O0", [], "one.so", [], runtime_header=str(header))
    assert objects(tmp_path) == objs
    assert os.path.getmtime(tmp_path / "obj" / objs[0]) == stamp          # nothing recompiled
    os.remove(tmp_path / "obj" / "recomp.h.sha1")                        # objects from before the stamp
    rbuild.build(str(tmp_path), 1, "O0", [], "one.so", [], runtime_header=str(header))
    assert os.path.getmtime(tmp_path / "obj" / objs[0]) == stamp          # kept, and stamped now
    assert (tmp_path / "obj" / "recomp.h.sha1").exists()
    header.write_text("/* v2 */\n")                                        # changed contents
    os.utime(header, (stamp - 3600, stamp - 3600))                       # even with an old date
    rbuild.build(str(tmp_path), 1, "O0", [], "one.so", [], runtime_header=str(header))
    assert objects(tmp_path) == objs
    assert os.path.getmtime(tmp_path / "obj" / objs[0]) > stamp           # recompiled


# ── the native runtime's build mode (RC_NATIVE) ──────────────────────────────────────────────────────

import ctypes  # noqa: E402
import subprocess  # noqa: E402

ROOT = os.path.dirname(HERE)
RUNTIME = os.path.join(ROOT, "tools", "recomp", "runtime")
needs_arm = pytest.mark.skipif(any(shutil.which(t) is None for t in ("ld.lld", "llvm-objcopy", "llvm-readelf")),
                               reason="clang/lld toolchain not installed")

# A loop (one yield point per iteration) and a caller of it, no memory accesses.
NATIVE_PROGRAM = r"""
__attribute__((noinline)) unsigned mix(unsigned n) {
    unsigned s = 1;
    for (unsigned i = 0; i < n; i++) s = (s * 31u) ^ (i + 7u);
    return s;
}
__attribute__((noinline)) unsigned mix_twice(unsigned n) { return mix(n) + mix(n + 1); }
"""
RET_ADDR = 0x0E000000


def mix(n: int) -> int:
    s = 1
    for i in range(n):
        s = ((s * 31) & 0xFFFFFFFF) ^ (i + 7)
    return s


@pytest.fixture(scope="module")
def native_program(tmp_path_factory):
    """NATIVE_PROGRAM recompiled to C: (output directory, {function name: guest address})."""
    if any(shutil.which(t) is None for t in ("ld.lld", "llvm-objcopy", "llvm-readelf")):
        pytest.skip("clang/lld toolchain not installed")
    import difftest as dt
    import recompile
    from recomp.analysis import Image

    tmp = tmp_path_factory.mktemp("native_program")
    base = 0x00100000
    src, obj = tmp / "p.c", tmp / "p.o"
    src.write_text(NATIVE_PROGRAM)
    subprocess.run(["clang", *[f for f in dt.CFLAGS if f != "-O2"], "-O1", "-c", str(src), "-o", str(obj)], check=True)
    rt, elf, binp = tmp / "rt.o", tmp / "p.elf", tmp / "p.bin"
    subprocess.run(["clang", *dt.CFLAGS, "-c", dt.RT_SOURCE, "-o", str(rt)], check=True)
    script = tmp / "link.ld"
    script.write_text(dt.LINKER_SCRIPT.replace(f"0x{dt.CAND_BASE:08x}", f"0x{base:08x}"))
    subprocess.run(["ld.lld", "-T", str(script), "--entry=0", str(obj), str(rt), "-o", str(elf)], check=True)
    subprocess.run(["llvm-objcopy", "-O", "binary", str(elf), str(binp)], check=True)
    code = binp.read_bytes()
    entries, names = {}, {}
    for line in subprocess.run(["llvm-readelf", "-s", str(elf)], capture_output=True, text=True,
                               check=True).stdout.splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[3] == "FUNC":
            a = int(parts[1], 16)
            entries[a & ~1] = "thumb" if a & 1 else "arm"
            names[parts[7]] = a & ~1
    out = tmp / "recomp"
    res = recompile.recompile(Image(code, base, min(len(code), max(entries) + 0x1000 - base)), entries, str(out),
                              log=lambda *a: None)
    assert not res["problems"], res["problems"]
    assert "RC_TICK" in "".join(p.read_text() for p in (out / "src").glob("f*.c"))
    return out, names


def harness_machine(program, tmp_path, *flags):
    """The flat harness built with `python3 -m recomp.build --harness <flags>`."""
    from recomp import build as rbuild
    from recomp.gametest import STACK_SIZE, STACK_TOP
    from recomp.harness.machines import NativeMachine, Region

    out, names = program
    lib = out / ("harness" + "".join(f.replace("--", "_") for f in flags) + ".so")
    assert rbuild.main([str(out), "--harness", "--opt", "O1", "-j", "2", *flags, "-o", lib.name]) == 0
    m = NativeMachine(str(lib), [Region(STACK_TOP - STACK_SIZE, STACK_SIZE, True, bytes(STACK_SIZE))])
    m.lib.rh_set_budget_resume.argtypes = [ctypes.c_int64]
    return m, names


def run_mix(m, names, n, fn="mix", budget=1 << 40):
    from recomp.gametest import STACK_TOP
    from recomp.harness.machines import State

    st = State()
    st.r[0], st.r[13], st.r[14] = n, STACK_TOP - 0x100, RET_ADDR
    return m.run(names[fn], False, st, budget=budget)


def test_native_build_runs_in_the_flat_harness(native_program, tmp_path):
    """--native selects flat memory and a yield point that only reads the preemption flag:
    the cycle budget is neither charged nor looked at, and with the flag clear nothing yields."""
    from recomp.harness.machines import ST_RETURN

    m, names = harness_machine(native_program, tmp_path, "--native")
    for n in (0, 1, 9, 1000):
        r = run_mix(m, names, n, budget=0)            # a paged build would yield at once
        assert r.status == ST_RETURN and r.state.r[0] == mix(n), (n, r)
    assert m.ctx.budget == 0
    assert m.lib.rh_budget_calls() == 0
    assert run_mix(m, names, 20, "mix_twice", budget=-5).state.r[0] == (mix(20) + mix(21)) & 0xFFFFFFFF


def test_preempt_flag_makes_rc_budget_run_once(native_program, tmp_path):
    from recomp.harness.machines import ST_BUDGET, ST_RETURN

    m, names = harness_machine(native_program, tmp_path, "--native")
    # as a host that yields and comes back: rc_budget clears the flag and returns
    m.lib.rh_set_budget_resume(0)
    m.lib.rh_set_preempt_flag(1)
    r = run_mix(m, names, 500)
    assert r.status == ST_RETURN and r.state.r[0] == mix(500)
    assert m.lib.rh_budget_calls() == 1 and m.lib.rh_preempt_flag() == 0
    # the flag is looked at again at every call: set it a second time, one more yield
    m.lib.rh_set_preempt_flag(1)
    assert run_mix(m, names, 3).state.r[0] == mix(3) and m.lib.rh_budget_calls() == 1
    # a flag that is never cleared (a host that keeps requesting) yields at every block
    m.lib.rh_set_budget_resume(-1)
    m.lib.rh_set_preempt_flag(1)
    r = run_mix(m, names, 500)
    assert r.status == ST_BUDGET and m.lib.rh_budget_calls() == 1     # ends the call, as the harness does
    m.lib.rh_set_preempt_flag(0)
    assert run_mix(m, names, 4).state.r[0] == mix(4) and m.lib.rh_budget_calls() == 0


def test_native_deterministic_charges_cycles(native_program, tmp_path):
    """--native --deterministic is flat memory with the paged build's cycle charging: rc_budget
    runs when the budget runs out, and the preemption flag is not looked at."""
    from recomp.harness.machines import ST_BUDGET, ST_RETURN

    m, names = harness_machine(native_program, tmp_path, "--native", "--deterministic")
    big = 1 << 40
    r = run_mix(m, names, 1000, budget=big)
    assert r.status == ST_RETURN and r.state.r[0] == mix(1000)
    spent = big - m.ctx.budget
    assert spent > 1000                                  # at least a cycle per iteration
    r = run_mix(m, names, 1000, budget=spent // 2)
    assert r.status == ST_BUDGET and m.lib.rh_budget_calls() == 1
    r = run_mix(m, names, 1000, budget=spent - 1)        # one cycle short: yields at the last block
    assert r.status == ST_BUDGET
    assert run_mix(m, names, 1000, budget=spent).status == ST_RETURN   # exactly enough
    # resuming host: refill when it runs out; the call ends normally, with the same result
    m.lib.rh_set_budget_resume(100)
    r = run_mix(m, names, 1000, budget=50)
    assert r.status == ST_RETURN and r.state.r[0] == mix(1000)
    yields = -(-(spent - 50) // 100)                    # as many refills as it takes to pay `spent`
    assert m.lib.rh_budget_calls() == yields and m.ctx.budget == 50 + 100 * yields - spent
    # the flag means nothing here
    m.lib.rh_set_budget_resume(-1)
    m.lib.rh_set_preempt_flag(1)
    r = run_mix(m, names, 1000, budget=big)
    assert r.status == ST_RETURN and m.lib.rh_budget_calls() == 0


def test_default_harness_build_is_unchanged(native_program, tmp_path):
    """No runtime flag (the build the other tests use): flat memory and cycle charging, as before."""
    from recomp.harness.machines import ST_BUDGET, ST_RETURN

    m, names = harness_machine(native_program, tmp_path)
    big = 1 << 40
    assert run_mix(m, names, 100, budget=big).state.r[0] == mix(100)
    spent = big - m.ctx.budget
    assert spent > 100
    assert run_mix(m, names, 100, budget=spent - 1).status == ST_BUDGET
    assert run_mix(m, names, 100, budget=spent).status == ST_RETURN
    m.lib.rh_set_preempt_flag(1)                          # not looked at
    assert run_mix(m, names, 100, budget=big).status == ST_RETURN and m.lib.rh_budget_calls() == 0


class CHost(ctypes.Structure):
    """RHost of recomp.h (version 6)."""
    _fields_ = [("version", ctypes.c_uint32), ("ctx_size", ctypes.c_uint32),
                ("call", ctypes.c_void_p), ("svc", ctypes.c_void_p), ("trap", ctypes.c_void_p),
                ("bad_return", ctypes.c_void_p), ("budget", ctypes.c_void_p),
                ("slow_read", ctypes.c_void_p), ("slow_write", ctypes.c_void_p),
                ("preempt_flag", ctypes.POINTER(ctypes.c_uint32))]


class CFuncEntry(ctypes.Structure):
    _fields_ = [("addr", ctypes.c_uint32), ("mode", ctypes.c_uint32), ("fn", ctypes.c_void_p)]


class CCodeInfo(ctypes.Structure):
    """RCodeInfo of recomp.h."""
    _fields_ = [("table", ctypes.POINTER(CFuncEntry)), ("count", ctypes.c_uint32),
                ("text_base", ctypes.c_uint32), ("text_size", ctypes.c_uint32), ("text_crc32", ctypes.c_uint32),
                ("title_id", ctypes.c_uint64),
                ("hook_addrs", ctypes.c_void_p), ("hook_names", ctypes.POINTER(ctypes.c_char_p)),
                ("hook_count", ctypes.c_uint32), ("set_hook", ctypes.c_void_p),
                ("tunables", ctypes.c_void_p), ("tunable_addrs", ctypes.c_void_p),
                ("tunable_names", ctypes.c_void_p), ("tunable_count", ctypes.c_uint32)]


MODES = {"paged": ([], "mh4u_code.so", 0),
         "native": (["--native"], "mh4u_code_native.so", 1),
         "native_det": (["--native", "--deterministic"], "mh4u_code_native_det.so", 2)}


def code_library(program, name):
    """The code library of `python3 -m recomp.build --code-library <flags>`, named as it names it."""
    from recomp import build as rbuild

    out, _ = program
    flags, lib_name, mode = MODES[name]
    assert rbuild.main([str(out), "--code-library", "--opt", "O1", "-j", "2", *flags]) == 0
    assert (out / lib_name).exists()
    return str(out / lib_name), mode


@pytest.mark.parametrize("name", MODES)
def test_code_library_exports_its_build_mode(native_program, name):
    lib_path, mode = code_library(native_program, name)
    lib = ctypes.CDLL(lib_path)
    assert ctypes.c_uint32.in_dll(lib, "rc_build_mode").value == mode
    if shutil.which("llvm-nm"):
        syms = subprocess.run(["llvm-nm", "-D", "--defined-only", lib_path], capture_output=True, text=True,
                              check=True).stdout
        exported = {line.split()[-1] for line in syms.splitlines()}
        assert {"rc_code_init", "rc_build_mode"} <= exported
        assert not exported & {"rc_call", "rc_budget", "rc_preempt_flag"}


class Host:
    """What a host does with a code library: init it, call a function, count the yields."""

    def __init__(self, lib_path, preempt=True):
        from recomp.harness.machines import CRCtx
        self.lib = ctypes.CDLL(lib_path)
        self.yields = 0
        self.flag = ctypes.c_uint32(0)
        self.refill = 0
        budget_t = ctypes.CFUNCTYPE(None, ctypes.c_void_p)

        @budget_t
        def on_budget(ctx):
            self.yields += 1
            self.flag.value = 0
            ctypes.cast(ctx, ctypes.POINTER(CRCtx)).contents.budget += self.refill

        self._keep = on_budget
        self.host = CHost(6, ctypes.sizeof(CRCtx), None, None, None, None, ctypes.cast(on_budget, ctypes.c_void_p),
                          None, None, ctypes.pointer(self.flag) if preempt else None)
        self.info = CCodeInfo()
        init = self.lib.rc_code_init
        init.argtypes = [ctypes.POINTER(CHost), ctypes.POINTER(CCodeInfo)]
        self.init = init

    def start(self):
        return self.init(ctypes.byref(self.host), ctypes.byref(self.info))

    def call(self, addr, n, budget):
        from recomp.harness.machines import CRCtx
        ctx = CRCtx()
        ctx.r[0], ctx.r[13], ctx.r[14] = n, 0x1800, RET_ADDR
        ctx.budget = budget
        # flat guest memory (native builds): the first 8 KiB of the address space is the stack;
        # the functions used here touch nothing else, and the paged build gets only `mix`,
        # which touches no memory at all
        stack = ctypes.create_string_buffer(0x2000)
        ctx.mem = ctypes.addressof(stack)
        fn = next(e.fn for e in self.info.table[:self.info.count] if e.addr == addr)
        ctypes.CFUNCTYPE(None, ctypes.POINTER(CRCtx))(fn)(ctypes.byref(ctx))
        return ctx.r[0], ctx.budget


def test_code_library_takes_the_hosts_preempt_flag(native_program):
    lib_path, _ = code_library(native_program, "native")
    _, names = native_program
    h = Host(lib_path)
    assert h.start() == 1
    big = 1 << 40
    assert h.call(names["mix"], 300, big) == (mix(300), big)      # no cycle charging
    assert h.yields == 0
    h.flag.value = 1                                              # the host asks for a yield
    assert h.call(names["mix"], 300, big)[0] == mix(300)
    assert h.yields == 1 and h.flag.value == 0
    h.flag.value = 1
    assert h.call(names["mix_twice"], 50, 0)[0] == (mix(50) + mix(51)) & 0xFFFFFFFF
    assert h.yields == 2                                          # set once, taken once
    assert h.call(names["mix"], 300, -(1 << 40))[0] == mix(300) and h.yields == 2   # budget ignored


@pytest.mark.parametrize("name", ["paged", "native"])
def test_host_without_a_preempt_flag(native_program, name):
    """A host that passes no flag gets one that stays 0 (and the paged build never looks)."""
    lib_path, _ = code_library(native_program, name)
    _, names = native_program
    h = Host(lib_path, preempt=False)
    assert h.start() == 1
    h.flag.value = 1                                              # not the library's flag
    assert h.call(names["mix"], 300, 1 << 40)[0] == mix(300)
    assert h.yields == 0


@pytest.mark.parametrize("name", ["paged", "native_det"])
def test_charging_builds_call_rc_budget_when_the_budget_runs_out(native_program, name):
    """The paged build and the deterministic native build charge cycles and yield on an empty
    budget, and ignore the preemption flag."""
    lib_path, _ = code_library(native_program, name)
    _, names = native_program
    h = Host(lib_path)
    assert h.start() == 1
    big = 1 << 40
    r, left = h.call(names["mix"], 300, big)
    assert r == mix(300) and h.yields == 0
    spent = big - left
    assert spent > 300
    h.flag.value = 1
    h.call(names["mix"], 300, big)
    assert h.yields == 0 and h.flag.value == 1                    # the flag is not looked at
    h.refill = 1 << 20
    assert h.call(names["mix"], 300, spent // 2)[0] == mix(300)
    assert h.yields == 1
    assert h.call(names["mix"], 300, spent)[0] == mix(300) and h.yields == 1   # exactly enough


def test_host_version_must_match(native_program):
    """RHost version 6 only: an older host (version 5: RCodeInfo without hook_names) is refused, not misread."""
    lib_path, _ = code_library(native_program, "native")
    h = Host(lib_path)
    h.host.version = 5
    assert h.start() == 0
    h.host.version = 6
    h.host.ctx_size += 8
    assert h.start() == 0
    h.host.ctx_size -= 8
    assert h.start() == 1


def test_runtime_flags_are_checked(tmp_path):
    """Conflicting or incomplete runtime flags fail to compile with a clear message."""
    def compile(*defines, source="recomp.h"):
        src = tmp_path / "t.c"
        src.write_text(f'#include "{source}"\nint main(void) {{ return 0; }}\n')
        r = subprocess.run(["clang", "-fsyntax-only", f"-I{RUNTIME}", *[f"-D{d}" for d in defines], str(src)],
                           capture_output=True, text=True)
        return r.returncode, r.stderr

    assert compile()[0] == 0
    assert compile("RC_PAGED")[0] == 0
    assert compile("RC_NATIVE")[0] == 0
    assert compile("RC_NATIVE", "RC_NATIVE_DETERMINISTIC")[0] == 0
    assert compile("RC_NATIVE", "RC_CHECK_RETURNS")[0] == 0
    rc, err = compile("RC_PAGED", "RC_NATIVE")
    assert rc != 0 and "RC_PAGED and RC_NATIVE" in err
    rc, err = compile("RC_NATIVE_DETERMINISTIC")
    assert rc != 0 and "together with RC_NATIVE" in err
    rc, err = compile("RC_NATIVE", "RC_NO_BUDGET")
    assert rc != 0 and "RC_NO_BUDGET" in err
    assert compile("RC_NO_BUDGET")[0] == 0                          # fuzzing keeps its build


def test_build_switches(tmp_path):
    from recomp import build as rbuild

    assert rbuild.native_defines(False, False) == []
    assert rbuild.native_defines(True, False) == ["RC_NATIVE"]
    assert rbuild.native_defines(True, True) == ["RC_NATIVE", "RC_NATIVE_DETERMINISTIC"]
    assert [rbuild.native_suffix(*a) for a in ((False, False), (True, False), (True, True))] == \
        ["", "_native", "_native_det"]
    with pytest.raises(SystemExit):
        rbuild.native_defines(False, True)
    with pytest.raises(SystemExit):
        rbuild.main([str(tmp_path), "--code-library", "--deterministic"])
