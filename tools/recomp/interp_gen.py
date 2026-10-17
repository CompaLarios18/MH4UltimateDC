#!/usr/bin/env python3
"""Fallback-interpreter handlers generated from the recompiler's own decoders (N1-24 spike).

The recompiler turns one instruction word into `Insn.body`, C statements in which the
registers, immediates and short-vector lengths are baked in as literals. This module
inverts that for a run-time decoder: for an instruction *form* (a set of encodings that
differ in a few bit fields) it asks the real decoders (`ArmTranslator`, `ThumbTranslator`)
for the C of many encodings, finds out how that C depends on the fields, and emits one
function `ih_<form>(RCtx *ctx, u32 insn)` whose body is that C with the fields read from
`insn` (and, for short vectors, the length and stride read from `ctx->fpscr`).

How the dependence is found (the decoders are used as black boxes, nothing in arm.py or
thumb.py is changed):
  * every encoding gives a text; numbers in it are replaced by markers, which leaves a
    "skeleton" and a list of values;
  * where encodings of one region give different skeletons, the field that separates them
    is split into classes (a `switch` in the handler), recursively: this is "where the
    emitter specialises on a field, a switch over its values";
  * where the skeleton is uniform, each varying number is fitted as an affine function of
    the fields (a register index, an offset, `32 - shift`), of a sign-extended field, or of
    the instruction address (PC-relative values), and failing that as a lookup table;
  * a register-list field (LDM/STM/PUSH/POP) is decomposed into head, one template per
    element and tail, and emitted as a loop (Thumb LDMIA also splits on "base register in the
    list");
  * the result is checked: the template is instantiated for random encodings and must give
    exactly the skeleton and numbers the decoder gives.
A form for which any step fails raises `GenError`; the reason is the finding of the spike.

Handler contract (the dispatcher of N1-28 is not part of the spike): on entry
`ctx->r[15]` is the address of the instruction; on exit it is the address of the next
instruction to run, with bit 0 set when that one is Thumb. The handler checks the ARM
condition itself. A read of r15 gives the address plus 8 (ARM) or 4 (Thumb); a write to
r15 never occurs in a handler, branches and returns set the next address instead.
"""
from __future__ import annotations

import argparse
import functools
import itertools
import os
import random
import re
import sys
import time
from dataclasses import dataclass
from fractions import Fraction
from typing import Callable

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from recomp.arm import ArmTranslator  # noqa: E402
from recomp.emit import _lr_value, wrap  # noqa: E402
from recomp.insn import (AL, B, BL, COND_NAMES, ICALL, IJUMP, JTAB, OP, RET, SVC, TRAP, Insn)  # noqa: E402
from recomp.insn import sext  # noqa: E402
from recomp.thumb import ThumbTranslator  # noqa: E402

MASK32 = 0xFFFFFFFF
ADDRS = [0x00100000 + 0x44 * i for i in range(6)]       # ARM: 4-aligned sample addresses
THUMB_ADDRS = [0x00100000 + 0x22 * i for i in range(6)]     # Thumb: 2-aligned (PC-relative values align down)
MAX_TABLE_BITS = 12
MAX_LEAVES = 1500


class GenError(Exception):
    """The handler of a form cannot be generated; the message says why."""


# ── forms ───────────────────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class Field:
    """A bit field of the instruction (or of FPSCR for `src="fpscr"`).

    kind: "reg" (a register number: 0-14 are explored, 15 reads the PC), "sel"/"num" (any
    value; the name only documents intent) or "list" (a bit set of registers)."""
    name: str
    lsb: int
    width: int
    kind: str = "num"
    src: str = "insn"
    values: tuple[int, ...] | None = None
    signed: bool = False

    @property
    def mask(self) -> int:
        return (1 << self.width) - 1

    def domain(self, test: bool = False) -> list[int]:
        if self.values is not None:
            return list(self.values)
        if self.kind == "reg" and not test:
            return list(range(min(1 << self.width, 15)))
        return list(range(1 << self.width))

    def c(self) -> str:
        src = "insn" if self.src == "insn" else "ctx->fpscr"
        sh = f"{src} >> {self.lsb}" if self.lsb else src
        return f"(({sh}) & 0x{self.mask:X}u)"


@dataclass
class Form:
    name: str
    mode: str                                   # "arm" | "thumb"
    base: int                                   # the bits that are not in a field
    fields: list[Field]
    kinds: tuple[str, ...] = (OP,)              # Insn kinds the form produces (what the test accepts)
    valid: Callable[[dict], bool] | None = None  # excludes encodings that belong to other forms
    doc: str = ""
    list_base: str | None = None      # register field whose bit in the list changes the C (Thumb LDMIA writeback)

    def __post_init__(self) -> None:
        used = {"insn": self.base, "fpscr": 0}
        names = set()
        for f in self.fields:
            if f.name in names:
                raise GenError(f"{self.name}: field {f.name} is declared twice")
            names.add(f.name)
            bits = f.mask << f.lsb
            if used[f.src] & bits:
                raise GenError(f"{self.name}: field {f.name} overlaps another field or the fixed bits")
            used[f.src] |= bits
        if self.list_base is not None and self.list_base not in names:
            raise GenError(f"{self.name}: list_base {self.list_base} is not a field")

    @property
    def list_field(self) -> Field | None:
        return next((f for f in self.fields if f.kind == "list"), None)

    def pack(self, a: dict) -> tuple[int, int]:
        word, vec = self.base, 0
        for f in self.fields:
            if f.src == "insn":
                word |= a[f.name] << f.lsb
            else:
                vec |= a[f.name] << f.lsb
        return word, vec


# ── numbers in C text ───────────────────────────────────────────────────────────────────

_TOK = re.compile(r"(?<![A-Za-z0-9_.])(0x[0-9A-Fa-f]+|[0-9]+)(ull|ul|ll|u|l)?(?![A-Za-z0-9_.])")


class Eff:
    """The effect of one encoding as text. The skeleton (the text with every number replaced by a
    marker) is cheap and is all the tree building needs; the numbers are split out on demand."""

    def __init__(self, text: str):
        self.text = text
        self._tok: tuple | None = None

    @functools.cached_property
    def skel(self) -> str:
        return _TOK.sub("\x01", self.text)

    def _split(self) -> tuple:
        if self._tok is None:
            pieces, vals, orig, sfx, last = [], [], [], [], 0
            for m in _TOK.finditer(self.text):
                pieces.append(self.text[last:m.start()])
                s = m.group(1)
                vals.append(int(s, 16) if s.startswith("0x") else int(s))
                orig.append(s)
                sfx.append(m.group(2) or "")
                last = m.end()
            pieces.append(self.text[last:])
            self._tok = (pieces, vals, orig, sfx)
        return self._tok

    @property
    def pieces(self) -> list[str]:
        return self._split()[0]

    @property
    def vals(self) -> list[int]:
        return self._split()[1]

    @property
    def orig(self) -> list[str]:
        return self._split()[2]

    @property
    def sfx(self) -> list[str]:
        return self._split()[3]


def tokenize(text: str) -> Eff:
    return Eff(text)


def effect_text(ins: Insn) -> str | None:
    """The semantics of a decoded instruction as text, or None for an UNPREDICTABLE encoding
    (any behaviour is allowed there, so it takes part in no comparison)."""
    k = ins.kind
    if k == TRAP:
        if ins.trap_reason.startswith("unpredictable"):
            return None
        return f"K:trap\nI:{ins.imm}"
    if k == JTAB:
        raise GenError("jump-table instruction in a form (restrict the form)")
    lines = [f"K:{k}"] + ["B:" + s for s in ins.body]
    if ins.cond != AL:                  # only Thumb's conditional branch: ARM words are decoded as AL
        lines.append(f"C:{ins.cond}")
    if ins.pcexpr:
        lines.append("P:" + ins.pcexpr)
    if k in (B, BL):
        lines += [f"T:{ins.target}", f"M:{ins.target_mode}"]
    if k in (BL, ICALL):
        lines.append(f"L:{_lr_value(ins)}")
    if k == SVC:
        lines.append(f"S:{ins.imm}")
    return "\n".join(lines)


