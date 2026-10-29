#!/usr/bin/env python3
"""Build the orchestrator's status table, config/functions.tsv.

Inputs: Ghidra's functions.tsv export, the code, config/symbols.txt and optionally an
existing config/functions.tsv (its status/batch/notes columns are preserved).

Columns: address, size, mode, category, name, status, batch, notes
  category  sdk       contains a system call (svc), or is named as a C runtime, C library or Nintendo
                      SDK function (SDK_NAMES, SDK_NAME_PREFIXES): replaced by the port layer
                      or linked from a library, not decompiled; with --propagate-sdk also a
                      function whose every direct call (ARM BL) goes to an sdk function
            dti-init  registers MT Framework classes (calls the DTI constructor)
            unknown   not classified yet
  status    todo | replace (sdk) | done | blocked | ...   (free-form after creation)

usage: build_function_table.py --exheader exheader.bin --code code.bin \
           --functions functions.tsv [--symbols config/symbols.txt] \
           [--dti-ctor 0x005bfa94] [--propagate-sdk] -o config/functions.tsv
"""
from __future__ import annotations

import argparse
import csv
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import codebin2elf  # noqa: E402
import exheader as exh  # noqa: E402
import mt_dti  # noqa: E402

COLUMNS = ["address", "size", "mode", "category", "name", "status", "batch", "notes"]
KEEP_FROM_EXISTING = ("status", "batch", "notes")
# The statuses the tool itself gives (by category): recomputed on a rebuild, so that a function a
# rebuild newly classifies sdk becomes replace. Any other status was set by someone and is kept.
DEFAULT_STATUSES = frozenset({"todo", "replace"})

# Names (config/symbols.txt) of C runtime and C library functions, matched exactly: a prefix
# such as "free" or "exit" would also take game functions (freeList, exitQuest).
SDK_NAMES = frozenset({
    "memcpy", "memmove", "memset", "memcmp", "memchr",
    "strlen", "strcpy", "strncpy", "strcat", "strncat", "strcmp", "strncmp", "strchr", "strrchr", "strstr",
    "malloc", "calloc", "realloc", "free", "abort", "exit",
    "rand", "srand", "atoi", "atof", "strtol", "strtoul", "strtod",
    "sprintf", "snprintf", "vsprintf", "vsnprintf", "sscanf",
})
# Prefixes that only the compiler's runtime (RVCT / ARM EABI, libgcc) and the Nintendo SDK use.
SDK_NAME_PREFIXES = ("__aeabi_", "__rt_", "__gnu_", "nn::", "_ZN2nn")


def load_symbols(path: str | None) -> dict[int, str]:
    out: dict[int, str] = {}
    if not path or not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                addr, name = line.split()[:2]
                out[int(addr, 16)] = name
    return out


def is_sdk_name(name: str) -> bool:
    return name in SDK_NAMES or name.startswith(SDK_NAME_PREFIXES)


def arm_call_targets(code: bytes, base: int, addr: int, size: int) -> set[int]:
    """The targets of the ARM BL instructions in a function (as calls_target decodes them)."""
    targets = set()
    off = addr - base
    for i in range((off + 3) & ~3, min(off + size, len(code)) - 3, 4):
        target = mt_dti.bl_target(base + i, struct.unpack_from("<I", code, i)[0])
        if target is not None:
            targets.add(target)
    return targets


def contains_svc(code: bytes, base: int, addr: int, size: int, thumb: bool) -> bool:
    off, end = addr - base, addr - base + size
    if off < 0 or end > len(code):
        return False
    if thumb:
        return any(code[i + 1] == 0xDF for i in range(off, end - 1, 2))
    return any(code[i + 3] == 0xEF and code[i + 3] >> 4 == 0xE
               for i in range((off + 3) & ~3, end - 3, 4))


def calls_target(code: bytes, base: int, addr: int, size: int, target: int) -> bool:
    off = addr - base
    for i in range((off + 3) & ~3, min(off + size, len(code)) - 3, 4):
        w = struct.unpack_from("<I", code, i)[0]
        if mt_dti.bl_target(base + i, w) == target:
            return True
    return False


