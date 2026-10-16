#!/usr/bin/env python3
"""Work-package status for the native port (docs/native/STATUS.md).

The work-package specs in docs/native/wp/*.md are the source of truth for each package's
title, size, agent, dependencies, needs and file allowlist; STATUS.md adds its status and a
note. This tool keeps the two consistent and tells the orchestrator what can start.

usage:
  wp_status.py check            specs and STATUS.md agree; dependencies exist; no cycles
  wp_status.py sync             rewrite STATUS.md's tables from the specs, keeping statuses and notes
  wp_status.py ready [--no-dump] [--assume-done ID,ID]
                                packages that can start (status todo, every dependency done),
                                the ones that unblock the most first. --no-dump leaves out the
                                ones that need the game files staged for their work; with
                                --assume-done, those packages count as done (merged into this
                                session's batch)
  wp_status.py overlaps ID ID… | --all [--assume-done ID,ID]
                                pairs of packages whose allowlists share files; with --all,
                                every pair that could be in flight at once (neither depends
                                on the other; none done). Exits 1 when it finds any
  wp_status.py pending          packages whose note says something is still owed
                                ("… pending", "waiting for …", "WIP on …")
  wp_status.py set ID STATUS [--note TEXT | --add-note TEXT]
                                change one package's status (then sync); --note replaces the
                                note, --add-note puts TEXT in front of it ("TEXT; old note"),
                                and without either the note is kept

Statuses: todo, active, review, done, blocked, needs-decision. "ready" is not stored: sync
shows a todo package as ready when all its dependencies are done.

Spec grammar (docs/native/AGENTS.md §4):
  ### <ID> · <title>
  - **Size / agent:** S|M|L · <agent> [anything]
  - **Depends on:** - | <ID>[, <ID> | <ID> to <ID>]… · **Needs:** - | <tag>[, <tag>]…
  - **Files (allowlist):** `path`, `dir/**`, `a/{b,c}/*.cpp`, `name.ext` (next to the path
    before it), `./NAME` (at the repository's root) … ; parenthesised remarks are ignored
IDs look like N1-07 or N2b-03; parenthesised remarks are ignored.
"""
from __future__ import annotations

import argparse
import fnmatch
import os
import re
import sys
from dataclasses import dataclass, field

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WP_DIR = os.path.join(ROOT, "docs", "native", "wp")
STATUS_PATH = os.path.join(ROOT, "docs", "native", "STATUS.md")

STATUSES = ("todo", "active", "review", "done", "blocked", "needs-decision")
NEEDS = ("none", "ci", "dump", "dump-check", "save", "pc", "mac", "3ds")
AGENTS = ("native-worker-high", "native-worker", "orchestrator", "maintainer")
# Note markers for something still owed (docs/native/AGENTS.md §4).
PENDING_RE = re.compile(r"\bpending\b|\bwaiting for\b|\bWIP on\b")

ID_RE = r"N[0-9][a-z]?-[0-9]+"
HEADING = re.compile(rf"^### ({ID_RE}) · (.+?)\s*$")
SIZE = re.compile(r"^- \*\*Size / agent:\*\* (S|M|L) · (.+)$")
DEPS = re.compile(r"^- \*\*Depends on:\*\* (.+?) · \*\*Needs:\*\* (.+)$")
FILES = re.compile(r"^- \*\*Files \(allowlist\):\*\* (.+)$")
BEGIN = re.compile(r"^<!-- wp-status:begin (\S+) -->$")
END = re.compile(r"^<!-- wp-status:end (\S+) -->$")
ROW = re.compile(rf"^\| ({ID_RE}) \|")
CELL_SPLIT = re.compile(r"(?<!\\)\|")


class SpecError(ValueError):
    pass


@dataclass
class Package:
    id: str
    title: str
    phase: str
    where: str = ""
    size: str = ""
    agent: str = ""
    depends: list[str] = field(default_factory=list)
    needs: list[str] = field(default_factory=list)
    files: list[str] | None = None   # allowlist patterns; None when the line is missing