# ── models of a varying number ──────────────────────────────────────────────────────────

@dataclass(frozen=True)
class Var:
    """A quantity a number can depend on: a field, a sign-extended field, the address, or
    (in list templates) n, r, k."""
    key: str
    width: int | None = None        # set: sign-extend the field of this width
    cexpr: str = ""

    def value(self, env: dict) -> int:
        v = env[self.key]
        return sext(v, self.width) if self.width else v


class Model:
    def eval(self, env: dict) -> int:
        raise NotImplementedError

    def c(self) -> str:
        raise NotImplementedError


@dataclass
class Affine(Model):
    b: int
    terms: list[tuple[int, Var]]

    def eval(self, env: dict) -> int:
        return (self.b + sum(c * v.value(env) for c, v in self.terms)) & MASK32

    def c(self) -> str:
        if self.b & MASK32 == 0 and len(self.terms) == 1 and self.terms[0][0] == 1 and not self.terms[0][1].width:
            return self.terms[0][1].cexpr
        parts = [f"0x{self.b & MASK32:X}u"] if self.b & MASK32 or not self.terms else []
        for c, v in self.terms:
            x = v.cexpr if not v.width else f"ih_sext({v.cexpr}, {v.width})"
            parts.append(x if c == 1 else f"0x{c & MASK32:X}u * {x}")
        return "(u32)(" + " + ".join(parts) + ")"


@dataclass
class Table(Model):
    fields: list[Field]
    values: list[int]
    name: str = ""

    def index(self, env: dict) -> int:
        idx, sh = 0, 0
        for f in self.fields:
            idx |= (env[f.name] & f.mask) << sh
            sh += f.width
        return idx

    def eval(self, env: dict) -> int:
        return self.values[self.index(env)]

    def c(self) -> str:
        idx, sh = [], 0
        for f in self.fields:
            idx.append(f.c() if sh == 0 else f"({f.c()} << {sh})")
            sh += f.width
        return f"{self.name}[{' | '.join(idx)}]"


def _solve(rows: list[tuple[list[int], int]], nvar: int) -> list[Fraction] | None:
    """Exact solution of y = b + sum c_i x_i (free unknowns 0), or None."""
    m = [[Fraction(1)] + [Fraction(x) for x in xs] + [Fraction(y)] for xs, y in rows]
    ncol = nvar + 1
    piv, r = [], 0
    for c in range(ncol):
        p = next((i for i in range(r, len(m)) if m[i][c] != 0), None)
        if p is None:
            continue
        m[r], m[p] = m[p], m[r]
        inv = 1 / m[r][c]
        m[r] = [x * inv for x in m[r]]
        for i in range(len(m)):
            if i != r and m[i][c] != 0:
                f = m[i][c]
                m[i] = [x - f * y for x, y in zip(m[i], m[r])]
        piv.append(c)
        r += 1
    if any(all(x == 0 for x in row[:-1]) and row[-1] != 0 for row in m):
        return None
    sol = [Fraction(0)] * ncol
    for i, c in enumerate(piv):
        sol[c] = m[i][-1]
    return sol


def _signed(v: int) -> int:
    v &= MASK32
    return v - (1 << 32) if v & 0x80000000 else v


def fit_affine(envs: list[dict], ys: list[int], vars_: list[Var]) -> Affine | None:
    seen, rows = set(), []
    for env, y in zip(envs, ys):
        xs = tuple(v.value(env) for v in vars_)
        if xs not in seen and len(rows) < 48:
            seen.add(xs)
            rows.append((list(xs), _signed(y)))
    sol = _solve(rows, len(vars_))
    if sol is None or any(c.denominator != 1 for c in sol):
        return None
    model = Affine(int(sol[0]), [(int(c), v) for c, v in zip(sol[1:], vars_) if c != 0])
    if all(model.eval(e) == (y & MASK32) for e, y in zip(envs, ys)):
        return model
    return None


# ── nodes of the decision tree ──────────────────────────────────────────────────────────

@dataclass
class Leaf:
    eff: Eff
    models: list[Model | None]                  # per number: None = constant

    def instantiate(self, env: dict) -> str:
        out = []
        for i, p in enumerate(self.eff.pieces):
            out.append(p)
            if i < len(self.models):
                m = self.models[i]
                out.append(self.eff.orig[i] + self.eff.sfx[i] if m is None else f"{m.eval(env)}{self.eff.sfx[i]}")
        return "".join(out)


@dataclass
class Alt:
    """A line whose shape differs for an empty list (the emitter prints `s` for the first
    element and `s + 4` for the later ones)."""
    zero: Leaf
    other: Leaf

    def instantiate(self, env: dict) -> str:
        return (self.zero if env["n"] == 0 else self.other).instantiate(env)


@dataclass
class ListLeaf:
    """Head, per-element templates (rank 0 and later) and tail of a register-list instruction."""
    lst: Field
    head: list[Leaf | Alt]
    elem: list[list[Leaf]]          # [rank class][line]; class 0 = first element, 1 = later ones
    tail: list[Leaf | Alt]
    nlines_elem: int
    same_elem: bool

    def instantiate(self, env: dict) -> str:
        bits = [i for i in range(self.lst.width) if (env[self.lst.name] >> i) & 1]
        n = len(bits)
        lines = []
        e = dict(env, n=n)
        lines += [ln.instantiate(e) for ln in self.head]
        for k, r in enumerate(bits):
            ek = dict(e, r=r, k=k)
            tpl = self.elem[0 if k == 0 else 1]
            lines += [ln.instantiate(ek) for ln in tpl]
        lines += [ln.instantiate(e) for ln in self.tail]
        return "\n".join(lines)


@dataclass
class Split:
    field: Field
    classes: list[tuple[list[int], object]]     # (values, subtree)

    def child(self, env: dict):
        v = env[self.field.name]
        for vals, node in self.classes:
            if v in vals:
                return node
        raise GenError(f"{self.field.name}={v} is in no class")


@dataclass
class BaseSplit:
    """Register-list instructions whose C depends on whether the base register is in the list."""
    lst: Field
    base: Field
    inside: object
    outside: object

    def child(self, env: dict):
        return self.inside if (env[self.lst.name] >> env[self.base.name]) & 1 else self.outside


# ── the generator for one form ──────────────────────────────────────────────────────────

@dataclass
class Sample:
    a: dict
    addr: int
    eff: Eff | None


class Decoder:
    def __init__(self, mode: str):
        self.mode = mode
        if mode == "arm":
            self.arm = ArmTranslator(lambda a: None)
        else:
            self.cur: dict[int, int] = {}
            self.thumb = ThumbTranslator(self.cur.get, lambda a: None)

    def decode(self, word: int, vec: int, addr: int) -> Insn:
        if self.mode == "arm":
            return self.arm.decode(addr, word, vec)
        self.cur.clear()
        self.cur[addr] = word & 0xFFFF
        if word > 0xFFFF:                       # BL / BLX pair: the second halfword is the high half
            self.cur[addr + 2] = word >> 16
        return self.thumb.decode(addr)