def build(code: bytes, header: exh.ExHeader, functions: list[dict], symbols: dict[int, str],
          dti_ctor: int | None, existing: dict[int, dict], propagate_sdk: bool = False) -> list[dict]:
    base = header.text.address
    rows = []
    for f in functions:
        addr, size = int(f["address"], 16), int(f["size"], 16)
        thumb = f["mode"] == "thumb"
        if contains_svc(code, base, addr, size, thumb) or is_sdk_name(symbols.get(addr, "")):
            category = "sdk"
        elif dti_ctor is not None and addr != dti_ctor and calls_target(code, base, addr, size, dti_ctor):
            category = "dti-init"
        else:
            category = "unknown"
        row = {
            "address": f"0x{addr:08x}", "size": f"0x{size:x}", "mode": f["mode"],
            "category": category, "name": symbols.get(addr, ""),
            "status": "replace" if category == "sdk" else "todo", "batch": "", "notes": "",
        }
        old = existing.get(addr)
        if old:
            for k in KEEP_FROM_EXISTING:
                if old.get(k) and not (k == "status" and old[k] in DEFAULT_STATUSES):
                    row[k] = old[k]
        rows.append(row)
    if propagate_sdk:
        propagate(code, base, functions, rows, existing)
    rows.sort(key=lambda r: int(r["address"], 16))
    return rows


def propagate(code: bytes, base: int, functions: list[dict], rows: list[dict], existing: dict[int, dict]) -> None:
    """Marks sdk an unknown ARM function that makes at least one direct call and calls only sdk
    functions, until nothing changes (an SDK wrapper of an SDK wrapper). Thumb functions are left
    alone: calls_target and this decode ARM BLs only. A status someone set in the existing table
    is kept."""
    by_addr = {int(r["address"], 16): r for r in rows}
    calls = {int(f["address"], 16): arm_call_targets(code, base, int(f["address"], 16), int(f["size"], 16))
             for f in functions if f["mode"] == "arm"}
    changed = True
    while changed:
        changed = False
        for addr, targets in calls.items():
            row = by_addr[addr]
            if row["category"] != "unknown" or not targets:
                continue
            if all(t in by_addr and by_addr[t]["category"] == "sdk" for t in targets):
                row["category"] = "sdk"
                if (existing.get(addr) or {}).get("status", "todo") in DEFAULT_STATUSES:
                    row["status"] = "replace"
                changed = True


def read_tsv(path: str) -> list[dict]:
    with open(path, encoding="utf-8") as f:
        return list(csv.DictReader((line for line in f if not line.startswith("#")), delimiter="\t"))


def write_tsv(rows: list[dict], path: str) -> None:
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write("# Function status table (single writer: orchestrator). Built by tools/build_function_table.py\n")
        w = csv.DictWriter(f, fieldnames=COLUMNS, delimiter="\t", lineterminator="\n")
        w.writeheader()
        w.writerows(rows)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exheader", required=True)
    ap.add_argument("--code", required=True)
    ap.add_argument("--functions", required=True)
    ap.add_argument("--symbols", default=os.path.join(os.path.dirname(__file__), "..", "config", "symbols.txt"))
    ap.add_argument("--dti-ctor", type=lambda s: int(s, 0))
    ap.add_argument("--propagate-sdk", action="store_true",
                    help="also mark sdk the ARM functions whose every direct call goes to an sdk function")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args(argv)

    with open(args.exheader, "rb") as f:
        header = exh.parse(f.read())
    with open(args.code, "rb") as f:
        code = codebin2elf.load_code(header, f.read())
    existing = {int(r["address"], 16): r for r in read_tsv(args.output)} if os.path.exists(args.output) else {}
    rows = build(code, header, read_tsv(args.functions), load_symbols(args.symbols), args.dti_ctor, existing,
                 args.propagate_sdk)
    write_tsv(rows, args.output)

    counts: dict[str, int] = {}
    for r in rows:
        counts[r["category"]] = counts.get(r["category"], 0) + 1
    print(f"{len(rows)} functions: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    print(f"named: {sum(1 for r in rows if r['name'])}")
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
