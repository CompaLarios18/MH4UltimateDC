"""C generation for analysed functions (and for single-instruction fuzz functions)."""
from __future__ import annotations

from .insn import (AL, B, BL, COND_NAMES, ICALL, IJUMP, JTAB, OP, RET, SVC, TRAP, Insn, hexc)


def fname(addr: int) -> str:
    return f"f_{addr:08x}"


def label(addr: int) -> str:
    return f"L_{addr:08x}"


def wrap(ins: Insn, lines: list[str]) -> list[str]:
    """Put `lines` in a scope, guarded by the instruction's condition."""
    if not lines:
        return []
    inner = " ".join(lines)
    if ins.cond == AL:
        return [f"{{ {inner} }}"]
    return [f"if (C_{COND_NAMES[ins.cond]}) {{ {inner} }}"]


def straight_line(ins: Insn) -> list[str]:
    """C for instructions without control flow decisions (OP / SVC / TRAP)."""
    if ins.kind == OP:
        return wrap(ins, ins.body)
    if ins.kind == SVC:
        return wrap(ins, [f"rc_svc(ctx, {hexc(ins.imm)}, {hexc(ins.next)});"])
    if ins.kind == TRAP:
        return wrap(ins, [f"rc_trap(ctx, {hexc(ins.addr)}, {ins.imm}); return;"])
    raise ValueError(f"not a straight-line instruction: {ins.kind}")


def comment(ins: Insn) -> str:
    w = f"{ins.word:08x}" if ins.size == 4 else f"{ins.word:04x}"
    return f"/* {ins.addr:08x} {w} */"


# ── whole functions ──────────────────────────────────────────────────────

def _lr_value(ins: Insn) -> int:
    return ins.next | 1 if ins.mode == "thumb" else ins.next