class FormGen:
    def __init__(self, form: Form, seed: int = 1):
        self.form = form
        self.dec = Decoder(form.mode)
        self.rng = random.Random(f"{form.name}:{seed}")
        self.byname = {f.name: f for f in form.fields}
        self.leaves = 0
        self.cache: dict = {}
        self._fd_cache: dict = {}
        self.tables: dict[tuple, str] = {}
        self.tablevals: dict[str, list[int]] = {}
        self.addrs = ADDRS if form.mode == "arm" else THUMB_ADDRS
        self.base_mode: str | None = None

    # decoding ------------------------------------------------------------------------
    def env(self, a: dict, addr: int, **extra) -> dict:
        """The values the fitted models are evaluated with: the fields, the instruction address
        and the PC rounded down to a word (PC-relative loads and address computations)."""
        off = 8 if self.form.mode == "arm" else 4
        return dict(a, ADDR=addr, PCA=(addr + off) & ~3 & MASK32, **extra)

    def effect(self, a: dict, addr: int) -> Eff | None:
        word, vec = self.form.pack(a)
        if self.form.mode == "arm":
            word = (word & 0x0FFFFFFF) | (0xE << 28)
        key = (word, vec, addr)
        if key in self.cache:
            return self.cache[key]
        text = effect_text(self.dec.decode(word, vec, addr))
        eff = self.cache[key] = None if text is None else tokenize(text)
        return eff

    def ok(self, a: dict) -> bool:
        if self.base_mode is not None:
            has = (a[self.form.list_field.name] >> a[self.form.list_base]) & 1
            if has != (self.base_mode == "in"):
                return False
        return self.form.valid is None or self.form.valid(a)

    def rand(self, region: dict) -> dict:
        for _ in range(400):
            a = {n: self.rng.choice(vals) for n, vals in region.items()}
            if self.ok(a):
                return a
        raise GenError("no valid encoding in region")

    # gathering -----------------------------------------------------------------------
    def gather(self, region: dict):
        samples: list[Sample] = []
        groups: list[tuple[str, list[int]]] = []
        for _ in range(100):
            a, addr = self.rand(region), self.rng.choice(self.addrs)
            samples.append(Sample(a, addr, self.effect(a, addr)))
        for _ in range(60):
            # most random encodings of a form with many "unsupported" combinations decode to
            # a trap: repair some of them so that the interesting shapes are sampled too
            s0 = self.rng.choice(samples[:100])
            if s0.eff is not None and not s0.eff.text.startswith("K:trap"):
                continue
            a = dict(s0.a)
            names = [n for n in region if len(region[n]) > 1 and self.byname[n].kind != "list"]
            for _ in range(14):
                if not names:
                    break
                n = self.rng.choice(names)
                a[n] = self.rng.choice(region[n])
                if self.ok(a):
                    e = self.effect(a, s0.addr)
                    if e is not None and not e.text.startswith("K:trap"):
                        samples.append(Sample(dict(a), s0.addr, e))
                        break
        bases = [i for i, s in enumerate(samples) if s.eff and not s.eff.text.startswith("K:trap")][:3] or \
            [i for i, s in enumerate(samples) if s.eff][:3]
        for bi in bases:
            base = samples[bi]
            for fname, vals in region.items():
                if self.byname[fname].kind == "list" or len(vals) < 2:
                    continue
                if len(vals) > 24:
                    vals = sorted(set(self.rng.sample(vals, 22) + [vals[0], vals[-1]]))
                idx = [bi]
                for v in vals:
                    a2 = dict(base.a, **{fname: v})
                    if v != base.a[fname] and self.ok(a2):
                        samples.append(Sample(a2, base.addr, self.effect(a2, base.addr)))
                        idx.append(len(samples) - 1)
                groups.append((fname, idx))
            idx = [bi]
            for ad in self.addrs:
                if ad != base.addr:
                    samples.append(Sample(base.a, ad, self.effect(base.a, ad)))
                    idx.append(len(samples) - 1)
            groups.append(("ADDR", idx))
        return samples, groups

    # tree ----------------------------------------------------------------------------
    def build(self, region: dict):
        samples, groups = self.gather(region)
        real = [s for s in samples if s.eff]
        skels = {s.eff.skel for s in real}
        if not real:
            self.leaves += 1
            e = tokenize("K:trap\nI:2")
            return Leaf(e, [None] * len(e.vals))
        if len(skels) == 1:
            return self.make_leaf(region, samples, groups)
        if self.leaves > MAX_LEAVES:
            raise GenError(f"more than {MAX_LEAVES} leaves: the form does not factor")
        score: dict[str, int] = {}
        for fname, idx in groups:
            if fname == "ADDR":
                continue
            ref = samples[idx[0]].eff
            for i in idx[1:]:
                e = samples[i].eff
                if e and ref and e.skel != ref.skel:
                    score[fname] = score.get(fname, 0) + 1
        order = sorted(score, key=lambda n: -score[n])
        order += [n for n in region if n not in order and self.byname[n].kind != "list"]
        for fname in order:
            f = self.byname[fname]
            if len(region[fname]) < 2:
                continue
            if len(region[fname]) > 64:
                if score.get(fname):
                    raise GenError(f"the C depends on the wide field {fname} ({len(region[fname])} values)")
                continue
            classes = self.partition(region, fname)
            if len(classes) > 1:
                children = []
                for vals in classes:
                    sub = dict(region)
                    sub[fname] = vals
                    children.append((vals, self.build(sub)))
                return Split(f, children)
        raise GenError("the C varies but no single field explains it (a relation between fields)")

    def valid_context(self, region: dict, fname: str, v: int):
        """A context (the other fields and an address) in which value `v` of `fname` decodes to
        something other than UNPREDICTABLE. Encodings with must-be-zero fields are rare in a random
        draw, so a failed draw is repaired by changing one other field at a time."""
        other = [n for n in region if n != fname and len(region[n]) > 1]
        for _ in range(8):
            a = self.rand(region)
            a[fname] = v
            addr = self.rng.choice(self.addrs)
            for _ in range(12):
                if self.ok(a) and self.effect(a, addr):
                    return a, addr
                if not other:
                    break
                n = self.rng.choice(other)
                a[n] = self.rng.choice(region[n])
        return None

    def partition(self, region: dict, fname: str) -> list[list[int]]:
        pool = []
        for v in region[fname]:
            for _ in range(3):
                c = self.valid_context(region, fname, v)
                if c:
                    pool.append(c)
        vec: dict[int, list[str | None]] = {}
        for v in region[fname]:
            row = []
            for a, addr in pool:
                a2 = dict(a, **{fname: v})
                e = self.effect(a2, addr) if self.ok(a2) else None
                row.append(e.skel if e else None)
            vec[v] = row

        def compat(x: int, y: int) -> bool:
            # two values share a class when they agree wherever both decode; values that never
            # decode in the same context (must-be-zero fields that conflict) stay apart, except
            # that values that decode nowhere at all are one class of their own
            both = [(p, q) for p, q in zip(vec[x], vec[y]) if p is not None and q is not None]
            if not both:
                return all(p is None for p in vec[x]) and all(q is None for q in vec[y])
            return all(p == q for p, q in both)
        classes: list[list[int]] = []
        for v in region[fname]:
            for cl in classes:
                if all(compat(v, w) for w in cl):
                    cl.append(v)
                    break
            else:
                classes.append([v])
        return classes

    # leaves --------------------------------------------------------------------------
    def make_leaf(self, region: dict, samples: list[Sample], groups):
        self.leaves += 1
        real = [(s, s.eff) for s in samples if s.eff]
        eff0 = real[0][1]
        lst = self.form.list_field
        if lst is not None and not eff0.text.startswith("K:trap"):
            return self.make_list_leaf(region, samples, groups)
        envs = [self.env(s.a, s.addr) for s, _ in real]
        models: list[Model | None] = []
        for j in range(len(eff0.vals)):
            ys = [e.vals[j] for _, e in real]
            if len(set(ys)) == 1:
                models.append(None)
                continue
            deps = []
            for fname, idx in groups:
                vs = {samples[i].eff.vals[j] for i in idx if samples[i].eff}
                if len(vs) > 1 and fname not in deps:
                    deps.append(fname)
            models.append(self.fit(region, j, envs, ys, deps, real))
        return Leaf(eff0, models)

    def vars_for(self, deps: list[str], with_sext: bool) -> list[Var]:
        out = []
        off = 8 if self.form.mode == "arm" else 4
        for d in deps:
            if d == "ADDR":
                out.append(Var("ADDR", None, "ih_pc"))
                if with_sext:
                    out.append(Var("PCA", None, f"((ih_pc + {off}u) & ~3u)"))
            else:
                f = self.byname[d]
                out.append(Var(d, None, f.c()))
                if with_sext and f.signed:
                    out.append(Var(d, f.width, f.c()))
        return out

    def fit(self, region: dict, j: int, envs: list[dict], ys: list[int], deps: list[str], real):
        for with_sext in (False, True):
            if deps:
                m = fit_affine(envs, ys, self.vars_for(deps, with_sext))
                if m:
                    return m
        # the sweeps may have missed a field the number depends on weakly (one bit of an
        # immediate): sweep every field completely around a few bases and look again
        deps = self.full_deps(region, j, envs[:12], real[0][1])
        for with_sext in (False, True):
            if deps:
                m = fit_affine(envs, ys, self.vars_for(deps, with_sext))
                if m:
                    return m
        if not deps:
            raise GenError(f"number {j} of the C varies but no field changes it")
        tdeps = [self.byname[d] for d in deps if d != "ADDR"]
        bits = sum(f.width for f in tdeps)
        if "ADDR" in deps or bits > MAX_TABLE_BITS:
            raise GenError(f"number {j} depends on {deps} in a way that is not affine and a table would need "
                           f"{bits} index bits")
        values = [0] * (1 << bits)
        base = envs[0]
        for combo in itertools.product(*[region[f.name] for f in tdeps]):
            a = {k: v for k, v in base.items() if k in self.byname}
            a.update({f.name: v for f, v in zip(tdeps, combo)})
            if not self.ok(a):
                continue
            e = self.effect(a, base["ADDR"])
            if e is None or e.skel != real[0][1].skel:
                continue
            values[Table(tdeps, []).index(a)] = e.vals[j]
        tbl = Table(tdeps, values)
        if not all(tbl.eval(env) == y for env, y in zip(envs, ys)):
            raise GenError(f"number {j}: the lookup table over {deps} does not reproduce the decoder")
        key = tuple(values)
        name = self.tables.setdefault(key, f"ih_tbl_{self.form.name}_{len(self.tables)}")
        self.tablevals[name] = values
        tbl.name = name
        return tbl

    def full_deps(self, region: dict, j: int, bases: list[dict], eff0: Eff) -> list[str]:
        """The fields the j-th number depends on, from sweeping every field around some bases.
        The sweep is done once per leaf for all of its numbers."""
        key = (id(eff0), tuple(sorted(region)))
        table = self._fd_cache.get(key)
        if table is None:
            table = self._fd_cache[key] = self._sweep_all(region, bases, eff0)
        return table.get(j, [])

    def _sweep_all(self, region: dict, bases: list[dict], eff0: Eff) -> dict[int, list[str]]:
        deps: dict[int, list[str]] = {}
        nv = len(eff0.vals)
        for fname, vals in region.items():
            if len(vals) < 2 or self.byname[fname].kind == "list":
                continue
            for env in bases:
                a = {k: v for k, v in env.items() if k in self.byname}
                seen = [set() for _ in range(nv)]
                for v in (vals if len(vals) <= 16 else self.rng.sample(vals, 16)):
                    a2 = dict(a, **{fname: v})
                    if not self.ok(a2):
                        continue
                    e = self.effect(a2, env["ADDR"])
                    if e is not None and e.skel == eff0.skel:
                        for j, x in enumerate(e.vals):
                            seen[j].add(x)
                for j in range(nv):
                    if len(seen[j]) > 1 and fname not in deps.setdefault(j, []):
                        deps[j].append(fname)
        for env in bases:
            a = {k: v for k, v in env.items() if k in self.byname}
            seen = [set() for _ in range(nv)]
            for ad in self.addrs:
                e = self.effect(a, ad)
                if e is not None and e.skel == eff0.skel:
                    for j, x in enumerate(e.vals):
                        seen[j].add(x)
            for j in range(nv):
                if len(seen[j]) > 1 and "ADDR" not in deps.setdefault(j, []):
                    deps[j].append("ADDR")
        return deps

    # register lists ------------------------------------------------------------------
    def make_list_leaf(self, region: dict, samples: list[Sample], groups):
        lst = self.form.list_field
        W = lst.width
        rng = self.rng
        other = {n: v for n, v in region.items() if n != lst.name}
        fvars = [n for n, v in other.items() if len(v) > 1]

        def sample(bits: list[int], addr: int):
            lv = sum(1 << b for b in bits)
            for _ in range(60):
                a = {n: rng.choice(v) for n, v in other.items()}
                a[lst.name] = lv
                if not self.ok(a):
                    continue
                e = self.effect(a, addr)
                if e is not None:
                    return a, addr, e
            return None
        data = []
        for n in range(0, W + 1):
            # every size gets every sample address: the empty list and the full one have a single
            # list each, and a number that depends on the address must not look constant there
            for t in range(max(8, len(self.addrs))):
                bits = sorted(rng.sample(range(W), n)) if t else list(range(n))
                s = sample(bits, self.addrs[t % len(self.addrs)])
                if s:
                    data.append((n, bits) + s)
        if len(data) < 6:
            raise GenError("too few valid register lists")
        lines = [(n, bits, a, addr, e.text.split("\n")) for n, bits, a, addr, e in data]
        ns = sorted({x[0] for x in lines})
        if len(ns) < 3:
            raise GenError("register lists of fewer than three sizes decode")
        # lines per element: growth of the line count with n
        cnt = {}
        for n, _, _, _, ls in lines:
            cnt.setdefault(n, set()).add(len(ls))
        if any(len(v) != 1 for v in cnt.values()):
            raise GenError("the number of lines is not a function of the list size")
        cnt = {n: next(iter(v)) for n, v in cnt.items()}
        es = {(cnt[b] - cnt[a]) / (b - a) for a, b in zip(ns, ns[1:])}
        if len(es) != 1 or next(iter(es)) != int(next(iter(es))) or next(iter(es)) < 1:
            raise GenError(f"element size is not constant: {sorted(es)}")
        e = int(next(iter(es)))
        def envs_of(extra):
            return [self.env(a, addr, **extra(n, bits)) for n, bits, a, addr, _ in lines]

        vlist = [Var("ADDR", None, "ih_pc"), Var("n", None, "ih_n")] + [Var(v, None, self.byname[v].c()) for v in fvars]

        def fit_one(texts: list[str], envs: list[dict], vars_: list[Var], what: str) -> Leaf:
            effs = [tokenize(x) for x in texts]
            e0 = effs[0]
            if len({x.skel for x in effs}) != 1:
                raise GenError(f"{what}: the C has {len({x.skel for x in effs})} shapes")
            models: list[Model | None] = []
            for j in range(len(e0.vals)):
                ys = [x.vals[j] for x in effs]
                if len(set(ys)) == 1:
                    models.append(None)
                    continue
                m = fit_affine(envs, ys, vars_)
                if m is None:
                    raise GenError(f"{what}: number {j} is not affine in the fields, n, r and k")
                models.append(m)
            return Leaf(e0, models)

        def fit_line(texts: list[str], envs: list[dict], vars_: list[Var], what: str):
            shapes = {tokenize(x).skel for x in texts}
            if len(shapes) == 1:
                return fit_one(texts, envs, vars_, what)
            zero = [(t, e) for t, e in zip(texts, envs) if e["n"] == 0]
            rest = [(t, e) for t, e in zip(texts, envs) if e["n"] != 0]
            if len(shapes) == 2 and zero and rest and len({tokenize(t).skel for t, _ in zero}) == 1 \
                    and len({tokenize(t).skel for t, _ in rest}) == 1:
                return Alt(fit_one([t for t, _ in zero], [e for _, e in zero], vars_, what + " (empty list)"),
                           fit_one([t for t, _ in rest], [e for _, e in rest], vars_, what))
            raise GenError(f"{what}: the C has {len(shapes)} shapes that the list size does not separate")
        env_n = envs_of(lambda n, bits: {"n": n})

        def build_with(h: int) -> ListLeaf:
            t_len = {cnt[n] - h - e * n for n in ns}
            if len(t_len) != 1 or next(iter(t_len)) < 0:
                raise GenError("head and tail lengths are not constant")
            t = next(iter(t_len))
            head = [fit_line([ls[i] for _, _, _, _, ls in lines], env_n, vlist, f"head line {i}") for i in range(h)]
            tail = [fit_line([ls[h + e * n + i] for n, _, _, _, ls in lines], env_n, vlist, f"tail line {i}")
                    for i in range(t)]
            evars = vlist + [Var("r", None, "ih_r"), Var("k", None, "ih_k")]
            elem_cls: dict[int, list] = {}
            for cls in (0, 1):
                per = []
                for li in range(e):
                    texts, envs = [], []
                    for n, bits, a, addr, ls in lines:
                        for k, r in enumerate(bits):
                            if (k > 0) != bool(cls):
                                continue
                            texts.append(ls[h + e * k + li])
                            envs.append(self.env(a, addr, n=n, r=r, k=k))
                    if not texts:
                        raise GenError("no element of rank %d" % cls)
                    per.append(fit_line(texts, envs, evars, f"element line {li} (rank class {cls})"))
                elem_cls[cls] = per
            same = all(x.eff.skel == y.eff.skel and x.models == y.models
                       for x, y in zip(elem_cls[0], elem_cls[1]))
            return ListLeaf(lst, head, [elem_cls[0], elem_cls[1]], tail, e, same)
        # the head is the lines before the first element: the shortest head for which the
        # head, element and tail templates all fit is the right one
        err = None
        for h in range(1, min(cnt.values()) + 1):
            try:
                return build_with(h)
            except GenError as ex:
                err = ex
        raise err or GenError("no head length fits")

    # verification --------------------------------------------------------------------
    def instantiate(self, node, a: dict, addr: int) -> str:
        env = self.env(a, addr)
        while isinstance(node, (Split, BaseSplit)):
            node = node.child(env)
        return node.instantiate(env)

    def verify(self, tree, count: int = 2000) -> int:
        """Instantiate the tree for random encodings; each must give the decoder's C. Returns
        how many encodings were compared."""
        full = {f.name: f.domain() for f in self.form.fields}
        lst = self.form.list_field
        done = 0
        for _ in range(count * 40):
            if done >= count:
                break
            # every field is at its smallest or largest value now and then: shifts of 0, offsets of 0
            # and of the maximum, the first and last register
            a = {n: (v[0] if r < 0.15 else v[-1] if r < 0.3 else self.rng.choice(v))
                 for n, v in full.items() for r in [self.rng.random()]}
            if lst is not None:            # every list size alike, not mostly the small ones
                a[lst.name] = sum(1 << b for b in self.rng.sample(range(lst.width), self.rng.randint(0, lst.width)))
            if not self.ok(a):
                continue
            addr = self.rng.choice(self.addrs)
            e = self.effect(a, addr)
            if e is None:
                continue
            got = tokenize(self.instantiate(tree, a, addr))
            if got.skel != e.skel or got.vals != e.vals:
                raise GenError(f"template differs from the decoder for {self.form.pack(a)[0]:08x}:\n"
                               f"  decoder : {e.text!r}\n  template: {got.text!r}")
            done += 1
        if done < min(count, 100):
            raise GenError(f"only {done} valid random encodings found to verify with")
        return done


