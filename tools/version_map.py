#!/usr/bin/env python3
"""Map code addresses between two versions of the game whose code differs only in .text.

The project's tables (config/functions.tsv, extra_entries.txt, hooks.txt, tunables.txt) hold
USA v1.3.0 addresses. Another version built from the same source (EUR v1.1.0) has the same
.rodata/.data/.bss layout, but its functions sit a little further along .text where code
before them changed. This tool finds, from both dumps, where each function went and writes
the result as a short list of shift intervals: USA addresses in [start, end) are at
address + shift in the other version. The list holds addresses only (no game data), so it
is committed (config/shifts/) and build_game.py applies it with only the other version's dump.

How a function is matched: the functions whose first 16 bytes occur exactly once in the other
code are anchors; the longest run of anchors in increasing order gives the candidate shifts
around each function, and a function is mapped when its bytes equal the other code's at one
of them, branch offsets that leave the function and code pointers set aside. Those set-aside
branches and pointers are then checked: each must land on the mapped address of its target.

usage: version_map.py build --src-exheader A/exheader.bin --src-code A/code.bin \
           --dst-exheader B/exheader.bin --dst-code B/code.bin \
           [--functions config/functions.tsv] [--name "EUR v1.1.0"] -o config/shifts/eur_v110.txt
       version_map.py translate config/shifts/eur_v110.txt 0x002951fc ...
"""
from __future__ import annotations

import argparse
import bisect
import hashlib
import os
import struct
import sys
from dataclasses import dataclass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_function_table as bft  # noqa: E402
import codebin2elf  # noqa: E402
import exheader as exh  # noqa: E402

ANCHOR_BYTES = 16
MAX_POINTER_DELTA = 0x10000   # a code pointer that moved further than this isn't the same pointer


@dataclass(frozen=True)
class Image:
    base: int
    code: bytes
    text_end: int   # absolute

    def in_text(self, addr: int) -> bool:
        return self.base <= (addr & ~1) < self.text_end


@dataclass(frozen=True)
class Function:
    addr: int
    size: int
    thumb: bool


class Shifts:
    """Sorted, non-overlapping intervals [start, end) of source .text with their shift."""

    def __init__(self, intervals: list[tuple[int, int, int]], text: tuple[int, int] | None = None,
                 header: list[str] | None = None):
        self.intervals = sorted(intervals)
        self.starts = [s for s, _, _ in self.intervals]
        self.text = text   # source .text [base, end): addresses outside it are the same in both
        self.header = header or []

    def translate(self, addr: int) -> int | None:
        """The other version's address, or None for a .text address in no interval."""
        thumb, a = addr & 1, addr & ~1
        if self.text and not self.text[0] <= a < self.text[1]:
            return addr
        i = bisect.bisect_right(self.starts, a) - 1
        if i >= 0 and a < self.intervals[i][1]:
            return (a + self.intervals[i][2]) | thumb
        return None

    def translate_keys(self, table: dict[int, object]) -> tuple[dict[int, object], list[int]]:
        """A table keyed by address, re-keyed; and the keys that have no mapping."""
        out, missing = {}, []
        for a, v in table.items():
            t = self.translate(a)
            if t is None:
                missing.append(a)
            else:
                out[t] = v
        return out, sorted(missing)

    def dumps(self) -> str:
        lines = [f"# {h}" for h in self.header]
        if self.text:
            lines.append(f"text 0x{self.text[0]:08x} 0x{self.text[1]:08x}")
        lines += [f"0x{s:08x} 0x{e:08x} {d:+#x}" for s, e, d in self.intervals]
        return "\n".join(lines) + "\n"

    @classmethod
    def loads(cls, text: str) -> "Shifts":
        intervals, rng, header = [], None, []
        for line in text.splitlines():
            if line.startswith("#"):
                header.append(line[1:].strip())
                continue
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "text":
                rng = (int(parts[1], 16), int(parts[2], 16))
            else:
                intervals.append((int(parts[0], 16), int(parts[1], 16), int(parts[2], 16)))
        return cls(intervals, rng, header)

    @classmethod
    def load(cls, path: str) -> "Shifts":
        with open(path, encoding="utf-8") as f:
            return cls.loads(f.read())


# ── comparing code ─────────────────────────────────────────────────────────

def _arm_branch(w: int) -> bool:
    """B, BL or BLX (immediate)."""
    return (w & 0x0E000000) == 0x0A000000