class FunctionEmitter:
    """Turns analysed functions into C. `known` maps entry address -> mode for every
    function that will be emitted, so direct calls can be resolved statically."""

    def __init__(self, known: dict[int, str]):
        self.known = known

    def call(self, target: int, mode: str) -> str:
        if self.known.get(target) == mode:
            return f"{fname(target)}(ctx);"
        return f"rc_call(ctx, {hexc(target | (1 if mode == 'thumb' else 0))});"

    def falls_through(self, f, ins: Insn) -> bool:
        if ins.addr in f.fall_tail:
            return False
        k = ins.kind
        if k == BL and ins.addr in f.mret_calls:
            return False
        if k in (OP, SVC, BL, ICALL):
            return True
        if k == B:
            return f.link_calls.get(ins.addr) == ins.next or ins.conditional
        if k in (RET, IJUMP):
            return f.link_calls.get(ins.addr) == ins.next or ins.conditional
        if k == JTAB:
            return ins.conditional
        return False

    def insn(self, f, ins: Insn) -> list[str]:
        k = ins.kind
        if k in (OP, SVC, TRAP):
            out = straight_line(ins)
        elif k == B:
            if ins.addr in f.link_calls:
                ret = f.link_calls[ins.addr]
                out = wrap(ins, [self.call(ins.target, ins.target_mode)] +
                           ([] if ret == ins.next else [f"goto {label(ret)};"]))
            elif ins.addr in f.tails:
                out = wrap(ins, [self.call(ins.target, ins.target_mode), "return;"])
            elif ins.cond == AL:
                out = [f"goto {label(ins.target)};"]
            else:
                out = [f"if (C_{COND_NAMES[ins.cond]}) goto {label(ins.target)};"]
        elif k == BL and ins.addr in f.mret_calls:
            # the callee continues at one of the continuations after its descriptor word,
            # or returns to our caller (see Analyzer.inline_arg_callee)
            cases = " ".join(f"case {hexc(t)}: goto {label(t)};" for t in f.mret_calls[ins.addr])
            out = wrap(ins, [f"R(14) = {hexc(_lr_value(ins))};", self.call(ins.target, ins.target_mode),
                             f"switch (R(15)) {{ {cases} default: RC_RET(R(15)); }}"])
        elif k == BL:
            out = wrap(ins, [f"R(14) = {hexc(_lr_value(ins))};", self.call(ins.target, ins.target_mode)])
        elif k == ICALL:
            out = wrap(ins, ins.body + [f"R(14) = {hexc(_lr_value(ins))};", f"rc_call(ctx, {ins.pcexpr});"])
        elif k == RET:
            out = wrap(ins, ins.body + [f"{'RC_RET_TO' if f.mret else 'RC_RET'}({ins.pcexpr});"])
        elif k == IJUMP:
            if ins.addr in f.link_calls:
                ret = f.link_calls[ins.addr]
                tail = [] if ret == ins.next else [f"goto {label(ret)};"]
                out = wrap(ins, ins.body + [f"rc_call(ctx, {ins.pcexpr});"] + tail)
            elif ins.addr in f.ijump_targets:
                cases = " ".join(f"case {hexc(t)}: goto {label(t)};" for t in f.ijump_targets[ins.addr])
                out = wrap(ins, ins.body + [f"switch ({ins.pcexpr}) {{ {cases} default: rc_call(ctx, {ins.pcexpr}); return; }}"])
            elif f.mret:
                out = wrap(ins, ins.body + [f"RC_RET_TO({ins.pcexpr});"])
            else:
                out = wrap(ins, ins.body + [f"rc_call(ctx, {ins.pcexpr});", "return;"])
        elif k == JTAB:
            cases = " ".join(f"case {i}u: goto {label(t)};" for i, t in enumerate(f.jtabs[ins.addr]))
            out = wrap(ins, [f"switch (R({ins.jt_index})) {{ {cases} default: "
                             f"rc_trap(ctx, {hexc(ins.addr)}, RC_TRAP_BAD_JUMP); return; }}"])
        else:
            raise ValueError(k)
        if ins.addr in f.fall_tail:
            t = f.fall_tail[ins.addr]
            out = out + [self.call(t, f.mode), "return;"]
        return out

    def function(self, f) -> list[str]:
        addrs = sorted(f.insns)
        labels = set(f.labels)
        entry_jump = bool(addrs) and addrs[0] != f.entry
        if entry_jump:
            labels.add(f.entry)        # body has code below the entry point
        # fallthrough across a gap/overlap needs an explicit goto (and a label)
        gotos = {}
        for i, a in enumerate(addrs):
            ins = f.insns[a]
            if self.falls_through(f, ins):
                nxt = addrs[i + 1] if i + 1 < len(addrs) else None
                if nxt != ins.next:
                    gotos[a] = ins.next
                    labels.add(ins.next)
        # basic blocks for cycle accounting
        starts = labels | {f.entry}
        ticks = {}
        cur, n = None, 0
        for i, a in enumerate(addrs):
            if a in starts or cur is None:
                if cur is not None:
                    ticks[cur] = n
                cur, n = a, 0
            n += 1
            if not self.falls_through(f, f.insns[a]) or a in gotos:
                ticks[cur] = n
                cur, n = None, 0
        if cur is not None:
            ticks[cur] = n
        out = [f"void {fname(f.entry)}(RCtx *ctx) {{", "    RC_ENTRY();"]
        if entry_jump:
            out.append(f"    goto {label(f.entry)};")
        for a in addrs:
            ins = f.insns[a]
            if a in labels:
                out.append(f"{label(a)}:;")
            if a in ticks:
                out.append(f"    RC_TICK({ticks[a]});")
            code = self.insn(f, ins)
            if code:
                out.append(f"    {comment(ins)} {' '.join(code)}")
            else:
                out.append(f"    {comment(ins)}")
            if a in gotos:
                if gotos[a] in f.insns:
                    out.append(f"    goto {label(gotos[a])};")
                else:
                    out.append(f"    rc_trap(ctx, {hexc(gotos[a])}, RC_TRAP_BAD_JUMP); return;")
        # labels for targets that aren't instructions of this function (shouldn't happen)
        for t in sorted(labels - set(addrs)):
            out.append(f"{label(t)}: rc_trap(ctx, {hexc(t)}, RC_TRAP_BAD_JUMP); return;")
        out.append("}")
        return out