# ── C emission ──────────────────────────────────────────────────────────────────────────

PREAMBLE = r"""/* Generated by tools/recomp/interp_gen.py (N1-24): handlers of the fallback interpreter.
 * Contract: on entry ctx->r[15] is the address of the instruction, on exit the address of the next
 * one (bit 0 set for Thumb). No game data: the handlers are derived from the recompiler's decoders. */
#include "recomp.h"

static inline u32 ih_sext(u32 v, u32 bits) { return (u32)((s32)(v << (32 - bits)) >> (32 - bits)); }
static inline u32 ih_popcount(u32 v) { return (u32)__builtin_popcount(v); }
static inline int ih_cond(RCtx *ctx, u32 c) {
    switch (c) {
%s
    default: return 0;
    }
}
"""

_RREG = re.compile(r"(?<![A-Za-z0-9_])R\(\x02([^\x03]*)\x03\)(\s*=(?!=))?")


def render(leaf: Leaf) -> str:
    """The leaf's text as C: numbers that vary become expressions; reads of a variable
    register go through RR (r15 reads the PC)."""
    out = []
    for i, p in enumerate(leaf.eff.pieces):
        out.append(p)
        if i < len(leaf.models):
            m = leaf.models[i]
            if m is None:
                out.append(leaf.eff.orig[i] + leaf.eff.sfx[i])
            else:
                c = m.c()
                if leaf.eff.sfx[i] in ("ull", "ll"):
                    c = f"(u64)({c})"
                out.append("\x02" + c + "\x03")
    text = "".join(out)
    text = _RREG.sub(lambda m: f"R({m[1]}){m[2]}" if m[2] else f"RR({m[1]})", text)
    return text.replace("\x02", "").replace("\x03", "")