@dataclass
class Entry:
    status: str = "todo"
    note: str = ""


def _strip_parens(text: str) -> str:
    while True:
        new = re.sub(r"\([^()]*\)", "", text)
        if new == text:
            return new
        text = new


def parse_depends(text: str) -> list[str]:
    """'-' → []; 'N0-03, N0-04' → both; 'N1-12 to N1-15' → the range; parentheses ignored."""
    text = _strip_parens(text).strip()
    if text in ("-", ""):
        return []
    out: list[str] = []
    for part in text.split(","):
        part = part.strip()
        m = re.fullmatch(r"(N[0-9][a-z]?)-([0-9]+) to (N[0-9][a-z]?)-([0-9]+)", part)
        if m:
            if m.group(1) != m.group(3):
                raise SpecError(f"range across phases: {part!r}")
            first, last = int(m.group(2)), int(m.group(4))
            if last < first:
                raise SpecError(f"reversed range: {part!r}")
            width = len(m.group(2))
            out.extend(f"{m.group(1)}-{n:0{width}d}" for n in range(first, last + 1))
            continue
        if not re.fullmatch(ID_RE, part):
            raise SpecError(f"can't read dependency {part!r} (write IDs separated by commas, or 'A to B')")
        out.append(part)
    return out


def parse_needs(text: str) -> list[str]:
    text = _strip_parens(text).strip()
    if text in ("-", ""):
        return ["none"]
    out = []
    for part in text.split(","):
        part = part.strip()
        if not re.fullmatch(r"[a-z0-9-]+", part):
            raise SpecError(f"can't read need {part!r} (one tag per comma; remarks go in parentheses)")
        out.append(part)
    return out


def parse_agent(text: str) -> str:
    for agent in AGENTS:
        if re.search(rf"(?<![\w-]){re.escape(agent)}(?![\w-])", text):
            return agent
    raise SpecError(f"no known agent in {text!r}")


def _expand_braces(pattern: str) -> list[str]:
    m = re.search(r"\{([^{}]*)\}", pattern)
    if not m:
        return [pattern]
    out = []
    for alt in m.group(1).split(","):
        out.extend(_expand_braces(pattern[:m.start()] + alt.strip() + pattern[m.end():]))
    return out


def parse_files(text: str) -> list[str]:
    """The path patterns of an allowlist line: backticked tokens outside parentheses.

    A bare file name (no '/') is taken to sit next to the path before it, and './name' is a
    file at the repository's root; anything after a '<' placeholder becomes '**'; options and
    words without '/' or '.' are skipped."""
    text = _strip_parens(text).strip()
    if text in ("-", ""):
        return []
    out: list[str] = []
    previous_dir = ""
    for token in re.findall(r"`([^`]+)`", text):
        token = token.strip()
        if "<" in token:
            token = token[:token.index("<")] + "**"
        if not token or token.startswith("-") or " " in token:
            continue
        if token.startswith("./"):        # a file at the repository's root
            token = token[2:]
            previous_dir = ""
            out.extend(_expand_braces(token))
            continue
        if "/" not in token:
            if "." not in token or not previous_dir:
                continue
            token = previous_dir + token
        previous_dir = token[:token.rindex("/") + 1]
        out.extend(_expand_braces(token))
    return out


def _segment_overlap(a: str, b: str) -> bool:
    """Whether one path segment could match both segment patterns ('*' and '?' wildcards)."""
    wild_a, wild_b = any(c in a for c in "*?"), any(c in b for c in "*?")
    if not wild_a and not wild_b:
        return a == b
    if not wild_a:
        return fnmatch.fnmatchcase(a, b)
    if not wild_b:
        return fnmatch.fnmatchcase(b, a)
    pa, pb = re.split(r"[*?]", a)[0], re.split(r"[*?]", b)[0]
    sa, sb = re.split(r"[*?]", a)[-1], re.split(r"[*?]", b)[-1]
    return (pa.startswith(pb) or pb.startswith(pa)) and (sa.endswith(sb) or sb.endswith(sa))


