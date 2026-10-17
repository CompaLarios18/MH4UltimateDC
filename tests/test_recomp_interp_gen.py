"""tools/recomp/interp_gen.py (N1-24): fallback-interpreter handlers generated from the recompiler's decoders.

Every one of the 20 forms of the spike is generated, then built with clang next to the C the
recompiler emits for the same instruction, and both are run on random states made by
tools/recomp/fuzz.py: registers, flags, VFP state and the memory buffer must agree (or both
must fault at the same address). Synthetic instruction words only; no game data.
"""
import os
import random
import shutil
import struct
import subprocess
import sys

import pytest

HERE = os.path.dirname(__file__)
TOOLS = os.path.join(HERE, "..", "tools")
sys.path.insert(0, TOOLS)

from recomp import interp_gen as ig  # noqa: E402
from recomp.insn import B, BL, OP, RET, TRAP  # noqa: E402

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
        _generated[name] = ig.generate(FORMS.get(name) or EXTRA[name], SEED)
    return _generated[name]


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
    assert len(words) >= 60, f"only {len(words)} encodings of the form decode"
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
@pytest.mark.parametrize("name", [f.name for f in ig.EXTRA_FORMS if f.name not in ig.EXPECTED_FAILURES])
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
    assert all(f.doc for f in ig.FORMS)


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