def leaf_c(lines: list[str], form: Form, indent: str) -> list[str]:
    """C for the text of an effect (K:, B:, P:, T:, M:, L:, S: lines)."""
    kind = lines[0][2:]
    body = [x[2:] for x in lines if x.startswith("B:")]
    get = {x[0]: x[2:] for x in lines if x[:2] in ("P:", "T:", "M:", "L:", "S:", "I:", "C:")}
    nxt = "ih_pc + 4" if form.mode == "arm" else "(ih_pc + 2) | 1u"
    out = ["{"] + [indent + s for s in body]
    if kind == OP:
        out.append(f"{indent}ctx->r[15] = {nxt};")
    elif kind in (RET, IJUMP):
        out.append(f"{indent}ctx->r[15] = {get['P']};")
    elif kind == ICALL:
        out.append(f"{indent}u32 ih_t = {get['P']}; ctx->r[14] = (u32)({get['L']}); ctx->r[15] = ih_t;")
    elif kind in (B, BL):
        bit = " | 1u" if get["M"] == "thumb" else ""
        pre = f"ctx->r[14] = (u32)({get['L']}); " if kind == BL else ""
        go = f"{pre}ctx->r[15] = (u32)({get['T']}){bit};"
        if "C" in get:
            out.append(f"{indent}ctx->r[15] = {nxt}; if (ih_cond(ctx, (u32)({get['C']}))) {{ {go} }}")
        else:
            out.append(f"{indent}{go}")
    elif kind == SVC:
        out.append(f"{indent}rc_svc(ctx, (u32)({get['S']}), {nxt}); ctx->r[15] = {nxt};")
    elif kind == "trap":
        out.append(f"{indent}rc_trap(ctx, ih_pc, {get['I']}); return;")
    else:
        raise GenError(f"no C for kind {kind}")
    out.append("}")
    return out


def emit_node(node, form: Form, ind: str) -> list[str]:
    sub = ind + "  "
    if isinstance(node, Leaf):
        lines = render(node).split("\n")
        return [ind + " ".join(x.strip() for x in leaf_c(lines, form, ""))]
    if isinstance(node, ListLeaf):
        return emit_list(node, form, ind)
    if isinstance(node, BaseSplit):
        cond = f"(({node.lst.c()} >> {node.base.c()}) & 1)"
        return ([f"{ind}if ({cond}) {{"] + emit_node(node.inside, form, sub) + [f"{ind}}} else {{"]
                + emit_node(node.outside, form, sub) + [f"{ind}}}"])
    f = node.field
    out = [f"{ind}switch ({f.c()}) {{"]
    big = max(range(len(node.classes)), key=lambda i: len(node.classes[i][0]))
    for i, (vals, child) in enumerate(node.classes):
        label = "default:" if i == big else " ".join(f"case {v}:" for v in vals)
        body = emit_node(child, form, sub)
        if len(body) == 1:
            out.append(f"{ind}{label} {body[0].strip()} break;")
        else:
            out.append(f"{ind}{label}")
            out += body
            out.append(f"{sub}break;")
    out.append(ind + "}")
    return out


_DECL = re.compile(r"^(u32|u64|s32|s64) (\w+) = (.*);$")


def _line_c(item, ind: str) -> tuple[list[str], list[str]]:
    """(C statements for the B: lines, other lines) of a head or tail line."""
    if isinstance(item, Leaf):
        lines = render(item).split("\n")
        return [x[2:] for x in lines if x.startswith("B:")], [x for x in lines if not x.startswith("B:")]
    z, o = render(item.zero).split("\n"), render(item.other).split("\n")
    if len(z) != 1 or len(o) != 1 or not z[0].startswith("B:") or not o[0].startswith("B:"):
        raise GenError("an empty-list variant must be a single statement")
    z, o = z[0][2:], o[0][2:]
    mz, mo = _DECL.match(z), _DECL.match(o)
    if mz and mo and mz[1] == mo[1] and mz[2] == mo[2]:
        return [f"{mz[1]} {mz[2]}; if (ih_n == 0) {mz[2]} = {mz[3]}; else {mz[2]} = {mo[3]};"], []
    if mz or mo:
        raise GenError("an empty-list variant declares a different variable")
    return [f"if (ih_n == 0) {{ {z} }} else {{ {o} }}"], []