def _arm_target(w: int, pc: int) -> int:
    imm = w & 0xFFFFFF
    imm -= (imm & 0x800000) << 1
    target = pc + 8 + (imm << 2)
    if (w >> 28) == 0xF:   # BLX: to Thumb, H bit gives the halfword
        target |= ((w >> 24) & 1) << 1 | 1
    return target & 0xFFFFFFFF


def _thumb_bl(h1: int, h2: int) -> bool:
    return (h1 & 0xF800) == 0xF000 and (h2 & 0xE800) == 0xE800


def _thumb_bl_target(h1: int, h2: int, pc: int) -> int:
    off = ((h1 & 0x7FF) << 12) | ((h2 & 0x7FF) << 1)
    off -= (off & 0x400000) << 1
    target = pc + 4 + off
    return (target | 1) if h2 & 0x1000 else (target & ~3)   # BL stays Thumb, BLX goes to ARM


def _thumb_b_target(h: int, pc: int) -> int | None:
    if (h & 0xF800) == 0xE000:            # B (unconditional)
        off = (h & 0x7FF) << 1
        off -= (off & 0x800) << 1
    elif (h & 0xF000) == 0xD000 and (h & 0x0F00) < 0x0E00:   # B<cond>
        off = (h & 0xFF) << 1
        off -= (off & 0x100) << 1
    else:
        return None
    return pc + 4 + off


def _pointer_pair(src: Image, dst: Image, wu: int, we: int) -> bool:
    return (src.in_text(wu) and dst.in_text(we) and (wu & 1) == (we & 1)
            and abs(we - wu) < MAX_POINTER_DELTA)


def compare(src: Image, dst: Image, sa: int, da: int, size: int, thumb: bool) -> list[tuple[int, int]] | None:
    """Whether `size` bytes at sa (source) and da (destination) are the same code once
    branches out of the range and code pointers are set aside. Returns those set-aside
    (source target, destination target) pairs, or None if the code differs. A branch
    inside the range must keep its offset within it."""
    so, do = sa - src.base, da - dst.base
    if so < 0 or do < 0 or so + size > len(src.code) or do + size > len(dst.code):
        return None
    s, d = src.code, dst.code
    pairs: list[tuple[int, int]] = []

    def branch(tu: int, te: int) -> bool:
        if sa <= (tu & ~1) < sa + size:
            return (te & ~1) - da == (tu & ~1) - sa
        pairs.append((tu, te))
        return True

    k = 0
    if not thumb:
        while k + 4 <= size:
            wu = struct.unpack_from("<I", s, so + k)[0]
            we = struct.unpack_from("<I", d, do + k)[0]
            if _arm_branch(wu) and _arm_branch(we) and (wu >> 24) == (we >> 24):
                if not branch(_arm_target(wu, sa + k), _arm_target(we, da + k)):
                    return None
            elif wu != we:
                if not _pointer_pair(src, dst, wu, we):
                    return None
                pairs.append((wu, we))
            k += 4
        return pairs if s[so + k:so + size] == d[do + k:do + size] else None
    while k + 2 <= size:
        hu = struct.unpack_from("<H", s, so + k)[0]
        he = struct.unpack_from("<H", d, do + k)[0]
        if k + 4 <= size:
            hu2 = struct.unpack_from("<H", s, so + k + 2)[0]
            he2 = struct.unpack_from("<H", d, do + k + 2)[0]
            if _thumb_bl(hu, hu2) and _thumb_bl(he, he2) and (hu2 & 0x1000) == (he2 & 0x1000):
                if not branch(_thumb_bl_target(hu, hu2, sa + k), _thumb_bl_target(he, he2, da + k)):
                    return None
                k += 4
                continue
        if hu != he:
            tu, te = _thumb_b_target(hu, sa + k), _thumb_b_target(he, da + k)
            if tu is not None and te is not None and (hu & 0xFF00 if hu & 0xF000 == 0xD000 else 0xE000) == \
                    (he & 0xFF00 if he & 0xF000 == 0xD000 else 0xE000):
                if not branch(tu | 1, te | 1):
                    return None
                k += 2
                continue
            if k % 4 == 0 and k + 4 <= size:   # a literal word holding a code pointer
                wu = struct.unpack_from("<I", s, so + k)[0]
                we = struct.unpack_from("<I", d, do + k)[0]
                if _pointer_pair(src, dst, wu, we):
                    pairs.append((wu, we))
                    k += 4
                    continue
            return None
        k += 2
    return pairs if s[so + k:so + size] == d[do + k:do + size] else None


# ── building the map ───────────────────────────────────────────────────────