def _segments_overlap(a: list[str], b: list[str]) -> bool:
    """'**' stands for one or more segments at the end of a pattern, zero or more elsewhere."""
    if not a or not b:
        return not a and not b
    if a[0] == "**":
        rest = a[1:]
        if not rest:
            return len(b) >= 1
        return _segments_overlap(rest, b) or _segments_overlap(a, b[1:])
    if b[0] == "**":
        return _segments_overlap(b, a)
    return _segment_overlap(a[0], b[0]) and _segments_overlap(a[1:], b[1:])


def patterns_overlap(a: str, b: str) -> bool:
    """Whether some file could match both allowlist patterns."""
    return _segments_overlap(a.split("/"), b.split("/"))


def parse_specs(paths: list[str]) -> tuple[list[Package], list[str]]:
    """The packages in the spec files, and the problems found reading them (file:line: …)."""
    packages: list[Package] = []
    errors: list[str] = []
    for path in paths:
        phase = os.path.splitext(os.path.basename(path))[0]
        rel = os.path.relpath(path, ROOT) if path.startswith(ROOT) else path
        current: Package | None = None
        with open(path, encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                line = line.rstrip("\n")
                where = f"{rel}:{n}"
                if line.startswith("### "):
                    m = HEADING.match(line)
                    if not m:
                        errors.append(f"{where}: heading not recognised as a work package: {line!r}")
                        current = None
                        continue
                    current = Package(m.group(1), m.group(2), phase, where)
                    packages.append(current)
                    continue
                if current is None:
                    continue
                try:
                    m = SIZE.match(line)
                    if m:
                        current.size, current.agent = m.group(1), parse_agent(m.group(2))
                        continue
                    m = DEPS.match(line)
                    if m:
                        current.depends = parse_depends(m.group(1))
                        current.needs = parse_needs(m.group(2))
                        continue
                    m = FILES.match(line)
                    if m and current.files is None:
                        current.files = parse_files(m.group(1))
                except SpecError as e:
                    errors.append(f"{where}: {current.id}: {e}")
    return packages, errors


def spec_paths(wp_dir: str = WP_DIR) -> list[str]:
    names = sorted(n for n in os.listdir(wp_dir) if n.endswith(".md") and n != "LATER.md")
    return [os.path.join(wp_dir, n) for n in names]


def _cells(line: str) -> list[str]:
    return [c.strip().replace("\\|", "|") for c in CELL_SPLIT.split(line.strip().strip("|"))]


def _escape(text: str) -> str:
    return text.replace("\\|", "|").replace("|", "\\|")


def parse_status(text: str) -> dict[str, Entry]:
    entries: dict[str, Entry] = {}
    for line in text.splitlines():
        if not ROW.match(line):
            continue
        cells = _cells(line)
        # ID | Title | Size | Agent | Depends on | Needs | Status | Note
        if len(cells) < 8:
            continue
        status = cells[6].strip("`")
        if status == "ready":
            status = "todo"
        entries[cells[0]] = Entry(status, cells[7])
    return entries


def marker_phases(text: str) -> list[str]:
    """Phases with a table in STATUS.md; raises on a begin marker without its end."""
    phases, open_phase = [], None
    for line in text.splitlines():
        m = BEGIN.match(line)
        if m:
            if open_phase:
                raise SpecError(f"wp-status:begin {m.group(1)} inside the table of {open_phase}")
            open_phase = m.group(1)
            phases.append(open_phase)
            continue
        m = END.match(line)
        if m:
            if m.group(1) != open_phase:
                raise SpecError(f"wp-status:end {m.group(1)} without its begin")
            open_phase = None
    if open_phase:
        raise SpecError(f"wp-status:begin {open_phase} has no end marker")
    return phases


def problems(packages: list[Package], entries: dict[str, Entry], phases: list[str]) -> list[str]:
    out = []
    ids = [p.id for p in packages]
    known = set(ids)
    for i in sorted({i for i in ids if ids.count(i) > 1}):
        out.append(f"{i}: defined more than once")
    for phase in sorted({p.phase for p in packages} - set(phases)):
        out.append(f"phase {phase}: no wp-status table in STATUS.md (sync adds one)")
    for p in packages:
        if not p.size or not p.agent:
            out.append(f"{p.where}: {p.id}: no 'Size / agent' line")
        if not p.needs:
            out.append(f"{p.where}: {p.id}: no 'Depends on / Needs' line")
        if p.files is None:
            out.append(f"{p.where}: {p.id}: no 'Files (allowlist)' line")
        for d in p.depends:
            if d not in known:
                out.append(f"{p.where}: {p.id}: depends on unknown {d}")
            if d == p.id:
                out.append(f"{p.where}: {p.id}: depends on itself")
        for n in p.needs:
            if n not in NEEDS:
                out.append(f"{p.where}: {p.id}: unknown need {n!r}")
        if p.id not in entries:
            out.append(f"{p.id}: missing from STATUS.md (run sync)")
    for i, e in entries.items():
        if i not in known:
            out.append(f"{i}: in STATUS.md but not in any spec")
        if e.status not in STATUSES:
            out.append(f"{i}: unknown status {e.status!r}")
    out.extend(cycles(packages))
    return out


def cycles(packages: list[Package]) -> list[str]:
    """Dependency cycles, each reported once as 'cycle: A → B → A'."""
    deps = {p.id: p.depends for p in packages}
    state: dict[str, int] = {}   # 1 = on the current path, 2 = finished
    found: list[str] = []

    def visit(node: str, path: list[str]) -> None:
        state[node] = 1
        for d in deps.get(node, []):
            if state.get(d) == 1:
                found.append("cycle: " + " → ".join(path[path.index(d):] + [d]))
            elif d in deps and d not in state:
                visit(d, path + [d])
        state[node] = 2

    for p in packages:
        if p.id not in state:
            visit(p.id, [p.id])
    return found


def shown_status(p: Package, entries: dict[str, Entry]) -> str:
    e = entries.get(p.id, Entry())
    if e.status == "todo" and all(entries.get(d, Entry()).status == "done" for d in p.depends):
        return "ready"
    return e.status


def table(phase: str, packages: list[Package], entries: dict[str, Entry]) -> list[str]:
    lines = ["| ID | Title | Size | Agent | Depends on | Needs | Status | Note |",
             "|---|---|---|---|---|---|---|---|"]
    for p in packages:
        if p.phase != phase:
            continue
        e = entries.get(p.id, Entry())
        deps = ", ".join(p.depends) or "-"
        lines.append(f"| {p.id} | {_escape(p.title)} | {p.size} | {p.agent} | {deps} | {', '.join(p.needs)} "
                     f"| `{shown_status(p, entries)}` | {_escape(e.note)} |")
    return lines


def sync(text: str, packages: list[Package]) -> str:
    entries = parse_status(text)
    phases = marker_phases(text)
    lines = text.splitlines()
    missing = [ph for ph in dict.fromkeys(p.phase for p in packages) if ph not in phases]
    if missing:
        last_end = max((i for i, l in enumerate(lines) if END.match(l)), default=len(lines) - 1)
        extra: list[str] = []
        for ph in missing:
            extra += ["", f"## {ph}", "", f"<!-- wp-status:begin {ph} -->", f"<!-- wp-status:end {ph} -->"]
        lines = lines[:last_end + 1] + extra + lines[last_end + 1:]
    out: list[str] = []
    skipping = None
    for line in lines:
        m = BEGIN.match(line)
        if m:
            out.append(line)
            out.extend(table(m.group(1), packages, entries))
            skipping = m.group(1)
            continue
        m = END.match(line)
        if m and skipping == m.group(1):
            skipping = None
            out.append(line)
            continue
        if skipping is None:
            out.append(line)
    return "\n".join(out) + "\n"


def ancestors(packages: list[Package]) -> dict[str, set[str]]:
    """Every package's dependencies, direct and indirect."""
    deps = {p.id: p.depends for p in packages}
    memo: dict[str, set[str]] = {}

    def walk(node: str, seen: frozenset[str]) -> set[str]:
        if node in memo:
            return memo[node]
        out: set[str] = set()
        for d in deps.get(node, []):
            if d in seen:          # a cycle: check reports it
                continue
            out.add(d)
            out |= walk(d, seen | {d})
        memo[node] = out
        return out

    return {p.id: walk(p.id, frozenset([p.id])) for p in packages}


def unblocks(packages: list[Package]) -> dict[str, int]:
    """How many packages depend on each one, directly or indirectly."""
    anc = ancestors(packages)
    count = {p.id: 0 for p in packages}
    for p in packages:
        for a in anc[p.id]:
            if a in count:
                count[a] += 1
    return count


def needs_game_files(p: Package) -> bool:
    """The package's own work (not only its '(with the dump)' checks) needs staged game files."""
    return "dump" in p.needs or ("save" in p.needs and "dump-check" not in p.needs)


def ready(packages: list[Package], entries: dict[str, Entry], no_dump: bool = False,
          assume_done: set[str] | None = None) -> list[Package]:
    assume_done = assume_done or set()
    order = {p.id: i for i, p in enumerate(packages)}
    count = unblocks(packages)
    out = []
    def satisfied(d: str) -> bool:
        e = entries.get(d, Entry())
        if d in assume_done:
            return "waiting for" not in e.note      # merged, but its decision isn't made yet
        return e.status == "done"

    for p in packages:
        if p.id in assume_done or entries.get(p.id, Entry()).status != "todo":
            continue
        if not all(satisfied(d) for d in p.depends):
            continue
        if no_dump and needs_game_files(p):
            continue
        out.append(p)
    return sorted(out, key=lambda p: (-count[p.id], order[p.id]))


def overlaps(packages: list[Package], ids: list[str] | None, entries: dict[str, Entry],
             assume_done: set[str] | None = None) -> list[tuple[str, str, str, str]]:
    """(A, B, pattern of A, pattern of B) for every pair whose allowlists share a file.

    With ids, the pairs among those packages; without, every pair that could be in flight
    at once: neither depends on the other and neither is done."""
    assume_done = assume_done or set()
    by_id = {p.id: p for p in packages}
    if ids is not None:
        chosen = [by_id[i] for i in ids]
        anc = None
    else:
        chosen = [p for p in packages
                  if p.id not in assume_done and entries.get(p.id, Entry()).status != "done"]
        anc = ancestors(packages)
    found = []
    for i, a in enumerate(chosen):
        for b in chosen[i + 1:]:
            if anc is not None and (a.id in anc[b.id] or b.id in anc[a.id]):
                continue
            for pa in a.files or []:
                hit = next((pb for pb in b.files or [] if patterns_overlap(pa, pb)), None)
                if hit:
                    found.append((a.id, b.id, pa, hit))
                    break
    return found


def set_status(text: str, wp: str, status: str, note: str | None, add_note: str | None = None) -> str:
    if status == "ready":
        status = "todo"
    if status not in STATUSES:
        raise SpecError(f"unknown status {status!r}")
    out, found = [], False
    for line in text.splitlines():
        if ROW.match(line) and _cells(line)[0] == wp:
            cells = _cells(line)
            cells[6] = f"`{status}`"
            if note is not None:
                cells[7] = note
            elif add_note is not None:
                cells[7] = f"{add_note}; {cells[7]}" if cells[7] else add_note
            line = "| " + " | ".join(_escape(c) for c in cells) + " |"
            found = True
        out.append(line)
    if not found:
        raise SpecError(f"{wp} is not in STATUS.md")
    return "\n".join(out) + "\n"


def parse_ids(values: list[str] | str | None, known: set[str]) -> set[str]:
    if isinstance(values, str):
        values = [values]
    ids = {t.strip() for v in (values or []) for t in v.split(",") if t.strip()}
    unknown = sorted(ids - known)
    if unknown:
        raise SpecError(f"unknown packages: {', '.join(unknown)}")
    return ids


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--status-file", default=STATUS_PATH)
    ap.add_argument("--wp-dir", default=WP_DIR)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check")
    sub.add_parser("sync")
    sub.add_parser("pending")
    r = sub.add_parser("ready")
    r.add_argument("--no-dump", action="store_true", help="leave out packages that need the game files staged")
    r.add_argument("--assume-done", nargs="+", default=[], metavar="ID",
                   help="IDs merged into this session's batch (commas or spaces)")
    o = sub.add_parser("overlaps")
    o.add_argument("ids", nargs="*")
    o.add_argument("--all", action="store_true")
    o.add_argument("--assume-done", nargs="+", default=[], metavar="ID",
                   help="IDs to treat as done (with --all)")
    s = sub.add_parser("set")
    s.add_argument("id")
    s.add_argument("status")
    notes = s.add_mutually_exclusive_group()
    notes.add_argument("--note")
    notes.add_argument("--add-note")
    args = ap.parse_args(argv)

    packages, spec_errors = parse_specs(spec_paths(args.wp_dir))
    with open(args.status_file, encoding="utf-8") as f:
        text = f.read()
    entries = parse_status(text)
    try:
        phases = marker_phases(text)
    except SpecError as e:
        print(f"STATUS.md: {e}")
        return 1

    if args.cmd == "check":
        found = spec_errors + problems(packages, entries, phases)
        if not spec_errors and sync(text, packages) != text:
            found.append("STATUS.md tables are not in sync with the specs (run sync)")
        for line in found:
            print(line)
        print(f"{len(packages)} packages, {len(found)} problems")
        return 1 if found else 0
    if spec_errors:
        for line in spec_errors:
            print(line, file=sys.stderr)
        print("fix the specs first (wp_status.py check lists everything)", file=sys.stderr)
        return 1
    if args.cmd == "sync":
        with open(args.status_file, "w", encoding="utf-8") as f:
            f.write(sync(text, packages))
        return 0
    known = {p.id for p in packages}
    if args.cmd == "ready":
        try:
            assumed = parse_ids(args.assume_done, known)
        except SpecError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
        count = unblocks(packages)
        for p in ready(packages, entries, args.no_dump, assumed):
            print(f"{p.id}\t{p.size}\t{p.agent}\t{','.join(p.needs)}\tunblocks={count[p.id]}\t{p.title}")
        return 0
    if args.cmd == "overlaps":
        try:
            assumed = parse_ids(args.assume_done, known)
            wanted = parse_ids(args.ids, known)
            ids = None if args.all else [p.id for p in packages if p.id in wanted]
        except SpecError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
        if not args.all and len(wanted) < 2:
            print("error: give two or more IDs, or --all", file=sys.stderr)
            return 2
        found = overlaps(packages, ids, entries, assumed)
        for a, b, pa, pb in found:
            print(f"{a} {b}: {pa}" + ("" if pa == pb else f" ~ {pb}"))
        return 1 if found else 0
    if args.cmd == "pending":
        for p in packages:
            e = entries.get(p.id, Entry())
            if PENDING_RE.search(e.note):
                print(f"{p.id}\t{e.status}\t{e.note}")
        return 0
    if args.cmd == "set":
        try:
            text = set_status(text, args.id, args.status, args.note, args.add_note)
        except SpecError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
        with open(args.status_file, "w", encoding="utf-8") as f:
            f.write(sync(text, packages))
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