def emit_list(node: ListLeaf, form: Form, ind: str) -> list[str]:
    sub = ind + "    "
    head_b, post = [], []
    for item in node.head:
        b_, p_ = _line_c(item, sub)
        head_b += b_
        post += p_
    kind_line, post = post[0], post[1:]
    tail_b = []
    for item in node.tail:
        b_, p_ = _line_c(item, sub)
        tail_b += b_
        post += p_

    def elem_lines(cls: int) -> list[str]:
        out = []
        for ln in node.elem[cls]:
            out += _line_c(ln, sub)[0]
        return out
    out = [ind + "{", sub + f"u32 ih_l = {node.lst.c()}; u32 ih_n = ih_popcount(ih_l); (void)ih_n;"]
    out += [sub + x for x in head_b]
    out.append(sub + f"for (u32 ih_r = 0, ih_k = 0; ih_r < {node.lst.width}; ih_r++) {{")
    out.append(sub + "    if (!((ih_l >> ih_r) & 1)) continue;")
    if node.same_elem:
        out += [sub + "    " + x for x in elem_lines(0)]
    else:
        out.append(sub + "    if (ih_k == 0) {")
        out += [sub + "        " + x for x in elem_lines(0)]
        out.append(sub + "    } else {")
        out += [sub + "        " + x for x in elem_lines(1)]
        out.append(sub + "    }")
    out.append(sub + "    ih_k++; (void)ih_k;")
    out.append(sub + "}")
    out += [sub + x for x in tail_b]
    out += [sub + x for x in leaf_c([kind_line] + post, form, sub)[1:-1]]
    out.append(ind + "}")
    return out


def tables_c(gen: FormGen) -> list[str]:
    out = []
    for name, vals in gen.tablevals.items():
        out.append(f"static const u32 {name}[{len(vals)}] = {{ " + ", ".join(f"0x{v & MASK32:X}u" for v in vals) + " };")
    return out


def emit_form(form: Form, tree, gen: FormGen) -> list[str]:
    out = tables_c(gen)
    off = 8 if form.mode == "arm" else 4
    out.append(f"#undef RR\n#define RR(n) ((n) == 15u ? ih_pc + {off}u : ctx->r[(n)])")
    out.append(f"void ih_{form.name}(RCtx *ctx, u32 insn) {{")
    out.append("    const u32 ih_pc = ctx->r[15]; (void)insn;")
    if form.mode == "arm":
        out.append("    if (!ih_cond(ctx, insn >> 28)) { ctx->r[15] = ih_pc + 4; return; }")
    out += emit_node(tree, form, "  ")
    out.append("}")
    return out


def cond_switch() -> str:
    return "\n".join(f"    case {i}: return {'1' if n == 'AL' else 'C_' + n};" for i, n in enumerate(COND_NAMES))


@dataclass
class Generated:
    form: Form
    c: list[str]
    leaves: int
    verified: int
    c_bytes: int
    tables: int = 0                  # lookup tables emitted (numbers that are not affine in the fields)
    table_entries: int = 0
    seconds: float = 0.0


def generate(form: Form, seed: int = 1, verify: int = 2000) -> Generated:
    t0 = time.time()
    gen = FormGen(form, seed)
    region = {f.name: f.domain() for f in form.fields}
    if form.list_field:
        # while the tree is built every list has two registers (the C has the same shape for
        # all of them); the lists of other sizes are explored when the list templates are fitted
        w = form.list_field.width
        region[form.list_field.name] = [(1 << i) | (1 << j) for i in range(w) for j in range(i + 1, w)]
    if form.list_base:
        gen.base_mode = "in"
        inside = gen.build(region)
        gen.base_mode = "out"
        outside = gen.build(region)
        gen.base_mode = None
        tree = BaseSplit(form.list_field, gen.byname[form.list_base], inside, outside)
    else:
        tree = gen.build(region)
    n = gen.verify(tree, verify) if verify else 0
    c = emit_form(form, tree, gen)
    return Generated(form, c, gen.leaves, n, sum(len(x) + 1 for x in c), len(gen.tablevals),
                     sum(len(v) for v in gen.tablevals.values()), time.time() - t0)


def reference_c(ins: Insn) -> list[str]:
    """C for what the recompiler does with one decoded instruction, ending in the same exit
    convention as the handlers (ctx->r[15] = next address, bit 0 for Thumb). Straight-line
    instructions use emit.straight_line; control flow, which the emitter turns into gotos and
    calls once function analysis has run, is written here from the Insn fields exactly as
    emit.FunctionEmitter.insn does."""
    from recomp.emit import straight_line
    bit = 1 if ins.mode == "thumb" else 0
    nxt = f"ctx->r[15] = 0x{(ins.next | bit) & MASK32:x}u;"
    k = ins.kind
    if k in (OP, SVC):
        return straight_line(ins) + [nxt]
    if k == TRAP:
        return straight_line(ins)
    tb = 1 if ins.target_mode == "thumb" else 0
    if k == B:
        return [nxt] + wrap(ins, [f"ctx->r[15] = 0x{(ins.target | tb) & MASK32:x}u;"])
    if k == BL:
        return [nxt] + wrap(ins, [f"R(14) = 0x{_lr_value(ins):x}u;", f"ctx->r[15] = 0x{(ins.target | tb) & MASK32:x}u;"])
    if k in (RET, IJUMP):
        return [nxt] + wrap(ins, ins.body + [f"ctx->r[15] = {ins.pcexpr};"])
    if k == ICALL:
        return [nxt] + wrap(ins, ins.body + [f"u32 t_ = {ins.pcexpr};", f"R(14) = 0x{_lr_value(ins):x}u;",
                                             "ctx->r[15] = t_;"])
    raise GenError(f"no reference for kind {k}")


def sample_words(form: Form, rng: random.Random, count: int) -> list[tuple[int, int]]:
    """Random (word, FPSCR vector bits) pairs of a form that the recompiler's decoder accepts as
    one of the form's kinds. All registers (including r15), all conditions."""
    dec = Decoder(form.mode)
    full = {f.name: f.domain(test=True) for f in form.fields}
    lst = form.list_field
    out: list[tuple[int, int]] = []
    seen = set()
    for _ in range(count * 200):
        if len(out) >= count:
            break
        a = {n: rng.choice(v) for n, v in full.items()}
        if lst is not None:
            a[lst.name] = rng.getrandbits(lst.width) & rng.getrandbits(lst.width)
        if form.valid is not None and not form.valid(a):
            continue
        word, vec = form.pack(a)
        if form.mode == "arm":
            word |= (0xE if rng.random() < 0.7 else rng.randrange(14)) << 28
        if (word, vec) in seen:
            continue
        ins = dec.decode(word, vec, ADDRS[0])
        if ins.kind in form.kinds:
            seen.add((word, vec))
            out.append((word, vec))
    return out


# ── the 20 forms of the spike ───────────────────────────────────────────────────────────

def _reg(name: str, lsb: int, width: int = 4) -> Field:
    return Field(name, lsb, width, "reg")


def _dp_valid(a: dict) -> bool:
    return not (8 <= a["opc"] <= 11 and a["S"] == 0)       # TST/TEQ/CMP/CMN without S are other instructions


def _vfp_arith_valid(a: dict) -> bool:
    return a["p"] == 0 or (a["q"] == 0 and a["r"] == 0 and a["s"] == 0)     # opcodes 0-8


_VEC = [Field("len", 16, 3, "num", "fpscr"), Field("stride", 20, 2, "num", "fpscr", (0, 3))]