def _anchors(src: Image, dst: Image, funcs: list[Function]) -> list[tuple[int, int]]:
    """(source, destination) starts of functions found exactly once in the destination,
    reduced to the longest run that increases in both."""
    index: dict[bytes, int] = {}
    seen_twice: set[bytes] = set()
    text = dst.text_end - dst.base
    for i in range(0, text - ANCHOR_BYTES + 1, 2):
        key = dst.code[i:i + ANCHOR_BYTES]
        if key in index:
            seen_twice.add(key)
        else:
            index[key] = i
    pairs = []
    for f in funcs:
        if f.size < ANCHOR_BYTES:
            continue
        key = src.code[f.addr - src.base:f.addr - src.base + ANCHOR_BYTES]
        if key in index and key not in seen_twice:
            pairs.append((f.addr, index[key] + dst.base))
    pairs.sort()
    # longest increasing subsequence on the destination address
    tails: list[int] = []
    tail_idx: list[int] = []
    prev = [-1] * len(pairs)
    for i, (_, b) in enumerate(pairs):
        j = bisect.bisect_left(tails, b)
        if j == len(tails):
            tails.append(b)
            tail_idx.append(i)
        else:
            tails[j] = b
            tail_idx[j] = i
        prev[i] = tail_idx[j - 1] if j else -1
    out = []
    i = tail_idx[-1] if tail_idx else -1
    while i != -1:
        out.append(pairs[i])
        i = prev[i]
    return out[::-1]


def _containing(funcs: list[Function], starts: list[int], addr: int) -> Function | None:
    i = bisect.bisect_right(starts, addr & ~1) - 1
    if i >= 0 and (addr & ~1) < funcs[i].addr + funcs[i].size:
        return funcs[i]
    return None


def match_functions(src: Image, dst: Image, funcs: list[Function]) -> tuple[dict[int, int], dict]:
    """{source function: destination address} and a report."""
    funcs = sorted(funcs, key=lambda f: f.addr)
    anchors = _anchors(src, dst, funcs)
    anchor_src = [a for a, _ in anchors]
    mapped: dict[int, int] = {}
    pairs: dict[int, list[tuple[int, int]]] = {}
    last_start, last_shift = dst.base, 0   # (listed functions can overlap: order by start)
    for f in funcs:
        j = bisect.bisect_right(anchor_src, f.addr)
        cands = [last_shift]
        for k in (j - 1, j, j - 2, j + 1):
            if 0 <= k < len(anchors):
                d = anchors[k][1] - anchors[k][0]
                if d not in cands:
                    cands.append(d)
        for d in cands:
            if f.addr + d < last_start:
                continue
            p = compare(src, dst, f.addr, f.addr + d, f.size, f.thumb)
            if p is not None:
                mapped[f.addr], pairs[f.addr] = f.addr + d, p
                last_start, last_shift = f.addr + d, d
                break
    # every branch and pointer that was set aside must land on its target's new address
    starts = [f.addr for f in funcs]
    inconsistent = 0
    while True:
        bad = set()
        for a, ps in pairs.items():
            for tu, te in ps:
                g = _containing(funcs, starts, tu)
                if g is not None and g.addr in mapped and mapped[g.addr] - g.addr != (te & ~1) - (tu & ~1):
                    bad.add(a)
        if not bad:
            break
        inconsistent += len(bad)
        for a in bad:
            del mapped[a], pairs[a]
    checked = sum(1 for ps in pairs.values() for tu, _ in ps
                  if (g := _containing(funcs, starts, tu)) is not None and g.addr in mapped)
    report = {"functions": len(funcs), "anchors": len(anchors), "mapped": len(mapped),
              "unmapped": [f for f in funcs if f.addr not in mapped],
              "cross_checked": checked, "dropped_inconsistent": inconsistent}
    return mapped, report


def build_shifts(src: Image, dst: Image, funcs: list[Function]) -> tuple[Shifts, dict]:
    mapped, report = match_functions(src, dst, funcs)
    # only source .text: a listed "function" outside it (a few in .rodata) would shift data that
    # doesn't move, and the last one's end would run past .text
    funcs = sorted((f for f in funcs if src.in_text(f.addr)), key=lambda f: f.addr)
    intervals: list[tuple[int, int, int]] = []
    cur: list[int] | None = None   # [start, end, shift]
    for f in funcs:
        if f.addr not in mapped:
            if cur:   # a changed function ends the interval
                intervals.append(tuple(cur))
                cur = None
            continue
        d = mapped[f.addr] - f.addr
        end = min(f.addr + f.size, src.text_end)
        if cur and cur[2] == d and _gap_same(src, dst, cur[1], f.addr, d):
            cur[1] = max(cur[1], end)
            continue
        if cur:
            intervals.append(tuple(cur))
        cur = [f.addr, end, d]
    if cur:
        intervals.append(tuple(cur))
    report["intervals"] = len(intervals)
    return Shifts(intervals, (src.base, src.text_end)), report


