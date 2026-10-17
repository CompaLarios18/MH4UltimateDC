"""tools/recomp/interp_gen.py (N1-24, N1-28): fallback-interpreter handlers generated from the recompiler's decoders.

Every form (the 20 of the spike and the rest) is generated, then built with clang next to the C the
recompiler emits for the same instruction, and both are run on random states made by
tools/recomp/fuzz.py: registers, flags, VFP state and the memory buffer must agree (or both
must fault at the same address, or both raise the same trap). The dispatcher is checked against the
decoders on every Thumb halfword and on large random samples of ARM words. Synthetic instruction
words only; no game data.
"""
import collections
import ctypes
import os
import random
import re
import shutil
import struct
import subprocess
import sys

import pytest

HERE = os.path.dirname(__file__)
TOOLS = os.path.join(HERE, "..", "tools")
sys.path.insert(0, TOOLS)

from recomp import interp_gen as ig  # noqa: E402
from recomp.insn import B, BL, OP, RET, SVC, TRAP  # noqa: E402

needs_clang = pytest.mark.skipif(shutil.which("clang") is None, reason="clang not installed")

FORMS = {f.name: f for f in ig.FORMS}
EXTRA = {f.name: f for f in ig.EXTRA_FORMS}
# INTERP_GEN_STATES / INTERP_GEN_SEED run a bigger or different campaign by hand; the defaults are the spike's
STATES_PER_FORM = int(os.environ.get("INTERP_GEN_STATES", "1000"))
WORDS_PER_FORM = max(100, STATES_PER_FORM // 10)
SEED = int(os.environ.get("INTERP_GEN_SEED", "1"))
_generated: dict[str, ig.Generated] = {}


def generated(name: str) -> ig.Generated:
    if name not in _generated:
        _generated[name] = ig.generate_form(FORMS.get(name) or EXTRA[name], SEED)
    return _generated[name]


def all_generated() -> list[ig.Generated]:
    """Every form, in ALL_FORMS order; the ones not generated yet are generated on two processes."""
    missing = [f for f in ig.ALL_FORMS if f.name not in _generated]
    if missing:
        for g in ig.generate_all(missing, SEED, jobs=2):
            _generated[g.form.name] = g
    return [_generated[f.name] for f in ig.ALL_FORMS]


# ── the comparison with the recompiled code ─────────────────────────────────────────────

def compare_with_recompiled(form, tmp_path, states=STATES_PER_FORM):
    """Build the handler of `form` and the C the recompiler emits for random encodings of it into one
    library, run both on the same random states, and return (compared, faulted)."""
    machines = pytest.importorskip("recomp.harness.machines")
    fuzz = pytest.importorskip("recomp.fuzz")
    from recomp.arm import ArmTranslator
    from recomp.thumb import ThumbTranslator

    name = form.name
    thumb = form.mode == "thumb"
    rng = random.Random(f"n1-24:{name}:{SEED}")
    words = ig.sample_words(form, rng, WORDS_PER_FORM)
    # a form with no field is a single word (CLREX): nothing to sample
    assert len(words) >= (60 if form.fields else 1), f"only {len(words)} encodings of the form decode"
    wide = [w > 0xFFFF for w, _ in words]
    assert not any(wide) or all(wide)
    step = 4 if (not thumb or wide[0]) else 2
    code = b"".join(struct.pack("<I" if step == 4 else "<H", w) for w, _ in words)
    read16, read32 = fuzz._image_readers(code)
    if thumb:
        tr = ThumbTranslator(read16, read32)
        insns = [tr.decode(fuzz.FUZZ_BASE + step * i) for i in range(len(words))]
    else:
        tr = ArmTranslator(read32)
        insns = [tr.decode(fuzz.FUZZ_BASE + step * i, w, vec) for i, (w, vec) in enumerate(words)]
    assert all(ins.kind in form.kinds for ins in insns)

    interp_base = 0x00400000
    c = [ig.PREAMBLE % ig.cond_switch(), *generated(name).c, ""]
    c.append(f"static volatile u32 words[{len(words)}] = {{ " + ", ".join(f"0x{w:x}u" for w, _ in words) + " };")
    for i, ins in enumerate(insns):
        c.append(f"static void rf_{i}(RCtx *ctx) {{ {' '.join(ig.reference_c(ins))} }}")
        c.append(f"static void iw_{i}(RCtx *ctx) {{ ctx->r[15] = 0x{ins.addr:x}u; ih_{name}(ctx, words[{i}]); }}")
    c.append("const RFuncEntry rc_func_table[] = {")
    c += [f"  {{0x{ins.addr:08x}u, {int(thumb)}, rf_{i}}}," for i, ins in enumerate(insns)]
    c += [f"  {{0x{interp_base + step * i:08x}u, {int(thumb)}, iw_{i}}}," for i in range(len(insns))]
    c += ["};", f"const u32 rc_func_count = {2 * len(insns)};"]
    c += ["const u32 rc_hook_addrs[] = {0};", "const u32 rc_hook_count = 0;", "RHookFn rc_hooks[1];",
          "u32 rc_tunables[1];", "const u32 rc_tunable_addrs[] = {0};",
          "const char *const rc_tunable_names[] = {0};", "const u32 rc_tunable_count = 0;"]
    src, lib = tmp_path / "interp.c", tmp_path / "interp.so"
    src.write_text("\n".join(c) + "\n")
    r = subprocess.run(["clang", *fuzz.CFLAGS, "-Wno-unused-function", fuzz.HARNESS, str(src), "-o", str(lib), "-lm"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-3000:]

    code_size = (len(code) + 0x1FFF) & ~0xFFF
    regions = [machines.Region(fuzz.FUZZ_BASE, code_size, False, code),
               machines.Region(fuzz.BUF_BASE, fuzz.BUF_SIZE, True), machines.Region(fuzz.TLS, 0x1000, True)]
    nat = machines.NativeMachine(str(lib), regions)
    nat.lib.rh_set_check_return(0)
    compared = faulted = done = 0
    # 1,000 random states, and more until half of them ran to the end of the instruction: loads and
    # stores through random registers often fault (on both sides alike)
    while done < states or (compared < states // 2 and done < 6 * states):
        i = done % len(insns)
        ins = insns[i]
        done += 1
        st = fuzz.random_state(rng)
        st.fpscr |= words[i][1]
        buf = rng.randbytes(fuzz.BUF_SIZE)
        nat.write(fuzz.BUF_BASE, buf)
        a = nat.run(ins.addr, thumb, st)
        mem_a = nat.read(fuzz.BUF_BASE, fuzz.BUF_SIZE)
        nat.write(fuzz.BUF_BASE, buf)
        b = nat.run(interp_base + step * i, thumb, st)
        mem_b = nat.read(fuzz.BUF_BASE, fuzz.BUF_SIZE)
        where = f"{words[i][0]:0{2 * step}x} (fpscr vec {words[i][1]:x}) state {done}"
        assert a.status == b.status, f"{where}: status {a.status} (recompiled) vs {b.status} (handler)"
        if a.status == machines.ST_FAULT:
            assert abs(a.info0 - b.info0) < 8, f"{where}: fault address {a.info0:#x} vs {b.info0:#x}"
            faulted += 1
            continue
        if a.status == machines.ST_TRAP:
            assert (a.info0, a.info1) == (b.info0, b.info1), f"{where}: trap {a.info0:#x}/{a.info1} vs {b.info0:#x}/{b.info1}"
            compared += 1
            continue
        if a.status == machines.ST_SVC:
            assert (a.info0, a.info1) == (b.info0, b.info1), f"{where}: svc {a.info0}/{a.info1:#x} vs {b.info0}/{b.info1:#x}"
        else:
            assert a.status == machines.ST_RETURN, f"{where}: status {a.status}"
        diff = machines.diff_states(a.state, b.state, compare_pc=a.status == machines.ST_RETURN)
        if mem_a != mem_b:
            diff.append("memory differs")
        assert not diff, f"{where}: {'; '.join(diff[:6])}"
        compared += 1
    return compared, faulted


@needs_clang
@pytest.mark.parametrize("name", [f.name for f in ig.FORMS])
def test_handler_matches_recompiled_code(name, tmp_path):
    compared, faulted = compare_with_recompiled(FORMS[name], tmp_path)
    assert compared >= STATES_PER_FORM // 2, f"only {compared} states ran without a fault ({faulted} faulted)"


@needs_clang
@pytest.mark.parametrize("name", [f.name for f in ig.EXTRA_FORMS])
def test_extra_forms_match_recompiled_code(name, tmp_path):
    form = EXTRA[name]
    compared, faulted = compare_with_recompiled(form, tmp_path)
    assert compared >= STATES_PER_FORM // 2, f"only {compared} states ran without a fault ({faulted} faulted)"


@pytest.mark.parametrize("name,reason", sorted(ig.EXPECTED_FAILURES.items()))
def test_forms_the_generator_cannot_express_say_why(name, reason):
    with pytest.raises(ig.GenError, match=reason):
        ig.generate(EXTRA[name])


def test_every_form_has_a_name_and_the_spike_has_20():
    assert len(ig.FORMS) == 20
    assert len(FORMS) == 20
    assert all(f.doc for f in ig.ALL_FORMS)
    assert len({f.name for f in ig.ALL_FORMS}) == len(ig.ALL_FORMS)


# ── the generator itself ────────────────────────────────────────────────────────────────

def test_tokenize_splits_numbers_and_keeps_identifiers():
    e = ig.tokenize("u32 a = R(5) + 0x00000abcu; rd32(ctx, a + 12); x = 3ull;")
    assert e.vals == [5, 0xABC, 12, 3]
    assert e.sfx == ["", "u", "", "ull"]
    assert "rd32" in e.skel and "\x01" in e.skel and "12" not in e.skel
    assert ig.tokenize("R(7) = r;").skel == ig.tokenize("R(14) = r;").skel


def test_effect_text_ignores_unpredictable_and_refuses_jump_tables():
    dec = ig.Decoder("arm")
    assert ig.effect_text(dec.decode(0xE0010092, 0, 0x100000)) is not None          # MUL r1, r2, r0
    assert ig.effect_text(dec.decode(0xE0010F92, 0, 0x100000)) is None              # MUL with SBZ bits set
    assert ig.effect_text(dec.decode(0xE2800002, 0, 0x100000)).startswith("K:op")
    jtab = dec.decode(0xE08FF102, 0, 0x100000)                                      # add pc, pc, r2, lsl #2
    with pytest.raises(ig.GenError, match="jump-table"):
        ig.effect_text(jtab)
    trap = ig.effect_text(dec.decode(0xE7F000F0, 0, 0x100000))
    assert trap.startswith("K:trap")


def test_fit_affine_finds_offsets_and_sign_extension_and_refuses_the_rest():
    fa = ig.Var("a", None, "A")
    fb = ig.Var("b", None, "B")
    envs = [{"a": a, "b": b} for a in range(0, 9) for b in range(0, 4)]
    m = ig.fit_affine(envs, [(32 - a + 4 * b) & 0xFFFFFFFF for a, b in ((e["a"], e["b"]) for e in envs)], [fa, fb])
    assert m is not None and m.eval({"a": 7, "b": 2}) == 33
    assert "A" in m.c() and "B" in m.c()
    sx = ig.Var("a", 4, "A")
    envs = [{"a": a} for a in range(16)]
    m = ig.fit_affine(envs, [(100 + 4 * ig.sext(a, 4)) & 0xFFFFFFFF for a in range(16)], [sx])
    assert m is not None and m.eval({"a": 15}) == 96
    assert ig.fit_affine(envs, [a * a for a in range(16)], [ig.Var("a", None, "A")]) is None      # not affine
    assert ig.fit_affine(envs, [1, 2] * 8, [ig.Var("a", None, "A")]) is None


def test_solver_reports_inconsistent_systems():
    assert ig._solve([([1], 1), ([1], 2)], 1) is None
    assert ig._solve([([0], 5), ([1], 7)], 1) == [5, 2]


def test_render_reads_through_RR_and_writes_directly():
    leaf = ig.Leaf(ig.tokenize("K:op\nB:R(3) = R(5) + R(7);"),
                   [None, ig.Affine(0, [(1, ig.Var("x", None, "X"))]), None])
    text = ig.render(leaf)
    assert "R(3) = R(5)" not in text or "RR(5)" in text
    leaf = ig.Leaf(ig.tokenize("K:op\nB:R(3) = R(5) + R(7);"),
                   [None, ig.Affine(0, [(1, ig.Var("x", None, "X"))]), ig.Affine(0, [(1, ig.Var("y", None, "Y"))])])
    text = ig.render(leaf)
    assert "R(3) = RR(X) + RR(Y);" in text
    leaf = ig.Leaf(ig.tokenize("K:op\nB:R(3) = R(5);"),
                   [ig.Affine(0, [(1, ig.Var("x", None, "X"))]), None])
    assert "R(X) = R(5);" in ig.render(leaf)


def test_the_arm_condition_switch_covers_every_condition():
    sw = ig.cond_switch()
    for i, n in enumerate(ig.COND_NAMES):
        assert f"case {i}: return {'1' if n == 'AL' else 'C_' + n};" in sw
    assert "case 15" not in sw


# ── error paths ─────────────────────────────────────────────────────────────────────────

def test_a_register_list_must_be_declared_as_a_list():
    """LDM/STM unrolls one statement per set bit: declared as a plain 15-bit field the C depends on a
    wide field and no switch can express it."""
    f = FORMS["arm_ldm_stm"]
    fields = [ig.Field("list", 0, 15) if x.kind == "list" else x for x in f.fields]
    bad = ig.Form("bad_ldm", "arm", f.base, fields, kinds=f.kinds)
    with pytest.raises(ig.GenError, match="wide field list"):
        ig.generate(bad)


def test_verification_notices_a_template_that_disagrees_with_the_decoder():
    """The generated tree is checked against the decoder on random encodings: a leaf whose text was
    changed must be reported."""
    f = FORMS["t_alu"]
    gen = ig.FormGen(f)
    tree = gen.build({x.name: x.domain() for x in f.fields})
    leaf = tree
    while isinstance(leaf, ig.Split):
        leaf = leaf.classes[0][1]
    assert isinstance(leaf, ig.Leaf)
    # corrupt one constant of one leaf: the verification must notice
    leaf.eff.pieces[0] = leaf.eff.pieces[0] + " "
    with pytest.raises(ig.GenError, match="template differs from the decoder"):
        gen.verify(tree, 2000)


def test_a_form_that_decodes_nothing_is_reported():
    f = ig.Form("nothing", "arm", 0x07F000F0, [ig.Field("rm", 0, 4, "reg")])        # UDF: always a trap
    g = ig.generate(f)
    assert "rc_trap" in "\n".join(g.c)
    never = ig.Form("never", "arm", 0x0, [ig.Field("x", 0, 4)], valid=lambda a: False)
    with pytest.raises(ig.GenError, match="no valid encoding"):
        ig.generate(never)


def test_a_relation_between_a_register_and_the_list_needs_list_base():
    """Thumb LDMIA writes the base back unless it is in the list: without list_base no single field
    explains the two shapes."""
    f = EXTRA["t_ldm"]
    bad = ig.Form("bad_ldm", "thumb", f.base, f.fields, kinds=f.kinds)
    with pytest.raises(ig.GenError, match="relation between fields"):
        ig.generate(bad)


def test_a_table_that_would_be_too_big_is_reported(monkeypatch):
    monkeypatch.setattr(ig, "MAX_TABLE_BITS", 8)
    with pytest.raises(ig.GenError, match=r"a table would need 12 index bits"):
        ig.generate(FORMS["arm_dp_imm"])


def test_register_lists_need_three_sizes():
    f = FORMS["t_push_pop"]
    bad = ig.Form("bad_push", "thumb", f.base, f.fields, kinds=f.kinds,
                  valid=lambda a: bin(a["list"]).count("1") in (1, 2))
    with pytest.raises(ig.GenError, match="fewer than three sizes"):
        ig.generate(bad)


def test_overlapping_and_repeated_fields_are_refused():
    with pytest.raises(ig.GenError, match="overlaps"):
        ig.Form("x", "arm", 0, [ig.Field("a", 0, 4), ig.Field("b", 2, 4)])
    with pytest.raises(ig.GenError, match="overlaps"):
        ig.Form("x", "arm", 0x10, [ig.Field("a", 0, 8)])
    with pytest.raises(ig.GenError, match="declared twice"):
        ig.Form("x", "arm", 0, [ig.Field("a", 0, 4), ig.Field("a", 8, 4)])
    with pytest.raises(ig.GenError, match="list_base"):
        ig.Form("x", "arm", 0, [ig.Field("a", 0, 4)], list_base="nope")
    ig.Form("x", "arm", 0, [ig.Field("a", 0, 4), ig.Field("v", 0, 4, "num", "fpscr")])   # FPSCR bits are separate


def test_cli_reports_failures_and_known_gaps(monkeypatch, capsys):
    assert ig.main(["--form", "arm_vldm"]) == 0
    assert "(hand-written)" in capsys.readouterr().out
    monkeypatch.setattr(ig, "HAND_WRITTEN", {})            # without the hand-written handler it is a known gap
    assert ig.main(["--form", "arm_vldm"]) == 0
    assert "not generated (known)" in capsys.readouterr().out
    f = FORMS["arm_ldm_stm"]
    bad = ig.Form("bad_cli", "arm", f.base, [ig.Field("list", 0, 15) if x.kind == "list" else x for x in f.fields])
    monkeypatch.setattr(ig, "FORMS", [bad])
    assert ig.main(["--form", "bad_cli"]) == 1
    assert "FAILED" in capsys.readouterr().out


def test_cli_lists_unknown_forms_and_writes_a_c_file(tmp_path, capsys):
    assert ig.main(["--form", "no_such_form"]) == 2
    assert "no_such_form" in capsys.readouterr().err
    out = tmp_path / "h.c"
    assert ig.main(["--form", "t_push_pop", "--out", str(out)]) == 0
    text = out.read_text()
    assert "void ih_t_push_pop(RCtx *ctx, u32 insn)" in text and "ih_cond" in text
    assert "t_push_pop" in capsys.readouterr().out


def test_sample_words_respects_kinds_and_validity():
    rng = random.Random(5)
    for name in ("arm_b_bl", "arm_bx_blx", "arm_ldm_stm", "t_push_pop"):
        form = FORMS[name]
        words = ig.sample_words(form, rng, 20)
        assert len(words) == 20
        dec = ig.Decoder(form.mode)
        assert all(dec.decode(w, v, ig.ADDRS[0]).kind in form.kinds for w, v in words)
    assert {dec_kind for dec_kind in (B, BL, RET, OP, TRAP)} >= set(FORMS["arm_b_bl"].kinds)


def test_reference_c_for_control_flow_and_unsupported_kinds():
    dec = ig.Decoder("arm")
    bl = dec.decode(0xEB000002, 0, 0x100000)
    ref = " ".join(ig.reference_c(bl))
    assert "R(14) = 0x100004u" in ref and "ctx->r[15] = 0x100010u" in ref
    cond_b = dec.decode(0x1A000000, 0, 0x100000)
    ref = " ".join(ig.reference_c(cond_b))
    assert ref.index("ctx->r[15] = 0x100004u") < ref.index("if (C_NE)")             # falls through first
    svc = dec.decode(0xEF000011, 0, 0x100000)
    assert "rc_svc" in " ".join(ig.reference_c(svc))
    thumb = ig.Decoder("thumb")
    assert "ctx->r[15] = R(14);" in " ".join(ig.reference_c(thumb.decode(0x4770, 0, 0x100000)))
    ins = dec.decode(0xE0010092, 0, 0x100000)
    ins.kind = "weird"
    with pytest.raises(ig.GenError, match="no reference"):
        ig.reference_c(ins)


# ── where expressions, routes and the dispatcher's tables (no compiler) ─────────────────────

def test_where_expressions_become_c_and_python_alike():
    fields = {f.name: f for f in (ig.Field("a", 0, 4), ig.Field("b", 4, 4), ig.Field("c", 8, 1))}
    c = ig.py_to_c("not (8 <= a <= 11 and c == 0) or b in (1, 2) or (a & 3) + 1 > b >> 1", fields)
    assert "&&" in c and "||" in c and "!" in c and "b" not in c.replace("0xF", "")   # names became field reads
    assert ig.py_to_c("a == 15", fields) == "(((insn) & 0xFu) == 15u)"
    form = ig.Form("w", "arm", 0x0A000000, list(fields.values()), where="not (8 <= a <= 11 and c == 0) or b in (1, 2)")
    assert form.valid({"a": 9, "b": 0, "c": 1}) and form.valid({"a": 9, "b": 2, "c": 0})
    assert not form.valid({"a": 9, "b": 0, "c": 0})
    assert form.where_c.count("||") == 2          # the `or`, and the two values of `in`


def test_where_expressions_refuse_what_has_no_translation():
    f = {"a": ig.Field("a", 0, 4)}
    with pytest.raises(ig.GenError, match="not a field"):
        ig.py_to_c("zz == 1", f)
    with pytest.raises(ig.GenError, match="unsupported construct"):
        ig.py_to_c("abs(a) == 1", f)
    with pytest.raises(ig.GenError, match="unsupported construct"):
        ig.py_to_c("a == -1", f)
    with pytest.raises(ig.GenError, match="unsupported comparison"):
        ig.py_to_c("a is 1", f)
    with pytest.raises(ig.GenError, match="not a field"):
        ig.Form("w", "arm", 0, [ig.Field("a", 0, 4)], where="b == 1")


def test_routes_fix_the_bits_outside_the_fields_and_limit_the_fields_with_values():
    r = ig.make_route(EXTRA["arm_cp15"], 7)
    assert r.mask & 0x0F000010 == 0x0F000010 and r.value & r.mask == r.value
    assert r.mask & (0x7 << 21) == (0x7 << 21)                              # opc1 has a single value: part of the mask
    assert [f.name for f, _ in r.sets] == ["crn", "opc2", "crm"]            # the others are sets
    assert r.matches(0x0E070F95 & 0x0FFFFFFF | 0x0E000000) in (True, False)  # (just: it does not raise)
    uncond = ig.make_route(EXTRA["arm_blx_imm"], 1)
    assert uncond.mask >> 28 == 0xF and uncond.value >> 28 == 0xF
    wide = ig.make_route(EXTRA["t_bl"], 2)
    assert wide.mask > 0xFFFF and ig.thumb_wide(EXTRA["t_bl"]) and not ig.thumb_wide(EXTRA["t_svc"])
    assert ig.make_route(EXTRA["t_svc"], 3).mask == 0xFF00


def test_dispatch_tables_have_a_route_for_every_word_the_model_routes():
    routes, disp = ig.build_dispatchers(ig.ALL_FORMS)
    assert [r.id for r in routes] == list(range(1, len(ig.ALL_FORMS) + 1))
    rng = random.Random(3)
    for key in ("arm", "arm_uncond", "thumb"):
        d = disp[key]
        assert len(d.slots) == 1 << bin(d.index_mask).count("1")
        assert all((s & d.CASCADE) == 0 or (s & 0x7FFF) < len(d.cascades) for s in d.slots)
    assert disp["thumb"].default == len(routes) + 1 and disp["arm"].default == 0
    # the lookup of the model is the scan of the routes in order
    for _ in range(3000):
        w = rng.getrandbits(32)
        d = disp["arm_uncond"] if w >> 28 == 0xF else disp["arm"]
        assert d.lookup(w) == next((r.id for r in d.routes if r.matches(w)), 0)


# ── the whole interpreter, built once ───────────────────────────────────────────────────────

@pytest.fixture(scope="module")
def interp(tmp_path_factory):
    """All forms and the dispatcher as one C file (what the native build generates), compiled with clang, and a
    small library exporting the classification functions for ctypes."""
    if shutil.which("clang") is None:
        pytest.skip("clang not installed")
    fuzz = pytest.importorskip("recomp.fuzz")
    work = tmp_path_factory.mktemp("interp")
    results = all_generated()
    c = ig.file_c(results, ig.ALL_FORMS)
    src = work / "handlers.c"
    src.write_text(c)
    flags = ["-O1", "-c", "-fPIC", "-ffp-contract=off", "-frounding-math", "-fno-strict-aliasing", "-fwrapv",
             "-Wall", "-Wextra", "-Wno-unused-parameter", f"-I{fuzz.RUNTIME}", "-DRC_NO_BUDGET"]
    r = subprocess.run(["clang", *flags, str(src), "-o", str(work / "handlers.o")], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-3000:]
    harness = work / "harness.o"
    r = subprocess.run(["clang", "-O1", "-c", "-fPIC", "-ffp-contract=off", "-frounding-math", "-fno-strict-aliasing",
                        "-fwrapv", "-DRC_NO_BUDGET", f"-I{fuzz.RUNTIME}", fuzz.HARNESS, "-o", str(harness)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-3000:]
    glue = work / "classify.c"
    glue.write_text('''#include "recomp.h"
void rc_svc(RCtx *c, u32 a, u32 b) { (void)c; (void)a; (void)b; }
void rc_trap(RCtx *c, u32 a, u32 b) { (void)c; (void)a; (void)b; }
u32 ih_classify_arm(u32); u32 ih_classify_thumb(u32); u32 ih_unpredictable_arm(u32); u32 ih_unpredictable_thumb(u32);
void batch_arm(const u32 *w, u32 n, u32 *route, u32 *unp) {
    for (u32 i = 0; i < n; i++) { route[i] = ih_classify_arm(w[i]); unp[i] = ih_unpredictable_arm(w[i]); } }
void batch_thumb(const u32 *w, u32 n, u32 *route, u32 *unp) {
    for (u32 i = 0; i < n; i++) { route[i] = ih_classify_thumb(w[i]); unp[i] = ih_unpredictable_thumb(w[i]); } }
''')
    lib = work / "classify.so"
    r = subprocess.run(["clang", "-O1", "-fPIC", "-shared", "-w", f"-I{fuzz.RUNTIME}", str(glue), str(work / "handlers.o"),
                        "-o", str(lib), "-lm"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-3000:]
    return work, ctypes.CDLL(str(lib)), ig.build_dispatchers(ig.ALL_FORMS)


def classify(lib, fn: str, words: list[int]) -> tuple[list[int], list[int]]:
    n = len(words)
    w, route, unp = (ctypes.c_uint32 * n)(*words), (ctypes.c_uint32 * n)(), (ctypes.c_uint32 * n)()
    getattr(lib, fn)(w, n, route, unp)
    return list(route), list(unp)


def arm_words(rng: random.Random, count: int) -> list[int]:
    """Random ARM words: a mix of uniform ones and ones from the busy spaces (VFP, media, multiplies), with the
    condition mostly 'always' and sometimes the unconditional space."""
    fuzz = pytest.importorskip("recomp.fuzz")
    out = []
    for _ in range(count):
        w = fuzz.random_word(rng, rng.choice([None, None, "vfp", "media", "mul"]))
        if rng.random() < 0.15:
            w = (w & 0x0FFFFFFF) | 0xF0000000
        out.append(w)
    return out


def thumb_pairs() -> list[tuple[int, int]]:
    """Every halfword; those that open a BL / BLX with each kind of second halfword (suffix of BL, of BLX with bit
    0 clear and set, neither)."""
    out = []
    for h in range(0x10000):
        if h >> 11 == 0x1E:
            out += [(h, 0xF800 | (h & 0x7FF)), (h, 0xE800 | (h & 0x7FE)), (h, 0xE801), (h, 0x0000)]
        else:
            out.append((h, 0))
    return out


def thumb_decode(h: int, h2: int):
    from recomp.thumb import ThumbTranslator
    img = {0x100000: h, 0x100002: h2}
    return ThumbTranslator(img.get, lambda a: None).decode(0x100000)


NARM = int(os.environ.get("INTERP_GEN_DISPATCH_WORDS", "300000"))


@needs_clang
def test_tables_in_c_agree_with_the_scan_of_the_routes(interp):
    work, lib, (routes, disp) = interp
    rng = random.Random(SEED + 100)
    words = arm_words(rng, NARM // 3)
    got, _ = classify(lib, "batch_arm", words)
    for w, r in zip(words, got):
        assert r == (disp["arm_uncond"] if w >> 28 == 0xF else disp["arm"]).lookup(w), f"{w:08x}"
    pairs = thumb_pairs()
    got, _ = classify(lib, "batch_thumb", [h | (h2 << 16) for h, h2 in pairs])
    bad = [(h, h2) for (h, h2), r in zip(pairs, got) if r != disp["thumb"].lookup(h | (h2 << 16))]
    assert not bad, [f"{h:04x} {h2:04x}" for h, h2 in bad[:10]]


@needs_clang
def test_the_dispatcher_routes_what_the_decoders_accept_and_says_what_is_unpredictable(interp):
    """Every word the decoder translates goes to a form (not to the 'unsupported' route), and the C predicate calls a
    word UNPREDICTABLE exactly when the decoder does (arm.py / thumb.py raise Unpredictable)."""
    from recomp.arm import ArmTranslator
    work, lib, (routes, disp) = interp
    rng = random.Random(SEED + 200)
    words = arm_words(rng, NARM)
    got, unp = classify(lib, "batch_arm", words)
    dec = ArmTranslator(lambda a: None)
    stats = collections.Counter()
    for w, r, u in zip(words, got, unp):
        ins = dec.decode(0x100000, w, 0)
        is_unp = ins.kind == TRAP and ins.trap_reason.startswith("unpredictable")
        assert bool(u) == is_unp, f"{w:08x}: predicate {u}, decoder {ins.trap_reason or ins.kind}"
        if ins.kind != TRAP:
            assert r != 0, f"{w:08x} decodes ({ins.kind}) but goes to the unsupported route"
        stats["unpredictable" if is_unp else "trap" if ins.kind == TRAP else "valid"] += 1
    assert min(stats.values()) > NARM // 50, stats               # the sample has all three kinds
    pairs = thumb_pairs()
    got, unp = classify(lib, "batch_thumb", [h | (h2 << 16) for h, h2 in pairs])
    for (h, h2), r, u in zip(pairs, got, unp):
        ins = thumb_decode(h, h2)
        is_unp = ins.kind == TRAP and ins.trap_reason.startswith("unpredictable")
        assert bool(u) == is_unp, f"{h:04x} {h2:04x}: predicate {u}, decoder {ins.trap_reason or ins.kind}"
        if ins.kind != TRAP:
            assert r != len(routes) + 1, f"{h:04x} {h2:04x} decodes ({ins.kind}) but goes to the unsupported route"


# ── the dispatcher against the recompiled code ─────────────────────────────────────────────

def run_dispatched(interp, words, thumb, states, seed):
    """Builds the recompiled C of each word next to a function that gives the word to the dispatcher, runs both
    from the same random states, and returns (counters, problems). `words` are (word, size) in memory order."""
    fuzz = pytest.importorskip("recomp.fuzz")
    machines = pytest.importorskip("recomp.harness.machines")
    from recomp.arm import ArmTranslator
    from recomp.thumb import ThumbTranslator
    work = interp[0]
    code = b"".join(struct.pack("<I" if sz == 4 else "<H", w) for w, sz in words)
    read16, read32 = fuzz._image_readers(code)
    items, off = [], 0
    for w, sz in words:
        addr = fuzz.FUZZ_BASE + off
        ins = ThumbTranslator(read16, read32).decode(addr) if thumb else ArmTranslator(read32).decode(addr, w, 0)
        items.append((addr, ins))
        off += sz
    base2 = 0x00800000
    c = ['#include "recomp.h"', "void ih_exec_arm(RCtx *, u32); void ih_exec_thumb(RCtx *, u32);"]
    entries = []
    for i, (addr, ins) in enumerate(items):
        if ins.kind == TRAP and ins.trap_reason.startswith("unpredictable"):
            continue                                     # any behaviour is allowed; ih_step logs it
        c.append(f"static void rf_{i}(RCtx *ctx) {{ {' '.join(ig.reference_c(ins))} }}")
        c.append(f"static void iw_{i}(RCtx *ctx) {{ ctx->r[15] = 0x{addr:x}u; "
                 f"ih_exec_{'thumb' if thumb else 'arm'}(ctx, 0x{ins.word:x}u); }}")
        entries += [(addr, i, "rf"), (base2 + addr - fuzz.FUZZ_BASE, i, "iw")]
    entries.sort()
    c.append("const RFuncEntry rc_func_table[] = {")
    c += [f"  {{0x{a:08x}u, {int(thumb)}, {k}_{i}}}," for a, i, k in entries]
    c += ["};", f"const u32 rc_func_count = {len(entries)};", "const u32 rc_hook_addrs[] = {0};",
          "const u32 rc_hook_count = 0;", "RHookFn rc_hooks[1];", "u32 rc_tunables[1];",
          "const u32 rc_tunable_addrs[] = {0};", "const char *const rc_tunable_names[] = {0};",
          "const u32 rc_tunable_count = 0;"]
    tag = f"d{'t' if thumb else 'a'}{seed}"
    (work / f"{tag}.c").write_text("\n".join(c) + "\n")
    r = subprocess.run(["clang", "-O0", "-c", "-fPIC", "-ffp-contract=off", "-frounding-math", "-fno-strict-aliasing",
                        "-fwrapv", "-w", "-DRC_NO_BUDGET", f"-I{fuzz.RUNTIME}", str(work / f"{tag}.c"), "-o",
                        str(work / f"{tag}.o")], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-3000:]
    r = subprocess.run(["clang", "-shared", str(work / "harness.o"), str(work / "handlers.o"), str(work / f"{tag}.o"),
                        "-o", str(work / f"{tag}.so"), "-lm"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-3000:]
    regions = [machines.Region(fuzz.FUZZ_BASE, (len(code) + 0x1FFF) & ~0xFFF, False, code),
               machines.Region(fuzz.BUF_BASE, fuzz.BUF_SIZE, True), machines.Region(fuzz.TLS, 0x1000, True)]
    nat = machines.NativeMachine(str(work / f"{tag}.so"), regions)
    nat.lib.rh_set_check_return(0)
    rng = random.Random(seed)
    stats, problems = collections.Counter(), []
    for addr, ins in items:
        if ins.kind == TRAP and ins.trap_reason.startswith("unpredictable"):
            stats["unpredictable"] += 1
            continue
        for _ in range(states):
            st = fuzz.random_state(rng)
            buf = rng.randbytes(fuzz.BUF_SIZE)
            nat.write(fuzz.BUF_BASE, buf)
            a = nat.run(addr, thumb, st)
            mem_a = nat.read(fuzz.BUF_BASE, fuzz.BUF_SIZE)
            nat.write(fuzz.BUF_BASE, buf)
            b = nat.run(base2 + addr - fuzz.FUZZ_BASE, thumb, st)
            mem_b = nat.read(fuzz.BUF_BASE, fuzz.BUF_SIZE)
            where = f"{ins.word:0{2 * ins.size}x} ({ins.kind}{' ' + ins.trap_reason if ins.trap_reason else ''})"
            msg = None
            if a.status != b.status:
                msg = f"status {a.status} ({a.info0:#x},{a.info1:#x}) vs {b.status} ({b.info0:#x},{b.info1:#x})"
            elif a.status == machines.ST_FAULT:
                msg = None if abs(a.info0 - b.info0) < 8 else f"fault {a.info0:#x} vs {b.info0:#x}"
            elif a.status in (machines.ST_TRAP, machines.ST_SVC):
                msg = None if (a.info0, a.info1) == (b.info0, b.info1) else f"{a.status}: {a.info0:#x},{a.info1:#x} vs {b.info0:#x},{b.info1:#x}"
            elif a.status == machines.ST_RETURN:
                d = machines.diff_states(a.state, b.state, compare_pc=True)
                if mem_a != mem_b:
                    d.append("memory differs")
                msg = "; ".join(d[:5]) or None
            else:
                msg = f"status {a.status}"
            stats[machines.STATUS_NAMES[a.status]] += 1
            if msg:
                problems.append(f"{where}: {msg}")
                break
    return stats, problems


@needs_clang
def test_every_thumb_halfword_runs_like_the_recompiled_code(interp):
    """All 65,536 halfwords (and the BL / BLX pairs they open), each through the dispatcher and through the C the
    recompiler emits, from random states."""
    words = []
    for h, h2 in thumb_pairs():
        words.append((h, 2))
        if h >> 11 == 0x1E:
            words.append((h2, 2))
    stats, problems = run_dispatched(interp, words, True, int(os.environ.get("INTERP_GEN_THUMB_STATES", "1")), SEED)
    assert not problems, problems[:10]
    assert stats["return"] > 20000 and stats["trap"] > 1000 and stats["svc"] > 100, stats


@needs_clang
def test_random_arm_words_run_like_the_recompiled_code(interp):
    rng = random.Random(SEED + 300)
    count = int(os.environ.get("INTERP_GEN_RUN_WORDS", "15000"))
    words = [(w, 4) for w in arm_words(rng, count)]
    stats, problems = run_dispatched(interp, words, False, 2, SEED)
    assert not problems, problems[:10]
    assert stats["return"] > count // 3 and stats["trap"] > count // 10 and stats["unpredictable"] > 100, stats


@needs_clang
def test_a_handler_survives_every_unpredictable_word_it_is_given(interp):
    """The nearest-form handlers touch only the register file and the buffer: run unpredictable words (the ones the
    run tests skip) from random states; each ends, with a fault or a trap at worst."""
    from recomp.arm import ArmTranslator
    rng = random.Random(SEED + 400)
    dec = ArmTranslator(lambda a: None)
    words = []
    while len(words) < 3000:
        w = arm_words(rng, 1)[0]
        ins = dec.decode(0x100000, w, 0)
        if ins.kind == TRAP and ins.trap_reason.startswith("unpredictable"):
            words.append(w)
    fuzz = pytest.importorskip("recomp.fuzz")
    machines = pytest.importorskip("recomp.harness.machines")
    work = interp[0]
    code = b"".join(struct.pack("<I", w) for w in words)
    c = ['#include "recomp.h"', "void ih_exec_arm(RCtx *, u32);"]
    c += [f"static void iw_{i}(RCtx *ctx) {{ ctx->r[15] = 0x{fuzz.FUZZ_BASE + 4 * i:x}u; ih_exec_arm(ctx, 0x{w:x}u); }}"
          for i, w in enumerate(words)]
    c.append("const RFuncEntry rc_func_table[] = {")
    c += [f"  {{0x{fuzz.FUZZ_BASE + 4 * i:08x}u, 0, iw_{i}}}," for i in range(len(words))]
    c += ["};", f"const u32 rc_func_count = {len(words)};", "const u32 rc_hook_addrs[] = {0};", "const u32 rc_hook_count = 0;",
          "RHookFn rc_hooks[1];", "u32 rc_tunables[1];", "const u32 rc_tunable_addrs[] = {0};",
          "const char *const rc_tunable_names[] = {0};", "const u32 rc_tunable_count = 0;"]
    (work / "unp.c").write_text("\n".join(c) + "\n")
    for cmd in (["clang", "-O0", "-c", "-fPIC", "-ffp-contract=off", "-frounding-math", "-fwrapv", "-w", "-DRC_NO_BUDGET",
                 f"-I{fuzz.RUNTIME}", str(work / "unp.c"), "-o", str(work / "unp.o")],
                ["clang", "-shared", str(work / "harness.o"), str(work / "handlers.o"), str(work / "unp.o"), "-o",
                 str(work / "unp.so"), "-lm"]):
        r = subprocess.run(cmd, capture_output=True, text=True)
        assert r.returncode == 0, r.stderr[-2000:]
    regions = [machines.Region(fuzz.FUZZ_BASE, (len(code) + 0x1FFF) & ~0xFFF, False, code),
               machines.Region(fuzz.BUF_BASE, fuzz.BUF_SIZE, True), machines.Region(fuzz.TLS, 0x1000, True)]
    nat = machines.NativeMachine(str(work / "unp.so"), regions)
    nat.lib.rh_set_check_return(0)
    seen = collections.Counter()
    for i in range(len(words)):
        for fpscr_vec in (0, 0x00070000 | 0x00100000):        # scalar, and the longest vectors with a bad stride
            st = fuzz.random_state(rng)
            st.fpscr |= fpscr_vec
            nat.write(fuzz.BUF_BASE, rng.randbytes(fuzz.BUF_SIZE))
            res = nat.run(fuzz.FUZZ_BASE + 4 * i, False, st)
            seen[machines.STATUS_NAMES[res.status]] += 1
            assert res.status in (machines.ST_RETURN, machines.ST_FAULT, machines.ST_TRAP, machines.ST_SVC), \
                f"{words[i]:08x}: {machines.STATUS_NAMES[res.status]}"
    assert seen["return"] > 1000, seen


# ── the generator's command line, with the dispatcher ─────────────────────────────────────

def test_cli_all_writes_the_dispatcher_and_the_step_function(tmp_path, monkeypatch, capsys):
    small = [EXTRA["arm_clz"], EXTRA["t_svc"], EXTRA["arm_blx_imm"], EXTRA["arm_vldm"]]
    monkeypatch.setattr(ig, "ALL_FORMS", small)
    out = tmp_path / "all.c"
    assert ig.main(["--all", "--out", str(out), "--jobs", "1"]) == 0
    text = out.read_text()
    for needle in ("u32 ih_step(RCtx *ctx)", "u32 ih_classify_arm", "u32 ih_classify_thumb", "ih_unpredictable_arm",
                   "ih_unpredictable_thumb", "const ih_fn ih_routes[]", "void ih_arm_vldm", "void ih_arm_clz",
                   "ih_trap_unsupported_thumb", '"t_svc"'):
        assert needle in text, needle
    assert "arm_vldm" in capsys.readouterr().out


def test_generate_all_in_processes_gives_what_one_process_gives():
    forms = [EXTRA["arm_clz"], EXTRA["arm_sel"], EXTRA["t_svc"]]
    one = ig.generate_all(forms, 1, verify=200, jobs=1)
    two = ig.generate_all(forms, 1, verify=200, jobs=2)
    assert [g.c for g in one] == [g.c for g in two] and [g.form.name for g in two] == [f.name for f in forms]


def test_reference_c_for_a_jump_table_instruction_jumps():
    dec = ig.Decoder("arm")
    add_pc = dec.decode(0xE08FF102, 0, 0x100000)                 # add pc, pc, r2, lsl #2
    ldr_pc = dec.decode(0xE79FF102, 0, 0x100000)                 # ldr pc, [pc, r2, lsl #2]
    assert add_pc.kind == "jtab" and ldr_pc.kind == "jtab"
    assert "ctx->r[15] = 0x100008u + (R(2) << 2);" in " ".join(ig.reference_c(add_pc))
    assert "rd32(ctx, 0x100008u + (R(2) << 2))" in " ".join(ig.reference_c(ldr_pc))


def test_the_hand_written_handlers_are_generated_forms_too():
    assert set(ig.HAND_WRITTEN) == set(ig.EXPECTED_FAILURES)
    g = ig.generate_form(EXTRA["arm_vldm"])
    assert g.c[0].startswith("void ih_arm_vldm(RCtx *ctx, u32 insn)") and g.leaves == 0


# ── svc_test through the native runner, its functions forced into the interpreter ───────────

def _svc_test_libraries(tmp_path):
    """Builds svc_test, its native code library with every function, and one without the worker functions and the
    functions called through pointers (as test_interpreter_fallback_matches_jit does for the Azahar build)."""
    sys.path.insert(0, os.path.join(HERE, "..", "port", "tests"))
    import build_svc_test
    from recomp import build as rbuild
    elf = build_svc_test.build(str(tmp_path), "svc_test")
    syms = {}
    for line in subprocess.run(["llvm-readelf", "-s", elf], capture_output=True, text=True, check=True).stdout.splitlines():
        p = line.split()
        if len(p) >= 8 and p[3] == "FUNC":
            syms[p[7]] = int(p[1], 16) & ~1
    build = lambda name: rbuild.build(str(tmp_path), 2, "O1", rbuild.native_defines(True, False), name,  # noqa: E731
                                      [rbuild.DLL_GLUE], extra_flags=["-fvisibility=hidden"])
    full = build("svc_all.so")
    table = tmp_path / "src" / "table.c"
    original = table.read_text().splitlines()

    def rewrite(keep):
        lines = [ln for ln in original if keep(ln)]
        count = sum(1 for ln in lines if ln.strip().startswith("{0x"))
        table.write_text("\n".join(re.sub(r"rc_func_count = \d+;", f"rc_func_count = {count};", ln) for ln in lines) + "\n")
    drop = {syms[n] for n in ("worker", "op_add", "op_xor", "op_mul")}
    rewrite(lambda ln: not any(f"{{0x{a:08x}u," in ln for a in drop))
    workers = build("svc_workers.so")
    rewrite(lambda ln: not ln.strip().startswith("{0x"))
    none = build("svc_none.so")
    return elf, full, workers, none, drop


@pytest.mark.skipif(not os.environ.get("MH4N_RUNNER"), reason="native runner not set (MH4N_RUNNER)")
@needs_clang
def test_svc_test_passes_natively_with_its_functions_forced_into_the_interpreter(tmp_path):
    runner = os.environ["MH4N_RUNNER"]
    elf, full, workers, none, drop = _svc_test_libraries(tmp_path)
    with open(os.path.join(HERE, "..", "port", "tests", "expected", "svc_test.txt"), encoding="utf-8") as f:
        expected = f.read().splitlines()

    def run(lib, entries):
        r = subprocess.run([runner, elf, "--code", lib, "--ms", "5000", "--user", str(tmp_path / "user"),
                            "--missing-entries", str(entries)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, timeout=600)
        assert r.returncode == 0, r.stdout[-2000:]
        lines = [m.group(1) for ln in r.stdout.splitlines() if (m := re.search(r"OutputDebugString:\d+: (.*)$", ln))]
        return lines, r.stdout
    full_entries = tmp_path / "full.txt"
    assert run(full, full_entries)[0] == expected
    assert not full_entries.exists()                              # nothing was interpreted
    worker_entries = tmp_path / "workers.txt"
    lines, out = run(workers, worker_entries)
    assert lines == expected
    interpreted = set(re.findall(r"interpreting ([0-9A-F]{8})", out))
    assert interpreted == {f"{a:08X}" for a in drop}, interpreted
    rows = [ln.split("\t") for ln in worker_entries.read_text().splitlines()]
    assert {int(r[0], 16) for r in rows} == drop and all(r[1:] == ["arm", "interpreted"] for r in rows)
    # the whole program in the interpreter, from its entry point
    none_entries = tmp_path / "none.txt"
    lines, out = run(none, none_entries)
    assert lines == expected
    assert "interpreting 00100000 (arm)" in out


def test_a_corner_the_where_rules_out_is_unreachable_not_an_error():
    """With seed 7 the tree of vfp_arith splits on fields in an order that leaves a sub-region (p = 1 with q, r or s
    set) in which no encoding is valid: it becomes a trap leaf no routed word can reach."""
    g = ig.generate_form(EXTRA.get("vfp_arith") or FORMS["vfp_arith"], 7, verify=300)
    assert g.leaves > 100
    never = ig.Form("never", "arm", 0x0, [ig.Field("x", 0, 4)], valid=lambda a: False)
    with pytest.raises(ig.EmptyRegion):            # at the top of the tree it is still an error
        ig.generate(never)


# ── the test data of native/tests/interp_test.cpp ────────────────────────────────────────

def _load_make_test_data():
    import importlib.util
    path = os.path.join(HERE, "..", "native", "src", "cpu", "interp", "make_test_data.py")
    spec = importlib.util.spec_from_file_location("make_test_data", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_the_buffer_the_vectors_use_is_a_documented_generator():
    mtd = _load_make_test_data()
    words = struct.unpack("<4096I", mtd.fill_window(1))
    assert words[0] == (1 * 1664525 + 1013904223) & 0xFFFFFFFF
    assert words[1] == (words[0] * 1664525 + 1013904223) & 0xFFFFFFFF
    buf = mtd.buffer(5)
    assert len(buf) == mtd.BUF_SIZE
    assert buf[:mtd.WINDOW] == bytes(mtd.WINDOW) and buf[mtd.WINDOW + 4 * 4096:] == bytes(mtd.BUF_SIZE - mtd.WINDOW - 16384)
    assert mtd.fill_window(5) != mtd.fill_window(6)


@needs_clang
def test_make_test_data_writes_vectors_the_cpp_test_can_read(tmp_path):
    pytest.importorskip("recomp.harness.machines")
    mtd = _load_make_test_data()
    assert mtd.main([str(tmp_path), "--vectors", "3"]) == 0
    for name in ("svc_test.elf", "svc_all.so", "svc_workers.so", "svc_none.so", "vectors.bin"):
        assert (tmp_path / name).stat().st_size > 1000, name
    data = (tmp_path / "vectors.bin").read_bytes()
    assert data[:4] == b"IHV1"
    pos = 4
    images, = struct.unpack_from("<I", data, pos)
    pos += 4
    assert images == 2
    for _ in range(images):
        base, size = struct.unpack_from("<II", data, pos)
        pos += 8 + size
        assert size % 4 == 0 and base in (0x00100000, 0x00200000)
    count, = struct.unpack_from("<I", data, pos)
    pos += 4
    kinds = collections.Counter()
    for _ in range(count):
        image, addr, thumb, word, kind = struct.unpack_from("<5I", data, pos)
        pos += 5 * 4 + (15 + 6 + 32 + 3) * 4
        assert kind in (0, 1, 2) and thumb in (0, 1) and image in (0x00100000, 0x00200000)
        pos += (16 + 6 + 32 + 1 + 1) * 4 if kind == 0 else 2 * 4
        kinds[kind] += 1
    assert pos == len(data), (pos, len(data))                # the record sizes are the ones interp_test.cpp reads
    assert count > 100 and kinds[0] > 50 and kinds[1] > 0 and kinds[2] > 0, kinds


def test_make_test_data_fails_loudly_without_its_compiler(tmp_path):
    mtd = _load_make_test_data()
    with pytest.raises((FileNotFoundError, SystemExit, subprocess.CalledProcessError)):
        mtd.main([str(tmp_path), "--cc", "/nonexistent/clang", "--no-vectors"])