FORMS: list[Form] = [
    Form("arm_dp_imm_shift", "arm", 0x00000000,
         [Field("opc", 21, 4), Field("S", 20, 1), _reg("rn", 16), _reg("rd", 12), Field("imm5", 7, 5),
          Field("typ", 5, 2), _reg("rm", 0)], valid=_dp_valid,
         doc="data processing, register operand shifted by an immediate (all 16 opcodes, S)"),
    Form("arm_dp_reg_shift", "arm", 0x00000010,
         [Field("opc", 21, 4), Field("S", 20, 1), _reg("rn", 16), _reg("rd", 12), _reg("rs", 8),
          Field("typ", 5, 2), _reg("rm", 0)], valid=_dp_valid,
         doc="data processing, register operand shifted by a register"),
    Form("arm_dp_imm", "arm", 0x02000000,
         [Field("opc", 21, 4), Field("S", 20, 1), _reg("rn", 16), _reg("rd", 12), Field("rot", 8, 4),
          Field("imm8", 0, 8)], valid=_dp_valid,
         doc="data processing, rotated 8-bit immediate"),
    Form("arm_mul", "arm", 0x00000090,
         [Field("op4", 20, 4), _reg("rdhi", 16), _reg("rdlo", 12), _reg("rs", 8), _reg("rm", 0)],
         doc="MUL, MLA, UMAAL and the long multiplies (UMULL, UMLAL, SMULL, SMLAL), S"),
    Form("arm_ldst_imm", "arm", 0x04000000,
         [Field("pubwl", 20, 5), _reg("rn", 16), _reg("rt", 12), Field("imm12", 0, 12)],
         doc="LDR, STR, LDRB, STRB, immediate offset, pre/post index, writeback"),
    Form("arm_ldst_reg", "arm", 0x06000000,
         [Field("pubwl", 20, 5), _reg("rn", 16), _reg("rt", 12), Field("shimm", 7, 5), Field("typ", 5, 2),
          _reg("rm", 0)],
         doc="LDR, STR, LDRB, STRB, shifted register offset, writeback"),
    Form("arm_ldst_extra", "arm", 0x00000090,
         [Field("puiwl", 20, 5), _reg("rn", 16), _reg("rt", 12), Field("hi", 8, 4), Field("sh", 5, 2, "num", "insn", (1, 2, 3)),
          Field("lo", 0, 4)],
         doc="LDRH, STRH, LDRSB, LDRSH, LDRD, STRD, immediate or register offset, writeback"),
    Form("arm_ldm_stm", "arm", 0x08000000,
         [Field("p", 24, 1), Field("u", 23, 1), Field("sbit", 22, 1), Field("w", 21, 1), Field("l", 20, 1),
          _reg("rn", 16), Field("pcbit", 15, 1), Field("list", 0, 15, "list")],
         kinds=(OP, RET, IJUMP), doc="LDM, STM in the four addressing modes, writeback (register list)"),
    Form("arm_b_bl", "arm", 0x0A000000,
         [Field("L", 24, 1), Field("imm24", 0, 24, "num", "insn", None, True)], kinds=(B, BL),
         doc="B and BL (the target is the address plus a sign-extended offset)"),
    Form("arm_bx_blx", "arm", 0x012FFF00,
         [Field("op2", 4, 4, "num", "insn", (1, 2, 3)), _reg("rm", 0)], kinds=(RET, IJUMP, ICALL),
         doc="BX, BXJ and BLX (register)"),
    Form("arm_media_ext", "arm", 0x06000070,
         [Field("op1", 20, 5, "num", "insn", (0b01000, 0b01010, 0b01011, 0b01100, 0b01110, 0b01111)),
          Field("rn", 16, 4, "sel"), _reg("rd", 12), Field("rot", 10, 2), _reg("rm", 0)],
         doc="SXTB/SXTH/UXTB/UXTH, their accumulating forms and the 16-bit pair extends"),
    Form("arm_qadd", "arm", 0x01000050,
         [Field("op", 21, 2), _reg("rn", 16), _reg("rd", 12), _reg("rm", 0)],
         doc="QADD, QSUB, QDADD, QDSUB (saturating, sticky Q)"),
    Form("vfp_arith", "arm", 0x0E000A00,
         [Field("p", 23, 1), Field("D", 22, 1), Field("q", 21, 1), Field("r", 20, 1), Field("vn", 16, 4),
          Field("vd", 12, 4), Field("sz", 8, 1), Field("N", 7, 1), Field("s", 6, 1), Field("M", 5, 1),
          Field("vm", 0, 4)] + _VEC, valid=_vfp_arith_valid,
         doc="VMLA, VMLS, VNMLS, VNMLA, VMUL, VNMUL, VADD, VSUB, VDIV; F32/F64; scalar and short vectors"),
    Form("vfp_ext", "arm", 0x0E000A40 | (1 << 23) | (1 << 21) | (1 << 20),
         [Field("D", 22, 1), Field("x", 16, 4), Field("vd", 12, 4), Field("sz", 8, 1), Field("N", 7, 1),
          Field("M", 5, 1), Field("vm", 0, 4)] + _VEC,
         doc="VMOV, VABS, VNEG, VSQRT (short vectors), VCMP, VCVT between sizes and to/from integers"),
    Form("vfp_ldst", "arm", 0x0D000A00,
         [Field("U", 23, 1), Field("D", 22, 1), Field("L", 20, 1), _reg("rn", 16), Field("vd", 12, 4),
          Field("sz", 8, 1), Field("imm8", 0, 8)],
         doc="VLDR, VSTR"),
    Form("vfp_xfer", "arm", 0x0E000A10,
         [Field("opc1", 21, 3), Field("L", 20, 1), Field("vn", 16, 4), Field("rt", 12, 4, "sel"),
          Field("sz", 8, 1), Field("N", 7, 1)],
         doc="VMOV core register to/from single or half of a double, VMRS, VMSR"),
    Form("t_shift_imm", "thumb", 0x0000,
         [Field("op", 11, 2, "num", "insn", (0, 1, 2)), Field("imm5", 6, 5), _reg("rm", 3, 3), _reg("rd", 0, 3)],
         doc="Thumb LSL, LSR, ASR by an immediate (sets flags)"),
    Form("t_alu", "thumb", 0x4000,
         [Field("op", 6, 4), _reg("rm", 3, 3), _reg("rd", 0, 3)],
         doc="Thumb ALU operations: AND, EOR, shifts, ADC, SBC, TST, NEG, CMP, CMN, ORR, MUL, BIC, MVN"),
    Form("t_ldst_imm", "thumb", 0x6000,
         [Field("B", 12, 1), Field("L", 11, 1), Field("imm5", 6, 5), _reg("rn", 3, 3), _reg("rd", 0, 3)],
         doc="Thumb LDR, STR, LDRB, STRB with a 5-bit immediate offset"),
    Form("t_push_pop", "thumb", 0xB400,
         [Field("L", 11, 1), Field("R", 8, 1), Field("list", 0, 8, "list")], kinds=(OP, RET),
         doc="Thumb PUSH and POP (register list, optional LR / PC)"),
]