def _gap_same(src: Image, dst: Image, start: int, end: int, d: int) -> bool:
    """Code between two functions with the same shift: unlisted functions, literal pools."""
    if end <= start:
        return True
    return (compare(src, dst, start, start + d, end - start, False) is not None
            or compare(src, dst, start, start + d, end - start, True) is not None)


def load_image(exheader_path: str, code_path: str) -> tuple[Image, exh.ExHeader, bytes]:
    with open(exheader_path, "rb") as f:
        header = exh.parse(f.read())
    with open(code_path, "rb") as f:
        code = codebin2elf.load_code(header, f.read())
    return Image(header.text.address, code, header.text.address + header.text.size), header, code


def same_layout(a: exh.ExHeader, b: exh.ExHeader) -> list[str]:
    """What, besides .text's size, differs between the two layouts (empty when the map applies)."""
    out = []
    for name in ("ro", "data"):
        sa, sb = getattr(a, name), getattr(b, name)
        if (sa.address, sa.size) != (sb.address, sb.size):
            out.append(f".{name}: 0x{sa.address:08x}+0x{sa.size:x} vs 0x{sb.address:08x}+0x{sb.size:x}")
    if a.text.address != b.text.address:
        out.append(f".text base: 0x{a.text.address:08x} vs 0x{b.text.address:08x}")
    if a.bss_size != b.bss_size:
        out.append(f".bss size: 0x{a.bss_size:x} vs 0x{b.bss_size:x}")
    return out


def load_functions(path: str) -> list[Function]:
    return [Function(int(r["address"], 16), int(r["size"], 16), r["mode"] == "thumb")
            for r in bft.read_tsv(path)]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="map the source version's functions into the destination's code")
    b.add_argument("--src-exheader", required=True)
    b.add_argument("--src-code", required=True)
    b.add_argument("--dst-exheader", required=True)
    b.add_argument("--dst-code", required=True)
    b.add_argument("--functions", default=os.path.join(os.path.dirname(__file__), "..", "config", "functions.tsv"))
    b.add_argument("--src-name", default="USA v1.3.0")
    b.add_argument("--name", required=True, help="the destination version, for the file's header")
    b.add_argument("-o", "--out", required=True)
    t = sub.add_parser("translate", help="print the destination address of source addresses")
    t.add_argument("shifts")
    t.add_argument("addresses", nargs="+")
    args = ap.parse_args(argv)

    if args.cmd == "translate":
        shifts = Shifts.load(args.shifts)
        for a in args.addresses:
            r = shifts.translate(int(a, 16))
            print(f"0x{int(a, 16):08x} -> " + ("unmapped" if r is None else f"0x{r:08x}"))
        return 0

    src, src_h, src_code = load_image(args.src_exheader, args.src_code)
    dst, dst_h, dst_code = load_image(args.dst_exheader, args.dst_code)
    problems = same_layout(src_h, dst_h)
    if problems:
        print("the two versions differ outside .text, which this map can't express:\n  " + "\n  ".join(problems),
              file=sys.stderr)
        return 1
    shifts, report = build_shifts(src, dst, load_functions(args.functions))
    shifts.header = [
        f".text shifts from {args.src_name} (code sha1 {hashlib.sha1(src_code).hexdigest()}) "
        f"to {args.name} (code sha1 {hashlib.sha1(dst_code).hexdigest()}).",
        "Built by tools/version_map.py from both dumps; addresses only. Each line: <start> <end> <shift>:",
        "source .text addresses in [start, end) are at address + shift. Outside .text, addresses are the same.",
        f"{report['mapped']}/{report['functions']} listed functions mapped, "
        f"{report['cross_checked']} branches and code pointers cross-checked.",
    ]
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(shifts.dumps())
    print(f"{report['mapped']}/{report['functions']} functions mapped ({report['anchors']} anchors), "
          f"{report['intervals']} intervals; {report['cross_checked']} branches/pointers cross-checked, "
          f"{report['dropped_inconsistent']} functions dropped as inconsistent")
    for f in report["unmapped"]:
        print(f"  unmapped: 0x{f.addr:08x} size 0x{f.size:x} {'thumb' if f.thumb else 'arm'}")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