# More forms, probed to size the work of N1-28: the rest of what the decoders handle that the 20
# forms above do not cover by shape. EXPECTED_FAILURES names the ones the generator cannot express and
# the reason it gives.
EXTRA_FORMS: list[Form] = [
    Form("arm_media_par", "arm", 0x06000F10,
         [Field("kind", 20, 3, "num", "insn", (1, 2, 3, 5, 6, 7)), Field("op2", 5, 3, "num", "insn", (0, 1, 2, 3, 4, 7)),
          _reg("rn", 16), _reg("rd", 12), _reg("rm", 0)],
         doc="parallel add/subtract (SADD16, QADD8, UHSUB16, USAX ...), sets GE"),
    Form("arm_media_sat", "arm", 0x06A00010,
         [Field("U", 22, 1), Field("sat", 16, 5), _reg("rd", 12), Field("imm5", 7, 5), Field("sh", 6, 1),
          _reg("rm", 0)], doc="SSAT, USAT"),
    Form("arm_halfmul", "arm", 0x01000080,
         [Field("op", 21, 2), _reg("rd", 16), _reg("rn", 12), _reg("rs", 8), Field("y", 6, 1), Field("x", 5, 1),
          _reg("rm", 0)], doc="SMLAxy, SMLAWy, SMULWy, SMLALxy, SMULxy"),
    Form("arm_swp", "arm", 0x01000090,
         [Field("B", 22, 1), _reg("rn", 16), _reg("rt", 12), _reg("rm", 0)], doc="SWP, SWPB"),
    Form("arm_excl", "arm", 0x01800F90,
         [Field("op", 20, 3), _reg("rn", 16), _reg("rt", 12), Field("rm", 0, 4, "sel")],
         doc="LDREX, STREX and the byte, halfword and doubleword variants"),
    Form("arm_vldm", "arm", 0x0C000A00,
         [Field("p", 24, 1), Field("u", 23, 1), Field("D", 22, 1), Field("w", 21, 1), Field("l", 20, 1),
          _reg("rn", 16), Field("vd", 12, 4), Field("sz", 8, 1), Field("imm8", 0, 8)],
         doc="VLDM, VSTM (a register count in imm8)"),
    Form("arm_vmov2", "arm", 0x0C400A10,
         [Field("l", 20, 1), _reg("rt2", 16), _reg("rt", 12), Field("sz", 8, 1), Field("M", 5, 1),
          Field("vm", 0, 4)], doc="VMOV between two core registers and a double or two singles"),
    Form("arm_mrs", "arm", 0x010F0000, [_reg("rd", 12)], doc="MRS Rd, CPSR"),
    Form("arm_msr", "arm", 0x0120F000, [Field("mask", 16, 4), _reg("rm", 0)], doc="MSR CPSR_fields, Rm"),
    Form("arm_cp15", "arm", 0x0E000F10,
         [Field("opc1", 21, 3, "num", "insn", (0,)), Field("L", 20, 1), Field("crn", 16, 4, "num", "insn", (7, 13)),
          _reg("rt", 12), Field("opc2", 5, 3, "num", "insn", (2, 3, 4, 5)),
          Field("crm", 0, 4, "num", "insn", (0, 5, 10))],
         doc="MRC/MCR to CP15 (thread ID registers, barriers; every other register is unsupported and traps)"),
    Form("arm_ldrpc", "arm", 0x04100000,
         [Field("pu", 23, 2), Field("b", 22, 1), Field("w", 21, 1), _reg("rn", 16), Field("rt", 12, 4, "sel"),
          Field("imm12", 0, 12)], kinds=(OP, RET, IJUMP), doc="LDR/LDRB immediate including loads into pc"),
    Form("arm_dp_rd15", "arm", 0x0000F000,
         [Field("opc", 21, 4), Field("S", 20, 1), _reg("rn", 16), Field("imm5", 7, 5), Field("typ", 5, 2),
          _reg("rm", 0)], valid=_dp_valid, kinds=(RET, IJUMP), doc="data processing with pc as the destination"),
    Form("arm_svc", "arm", 0x0F000000, [Field("imm", 0, 24)], kinds=(SVC,), doc="SVC"),
    Form("t_addsub", "thumb", 0x1800,
         [Field("i", 10, 1), Field("sub", 9, 1), Field("rn", 6, 3), _reg("rs", 3, 3), _reg("rd", 0, 3)],
         doc="Thumb ADD/SUB with a register or a 3-bit immediate"),
    Form("t_imm8", "thumb", 0x2000, [Field("op", 11, 2), _reg("rd", 8, 3), Field("imm", 0, 8)],
         doc="Thumb MOV, CMP, ADD, SUB with an 8-bit immediate"),
    Form("t_hireg", "thumb", 0x4400,
         [Field("op", 8, 2), Field("h1", 7, 1), Field("rm", 3, 4, "sel"), Field("rd3", 0, 3)],
         kinds=(OP, RET, IJUMP, ICALL, B), doc="Thumb ADD, CMP, MOV with high registers, BX, BLX"),
    Form("t_ldr_lit", "thumb", 0x4800, [_reg("rd", 8, 3), Field("imm", 0, 8)], doc="Thumb LDR Rd, [pc, #imm]"),
    Form("t_ldst_reg", "thumb", 0x5000, [Field("op", 9, 3), _reg("rm", 6, 3), _reg("rn", 3, 3), _reg("rd", 0, 3)],
         doc="Thumb loads and stores with a register offset"),
    Form("t_ldst_half", "thumb", 0x8000,
         [Field("L", 11, 1), Field("imm5", 6, 5), _reg("rn", 3, 3), _reg("rd", 0, 3)], doc="Thumb LDRH, STRH"),
    Form("t_ldst_sp", "thumb", 0x9000, [Field("L", 11, 1), _reg("rd", 8, 3), Field("imm", 0, 8)],
         doc="Thumb LDR, STR relative to sp"),
    Form("t_addr", "thumb", 0xA000, [Field("sp", 11, 1), _reg("rd", 8, 3), Field("imm", 0, 8)],
         doc="Thumb ADD Rd, pc/sp, #imm"),
    Form("t_misc_sp", "thumb", 0xB000, [Field("s", 7, 1), Field("imm", 0, 7)], doc="Thumb ADD/SUB sp, #imm"),
    Form("t_ext", "thumb", 0xB200, [Field("op", 6, 2), _reg("rm", 3, 3), _reg("rd", 0, 3)],
         doc="Thumb SXTH, SXTB, UXTH, UXTB"),
    Form("t_rev", "thumb", 0xBA00, [Field("op", 6, 2, "num", "insn", (0, 1, 3)), _reg("rm", 3, 3), _reg("rd", 0, 3)],
         doc="Thumb REV, REV16, REVSH"),
    Form("t_ldm", "thumb", 0xC000, [Field("L", 11, 1), _reg("rn", 8, 3), Field("list", 0, 8, "list")],
         list_base="rn", doc="Thumb LDMIA, STMIA (the C depends on the base register being in the list)"),
    Form("t_bcond", "thumb", 0xD000,
         [Field("cond", 8, 4, "num", "insn", tuple(range(14))), Field("off", 0, 8, "num", "insn", None, True)],
         kinds=(B,), doc="Thumb conditional branch"),
    Form("t_b", "thumb", 0xE000, [Field("off", 0, 11, "num", "insn", None, True)], kinds=(B,),
         doc="Thumb unconditional branch"),
    Form("t_bl", "thumb", 0xF800F000,
         [Field("hi", 0, 11, "num", "insn", None, True), Field("lo", 16, 11)], kinds=(BL,),
         doc="Thumb BL (two halfwords)"),
    Form("t_blx_imm", "thumb", 0xE800F000,
         [Field("hi", 0, 11, "num", "insn", None, True), Field("lo", 17, 10)], kinds=(BL,),
         doc="Thumb BLX immediate (two halfwords; the target is aligned down to a word)"),
]

EXPECTED_FAILURES = {
    "arm_vldm": "wide field imm8",
}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Generate the fallback interpreter's instruction handlers.")
    ap.add_argument("--out", help="write the C file here")
    ap.add_argument("--form", action="append", help="only these forms (default: the 20 of the spike)")
    ap.add_argument("--extra", action="store_true", help="also the extra forms probed for the estimate")
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args(argv)
    todo = FORMS + (EXTRA_FORMS if args.extra or args.form else [])
    names = {f.name for f in FORMS + EXTRA_FORMS}
    for n in args.form or []:
        if n not in names:
            print(f"unknown form {n!r}; the forms are: {', '.join(sorted(names))}", file=sys.stderr)
            return 2
    out = [PREAMBLE % cond_switch()]
    failed = 0
    for form in todo:
        if args.form and form.name not in args.form:
            continue
        try:
            g = generate(form, args.seed)
        except GenError as e:
            if form.name in EXPECTED_FAILURES and EXPECTED_FAILURES[form.name] in str(e):
                print(f"{form.name:18} not generated (known): {str(e).splitlines()[0]}")
                continue
            failed += 1
            print(f"{form.name:18} FAILED: {str(e).splitlines()[0]}")
            continue
        print(f"{form.name:18} leaves={g.leaves:5d} C={g.c_bytes:7d} bytes  tables={g.tables}({g.table_entries})"
              f"  verified={g.verified}  {g.seconds:.1f}s")
        out += [""] + g.c
    if args.out and not failed:
        with open(args.out, "w") as f:
            f.write("\n".join(out) + "\n")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
